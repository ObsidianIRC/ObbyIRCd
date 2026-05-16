#!/usr/bin/env python3
"""'help' PushBot -- server-wide help bot, reachable from any channel.

Server-wide bots (scope=server, umodes +B+S) don't auto-join channels.
You invoke them with /help <topic> from anywhere on the network; the
IRCd routes the COMMAND_INVOKE based on the bot's nick, not channel
membership.

Demonstrates:
  * scope="server" registration
  * scopes=["channel", "dm"] so the cmd works both in-channel and DM
  * a `choices` array driving the slash-popover hint
  * sub-topics: one command name maps to several lookup paths

Run with:
  PUSHBOT_TOKEN=<help-bot-token> PUSHBOT_NICK=help \
  python3 helpbot.py
"""
from __future__ import annotations

import sys
from pathlib import Path
from textwrap import dedent

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from pushbot_client import PushBot, setup_logging  # noqa: E402


# --- the help corpus ------------------------------------------------------

TOPICS: dict[str, str] = {
    "channels": dedent("""\
        /join #channel — join a channel
        /part [#channel] — leave (current channel by default)
        /topic [text] — view or set the channel topic
        /invite <nick> #channel — invite someone
        Persistent (+P) channels keep their topic and op list across server restarts."""),
    "modes": dedent("""\
        Common channel modes:
          +m moderated   +n no external messages
          +s secret      +t topic op-only
          +P persistent  +R registered-only
        Common user modes:
          +i invisible   +B bot   +S service  +w see snomasks
        Set with: /mode #channel +o nick (or via member-roles)."""),
    "auth": dedent("""\
        Register an account:  /msg NickServ register <password> <email>
        Identify on connect:  use SASL PLAIN in your client, or
                              /msg NickServ identify <password>
        2FA / TOTP enrol:     /2FA totp init
        OAuth providers:      /2FA oauth list"""),
    "bots": dedent("""\
        Use /<cmd> in any channel a bot is in (channel bots) or
        anywhere on the network (server-wide bots like me).
        Discover commands by typing '/' -- channel bots show up
        with an amber 'channel-bot' badge, server-wide bots with
        a purple 'server-bot' badge.
        Disambiguate with /cmd@botnick when two bots share a name."""),
    "ircop": dedent("""\
        IRCop tools:
          /STATS, /TRACE, /KILL, /GLINE, /ZLINE
          /MODE <nick> +o     -- give chanop
          /PUSHBOT LIST       -- list known bots
          /PUSHBOT APPROVE x  -- approve a pending bot registration
          /PUSHBOT SUSPEND x  -- yank a misbehaving bot
        Logs land in snomask 'o' (set umode +o + +s with masks)."""),
}

TOPIC_NAMES = sorted(TOPICS.keys())


# --- bot wiring -----------------------------------------------------------

setup_logging()
bot = PushBot.from_env()


@bot.command(
    name="help",
    description="Show help on an obbyircd topic",
    visibility="public",
    scopes=["channel", "dm"],
    options=[
        {
            "name": "topic",
            "type": "string",
            "required": False,
            "description": "Topic name; omit to see the topic list",
            "choices": TOPIC_NAMES,
        },
    ],
)
def cmd_help(invoker, channel, opts) -> str:
    topic = (opts.get("topic") or "").strip().lower()
    if not topic:
        return (
            "topics: "
            + ", ".join(TOPIC_NAMES)
            + " -- e.g. /help channels"
        )
    if topic in TOPICS:
        return f"{topic}: " + TOPICS[topic].replace("\n", " // ")
    # fuzzy: leading-substring match
    matches = [t for t in TOPIC_NAMES if t.startswith(topic)]
    if len(matches) == 1:
        return f"{matches[0]}: " + TOPICS[matches[0]].replace("\n", " // ")
    if matches:
        return f"did you mean one of: {', '.join(matches)}?"
    return f"no topic '{topic}'.  try one of: {', '.join(TOPIC_NAMES)}"


@bot.command(
    name="report",
    description="Privately flag a user or channel to network operators",
    visibility="private",   # response goes back as a NOTICE only the invoker sees
    scopes=["channel", "dm"],
    options=[
        {"name": "subject", "type": "string", "required": True,
         "description": "Who or what to report (nick or #channel)"},
        {"name": "reason", "type": "string", "required": True,
         "description": "Short description"},
    ],
)
def cmd_report(invoker, channel, opts) -> str:
    # A real implementation would POST to a snomask / opers channel / Slack.
    # The reference impl just echoes back so the operator sees something
    # got captured.
    subject = (opts.get("subject") or "").strip()
    reason = (opts.get("reason") or "").strip()
    bot.log.info(
        "REPORT from=%s subject=%s reason=%s channel=%s",
        invoker.get("nick"), subject, reason, channel,
    )
    return (
        f"got it.  report logged against {subject}: '{reason}'.  "
        f"a network operator will follow up."
    )


if __name__ == "__main__":
    bot.run()
