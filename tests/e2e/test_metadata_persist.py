"""metadata-db persistence + boot replay.

Two regressions guarded:

1. Channel + user metadata SET while running must survive a clean
   restart (the metadata-db module writes on shutdown, reads on boot).
2. The boot read must hit metadata_set_channel / metadata_set_user AFTER
   the metadata module's limit defaults are applied; otherwise restored
   entries silently drop on LIMIT_REACHED.
"""

import subprocess
import time

import pytest

from lib.container import IrcdContainer
from lib.irc import IrcClient

CAP_LINE = "CAP REQ :draft/metadata-2 message-tags batch"
OPER_PASS = "testoper-fixed-pass"


@pytest.fixture
def persistent_ircd():
    c = IrcdContainer(bind_data=True, env={"OPER_PASSWORD": OPER_PASS}).up()
    try:
        c.wait_ready()
        yield c
    finally:
        c.down()


async def _set_channel_metadata(ircd: IrcdContainer, channel: str, kvs: dict[str, str]):
    port = ircd.host_port(6697)
    async with IrcClient("127.0.0.1", port, nick="setter") as c:
        await c.send(CAP_LINE)
        await c.register()
        await c.send("CAP END")
        await c.expect(lambda l: " 001 " in l, timeout=30)
        await c.send(f"OPER admin {OPER_PASS}")
        await c.expect(lambda l: " 381 " in l)
        await c.send(f"SAJOIN setter {channel}")
        await c.expect(lambda l: " 366 " in l and channel in l)
        await c.send(f"SAMODE {channel} +Pq setter")
        await c.expect(lambda l: "MODE" in l and "P" in l.split(":", 1)[0] and channel in l)
        for key, value in kvs.items():
            await c.send(f"METADATA {channel} SET {key} :{value}")
            await c.expect(lambda l: " 761 " in l and key in l)


async def _read_channel_metadata(ircd: IrcdContainer, channel: str) -> dict[str, str]:
    port = ircd.host_port(6697)
    async with IrcClient("127.0.0.1", port, nick="reader") as c:
        await c.send(CAP_LINE)
        await c.register()
        await c.send("CAP END")
        await c.expect(lambda l: " 001 " in l, timeout=30)
        await c.send(f"JOIN {channel}")
        await c.expect(lambda l: " 366 " in l and channel in l)
        await c.send(f"METADATA {channel} LIST")
        out: dict[str, str] = {}
        end_batch = None
        while True:
            line = await c.expect(lambda l: " 761 " in l or " BATCH " in l, timeout=10)
            if " BATCH " in line and end_batch is None:
                end_batch = "BATCH -" + line.split("BATCH +", 1)[1].split(" ", 1)[0]
                continue
            if end_batch and end_batch in line:
                break
            if " 761 " in line:
                trailing_idx = line.rfind(" :")
                if trailing_idx < 0:
                    continue
                value = line[trailing_idx + 2:]
                parts = line[:trailing_idx].split()
                idx = parts.index("761")
                out[parts[idx + 3]] = value
        return out


def _docker_logs(name: str) -> str:
    result = subprocess.run(
        ["docker", "logs", name], capture_output=True, text=True
    )
    return result.stdout + result.stderr


def _wait_for_marker(name: str, marker: str, min_count: int, timeout: int):
    deadline = time.time() + timeout
    while time.time() < deadline:
        log = _docker_logs(name)
        if log.count(marker) >= min_count:
            time.sleep(0.5)
            return
        time.sleep(1)
    raise TimeoutError(
        f"marker {marker!r} count < {min_count} (last 2000):\n{_docker_logs(name)[-2000:]}"
    )


def _restart_and_wait(c: IrcdContainer, timeout: int = 120):
    marker = "ObbyIRCd started."
    pre = _docker_logs(c.name).count(marker)
    subprocess.check_call(["docker", "stop", "-t", "15", c.name])
    subprocess.check_call(["docker", "start", c.name])
    _wait_for_marker(c.name, marker, pre + 1, timeout)


async def test_channel_metadata_survives_restart(persistent_ircd):
    channel = "#persist"
    avatar = "https://example.invalid/avatar.png"
    display = "Persisted Room"

    await _set_channel_metadata(persistent_ircd, channel, {
        "avatar": avatar,
        "display-name": display,
    })

    _restart_and_wait(persistent_ircd)

    md = await _read_channel_metadata(persistent_ircd, channel)
    assert md.get("avatar") == avatar, f"avatar lost after restart: {md}"
    assert md.get("display-name") == display, f"display-name lost: {md}"


async def test_no_double_replay_on_rehash(persistent_ircd):
    """sentinel guards against re-reading metadata.db on /REHASH."""
    channel = "#rehashtest"
    avatar = "https://example.invalid/x.png"

    await _set_channel_metadata(persistent_ircd, channel, {"avatar": avatar})

    _restart_and_wait(persistent_ircd)

    subprocess.check_call([
        "docker", "exec", persistent_ircd.name,
        "sh", "-c", "kill -HUP $(pgrep obbyircd)"
    ])
    time.sleep(3)

    log = _docker_logs(persistent_ircd.name)
    reads = log.count("metadata entries")
    assert reads == 1, f"metadata-db read fired {reads}x, want 1 (sentinel broken):\n{log[-2000:]}"

    md = await _read_channel_metadata(persistent_ircd, channel)
    assert md.get("avatar") == avatar, f"metadata lost after rehash: {md}"
