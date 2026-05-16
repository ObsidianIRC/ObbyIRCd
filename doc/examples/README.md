# PushBot examples

Reference Python implementations of the PushBot protocol, all stdlib-only.
Read [`../specs/pushbot-spec.md`](../specs/pushbot-spec.md) and
[`../specs/pushbot-architecture.md`](../specs/pushbot-architecture.md) for
the wire-level details.

## Shared client

[`pushbot_client.py`](pushbot_client.py) is a ~400-line library that
handles WS framing, IDENTIFY/RESUME, the heartbeat thread, reconnect
backoff, and COMMAND_INVOKE dispatch.  Each bot file below is just a
list of `@bot.command(...)` decorators plus the handler functions.

```python
from pushbot_client import PushBot, setup_logging
setup_logging()
bot = PushBot.from_env()

@bot.command(name="ping", description="Reply with pong")
def cmd_ping(invoker, channel, opts):
    return "pong"

bot.run()
```

Slow handlers (anything that hits the network) should pass
`deferred=True` so the client emits `INTERACTION_DEFER` before
running the handler, extending the server-side timeout from 3 s to
15 s.

## Bots

| Bot | Scope | Demonstrates |
|---|---|---|
| [`weatherbot/`](weatherbot/) | **channel** (auto-joins `#weather`, `#general`) | `deferred=True` for a wttr.in lookup, `@mention` reply via REST API |
| [`helpbot/`](helpbot/) | **server-wide** | multiple commands, `choices` array, `visibility=private` for the report flow |
| [`dicebot/`](dicebot/) | **server-wide** | input parsing, no external deps, `/8ball`, `/roll 2d20+3`, `/choose a,b,c` |

### Channel-scope vs server-wide

- **Channel bots** (`scope=channel`, umode `+B`) auto-join a list of
  channels and only handle slash commands invoked in those channels.
  They show up with an amber `channel-bot` badge in ObsidianIRC's
  slash popover.
- **Server-wide bots** (`scope=server`, umodes `+B+S`) don't join any
  channels and are reachable network-wide.  `/help`, `/roll 1d20`,
  `/8ball is it Friday?` from any channel or DM routes to them.
  Purple `server-bot` badge.

If both a channel bot and a server-wide bot publish the same command
name, the channel bot wins for users in that channel; disambiguate
manually with `/cmd@botnick`.

## Running them

### One-off

```bash
PUSHBOT_HOST=obby.example.com PUSHBOT_PORT=6670 \
PUSHBOT_TOKEN=<token> PUSHBOT_NICK=<nick> \
python3 weatherbot/weatherbot.py
```

### As systemd services

Each bot directory ships a `.service` unit.  Edit the
`Environment=PUSHBOT_TOKEN=` line to point at your real token, then:

```bash
sudo cp helpbot/helpbot.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now helpbot.service
journalctl -fu helpbot.service
```

The unit pins `Restart=always` with `RestartSec=5` so a crash or a
brief loss of the IRCd is recovered automatically.  Logs go to
`/var/log/<botname>.log` plus journald.

## Server-side config

Add a matching `bot { }` block under `pushbot { }` for each bot:

```
pushbot {
    mode "admin";
    database "pushbot.db";

    bot "weather" {
        token "<random>";
        realname "Weather Bot";
        scope "channel";
        transport "gateway";
        auto-join { "#weather"; "#general"; };
    };

    bot "help" {
        token "<random>";
        realname "Help Bot";
        scope "server";
        transport "gateway";
    };

    bot "dice" {
        token "<random>";
        realname "Dice Bot";
        scope "server";
        transport "gateway";
    };
};
```

Then `/REHASH` from an IRCop session.  The ghost users materialise
immediately; once your bot processes connect to the gateway and
IDENTIFY, the bots flip from "away (bot offline)" to fully
reachable, and their `/help`, `/forecast`, `/roll` commands start
appearing in the slash popover.

## Implementing a new bot

1. Copy `dicebot/` (smallest example).
2. Replace the command handlers with whatever your bot does.
3. Add a `bot { }` block to your pushbot config with a fresh token.
4. Edit the systemd unit to point at the new dir + token + nick.
5. `systemctl daemon-reload && systemctl enable --now mybot`.
