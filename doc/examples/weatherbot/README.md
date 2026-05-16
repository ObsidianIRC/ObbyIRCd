# weatherbot — channel-scope PushBot example

Reference Python implementation that drives the `weather` ghost
configured in [`../../conf/examples/pushbot.conf`](../../conf/examples/pushbot.conf).
Built on the shared client at [`../pushbot_client.py`](../pushbot_client.py)
— see [`../README.md`](../README.md) for the umbrella overview of
client + bots and the channel-vs-server-scope distinction.

## What this bot demonstrates

- **`scope=channel`**: auto-joins `#weather` and `#general` from
  the config block.  Slash commands only appear in those channels'
  popovers.
- **`deferred=True`** on `/forecast` — wttr.in can be slow, so the
  client emits `INTERACTION_DEFER` before invoking the handler to
  extend the server-side window from 3 s to 15 s.
- **`@bot.on_event("MESSAGE_CREATE")`** plus `bot.post_to_channel()`
  — replies in-channel via the REST API when someone mentions the
  bot by nick.

## Commands

| Command | Args | What it does |
|---|---|---|
| `/forecast <city>` | required string | wttr.in summary (defers) |
| `/flip` | — | heads or tails |

## Run

```bash
PUSHBOT_HOST=obby.example.com PUSHBOT_PORT=6670 \
PUSHBOT_TOKEN=<token-from-pushbot.conf> PUSHBOT_NICK=weather \
python3 weatherbot.py
```

Or as a systemd service — edit `weatherbot.service` to point at your
token and path, then:

```bash
sudo cp weatherbot.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now weatherbot.service
journalctl -fu weatherbot.service
```
