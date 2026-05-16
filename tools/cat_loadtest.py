#!/usr/bin/env python3
"""Cat-bot load test for the lazy metadata-2 fix (issue #116).

Connects N (default 500) anonymous WSS clients to obby.t3ks.com:6664,
sets a full metadata payload on each (avatar pointing at a unique cat,
display-name, status, color, pronouns), and joins them all into
#cattest. Bots stay connected -- answering PING -- until you Ctrl-C.

Each METADATA SET produces a metadata-2 push to subscribers, so the
test exercises both the server-side firehose path and the client-side
lazy fetch we shipped in ObsidianIRC PR #212.

Requires server-side load-test overrides (see obbyircd.conf):
  allow { maxperip 1000; }
  set { anti-flood { connect-flood 1000:1; } }

Usage:
  python3 tools/cat_loadtest.py                 # 500 bots, #cattest
  python3 tools/cat_loadtest.py --count 100     # smaller fleet
  python3 tools/cat_loadtest.py --channel '#x'  # custom channel
  python3 tools/cat_loadtest.py --rate 50       # connects/sec ramp
"""

import argparse
import asyncio
import random
import signal
import ssl
import sys
import time
import uuid

try:
    import websockets
except ImportError:  # pragma: no cover - dev tooling
    print("Need: pip install websockets", file=sys.stderr)
    sys.exit(2)

WSS_URL = "wss://obby.t3ks.com:6664"
SUBPROTOCOL = "text.ircv3.net"
CAPS = ["message-tags", "draft/metadata-2", "server-time", "batch"]
DEFAULT_CHANNEL = "#cattest"

# cataas.com returns a different cat picture per request; the `random`
# query param keeps each bot's avatar URL unique so the server stores
# them as distinct metadata values.
def cat_url(seed: str) -> str:
    return f"https://cataas.com/cat?random={seed}"

ADJECTIVES = [
    "sleepy", "grumpy", "fluffy", "stealth", "majestic", "tiny", "tubby",
    "smug", "chonky", "noisy", "regal", "whiskered", "midnight", "ginger",
    "stripey", "silken", "mischievous", "perched", "yawning", "dignified",
]
NOUNS = [
    "biscuit", "muffin", "noodle", "pebble", "marble", "shadow",
    "mochi", "waffle", "pickle", "luna", "ozzy", "ziggy", "binx",
    "pumpkin", "tofu", "snickers", "willow", "miso", "nala", "kiwi",
]
PRONOUN_SETS = ["he/him", "she/her", "they/them", "it/its", "any/all"]
STATUSES = [
    "kneading the carpet",
    "judging you",
    "asleep on the keyboard",
    "biscuiting",
    "loaf mode",
    "in the bath sink",
    "demanding food",
    "Schroedinger's mood",
    "vibing",
    "very busy doing nothing",
]
COLORS = [
    "#ff6961", "#ffb347", "#fdfd96", "#77dd77", "#84b6f4",
    "#ce93d8", "#f7a3c4", "#bc8f8f", "#a0d8ef", "#d4a373",
]


def random_display_name(i: int) -> str:
    a = random.choice(ADJECTIVES)
    n = random.choice(NOUNS)
    return f"{a.title()} {n.title()} {i}"


class CatBot:
    def __init__(self, idx: int, channel: str, debug: bool):
        self.idx = idx
        self.channel = channel
        self.debug = debug
        # Tag connections so we can identify them in /WHO
        self.user_id = f"cat{idx:04d}-{uuid.uuid4().hex[:6]}"
        self.assigned_nick: str | None = None  # server gives us a Guest nick
        self.ws: websockets.WebSocketClientProtocol | None = None
        self.metadata_done = asyncio.Event()
        self.joined = asyncio.Event()
        self.died = asyncio.Event()

    def log(self, msg: str) -> None:
        if self.debug:
            print(f"[cat{self.idx:04d}] {msg}")

    async def send(self, line: str) -> None:
        assert self.ws is not None
        await self.ws.send(line + "\r\n")

    async def _reader(self) -> None:
        assert self.ws is not None
        try:
            async for raw in self.ws:
                for line in raw.replace("\r", "").split("\n"):
                    if not line:
                        continue
                    if line.startswith("PING "):
                        await self.send("PONG " + line.split(" ", 1)[1])
                    elif " 001 " in line:
                        # 001 RPL_WELCOME -- params[0] is the
                        # nick the server actually gave us
                        parts = line.split(" ")
                        if len(parts) >= 3:
                            self.assigned_nick = parts[2]
                    elif " 366 " in line and self.channel.lower() in line.lower():
                        self.joined.set()
        except Exception:
            pass

    async def run(self) -> None:
        ctx = ssl.create_default_context()
        # Production cert is fine but we don't need strict hostname
        # checks for a load test on our own infra.
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE
        reader_task: asyncio.Task | None = None
        try:
            async with websockets.connect(
                WSS_URL,
                ssl=ctx,
                subprotocols=[SUBPROTOCOL],
                open_timeout=30,
                ping_interval=None,  # we answer IRC PING ourselves
            ) as ws:
                self.ws = ws
                # Start reading immediately so PING / 001 / 366 are
                # processed while we're still issuing the registration
                # sequence -- otherwise the server pings during
                # metadata SETs and kills us for ping timeout.
                reader_task = asyncio.create_task(self._reader())
                await self._handshake()
                await self._set_metadata()
                await self.send(f"JOIN {self.channel}")
                # Hold the connection open until the reader stops (i.e.
                # the server closed it) or we're cancelled.
                await reader_task
        except asyncio.CancelledError:
            self.log("cancelled")
            if reader_task and not reader_task.done():
                reader_task.cancel()
            raise
        except Exception as exc:
            self.log(f"died: {exc!r}")
            self.died.set()

    async def _handshake(self) -> None:
        await self.send("CAP LS 302")
        await self.send("CAP REQ :" + " ".join(CAPS))
        await self.send("CAP END")
        await self.send(f"NICK {self.user_id}")
        await self.send(f"USER {self.user_id} 0 * :{self.user_id}")

    async def _set_metadata(self) -> None:
        # Wait briefly for 001 before SETs (the server accepts METADATA
        # earlier but it's safer to be registered).
        for _ in range(50):
            if self.assigned_nick:
                break
            await asyncio.sleep(0.1)
        avatar = cat_url(self.user_id)
        display = random_display_name(self.idx)
        status = random.choice(STATUSES)
        color = random.choice(COLORS)
        pronouns = random.choice(PRONOUN_SETS)
        # Stagger each user's own SETs slightly so we don't burst 5
        # lines instantly per connection -- the server's per-conn
        # flood threshold is generous, but let's be polite.
        await self.send(f"METADATA * SET avatar :{avatar}")
        await asyncio.sleep(0.05)
        await self.send(f"METADATA * SET display-name :{display}")
        await asyncio.sleep(0.05)
        await self.send(f"METADATA * SET status :{status}")
        await asyncio.sleep(0.05)
        await self.send(f"METADATA * SET color :{color}")
        await asyncio.sleep(0.05)
        await self.send(f"METADATA * SET pronouns :{pronouns}")
        self.metadata_done.set()


async def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--count", type=int, default=500)
    ap.add_argument("--channel", default=DEFAULT_CHANNEL)
    ap.add_argument(
        "--rate",
        type=float,
        default=25.0,
        help="connections per second during ramp-up",
    )
    ap.add_argument("--debug", action="store_true")
    args = ap.parse_args(argv)

    bots = [CatBot(i, args.channel, args.debug) for i in range(args.count)]
    tasks: list[asyncio.Task] = []

    # Cooperative shutdown.
    stop = asyncio.Event()
    loop = asyncio.get_running_loop()
    for sig in (signal.SIGINT, signal.SIGTERM):
        loop.add_signal_handler(sig, stop.set)

    print(f"Spawning {args.count} bots into {args.channel} "
          f"at {args.rate}/s ramp...")
    interval = 1.0 / args.rate if args.rate > 0 else 0
    started = time.time()
    for bot in bots:
        if stop.is_set():
            break
        tasks.append(asyncio.create_task(bot.run()))
        if interval:
            await asyncio.sleep(interval)

    print(f"Ramp complete in {time.time() - started:.1f}s; "
          f"waiting for metadata + JOIN...")

    # Wait up to 60s for at least 80% to fully join.
    deadline = loop.time() + 60
    while loop.time() < deadline and not stop.is_set():
        ready = sum(1 for b in bots if b.joined.is_set())
        dead = sum(1 for b in bots if b.died.is_set())
        if ready >= int(args.count * 0.8):
            break
        print(f"  joined={ready}/{args.count}  dead={dead}")
        await asyncio.sleep(2)

    ready = sum(1 for b in bots if b.joined.is_set())
    dead = sum(1 for b in bots if b.died.is_set())
    print(f"\nFleet status: joined={ready}/{args.count}  dead={dead}")
    print("Bots will stay connected. Ctrl-C to disconnect everyone.\n")

    await stop.wait()
    print("\nShutting down...")
    for t in tasks:
        t.cancel()
    await asyncio.gather(*tasks, return_exceptions=True)
    print("All cats have left the building.")
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main(sys.argv[1:])))
