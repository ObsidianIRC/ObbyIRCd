import os
import secrets
import shutil
import string
import subprocess
import tempfile
import time
from pathlib import Path

ALNUM = string.ascii_letters + string.digits


def _cloak():
    return "".join(secrets.choice(ALNUM) for _ in range(96))


class IrcdContainer:
    """One isolated obbyircd container for IRCd-only smoke tests."""

    def __init__(self, image: str | None = None, env: dict | None = None, bind_data: bool = False):
        self.image = image or os.environ.get("OBBYIRCD_IMAGE", "obbyircd:e2e")
        self.name = f"obbyircd-e2e-{secrets.token_hex(4)}"
        self.bind_data = bind_data
        self.data_root = Path(tempfile.mkdtemp(prefix="obbyircd-e2e-")) if bind_data else None
        if self.data_root:
            for sub in ("conf", "data", "logs", "tls", "custom-modules"):
                (self.data_root / sub).mkdir(parents=True, exist_ok=True)
        self.env = self._build_env(env or {})

    def _build_env(self, extra: dict):
        env = {
            "SERVER_NAME": "irc.test.local",
            "NETWORK_NAME": "TestNet",
            "ADMIN_EMAIL": "test@test.local",
            "MOTD_TEXT": "test",
            "OPER_NAME": "admin",
            "OPER_PASSWORD": secrets.token_hex(8),
            "SSL_PORT": "6697",
            "WS_PORT": "8080",
            "CLOAK_KEY1": _cloak(),
            "CLOAK_KEY2": _cloak(),
            "CLOAK_KEY3": _cloak(),
        }
        env.update(extra)
        return env

    def up(self):
        cmd = [
            "docker", "run", "-d", "--name", self.name,
            "-p", "127.0.0.1::6697",
            "-p", "127.0.0.1::8080",
        ]
        if self.bind_data and self.data_root:
            for sub in ("conf", "data", "logs", "tls", "custom-modules"):
                cmd += ["-v", f"{self.data_root}/{sub}:/home/obbyircd/obby/{sub}"]
        for k, v in self.env.items():
            cmd += ["-e", f"{k}={v}"]
        cmd.append(self.image)
        subprocess.run(cmd, check=True, capture_output=True, text=True)
        return self

    def down(self):
        subprocess.run(["docker", "rm", "-f", self.name], capture_output=True)
        if self.data_root:
            shutil.rmtree(self.data_root, ignore_errors=True)

    def host_port(self, container_port: int) -> int:
        out = subprocess.check_output(
            ["docker", "port", self.name, str(container_port)], text=True
        ).strip()
        return int(out.splitlines()[0].rsplit(":", 1)[1])

    def logs(self) -> str:
        r = subprocess.run(
            ["docker", "logs", self.name],
            capture_output=True, text=True,
        )
        return r.stdout + r.stderr

    def wait_ready(self, timeout=120):
        deadline = time.time() + timeout
        while time.time() < deadline:
            log = self.logs()
            if "ObbyIRCd started." in log or "is now listening" in log:
                return
            time.sleep(1)
        raise TimeoutError(f"never ready: {self.logs()[-2000:]}")
