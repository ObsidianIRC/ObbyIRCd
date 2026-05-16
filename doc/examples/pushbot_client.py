#!/usr/bin/env python3
"""Shared PushBot client library used by every bot in doc/examples/.

Stdlib-only.  Lifts the boilerplate (WS framing, IDENTIFY/RESUME,
heartbeats, reconnect with backoff, COMMAND_INVOKE dispatch, REST
helpers) out of each bot so the bot file only contains the
interesting bits: command schemas + handlers.

Wire-protocol details: doc/specs/pushbot-spec.md, doc/specs/pushbot-architecture.md.

Usage (see helpbot/helpbot.py for a full example):

    from pushbot_client import PushBot

    bot = PushBot.from_env()

    @bot.command(
        name="ping",
        description="Reply with pong",
        visibility="public",
    )
    def cmd_ping(invoker, channel, opts):
        return "pong"

    bot.run()

Slow handlers (anything that hits the network) should pass
`deferred=True` so the client emits INTERACTION_DEFER before invoking
the handler, extending the server-side window from 3 s to 15 s.
"""
from __future__ import annotations

import json
import logging
import os
import socket
import ssl
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from base64 import b64encode
from dataclasses import dataclass, field
from os import urandom
from typing import Any, Callable, Optional

# --- opcodes (matches pushbot.c) -------------------------------------------

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

HEARTBEAT_DEFAULT_MS = 30_000

# Close codes the server uses to mean "your resume id is dead, IDENTIFY fresh".
SESSION_INVALIDATING_CLOSE_CODES = frozenset({4001, 4006})


# --- WebSocket framing primitives ------------------------------------------


class WebSocketClose(ConnectionError):
    """Server sent a Close frame.  Carries the 2-byte status code."""

    def __init__(self, code: int, reason: str) -> None:
        super().__init__(f"WS close {code}: {reason}")
        self.code = code
        self.reason = reason


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


def _recv_frame(sock: ssl.SSLSocket) -> Optional[bytes]:
    try:
        head = _read_n(sock, 2)
    except (TimeoutError, socket.timeout):
        return None
    if len(head) < 2:
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
    if opcode == 0x9:  # PING -> PONG
        _send_frame(sock, 0xA, data or b"")
        return _recv_frame(sock)
    if opcode == 0xA:  # PONG, ignore
        return _recv_frame(sock)
    return data


def _send_frame(sock: ssl.SSLSocket, op: int, payload: bytes) -> None:
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


def _send_text(sock: ssl.SSLSocket, obj: dict) -> None:
    _send_frame(sock, 0x1, json.dumps(obj, separators=(",", ":")).encode())


def _tls_ctx(insecure: bool) -> ssl.SSLContext:
    ctx = ssl.create_default_context()
    if insecure:
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE
    return ctx


def _ws_connect(host: str, port: int, path: str, token: str, insecure: bool) -> ssl.SSLSocket:
    raw = socket.create_connection((host, port), timeout=15)
    sock = _tls_ctx(insecure).wrap_socket(raw, server_hostname=host)
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


# --- session bookkeeping ---------------------------------------------------


@dataclass
class _SessionState:
    session_id: Optional[str] = None
    last_seq: int = 0
    heartbeat_interval_ms: int = HEARTBEAT_DEFAULT_MS


@dataclass
class _Command:
    schema: dict
    handler: Callable[[dict, Optional[str], dict], str]
    deferred: bool = False


# --- the PushBot itself ----------------------------------------------------


class PushBot:
    """One pushbot connection.  Long-lived; reconnects forever."""

    def __init__(
        self,
        *,
        host: str,
        port: int,
        token: str,
        nick: str,
        insecure: bool = True,
        log: Optional[logging.Logger] = None,
    ) -> None:
        self.host = host
        self.port = port
        self.token = token
        self.nick = nick
        self.insecure = insecure
        self.log = log or logging.getLogger(nick)
        self._commands: dict[str, _Command] = {}
        self._event_handlers: dict[str, Callable[[dict], None]] = {}
        self._state = _SessionState()

    # --- public registration API ------------------------------------------

    @classmethod
    def from_env(cls, **overrides: Any) -> "PushBot":
        """Build from PUSHBOT_HOST/PORT/TOKEN/NICK environment variables."""
        return cls(
            host=overrides.pop("host", os.environ.get("PUSHBOT_HOST", "127.0.0.1")),
            port=int(overrides.pop("port", os.environ.get("PUSHBOT_PORT", "6670"))),
            token=overrides.pop("token", os.environ["PUSHBOT_TOKEN"]),
            nick=overrides.pop("nick", os.environ.get("PUSHBOT_NICK", "bot")),
            insecure=overrides.pop("insecure",
                                   os.environ.get("PUSHBOT_INSECURE", "1") == "1"),
            **overrides,
        )

    def command(
        self,
        *,
        name: str,
        description: str,
        options: Optional[list[dict]] = None,
        visibility: str = "public",
        scopes: Optional[list[str]] = None,
        deferred: bool = False,
    ) -> Callable[[Callable], Callable]:
        """Decorator: register a slash command.  The decorated function is
        called as handler(invoker, channel, options_dict) and must return a
        string (the reply content)."""

        def wrap(fn: Callable[[dict, Optional[str], dict], str]) -> Callable:
            self._commands[name] = _Command(
                schema={
                    "name": name,
                    "description": description,
                    "visibility": visibility,
                    "scopes": scopes or ["channel", "dm"],
                    "options": options or [],
                },
                handler=fn,
                deferred=deferred,
            )
            return fn

        return wrap

    def on_event(self, event_name: str) -> Callable[[Callable], Callable]:
        """Decorator: register a handler for a DISPATCH event type like
        MESSAGE_CREATE or CHANNEL_JOIN."""

        def wrap(fn: Callable[[dict], None]) -> Callable:
            self._event_handlers[event_name] = fn
            return fn

        return wrap

    # --- REST helpers (handy from event handlers) -------------------------

    def post_to_channel(self, channel: str, content: str) -> None:
        self._rest_post(
            f"/pushbot/v1/channels/{urllib.parse.quote(channel, safe='')}/messages",
            {"content": content},
        )

    def post_to_user(self, nick: str, content: str) -> None:
        self._rest_post(
            f"/pushbot/v1/users/{urllib.parse.quote(nick, safe='')}/messages",
            {"content": content},
        )

    def _rest_post(self, path: str, body: dict) -> None:
        ctx = _tls_ctx(self.insecure)
        req = urllib.request.Request(
            f"https://{self.host}:{self.port}{path}",
            data=json.dumps(body).encode(),
            headers={
                "Authorization": f"Bearer {self.token}",
                "Content-Type": "application/json",
            },
            method="POST",
        )
        try:
            with urllib.request.urlopen(req, context=ctx, timeout=5) as r:
                r.read()
        except Exception as e:
            self.log.warning("REST POST %s failed: %s", path, e)

    # --- main loop --------------------------------------------------------

    def run(self) -> None:
        """Block forever, reconnecting on failure."""
        backoff = 1.0
        while True:
            try:
                should_resume = self._run_once()
                if should_resume:
                    self.log.info("scheduled reconnect, attempting RESUME")
                    backoff = 1.0
                    continue
                backoff = 1.0
            except KeyboardInterrupt:
                self.log.info("interrupted, exiting")
                return
            except WebSocketClose as e:
                self.log.warning("connection lost: %s", e)
                if e.code in SESSION_INVALIDATING_CLOSE_CODES:
                    self._state.session_id = None
                    self._state.last_seq = 0
                    backoff = 1.0
            except Exception as e:
                self.log.warning("connection lost: %s", e)
            self.log.info("reconnecting in %.1f s", backoff)
            time.sleep(backoff)
            backoff = min(backoff * 2, 30.0)

    # --- internals --------------------------------------------------------

    def _run_once(self) -> bool:
        self.log.info("connecting to wss://%s:%d/pushbot/v1/gateway", self.host, self.port)
        sock = _ws_connect(self.host, self.port, "/pushbot/v1/gateway",
                           self.token, self.insecure)
        self.log.info("WebSocket upgraded")

        hello = self._recv_json(sock)
        if not hello or hello.get("op") != OP_HELLO:
            raise ConnectionError(f"expected HELLO, got {hello}")
        self._state.heartbeat_interval_ms = int(hello["d"]["heartbeat_interval"])
        self.log.info("HELLO heartbeat_interval=%d ms",
                      self._state.heartbeat_interval_ms)

        if self._state.session_id and self._state.last_seq:
            self.log.info("attempting RESUME session_id=%s seq=%d",
                          self._state.session_id, self._state.last_seq)
            _send_text(sock, {"op": OP_RESUME, "d": {
                "token": self.token,
                "session_id": self._state.session_id,
                "seq": self._state.last_seq,
            }})
        else:
            self.log.info("IDENTIFYing fresh")
            _send_text(sock, {"op": OP_IDENTIFY, "d": {"token": self.token}})

        stop = threading.Event()
        hb = threading.Thread(target=self._heartbeat, args=(sock, stop), daemon=True)
        hb.start()
        registered = False

        try:
            while True:
                frame = self._recv_json(sock)
                if frame is None:
                    continue
                op = frame.get("op")

                if op == OP_HEARTBEAT_ACK:
                    continue
                if op == OP_RECONNECT:
                    self.log.info("server told us to reconnect")
                    return True
                if op == OP_INVALID_SESSION:
                    self.log.warning("INVALID_SESSION: dropping resume id")
                    self._state.session_id = None
                    self._state.last_seq = 0
                    return False
                if op == OP_DISPATCH:
                    if not registered:
                        # On the first dispatch we get (READY or RESUMED),
                        # capture the session id and publish commands.
                        if frame.get("t") == "READY":
                            self._state.session_id = (frame.get("d") or {}).get("session_id")
                        if self._commands:
                            self.log.info("registering %d commands", len(self._commands))
                            _send_text(sock, {
                                "op": OP_COMMAND_REGISTER,
                                "d": {"commands": [c.schema for c in self._commands.values()]},
                            })
                        registered = True
                    self._dispatch(sock, frame)
                    continue
        finally:
            stop.set()
            try:
                sock.close()
            except Exception:
                pass

    def _recv_json(self, sock: ssl.SSLSocket) -> Optional[dict]:
        data = _recv_frame(sock)
        if not data:
            return None
        try:
            return json.loads(data)
        except json.JSONDecodeError as e:
            self.log.warning("non-JSON frame (%d bytes): %s -- %r",
                             len(data), e, data[:120])
            return None

    def _dispatch(self, sock: ssl.SSLSocket, frame: dict) -> None:
        seq = frame.get("s")
        if isinstance(seq, int):
            self._state.last_seq = seq
        evt = frame.get("t")
        d = frame.get("d") or {}

        if evt == "COMMAND_INVOKE":
            self._handle_command_invoke(sock, d)
            return

        handler = self._event_handlers.get(evt)
        if handler:
            try:
                handler(d)
            except Exception:
                self.log.exception("handler %s raised", evt)

    def _handle_command_invoke(self, sock: ssl.SSLSocket, d: dict) -> None:
        name = d.get("name") or ""
        cmd = self._commands.get(name)
        iid = d.get("id")
        channel = d.get("channel")
        invoker = d.get("invoker") or {}
        options = d.get("options") or {}

        if not cmd:
            content = f"unknown command: {name}"
        else:
            if cmd.deferred:
                _send_text(sock, {"op": OP_INTERACTION_DEFER, "d": {"id": iid}})
            try:
                content = cmd.handler(invoker, channel, options)
            except Exception as e:
                self.log.exception("command %s failed", name)
                content = f"command failed: {e}"

        _send_text(sock, {
            "op": OP_INTERACTION_RESPONSE,
            "d": {
                "id": iid,
                "channel": channel,
                "content": str(content),
                "visibility": "public",
            },
        })

    def _heartbeat(self, sock: ssl.SSLSocket, stop: threading.Event) -> None:
        interval = max(5, self._state.heartbeat_interval_ms // 1000)
        while not stop.wait(interval):
            try:
                _send_text(sock, {"op": OP_HEARTBEAT, "d": self._state.last_seq})
            except Exception as e:
                self.log.warning("heartbeat send failed: %s", e)
                return


def setup_logging(level: int = logging.INFO) -> None:
    logging.basicConfig(
        level=level,
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )
