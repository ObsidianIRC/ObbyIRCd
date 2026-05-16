#!/usr/bin/env python3
"""Long-running PushBot client that drives the 'weather' ghost on obbyircd.

Behaviour:
  - opens a WSS connection to /pushbot/v1/gateway with the configured
    bearer token, IDENTIFYs, sends a periodic HEARTBEAT, and tries to
    RESUME on transient disconnects within the 60 s window
  - registers two slash commands (op=20):
      * /forecast <city>    -> a short wttr.in summary, "public" visibility
      * /flip               -> coin flip, "public" visibility
  - listens for COMMAND_INVOKE (op=0, t=COMMAND_INVOKE) and replies
    with INTERACTION_RESPONSE (op=21).  If wttr.in is slow it sends
    INTERACTION_DEFER (op=22) first so the server extends the
    interaction window from 3 s to 15 s.
  - also listens for MESSAGE_CREATE that @mentions the bot's nick
    and posts a short hello back into the channel via the REST API.

Run with:
  PUSHBOT_HOST=obby.t3ks.com PUSHBOT_PORT=6670 \
  PUSHBOT_TOKEN=replace-me-with-a-long-random-string \
  PUSHBOT_NICK=weather python3 weatherbot.py
"""
from __future__ import annotations

import json
import logging
import os
import random
import socket
import ssl
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from base64 import b64encode
from dataclasses import dataclass
from os import urandom
from typing import Callable, Optional

log = logging.getLogger("weatherbot")

HOST = os.environ.get("PUSHBOT_HOST", "obby.t3ks.com")
PORT = int(os.environ.get("PUSHBOT_PORT", "6670"))
TOKEN = os.environ.get("PUSHBOT_TOKEN", "replace-me-with-a-long-random-string")
NICK = os.environ.get("PUSHBOT_NICK", "weather")
INSECURE = os.environ.get("PUSHBOT_INSECURE", "1") == "1"
HEARTBEAT_DEFAULT_MS = 30_000

OP_DISPATCH = 0
OP_HEARTBEAT = 1
OP_IDENTIFY = 2
OP_RESUME = 6
OP_RECONNECT = 7
OP_INVALID_SESSION = 9
OP_HELLO = 10
OP_HEARTBEAT_ACK = 11
OP_COMMAND_REGISTER = 20
OP_INTERACTION_RESPONSE = 21
OP_INTERACTION_DEFER = 22


def make_tls_ctx() -> ssl.SSLContext:
    ctx = ssl.create_default_context()
    if INSECURE:
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE
    return ctx


# ---------------------------------------------------------------------------
# WebSocket framing — keep it minimal, no permessage-deflate, text frames only
# ---------------------------------------------------------------------------

def ws_connect(host: str, port: int, path: str, token: str) -> ssl.SSLSocket:
    ctx = make_tls_ctx()
    raw = socket.create_connection((host, port), timeout=15)
    sock = ctx.wrap_socket(raw, server_hostname=host)
    key = b64encode(urandom(16)).decode()
    req = (
        f"GET {path} HTTP/1.1\r\n"
        f"Host: {host}:{port}\r\n"
        f"Upgrade: websocket\r\nConnection: Upgrade\r\n"
        f"Sec-WebSocket-Key: {key}\r\n"
        f"Sec-WebSocket-Version: 13\r\n"
        f"Authorization: Bearer {token}\r\n\r\n"
    )
    sock.sendall(req.encode())
    header = b""
    while not header.endswith(b"\r\n\r\n"):
        b = sock.recv(1)
        if not b:
            raise ConnectionError("connection closed during handshake")
        header += b
    if b"101" not in header.split(b"\r\n", 1)[0]:
        raise ConnectionError(f"bad upgrade response: {header[:200]!r}")
    sock.settimeout(60)
    return sock


class WebSocketClose(ConnectionError):
    """Server sent a Close frame.  Carries the 2-byte status code."""

    def __init__(self, code: int, reason: str) -> None:
        super().__init__(f"WS close {code}: {reason}")
        self.code = code
        self.reason = reason


def recv_frame(sock: ssl.SSLSocket) -> Optional[bytes]:
    try:
        head = _read_n(sock, 2)
    except (TimeoutError, socket.timeout):
        return None
    if head is None or len(head) < 2:
        return None
    opcode = head[0] & 0x0F
    plen = head[1] & 0x7F
    if plen == 126:
        plen = int.from_bytes(_read_n(sock, 2), "big")
    elif plen == 127:
        plen = int.from_bytes(_read_n(sock, 8), "big")
    data = _read_n(sock, plen)
    if opcode == 0x8:
        code = int.from_bytes(data[:2], "big") if len(data) >= 2 else 0
        reason = data[2:].decode("utf-8", errors="replace")
        raise WebSocketClose(code, reason)
    if opcode == 0x9:  # PING -> auto PONG
        send_frame(sock, 0xA, data or b"")
        return recv_frame(sock)
    if opcode == 0xA:  # PONG ignored
        return recv_frame(sock)
    return data


def _read_n(sock: ssl.SSLSocket, n: int) -> bytes:
    if n <= 0:
        return b""
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("short read")
        buf += chunk
    return buf


def send_frame(sock: ssl.SSLSocket, op: int, payload: bytes) -> None:
    head = bytes([0x80 | op])
    plen = len(payload)
    mask = urandom(4)
    if plen < 126:
        head += bytes([0x80 | plen])
    elif plen < 65536:
        head += bytes([0x80 | 126]) + plen.to_bytes(2, "big")
    else:
        head += bytes([0x80 | 127]) + plen.to_bytes(8, "big")
    head += mask
    masked = bytes(payload[i] ^ mask[i % 4] for i in range(plen))
    sock.sendall(head + masked)


def send_text(sock: ssl.SSLSocket, obj: dict) -> None:
    send_frame(sock, 0x1, json.dumps(obj, separators=(",", ":")).encode())


# ---------------------------------------------------------------------------
# Slash-command handlers
# ---------------------------------------------------------------------------

def cmd_forecast(opts: dict) -> str:
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
    except Exception as e:
        return f"weather lookup failed: {e}"


def cmd_flip(_opts: dict) -> str:
    return random.choice(("heads", "tails"))


COMMANDS = {
    "forecast": cmd_forecast,
    "flip": cmd_flip,
}

COMMAND_SCHEMA = [
    {
        "name": "forecast",
        "description": "Look up the current weather for a city",
        "visibility": "public",
        "scopes": ["channel", "dm"],
        "options": [
            {
                "name": "city",
                "type": "string",
                "required": True,
                "description": "City name (e.g. London, Tokyo, San Francisco)",
            }
        ],
    },
    {
        "name": "flip",
        "description": "Flip a coin",
        "visibility": "public",
        "scopes": ["channel", "dm"],
        "options": [],
    },
]


# ---------------------------------------------------------------------------
# Session state machine
# ---------------------------------------------------------------------------

@dataclass
class SessionState:
    session_id: Optional[str] = None
    last_seq: int = 0
    heartbeat_interval_ms: int = HEARTBEAT_DEFAULT_MS


def handle_dispatch(frame: dict, state: SessionState, sock: ssl.SSLSocket) -> None:
    seq = frame.get("s")
    if isinstance(seq, int):
        state.last_seq = seq
    evt = frame.get("t")
    d = frame.get("d") or {}

    if evt == "COMMAND_INVOKE":
        cmd_name = d.get("name") or ""
        handler = COMMANDS.get(cmd_name)
        channel = d.get("channel")
        iid = d.get("id")
        if handler is None:
            content = f"unknown command: {cmd_name}"
        else:
            # If a handler might be slow (forecast), defer first so the
            # 3 s server-side window doesn't fire FAIL BOTCMD TIMEOUT.
            if cmd_name == "forecast":
                send_text(sock, {"op": OP_INTERACTION_DEFER, "d": {"id": iid}})
            try:
                content = handler(d.get("options") or {})
            except Exception as e:
                log.exception("command %s failed", cmd_name)
                content = f"command failed: {e}"
        send_text(
            sock,
            {
                "op": OP_INTERACTION_RESPONSE,
                "d": {
                    "id": iid,
                    "channel": channel,
                    "content": content,
                    "visibility": "public",
                },
            },
        )
        return

    if evt == "MESSAGE_CREATE":
        author = (d.get("author") or {}).get("nick", "")
        text = d.get("content") or ""
        channel = (d.get("channel") or {}).get("name") if isinstance(d.get("channel"), dict) else None
        if NICK.lower() in text.lower() and channel:
            # REST POST back into the channel as the bot — the gateway
            # only carries opcodes, not channel chatter.
            rest_send_channel(channel, f"hi {author}!  try /forecast <city> or /flip")
        return

    if evt in ("READY", "RESUMED", "COMMANDS_REGISTERED"):
        log.info("dispatch %s d=%s", evt, json.dumps(d)[:200])
        return

    log.debug("ignoring dispatch %s", evt)


def rest_send_channel(channel: str, content: str) -> None:
    """Use the REST API to send a PRIVMSG as the bot.  Fire-and-forget."""
    ctx = make_tls_ctx()
    path = f"/pushbot/v1/channels/{urllib.parse.quote(channel, safe='')}/messages"
    req = urllib.request.Request(
        f"https://{HOST}:{PORT}{path}",
        data=json.dumps({"content": content}).encode(),
        headers={
            "Authorization": f"Bearer {TOKEN}",
            "Content-Type": "application/json",
        },
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, context=ctx, timeout=5) as r:
            r.read()
    except Exception as e:
        log.warning("REST send to %s failed: %s", channel, e)


def heartbeat_thread(sock: ssl.SSLSocket, state: SessionState, stop: threading.Event) -> None:
    interval = max(5, state.heartbeat_interval_ms // 1000)
    while not stop.wait(interval):
        try:
            send_text(sock, {"op": OP_HEARTBEAT, "d": state.last_seq})
        except Exception as e:
            log.warning("heartbeat send failed: %s", e)
            return


# ---------------------------------------------------------------------------
# Connect, identify (or resume), pump frames forever
# ---------------------------------------------------------------------------

def run_once(state: SessionState) -> bool:
    """One connection lifecycle.  Returns True if we should attempt RESUME."""
    log.info("connecting to wss://%s:%d/pushbot/v1/gateway", HOST, PORT)
    sock = ws_connect(HOST, PORT, "/pushbot/v1/gateway", TOKEN)
    log.info("WebSocket upgraded")

    raw = recv_frame(sock)
    if not raw:
        raise ConnectionError("no HELLO")
    hello = json.loads(raw)
    if hello.get("op") != OP_HELLO:
        raise ConnectionError(f"expected HELLO, got {hello}")
    state.heartbeat_interval_ms = int(hello["d"]["heartbeat_interval"])
    log.info("HELLO heartbeat_interval=%d ms", state.heartbeat_interval_ms)

    if state.session_id and state.last_seq:
        log.info("attempting RESUME session_id=%s seq=%d", state.session_id, state.last_seq)
        send_text(sock, {
            "op": OP_RESUME,
            "d": {
                "token": TOKEN,
                "session_id": state.session_id,
                "seq": state.last_seq,
            },
        })
    else:
        log.info("IDENTIFYing fresh")
        send_text(sock, {"op": OP_IDENTIFY, "d": {"token": TOKEN}})

    stop = threading.Event()
    hb = threading.Thread(target=heartbeat_thread, args=(sock, state, stop), daemon=True)
    hb.start()

    registered = False
    try:
        while True:
            data = recv_frame(sock)
            if not data:
                continue
            try:
                frame = json.loads(data)
            except json.JSONDecodeError as e:
                log.warning("ignoring non-JSON frame (%d bytes): %s -- bytes=%r",
                           len(data), e, data[:120])
                continue
            op = frame.get("op")
            if op == OP_HEARTBEAT_ACK:
                continue
            if op == OP_RECONNECT:
                log.info("server told us to reconnect")
                return True
            if op == OP_INVALID_SESSION:
                log.warning("INVALID_SESSION: dropping resume id and reconnecting fresh")
                state.session_id = None
                state.last_seq = 0
                return False
            if op == OP_DISPATCH:
                # On READY we get session_id for RESUME.
                if frame.get("t") == "READY":
                    state.session_id = (frame.get("d") or {}).get("session_id")
                    if not registered:
                        log.info("registering commands")
                        send_text(sock, {
                            "op": OP_COMMAND_REGISTER,
                            "d": {"commands": COMMAND_SCHEMA},
                        })
                        registered = True
                handle_dispatch(frame, state, sock)
                continue
            log.debug("ignoring op=%s", op)
    finally:
        stop.set()
        try:
            sock.close()
        except Exception:
            pass


def main() -> None:
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )
    state = SessionState()
    backoff = 1.0
    while True:
        try:
            should_resume = run_once(state)
            if should_resume:
                log.info("scheduled reconnect, attempting RESUME")
                backoff = 1.0
                continue
            backoff = 1.0
        except KeyboardInterrupt:
            log.info("interrupted; exiting")
            return
        except WebSocketClose as e:
            log.warning("connection lost: %s", e)
            # Close codes that signal the server forgot our session ->
            # drop the resume id so the next attempt IDENTIFYs fresh.
            # 4001 AUTH_FAILED (session displaced), 4006 INVALID_SESSION.
            if e.code in (4001, 4006):
                state.session_id = None
                state.last_seq = 0
                backoff = 1.0
        except Exception as e:
            log.warning("connection lost: %s", e)
        log.info("reconnecting in %.1f s", backoff)
        time.sleep(backoff)
        backoff = min(backoff * 2, 30.0)


if __name__ == "__main__":
    main()
