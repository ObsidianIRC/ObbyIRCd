# PushBot — how users and bots interact

Visual companion to [`pushbot-spec.md`](./pushbot-spec.md).  Five Mermaid diagrams: a 30,000-ft
architecture view, then one sequence diagram per major flow.

---

## 1. System architecture

A PushBot is two things at once:

- a **real IRC user** (the ghost) — joins channels, shows up in `/NAMES`,
  receives PRIVMSGs alongside everyone else
- an **external process** somewhere on the internet that drives the ghost
  via either a long-lived WebSocket gateway or stateless HTTPS webhooks

```mermaid
flowchart LR
    subgraph Users["IRC users"]
        Alice([alice])
        Bob([bob — ObsidianIRC])
    end

    subgraph IRCd["obbyircd"]
        direction TB
        Hooks["chanmsg / usermsg / join / part / kick hooks"]
        WS["WS gateway<br/>:6670/pushbot/v1/gateway"]
        REST["REST API<br/>:6670/pushbot/v1/..."]
        Ghost1(("weather ghost +B"))
        Ghost2(("help ghost +B+S"))
        DB[("pushbot.db")]
    end

    subgraph Bots["external bot processes"]
        Wbot["weatherbot.py<br/>transport: gateway"]
        Hbot["help-bot<br/>transport: webhook"]
    end

    Alice -. "PRIVMSG / TAGMSG" .-> IRCd
    Bob   -. "PRIVMSG / TAGMSG" .-> IRCd
    Ghost1 -. "appears in /NAMES" .-> Users
    Ghost2 -. "appears in /NAMES" .-> Users

    Hooks --> WS
    Hooks --> Hbot
    WS --> Wbot
    Wbot --> WS
    Wbot --> REST
    Hbot --> REST
    REST --> Ghost1
    REST --> Ghost2
    IRCd --- DB
```

Channel bots are scoped to specific channels (`scope=channel`, umode `+B`).
Server-wide bots like `/help` are reachable from any channel
(`scope=server`, umodes `+B+S`).  The `pushbot.db` SQLite store holds
registrations, hashed tokens, and the dead-letter queue.

---

## 2. Slash command lifecycle

User types `/forecast london` in `#weather`.  The PushBot client
serializes the invocation as a `TAGMSG` with `+draft/bot-cmd`, the IRCd
validates and routes it to the bot's gateway as `COMMAND_INVOKE`, and the
bot's reply comes back as a normal PRIVMSG tagged `+reply`.

```mermaid
sequenceDiagram
    participant U as alice (client)
    participant S as obbyircd
    participant B as weatherbot

    Note over U: types "/forecast london"
    U->>S: "@+draft/bot-cmd=b64 TAGMSG #weather"
    Note over S: validate token, schema, requires{}, channel-context
    S->>B: "op=0 DISPATCH t=COMMAND_INVOKE { id, channel, invoker, options }"

    alt slow handler
        B-->>S: "op=22 INTERACTION_DEFER (extend window 3s → 15s)"
    end

    B->>S: "op=21 INTERACTION_RESPONSE { id, content, visibility }"
    Note over S: look up interaction id, render as PRIVMSG or NOTICE
    S->>U: "@+reply=msgid :weather PRIVMSG #weather :..."
    Note over U: rendered as a reply to alice's original line
```

If the bot never responds within 3 s (or 15 s after defer), the IRCd
sends `:server FAIL BOTCMD TIMEOUT weather :bot timed out` to alice.
Server-wide bots use the same flow but route on bot nick instead of
channel-membership.

---

## 3. Channel message events

Regular chatter in a channel the bot is in fires `MESSAGE_CREATE` on the
bot's gateway / webhook.  This is also how bot @mentions work — the bot
sees the message and chooses how to reply (or stays quiet).

```mermaid
sequenceDiagram
    participant U as alice
    participant S as obbyircd
    participant Wbot as weatherbot (gateway)
    participant Hbot as helpbot (webhook)

    U->>S: "PRIVMSG #general :@weather what's up?"
    Note over S: chanmsg hook fires for every PbBot in #general

    par gateway delivery
        S->>Wbot: "op=0 DISPATCH t=MESSAGE_CREATE { author, content, mention_bot:true }"
    and webhook delivery
        S->>Hbot: "HTTPS POST + X-PushBot-Signature: sha256=..."
        Hbot-->>S: "200 OK (or inline action)"
    end

    opt bot decides to reply
        Wbot->>S: "POST /pushbot/v1/channels/#general/messages"
        S->>U: ":weather PRIVMSG #general :hi alice!"
    end
```

Webhook delivery includes an HMAC-SHA256 signature so the bot can verify
the POST came from the IRCd.  Failures get retried (1s → 5s → 30s → 5m)
then dead-lettered; after a configurable failure window the bot is
auto-suspended with a server-notice to opers.

---

## 4. Command discovery

The popover in ObsidianIRC populates from `+draft/bot-cmds-query` TAGMSGs
sent to each `+B` user the client shares a channel with.  The server
either answers directly (for PushBots) or relays the query to the bot
which answers itself (for obbyscript/obbypy bots that implement the
responder).  The base64-JSON payload carries the full schema:
description, options, types, choices, requirements.

```mermaid
sequenceDiagram
    participant C as ObsidianIRC (bob)
    participant S as obbyircd
    participant B as weatherbot

    Note over C: WHO #weather completes, sees +B user "weather"
    C->>S: "@+draft/bot-cmds-query=1 TAGMSG weather"

    alt bot is a PushBot
        Note over S: server answers from cached b->commands
        S->>C: "@+draft/bot-cmds=b64 :weather TAGMSG bob"
    else third-party +B bot
        S->>B: TAGMSG with query tag (proxied)
        B->>S: TAGMSG with response tag
        S->>C: "@+draft/bot-cmds=b64 :weather TAGMSG bob"
    end

    Note over C: decode JSON, cache in server.botCommands[weather]

    opt bot's commands change later
        B->>S: "COMMAND_REGISTER (new schema)"
        S->>C: "@+draft/bot-cmds-changed :weather TAGMSG #weather"
        Note over C: invalidate cache, re-query on next use
    end
```

Cache is keyed by `(serverId, botNick)` with a 24 h TTL or until
explicit invalidation by `+draft/bot-cmds-changed`.

---

## 5. Bot connection lifecycle (gateway transport)

The opcode-driven state machine the gateway implements.  Identical in
shape to Discord's gateway, with one obby-specific bolt-on:
`COMMAND_REGISTER` (op=20), which is how slash command schemas get
published from the bot to the server after IDENTIFY.

```mermaid
sequenceDiagram
    participant B as weatherbot
    participant S as obbyircd

    Note over B,S: TLS + HTTP/1.1 upgrade
    B->>S: "GET /pushbot/v1/gateway + Authorization: Bearer token"
    S-->>B: "101 Switching Protocols"

    S->>B: "op=10 HELLO { heartbeat_interval: 30000 }"

    alt fresh connection
        B->>S: "op=2 IDENTIFY { token }"
        S->>B: "op=0 DISPATCH t=READY { session_id, bot_nick, channels }"
    else resuming
        B->>S: "op=6 RESUME { session_id, seq }"
        alt session still in queue
            S->>B: "(replay queued DISPATCHes by seq)"
            S->>B: "op=0 DISPATCH t=RESUMED { replayed: N }"
        else session expired
            S-->>B: "WS close 4006 Unknown session_id"
            Note over B: drop resume id, reconnect as fresh IDENTIFY
        end
    end

    B->>S: "op=20 COMMAND_REGISTER { commands: [/forecast, /flip] }"
    S->>B: "op=0 DISPATCH t=COMMANDS_REGISTERED { count }"

    loop normal operation
        B->>S: "op=1 HEARTBEAT { last_seq }"
        S->>B: "op=11 HEARTBEAT_ACK"
        S-->>B: "op=0 DISPATCH (events as they happen)"
        B-->>S: "op=21 INTERACTION_RESPONSE / REST calls"
    end

    alt server asks bot to reconnect
        S->>B: "op=7 RECONNECT"
        Note over B: close, then go around again with RESUME
    else bot displaced by second IDENTIFY
        S->>B: "WS close 4001 AUTH_FAILED — Session displaced by new IDENTIFY"
        Note over B: drop resume id, treat as INVALID_SESSION
    else queue overflow
        S->>B: "WS close 4011 QUEUE_OVERFLOW"
        Note over B: reconnect, RESUME catches up from buffered events
    end
```

Webhook bots skip everything above the `loop` block — they don't open a
gateway, the IRCd just POSTs them events directly and they reply with an
optional inline action.

---

*See [`doc/examples/weatherbot/`](../examples/weatherbot/) for a working
reference implementation of the gateway side of this protocol.*
