# weatherbot — sample PushBot client

A small, dependency-free Python reference implementation that drives
the `weather` ghost configured in `doc/conf/examples/pushbot.conf`.

It demonstrates every piece of the gateway protocol you need to write
your own bot:

| Phase | What it shows |
|-------|---------------|
| WS upgrade + IDENTIFY + RESUME | `ws_connect`, `run_once`, `SessionState` |
| HELLO / heartbeat / HEARTBEAT_ACK | `heartbeat_thread` |
| COMMAND_REGISTER + COMMANDS_REGISTERED ack | `COMMAND_SCHEMA`, `OP_COMMAND_REGISTER` send |
| COMMAND_INVOKE dispatch + INTERACTION_RESPONSE | `handle_dispatch` |
| INTERACTION_DEFER for slow handlers | `cmd_forecast` defers before hitting wttr.in |
| MESSAGE_CREATE @mention reply | `MESSAGE_CREATE` branch in `handle_dispatch` |
| REST as the bot ghost | `rest_send_channel` |
| auto-reconnect with exponential backoff | `main` loop |

## Run it

Set up a `bot { … }` block in your pushbot config that matches the
nick + token the bot will use, then:

```bash
PUSHBOT_HOST=obby.example.com \
PUSHBOT_PORT=6670 \
PUSHBOT_TOKEN=<your-long-random-token> \
PUSHBOT_NICK=weather \
python3 weatherbot.py
```

For a persistent install with systemd, drop `weatherbot.service`
into `/etc/systemd/system/` (edit the `Environment=` and `User=`
lines first), then:

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now weatherbot.service
```

Logs go to `/var/log/weatherbot.log`.

## Commands it registers

- `/forecast <city>` — public, fetches a short summary from wttr.in
- `/flip` — public, classic coin flip

It also responds to MESSAGE_CREATE events that mention the bot's
nick by name (in any channel it's in) with a one-line "hi, try
/forecast or /flip" via the REST API.

## What it deliberately keeps simple

- No `permessage-deflate` (the gateway doesn't compress yet).
- Text frames only — no fragmentation, no binary.
- No persistence: if it crashes and the resume window (60 s) expires
  the bot just IDENTIFYs fresh and re-registers its commands.
- No interaction-response REST path — the bot always answers over
  the gateway with `op=21 INTERACTION_RESPONSE`.

For a richer example (option type coercion, choices, server-wide
bots, multi-step interactions) build on top of this scaffold; the
wire format is identical regardless of language.
