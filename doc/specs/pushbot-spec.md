# IRC PushBot — Specification

Status: **draft for sign-off**.  After agreement, implementation
proceeds in the phases listed in §15.

---

## 1. Motivation

obbyircd already has two in-process scripting modules (`obbyscript`
for JS, `obbypy` for Python).  Those are great for small bots tied
to the IRCd's lifetime, but they require write access to the server,
they lock you into one of two languages, and you can't deploy one to
your own infrastructure.

**PushBot** is the out-of-process analogue.  A PushBot is an HTTP
service somewhere on the internet, written in any language, that
talks to obbyircd over the network:

* a **Gateway WebSocket** for receiving events;
* a **REST API** for sending actions;
* an optional **HTTP-webhook mode** as an alternative to the gateway,
  for serverless bots that can't hold a long-lived connection.

This is the same shape Discord ships.  Discord-style features adopted
verbatim: gateway opcodes, RESUME, slash-command interactions,
permessage-deflate WebSocket compression.

Each PushBot **is a real IRC user** on the network: a registered
services account plus an always-on ghost client that joins channels
and shows up in `/WHO`, `/NAMES`, and `WHOIS`.

---

## 2. Architecture overview

```
                    ┌───────────────────────┐
                    │      obbyircd         │
                    │   ┌────────────────┐  │
   IRC users ◀───▶  │   │  channels +    │  │
                    │   │  client tree   │  │
                    │   └───────┬────────┘  │
                    │           │           │
                    │   ┌───────▼────────┐  │
                    │   │  pushbot       │  │
                    │   │  module        │  │
                    │   └───┬───────┬────┘  │
                    └───────┼───────┼───────┘
                            │       │
                Gateway     │       │    REST
                (WS, server │       │    (HTTPS, bot
                 → bot)     │       │     → server)
                            ▼       ▲
                    ┌───────────────────────┐
                    │   PushBot service     │
                    │   (anywhere,          │
                    │    any language)      │
                    └───────────────────────┘
```

Or, for serverless / webhook mode:

```
   obbyircd  ──HTTPS POST──▶  bot URL  ──HTTPS resp──▶  obbyircd
                  (event)                 (action)
```

Single-server design.  Multi-server S2S sync is out of scope for v1;
we'll revisit if obbyircd grows real linked-network deployments.

---

## 3. Bot identity & lifecycle

### 3.1 What a PushBot looks like on the network

Each bot has:

* a unique **nick** (e.g. `weather`);
* a **services account** of the same name (auto-registered);
* user mode **`+B`** (channel bots); server-wide bots additionally get
  **`+S`** (service);
* a virtual **ghost client** owned by the pushbot module — no real
  socket, attached as a module-owned `Client*` the way services
  pseudo-clients work.

The ghost exists from the moment the bot is approved until the bot is
deleted or suspended.  This is **not** tied to whether the bot's
gateway connection is currently attached.  Rationale: keeping the
ghost connected means `/WHO`, `/NAMES`, and message routing stay
consistent when the bot's gateway disconnects briefly.

When the bot is suspended or deleted, the ghost QUITs with a suitable
reason ("PushBot suspended by admin").

### 3.2 Online vs offline status

The bot's `AWAY` state mirrors gateway/webhook reachability:

* gateway connected, OR webhook URL returning 2xx in the last 5 min →
  not away;
* otherwise → away with message "bot offline".

### 3.3 Two scopes: channel bots and server-wide bots

| Scope         | Joins channels? | Default command visibility | Umodes |
|---------------|-----------------|----------------------------|--------|
| Channel bot   | yes (per-channel approval, §6) | per-command (`public`/`private`) | `+B` |
| Server-wide bot | no — always reachable network-wide | `private` (override via `force-public`) | `+B+S` |

Server-wide bots resolve **after** channel bots in slash dispatch:
`/help` in a channel containing a `help` channel-bot uses the
channel-bot; `/help` in a channel without one falls through to the
network-wide `help` bot if any.

### 3.4 Registration modes

The IRCd config sets a global default; per-bot blocks may override:

```
pushbot {
    mode "approval";   /* admin | approval | open */
};
```

| Mode       | Who can register?                          | When does it go live?           |
|------------|--------------------------------------------|---------------------------------|
| `admin`    | Only via `pushbot { bot { } }` config blocks | After `/REHASH`                 |
| `approval` | Any logged-in account via REST register    | After an IRCop approves         |
| `open`     | Any logged-in account via REST register    | Immediately                     |

All three are implemented; admin picks per-server.

### 3.5 Permissions

Per-bot, set at registration / by admin:

| Permission              | Default | Description                                |
|-------------------------|---------|--------------------------------------------|
| `send-messages`         | yes     | Bot can PRIVMSG/NOTICE                     |
| `read-messages`         | yes     | Bot receives MESSAGE_CREATE                |
| `react`                 | yes     | Bot can react and unreact                  |
| `redact`                | no      | Bot can delete messages it didn't send     |
| `bypass-channel-modes`  | no      | Bot ignores +m, +R, etc.                   |
| `see-private-channels`  | no      | Bot can see +s channels w/o being a member |
| `manage-channel`        | no      | Bot can KICK / MODE / TOPIC                |
| `bypass-privacy`        | no      | Bot receives unredacted Client structs     |
| `slash-commands`        | yes     | Bot can register slash commands            |

---

## 4. Gateway WebSocket

### 4.1 Connection

Endpoint: `wss://<server>:<port>/pushbot/v1/gateway`

Served on a dedicated `listen { type pushbot; }` block (§9).  TLS
required externally — refuses to start without TLS unless the IRCop
sets `allow-insecure`.

Handshake:

```
GET /pushbot/v1/gateway HTTP/1.1
Host: irc.example.com
Upgrade: websocket
Connection: Upgrade
Sec-WebSocket-Key: ...
Sec-WebSocket-Version: 13
Sec-WebSocket-Extensions: permessage-deflate
Authorization: Bearer <bot_token>
```

`permessage-deflate` (RFC 7692) is negotiated by default; bots may
opt out.

### 4.2 Frame format

JSON text, Discord-style:

```json
{ "op": 0, "t": "MESSAGE_CREATE", "s": 42, "d": { ... } }
```

* `op` — opcode (table below)
* `t`  — event type, only set on dispatch (`op` = 0)
* `s`  — sequence number, monotonic per session
* `d`  — payload

### 4.3 Opcodes

| Op | Name                | Direction      | Purpose                          |
|----|---------------------|----------------|----------------------------------|
| 0  | DISPATCH            | server → bot   | Event in `t` + `d`               |
| 1  | HEARTBEAT           | bot → server   | Keep-alive, contains last seen `s` |
| 2  | IDENTIFY            | bot → server   | First frame after connect        |
| 6  | RESUME              | bot → server   | Replay events after reconnect    |
| 7  | RECONNECT           | server → bot   | "Disconnect and resume"          |
| 9  | INVALID_SESSION     | server → bot   | "Resume rejected, IDENTIFY again" |
| 10 | HELLO               | server → bot   | Sent right after upgrade         |
| 11 | HEARTBEAT_ACK       | server → bot   | Ack for a HEARTBEAT              |

### 4.4 IDENTIFY

```json
{
  "op": 2,
  "d": {
    "token": "<bot_token>",
    "filter_overrides": { ... }   // optional, see §5
  }
}
```

Server replies with a `READY` dispatch carrying the bot's profile,
the channels it's currently in, and the `session_id` + `resume_url`
for reconnection.

### 4.5 Resume

On disconnect → bot reconnects → sends `op=6 RESUME { session_id,
seq }`.  The server replays events with `s` > `seq` from the
per-bot queue (§4.7) and the connection continues.

Resume buffer TTL: 60 s.  After that, `op=9 INVALID_SESSION` and the
bot must `IDENTIFY` fresh — events between then and now are lost.

### 4.6 Event types (initial set)

| Event              | When fired                                              |
|--------------------|---------------------------------------------------------|
| `READY`            | After IDENTIFY succeeds                                 |
| `MESSAGE_CREATE`   | PRIVMSG in a channel the bot is in, or DM (if accepted) |
| `MESSAGE_DELETE`   | REDACT for a message the bot saw                        |
| `REACTION_ADD`     | Reaction added to a message the bot saw                 |
| `REACTION_REMOVE`  | Reaction removed                                        |
| `CHANNEL_JOIN`     | A user joined a channel the bot is in                   |
| `CHANNEL_PART`     | A user parted                                           |
| `CHANNEL_KICK`     | A user was kicked                                       |
| `USER_NICK_CHANGE` | A user the bot can see changed nick                     |
| `COMMAND_INVOKE`   | A user ran a slash command this bot registered          |
| `BOT_INVITED`      | A user `/INVITE`d the bot to a channel                  |
| `BOT_KICKED`       | The bot was kicked                                      |
| `BOT_JOIN_REQUEST` | Someone requested adding the bot to a channel (§6)      |
| `RESUMED`          | After a successful RESUME                               |

### 4.7 Backpressure: no event loss

Per-bot event queue (default 1024 entries, configurable).  When the
bot's gateway socket is slow:

1. Events buffer in the queue.
2. If the queue hits its cap, the IRCd closes the bot's gateway with
   code `4011` ("queue overflow, resume to catch up").
3. The bot reconnects via RESUME and gets the buffered events in
   sequence.

The IRCd's main send loop is **never** blocked on a bot — bots are
isolated from the rest of the network's send throughput.

If the queue overflows AND the bot has been disconnected for longer
than the resume TTL, the **oldest events are dropped** to keep the
queue size bounded, and the bot gets `INVALID_SESSION` on next
connect.  This is the only path that loses events, and it requires
both prolonged disconnection *and* a flood — both observable
operationally.

### 4.8 Example: MESSAGE_CREATE payload

```json
{
  "op": 0,
  "t": "MESSAGE_CREATE",
  "s": 42,
  "d": {
    "msgid": "abc123",
    "channel": { "name": "#weather", "topic": "Weather chat" },
    "author": { "nick": "alice", "account": "alice",
                "is_oper": false, "is_secure": true,
                "is_bot": false, "is_logged_in": true,
                "umodes": "+iwx" },
    "content": "@weather what's it like in London?",
    "tags": { "time": "2026-05-15T20:31:00.123Z", "msgid": "abc123" },
    "mention_bot": true
  }
}
```

Author shape is the redacted-Client form (§7).  Channel shape is the
RPC-style Channel form (§7).

---

## 5. Triggers (per-bot subscription filters)

Replaces Discord's "intents bitfield".  Each bot declares filters
in config (or REST update).  The bot's IDENTIFY may further narrow
within the configured bounds.

```
pushbot "weather" {
    triggers {
        on message {
            channels  { "#weather"; "#general"; };
            match     "prefix";        /* prefix | exact | anywhere | regex */
            pattern   "weather ";      /* glob; for regex use pattern-regex */
            from {
                logged-in        yes;
                bots             no;
                opers            any;       /* any | yes | no */
                min-channel-rank "voice";   /* voice | halfop | op | admin | owner */
                match { ... };               /* full match-block, §11.2 */
            };
        };

        on dm {
            accept yes;        /* if no, server refuses PRIVMSG/NOTICE to bot */
            from { match { ... }; };
        };

        on command "weather";   /* slash command — always delivered */
    };
};
```

Pattern matching engine: **glob by default** (`*` and `?`,
ban-mask style); `pattern-regex` opts into RE2-syntax regex with a
per-match CPU timeout cap (server-side, hard-coded at 50 ms).

---

## 6. Channel-bot management (the cap layer)

### 6.1 Capability

**`obby.world/channel-bots`** — negotiated via CAP LS / REQ.  Once
negotiated, a client receives:

* burst of server-wide bots' command lists at CAP ACK time (§6.4);
* burst of each channel's bot list on JOIN;
* push notifications when bots are added / removed / change commands;
* pending-request notifications (only if the user holds the
  `manage-bots` member-role permission in the relevant channel).

The cap also **enables slash-command invocation** for cap-aware
clients (the structured TAGMSG dispatch path).  Non-cap clients see
bots as plain `+B` users and can only interact via raw PRIVMSG.

### 6.2 Member-roles permission

**`manage-bots`** — letterless, lives next to `manage-channel`,
`manage-bans`, etc. in the member-roles module.  Channel-scoped.
Holding this permission grants:

* add a bot to the channel directly (`POST /channels/:chan/bots`)
* remove a bot
* approve / deny pending bot-add requests
* view & manage per-channel bot command visibility

### 6.3 Request queue lifecycle

* **Anyone in the channel** can `POST /channel/:name/bot-requests`
  with `{ bot_nick, message? }`.  Request enters `pending`.
* **A bot** can `POST` the same endpoint on behalf of itself
  ("I'd like to be added to #weather").
* **A `manage-bots` holder** sees the queue, approves or denies.
* **Approve** → bot auto-joins channel.
* **Deny** → request closed, optional reason returned to requester.
* Expiry: pending requests auto-close after **7 days**.
* Re-request after denial: rate-limited to **1 per 24 h** per
  (bot, channel) pair.

### 6.4 Bot discovery burst

When `obby.world/channel-bots` is acked the server emits a `BATCH`
of type `obby.world/bot-list`, each member a TAGMSG carrying the
existing `+draft/bot-cmds` tag (§7.2).  Same TAGMSG-with-bot-cmds
shape is reused for:

* server-wide bots (sent at CAP ACK);
* channel bots (sent on JOIN);
* incremental updates (sent ad-hoc).

A single discovery format covers all cases — clients learn one
parser.

### 6.5 Request-queue UI (ObsidianIRC)

Dedicated modal accessible from the channel header for users with
`manage-bots`.  Shows pending requests with requester, bot, message,
timestamp, and accept/deny buttons.  Non-permitted users see a
"request bot for this channel" entry instead, which opens the
request-creation flow.

---

## 7. Slash-command protocol (`draft/bot-cmds`)

The same TAGMSG protocol used for discovery AND invocation.  Works
for any +B user, not just PushBots — obbyscript/obbypy bots and any
third-party IRC bot can participate by implementing the responder.

### 7.1 Three tags

| Tag | Direction | Carrier | Purpose |
|---|---|---|---|
| `+draft/bot-cmds-query` | client → bot | TAGMSG | "Tell me your commands" |
| `+draft/bot-cmds` | bot → client | TAGMSG (in BATCH) | The commands list |
| `+draft/bot-cmds-changed` | bot → channel | TAGMSG | "My commands changed, refetch" |

Plus the invocation tag:

| Tag | Direction | Carrier | Purpose |
|---|---|---|---|
| `+draft/bot-cmd` | client → bot/channel | TAGMSG | Slash-command invocation |

### 7.2 Discovery flow

```
Client → bot:    @+draft/bot-cmds-query=1  TAGMSG weather :

Bot → client:    BATCH +xyz draft/bot-cmds
                 @batch=xyz;+draft/bot-cmds=<base64-json>  TAGMSG alice :
                 BATCH -xyz
```

Response value = base64-encoded JSON (tag-value escapes are a
nightmare otherwise):

```json
{
  "version": 1,
  "commands": [
    {
      "name": "weather",
      "description": "Look up the weather",
      "visibility": "public",       // public | private
      "scopes": ["channel", "dm"],
      "options": [
        { "name": "city", "type": "string", "required": true,
          "description": "Where?",
          "choices": ["london", "berlin", "tokyo"] }
      ],
      "requires": {                   // optional gating
        "min-channel-rank": "voice",
        "match": {                    // match-block, §11.2
          "security-group": ["known-users"],
          "tls": true
        }
      }
    }
  ]
}
```

Option `type` ∈ `string | int | bool | user | channel`.

### 7.3 Caching + invalidation

Clients cache the list by `(serverId, botNick)`.  Invalidate on:

* `+draft/bot-cmds-changed` for that bot;
* hard TTL: 24 h;
* explicit user "refresh" action.

### 7.4 Invocation

User runs `/weather london` in `#weather`.

* If the command's `visibility = "public"` →
  `@+draft/bot-cmd=<b64>  TAGMSG #weather :`
* If `visibility = "private"` →
  `@+draft/bot-cmd=<b64>;+draft/channel-context=#weather  TAGMSG weather :`

`<b64>` payload:

```json
{ "name": "weather", "options": { "city": "london" } }
```

DM invocation (no channel context): omit `+draft/channel-context`.

### 7.5 Server-side validation

When a `+draft/bot-cmd` TAGMSG arrives, the IRCd MUST:

1. Resolve the target to a registered bot.
2. Validate the JSON against the bot's registered command schema —
   wrong types, missing required options → FAIL `INVALID_CMD_ARGS`.
3. Evaluate the command's `requires` clause against the invoker;
   fail with `INSUFFICIENT_PRIVS` if it doesn't match.
4. If `+draft/channel-context` is present, validate that the
   invoker AND the bot are both in that channel; on mismatch FAIL
   `INVALID_CHANNEL_CONTEXT` (never silently strip).
5. Dispatch as `COMMAND_INVOKE` on the bot's gateway / webhook.

### 7.6 Replying

**Public reply** (channel-visible):

```
@+reply=<original-msgid>  PRIVMSG #weather :London: 14°C, light rain
```

**Private reply** (whisper, channel-context preserved):

```
@+reply=<original-msgid>;+draft/channel-context=#weather  NOTICE alice :London: 14°C, light rain
```

ObsidianIRC renders the NOTICE in the channel's active view as a
whisper-reply (same as existing `+draft/channel-context` handling).

Bots that don't reply within **3 seconds** → IRCd sends a FAIL
standard-reply to the invoker: "bot timed out".  Bots that need
more time send an interim `defer` action via REST, extending the
window to 15 s.

### 7.7 Name collisions

Within a single channel, command names are unique.  Registering a
duplicate fails at the REST API with `409 Conflict`.

A user may always disambiguate by typing `/cmd@botnick` even when
no collision exists — useful for resolving channel-bot vs
server-wide-bot ambiguity manually.

### 7.8 Bot-offline behavior

If a slash command resolves to a bot whose gateway is closed AND
whose webhook hasn't responded 2xx in 5 min, the IRCd sends a FAIL
`BOT_OFFLINE` to the invoker.  Slash invocations are **not** queued
(unlike events); a 10-minute-late command response is useless.

---

## 8. REST API

Base path: `https://<server>:<port>/pushbot/v1/`
Auth: `Authorization: Bearer <bot_token>` on every request.

### 8.1 Endpoints

| Method | Path                                       | Description                                |
|--------|--------------------------------------------|--------------------------------------------|
| GET    | `/bot`                                     | Get this bot's profile + permissions       |
| PATCH  | `/bot`                                     | Update realname, webhook_url, triggers     |
| GET    | `/channels`                                | List channels the bot is in                |
| POST   | `/channels/:name/join`                     | Join (needs invite if +i)                  |
| POST   | `/channels/:name/part`                     | Part                                       |
| POST   | `/channels/:name/messages`                 | PRIVMSG to channel                         |
| POST   | `/channels/:name/messages/:msgid/react`    | Add reaction                               |
| DELETE | `/channels/:name/messages/:msgid/react/:emoji` | Remove reaction                        |
| DELETE | `/channels/:name/messages/:msgid`          | Redact (needs `redact` permission)         |
| POST   | `/users/:nick/messages`                    | DM (NOTICE)                                |
| GET    | `/channels/:name/members`                  | List members                               |
| POST   | `/commands`                                | Register / replace command list            |
| DELETE | `/commands/:name`                          | Unregister                                 |
| POST   | `/interactions/:id/respond`                | Respond to COMMAND_INVOKE                  |
| POST   | `/interactions/:id/defer`                  | Buy 15 s for a slow reply                  |
| POST   | `/channel/:name/bot-requests`              | Request adding self to a channel           |
| GET    | `/channel/:name/bot-requests`              | List pending requests (manage-bots only)   |
| POST   | `/channel/:name/bot-requests/:id/approve`  | Approve (manage-bots only)                 |
| POST   | `/channel/:name/bot-requests/:id/deny`     | Deny                                       |

### 8.2 Idempotency, rate limits

All POSTs accept `Idempotency-Key`; same key+body within 60 s
returns the original result.

Per-bot rate limit defaults: 10 actions/s, 200/min.  Headers
`X-PushBot-RateLimit-{Limit,Remaining,Reset}`; `429` with
`Retry-After` on exceed.

---

## 9. HTTP-webhook mode

Alternative transport for serverless bots.  At registration the bot
sets `transport=webhook` and a `webhook_url`.

Request shape:

```
POST <webhook_url>
Content-Type: application/json
X-PushBot-Event: COMMAND_INVOKE
X-PushBot-Timestamp: 1748287200
X-PushBot-Signature: sha256=<hex(HMAC-SHA256(secret, timestamp + "." + body))>

<same JSON body as the gateway DISPATCH `d` field>
```

Replay window: ±5 min on the timestamp.

Inline-action response is supported:

```http
HTTP/1.1 200 OK
Content-Type: application/json

{ "type": "send_message", "channel": "#weather",
  "content": "London: 14°C, light rain" }
```

Supported inline action types: `send_message`, `react`,
`ephemeral_reply`, `defer`.

Retries: 1s → 5s → 30s → 5m, then dead-letter.  Bots failing
continuously for >24 h auto-suspend with a server-notice to opers.

---

## 10. Config block

```
listen {
    ip     "*";
    port   6669;
    type   pushbot;
    tls;
    tls-options { certificate "cert.pem"; key "key.pem"; };
};

pushbot {
    /* Registration policy default */
    mode "approval";

    /* Persistence */
    database "pushbot.db";

    /* Auto-suspend persistently-failing webhook bots */
    webhook-failure-suspend-after 86400;

    /* Default permissions for self-registered (open / approval) bots */
    default-permissions {
        send-messages   yes;
        read-messages   yes;
        react           yes;
        redact          no;
        manage-channel  no;
        bypass-privacy  no;
    };

    /* Statically-defined bot (admin mode; works in any policy) */
    bot "weather" {
        token       "<opaque>";
        realname    "Weather Bot";
        scope       "channel";       /* channel | server */
        transport   "gateway";       /* gateway | webhook | both */
        webhook-url "https://my-bot.example.com/hook";
        webhook-secret "<32-byte hex>";
        auto-join { "#weather"; "#general"; };
        triggers { ... };            /* §5 */
        permissions { send-messages yes; read-messages yes; };
    };

    /* Server-wide bot (e.g. /help, /report) */
    bot "help" {
        token "...";
        scope "server";
        transport "gateway";
        permissions { send-messages yes; };
    };
};
```

### 10.1 ISUPPORT advertisement

Loaded module advertises:

* `PUSHBOTAPI=v1` in 005 numerics;
* `obby.world/channel-bots` in CAP LS;
* `draft/bot-cmds` in CAP LS (lets non-PushBot bots — obbyscript /
  obbypy / third-party — opt into the protocol).

---

## 11. Client struct in event payloads

### 11.1 Default redactions

Bot receives a redacted Client unless it holds `bypass-privacy`:

| Field                | Default | With `bypass-privacy` |
|----------------------|---------|------------------------|
| `nick`, `id` (UID)   | yes     | yes                    |
| `account`            | yes     | yes                    |
| `umodes`             | yes     | yes                    |
| `is_secure`, `is_oper`, `is_logged_in`, `is_bot` | yes | yes |
| `realname` (gecos)   | yes     | yes                    |
| `virthost` (cloak)   | yes     | yes                    |
| `ip`                 | **no**  | yes                    |
| `realhost`           | **no**  | yes                    |
| `geoip` (country, asn, asname) | **no** | yes               |
| `reputation`         | **no**  | yes                    |
| `security-groups`    | **no**  | yes                    |
| `idle`, `signon`     | yes     | yes                    |

JSON shape mirrors what `rpc/user.c` already emits — same field
names, same nesting.  Channel struct similarly mirrors
`rpc/channel.c`.

### 11.2 `requires` match-block

Bot commands can require any subset of UnrealIRCd's standard
match-block syntax (which is *much* richer than first glance — see
`src/securitygroup.c`):

| Item                 | Notes                                       |
|----------------------|---------------------------------------------|
| `mask { }`           | nick!user@host globs; `!` prefix for negate |
| `ip { }`             | IP/CIDR list                                |
| `security-group`     | named security-groups                       |
| `webirc`, `websocket`, `tls`, `identified` | boolean flags         |
| `reputation-score`   | `<5` style threshold                        |
| `connect-time`       | `<1h` style minimum                         |
| `rule`               | full crule expression                       |
| any registered extban (e.g. `account`, `country`, `asn`) | dynamic |

Plus the bot-cmds-specific top-level keys:

| Key                  | Notes                                       |
|----------------------|---------------------------------------------|
| `min-channel-rank`   | `voice` / `halfop` / `op` / `admin` / `owner` — required rank on the channel the command was invoked in |

Server evaluates with the existing `unreal_match_iplist` /
`unreal_mask_match_string` machinery before delivering
`COMMAND_INVOKE`.

---

## 12. Storage

SQLite, default `pushbot.db`.

```sql
CREATE TABLE pushbots (
    bot_id        TEXT PRIMARY KEY,
    nick          TEXT UNIQUE NOT NULL,
    account       TEXT UNIQUE NOT NULL,
    realname      TEXT NOT NULL,
    scope         TEXT NOT NULL CHECK (scope IN ('channel','server')),
    token_hash    TEXT NOT NULL,
    webhook_url   TEXT,
    webhook_secret_hash TEXT,
    transport     TEXT NOT NULL CHECK (transport IN ('gateway','webhook','both')),
    status        TEXT NOT NULL CHECK (status IN ('pending','active','suspended','deleted')),
    triggers      TEXT NOT NULL,     /* json */
    permissions   TEXT NOT NULL,     /* json */
    created_at    INTEGER NOT NULL,
    created_by    TEXT NOT NULL,
    approved_by   TEXT,
    approved_at   INTEGER,
    last_seen     INTEGER
);

CREATE TABLE pushbot_channels (
    bot_id   TEXT NOT NULL REFERENCES pushbots(bot_id) ON DELETE CASCADE,
    channel  TEXT NOT NULL,
    PRIMARY KEY (bot_id, channel)
);

CREATE TABLE pushbot_commands (
    bot_id       TEXT NOT NULL REFERENCES pushbots(bot_id) ON DELETE CASCADE,
    name         TEXT NOT NULL,
    description  TEXT NOT NULL,
    visibility   TEXT NOT NULL CHECK (visibility IN ('public','private')),
    scopes       TEXT NOT NULL,        /* json array */
    options      TEXT NOT NULL,        /* json */
    requires     TEXT,                  /* json */
    version      INTEGER NOT NULL,      /* increments on update */
    PRIMARY KEY (bot_id, name)
);

CREATE TABLE pushbot_requests (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    bot_id      TEXT NOT NULL REFERENCES pushbots(bot_id) ON DELETE CASCADE,
    channel     TEXT NOT NULL,
    requested_by TEXT NOT NULL,
    message     TEXT,
    status      TEXT NOT NULL CHECK (status IN ('pending','approved','denied','expired')),
    resolved_by TEXT,
    resolved_at INTEGER,
    deny_reason TEXT,
    created_at  INTEGER NOT NULL,
    expires_at  INTEGER NOT NULL
);

CREATE TABLE pushbot_dead_letters (
    id        INTEGER PRIMARY KEY AUTOINCREMENT,
    bot_id    TEXT NOT NULL REFERENCES pushbots(bot_id) ON DELETE CASCADE,
    event_type TEXT NOT NULL,
    payload   TEXT NOT NULL,
    error     TEXT,
    failed_at INTEGER NOT NULL
);
```

Tokens / webhook secrets are stored as argon2id hashes (same params
as `account-registration`).  Plaintext is returned only at
registration time.

---

## 13. Admin commands & JSON-RPC

### 13.1 IRC commands (IRCop-only)

```
/PUSHBOT LIST
/PUSHBOT INFO <bot-nick>
/PUSHBOT APPROVE <bot-nick>
/PUSHBOT SUSPEND <bot-nick> [reason]
/PUSHBOT UNSUSPEND <bot-nick>
/PUSHBOT DELETE <bot-nick>
/PUSHBOT ROTATE-TOKEN <bot-nick>
/PUSHBOT GRANT <bot-nick> <permission>
/PUSHBOT REVOKE <bot-nick> <permission>
/PUSHBOT DEAD-LETTERS <bot-nick>
```

All admin actions emit a server-notice to snomask `o`.

### 13.2 JSON-RPC mirror

For the hosted-backend / webpanel:

```
pushbot.list           pushbot.get          pushbot.create
pushbot.update         pushbot.delete       pushbot.approve
pushbot.suspend        pushbot.rotate_token
pushbot.dead_letters
```

Uses the existing `rpc-user { }` auth.

---

## 14. Security summary

* TLS required for external pushbot listener.
* Tokens never logged in cleartext; snomask events show only nick +
  hash tail.
* HMAC on every outgoing webhook delivery; timestamp replay ±5 min.
* Bot can be op'd / voiced / banned in channels like any user — the
  standard channel-mode enforcement still applies.
* `+draft/channel-context` validated server-side on every TAGMSG;
  mismatched contexts FAIL the message.
* Slash-command `requires` evaluated server-side before dispatch.
* Default Client struct is redacted (§11.1); `bypass-privacy` is an
  admin-grant-only permission.

---

## 15. Implementation roadmap

* **Phase 1 — Skeleton**.  `pushbot` module shell, config-block
  parsing, SQLite schema migration, ghost-client creation for
  config-defined bots, admin `/PUSHBOT LIST/INFO` commands.  No
  routing yet — bots exist in `/NAMES`, nothing fires.
* **Phase 2 — Gateway**.  `listen { type pushbot; }`, WebSocket
  upgrade + permessage-deflate, IDENTIFY/RESUME/HEARTBEAT loop,
  MESSAGE_CREATE delivery, per-bot queue + backpressure handling.
* **Phase 3 — Triggers**.  Subscription filters (§5) including
  match-block evaluation, pattern engine (glob + regex).
* **Phase 4 — REST**.  REST API for send-message + react + DM +
  join/part + idempotency + rate limits.
* **Phase 5 — Slash commands**.  `draft/bot-cmds` discovery,
  `+draft/bot-cmd` invocation TAGMSG, COMMAND_INVOKE dispatch,
  interaction-response REST, `requires` enforcement.
* **Phase 6 — Webhook mode**.  Outgoing POST delivery with HMAC,
  retries, dead-letter table, auto-suspend.
* **Phase 7 — Channel management cap**.  `obby.world/channel-bots`
  cap, `manage-bots` member-roles permission, request queue REST +
  IRC commands, discovery bursts, ObsidianIRC modal UI.
* **Phase 8 — Self-registration**.  REST register endpoint, approval
  flow.
* **Phase 9 — JSON-RPC parity**.  Mirror admin commands.

Each phase ends with tests + a working slice; we merge phase-by-phase
to the release branch.

---

*End of spec.*  Comments / objections / additions please.  Sign off
to start Phase 1.
