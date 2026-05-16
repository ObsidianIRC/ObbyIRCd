#!/usr/bin/env python3
"""'dice' PushBot -- server-wide dice/RNG bot, no state, no network.

A pure-RNG server-wide bot showcasing:
  * an int option with min/max metadata
  * an enum/choices option that the slash-popover renders as
    "one of: …" beneath the param hint
  * a dice-notation parser (NdM+K) implemented in pure Python

Run with:
  PUSHBOT_TOKEN=<dice-bot-token> PUSHBOT_NICK=dice \
  python3 dicebot.py
"""
from __future__ import annotations

import random
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from pushbot_client import PushBot, setup_logging  # noqa: E402


# --- dice ----------------------------------------------------------------

DICE_RE = re.compile(r"^\s*(\d*)d(\d+)\s*([+-]\s*\d+)?\s*$", re.IGNORECASE)
MAX_DICE = 100
MAX_SIDES = 1000


def roll_spec(spec: str) -> str:
    """Parse and execute a 'NdM+K' string.  Returns the result line."""
    m = DICE_RE.match(spec)
    if not m:
        return f"didn't understand '{spec}' — try '2d20' or '1d8+3'"
    n = int(m.group(1) or "1")
    sides = int(m.group(2))
    modifier = int((m.group(3) or "0").replace(" ", ""))
    if n < 1 or n > MAX_DICE:
        return f"number of dice must be 1..{MAX_DICE}"
    if sides < 2 or sides > MAX_SIDES:
        return f"sides must be 2..{MAX_SIDES}"
    rolls = [random.randint(1, sides) for _ in range(n)]
    total = sum(rolls) + modifier
    detail = " ".join(str(r) for r in rolls)
    if modifier:
        sign = "+" if modifier > 0 else ""
        return f"{spec}: [{detail}] {sign}{modifier} = {total}"
    return f"{spec}: [{detail}] = {total}"


# --- bot ------------------------------------------------------------------

setup_logging()
bot = PushBot.from_env()


@bot.command(
    name="roll",
    description="Roll dice in NdM+K notation",
    visibility="public",
    scopes=["channel", "dm"],
    options=[
        {"name": "spec", "type": "string", "required": True,
         "description": "e.g. 2d20, 1d8+3, 4d6"},
    ],
)
def cmd_roll(invoker, channel, opts) -> str:
    return roll_spec(opts.get("spec") or "1d6")


@bot.command(
    name="flip",
    description="Flip a coin",
    visibility="public",
)
def cmd_flip(invoker, channel, opts) -> str:
    return random.choice(("heads", "tails"))


@bot.command(
    name="choose",
    description="Pick one option at random",
    visibility="public",
    options=[
        {"name": "options", "type": "string", "required": True,
         "description": "Comma- or pipe-separated list (e.g. 'pizza,tacos,sushi')"},
    ],
)
def cmd_choose(invoker, channel, opts) -> str:
    raw = (opts.get("options") or "").strip()
    if not raw:
        return "give me something to choose from"
    parts = [p.strip() for p in re.split(r"[,|]", raw) if p.strip()]
    if not parts:
        return "give me something to choose from"
    if len(parts) == 1:
        return parts[0]
    return random.choice(parts)


@bot.command(
    name="8ball",
    description="Magic 8-ball — ask a yes/no question",
    visibility="public",
    options=[
        {"name": "question", "type": "string", "required": False},
    ],
)
def cmd_8ball(invoker, channel, opts) -> str:
    answers = [
        "it is certain", "without a doubt", "yes, definitely",
        "you may rely on it", "as I see it, yes", "most likely",
        "outlook good", "yes", "signs point to yes",
        "reply hazy, try again", "ask again later",
        "cannot predict now", "concentrate and ask again",
        "don't count on it", "my reply is no", "my sources say no",
        "outlook not so good", "very doubtful",
    ]
    return random.choice(answers)


if __name__ == "__main__":
    bot.run()
