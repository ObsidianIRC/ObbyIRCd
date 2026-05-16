#!/usr/bin/env python3
"""'weather' PushBot -- channel-scope bot demoing INTERACTION_DEFER for a
network-bound handler and an @mention auto-reply via the REST API.

Built on the shared client at doc/examples/pushbot_client.py.  The
interesting bits live in the @bot.command decorators; everything else
(WS framing, IDENTIFY, heartbeat, reconnect) is in the library.

Run with:
  PUSHBOT_HOST=obby.example.com PUSHBOT_PORT=6670 \
  PUSHBOT_TOKEN=<bot-token> PUSHBOT_NICK=weather \
  python3 weatherbot.py
"""
from __future__ import annotations

import random
import sys
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

# Allow running this file directly from doc/examples/weatherbot/ or
# after a `pip install -e .` style install.
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from pushbot_client import PushBot, setup_logging  # noqa: E402


setup_logging()
bot = PushBot.from_env()


@bot.command(
    name="forecast",
    description="Look up the current weather for a city",
    visibility="public",
    scopes=["channel", "dm"],
    options=[
        {
            "name": "city",
            "type": "string",
            "required": True,
            "description": "City name (e.g. London, Tokyo, San Francisco)",
        }
    ],
    deferred=True,  # wttr.in can be slow; buy 15 s of window
)
def cmd_forecast(invoker, channel, opts) -> str:
    city = (opts.get("city") or "").strip() or "London"
    safe = urllib.parse.quote(city, safe="")
    try:
        req = urllib.request.Request(
            f"https://wttr.in/{safe}?format=%l:+%C,+%t+(feels+%f),+wind+%w",
            headers={"User-Agent": "obbyircd-pushbot/1.0"},
        )
        with urllib.request.urlopen(req, timeout=8) as r:
            body = r.read().decode("utf-8", errors="replace").strip()
            if not body or "Unknown location" in body:
                return f"sorry, couldn't find weather for {city}"
            return body
    except urllib.error.URLError as e:
        return f"wttr.in is unreachable: {e}"


@bot.command(
    name="flip",
    description="Flip a coin",
    visibility="public",
)
def cmd_flip(invoker, channel, opts) -> str:
    return random.choice(("heads", "tails"))


@bot.on_event("MESSAGE_CREATE")
def on_message(d) -> None:
    """Reply to channel mentions of our nick with a one-liner."""
    text = (d.get("content") or "").lower()
    author = (d.get("author") or {}).get("nick", "")
    channel = d.get("channel")
    if isinstance(channel, dict):
        channel_name = channel.get("name")
    else:
        channel_name = None
    if bot.nick.lower() in text and channel_name:
        bot.post_to_channel(
            channel_name, f"hi {author}!  try /forecast <city> or /flip"
        )


if __name__ == "__main__":
    bot.run()
