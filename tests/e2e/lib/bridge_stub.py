import asyncio
import json
import secrets
import sys
from pathlib import Path


SIDECAR_SCRIPT = Path(__file__).parent / "bridge_sidecar.py"


class BridgeStub:
    """Driven via docker stdin/stdout — macOS Docker Desktop doesn't
    reliably forward host TCP into asyncio listeners inside containers."""

    def __init__(self, volume_name: str):
        self.volume_name = volume_name
        self.container = f"voicestub-{secrets.token_hex(4)}"
        self.received: list[dict] = []
        self.proc: asyncio.subprocess.Process | None = None
        self._read_task: asyncio.Task | None = None

    async def __aenter__(self):
        self.proc = await asyncio.create_subprocess_exec(
            "docker", "run", "-i", "--rm",
            "--name", self.container,
            "-v", f"{self.volume_name}:/run/obbyirc",
            "-v", f"{SIDECAR_SCRIPT}:/sidecar.py:ro",
            "-e", "PYTHONUNBUFFERED=1",
            "python:3.12-alpine",
            "python", "-u", "/sidecar.py",
            stdin=asyncio.subprocess.PIPE,
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE,
        )
        self._read_task = asyncio.create_task(self._read_stdout())
        await self._wait_socket_ready()
        return self

    async def __aexit__(self, *_):
        if self._read_task:
            self._read_task.cancel()
        if self.proc and self.proc.returncode is None:
            try:
                self.proc.stdin.close()  # type: ignore[union-attr]
            except (BrokenPipeError, AttributeError):
                pass
            try:
                self.proc.kill()
            except ProcessLookupError:
                pass
            await self.proc.wait()

    async def _read_stdout(self):
        assert self.proc and self.proc.stdout
        while True:
            try:
                raw = await self.proc.stdout.readline()
            except asyncio.CancelledError:
                return
            if not raw:
                return
            try:
                self.received.append(json.loads(raw))
            except json.JSONDecodeError as e:
                print(f"[BridgeStub] dropped malformed frame: {e}: {raw!r}", file=sys.stderr)

    async def _wait_socket_ready(self, timeout: float = 15):
        deadline = asyncio.get_event_loop().time() + timeout
        assert self.proc and self.proc.stderr
        while asyncio.get_event_loop().time() < deadline:
            try:
                line = await asyncio.wait_for(self.proc.stderr.readline(), 0.5)
            except asyncio.TimeoutError:
                continue
            if b"socket-ready" in line:
                return
        raise RuntimeError("sidecar never bound its unix socket")

    async def wait_ircd_connected(self, timeout: float = 30):
        """Wait until the sidecar stderr says `unix accepted`."""
        deadline = asyncio.get_event_loop().time() + timeout
        assert self.proc and self.proc.stderr
        while asyncio.get_event_loop().time() < deadline:
            try:
                line = await asyncio.wait_for(self.proc.stderr.readline(), 0.5)
            except asyncio.TimeoutError:
                continue
            if b"unix accepted" in line:
                return
        raise TimeoutError("IRCd never connected to the bridge")

    async def send_frame(self, frame: dict):
        assert self.proc and self.proc.stdin
        self.proc.stdin.write((json.dumps(frame) + "\n").encode())
        await self.proc.stdin.drain()
