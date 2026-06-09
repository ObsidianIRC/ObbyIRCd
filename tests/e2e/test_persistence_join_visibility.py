"""
When a user with persistence OFF reconnects, observers in the channels
they're re-restored to must see a JOIN broadcast.  The prior bug was a
silent add via persist_account_login -> restore_channels' same_nick
branch, which only emitted a JOIN to the joining client itself.
"""

import asyncio
import base64

import pytest

from lib.irc import IrcClient


async def _drain_motd(c):
    await c.expect(lambda l: " 376 " in l or " 422 " in l, timeout=20)


async def _sasl_plain(c, account, password):
    await c.send("AUTHENTICATE PLAIN")
    await c.expect(lambda l: "AUTHENTICATE +" in l, timeout=10)
    payload = b"\0" + account.encode() + b"\0" + password.encode()
    await c.send("AUTHENTICATE " + base64.b64encode(payload).decode())
    await c.expect(lambda l: " 903 " in l or " 904 " in l or " 905 " in l, timeout=10)


async def _register(host, port, account, password):
    """One-shot account creation."""
    async with IrcClient(host, port, nick=account) as c:
        await c.send("CAP LS 302")
        await c.send("CAP END")
        await c.register()
        await _drain_motd(c)
        await c.send(f"REGISTER {account} {account}@test.local {password}")
        # 944 = REGISTER_SUCCESS or a fail numeric.  Tolerate "already
        # registered" so the test can rerun against a stateful container.
        await c.expect(
            lambda l: (
                " 944 " in l
                or "REGISTER " in l and ("SUCCESS" in l or "EXISTS" in l)
                or "ACCOUNT_EXISTS" in l
            ),
            timeout=10,
        )


async def _login(host, port, account, password, with_persistence: bool):
    c = IrcClient(host, port, nick=account)
    await c.__aenter__()
    caps = ["sasl", "draft/persistence", "account-tag"] if with_persistence else ["sasl", "account-tag"]
    await c.send("CAP LS 302")
    await c.send("CAP REQ :" + " ".join(caps))
    await c.expect(lambda l: " ACK " in l and "sasl" in l, timeout=10)
    await _sasl_plain(c, account, password)
    await c.send("CAP END")
    await c.send(f"NICK {account}")
    await c.send(f"USER {account} 0 * :{account}")
    await _drain_motd(c)
    return c


async def _close(c):
    try:
        await c.__aexit__(None, None, None)
    except Exception:
        pass


@pytest.mark.asyncio
async def test_persistence_off_reconnect_broadcasts_join(ircd):
    host = "127.0.0.1"
    port = ircd.host_port(6697)

    account = "valware"
    password = "testpw1234"
    channel = "#room"

    await _register(host, port, account, password)

    # Observer joins channel first.
    obs = IrcClient(host, port, nick="watcher")
    await obs.__aenter__()
    await obs.register()
    await _drain_motd(obs)
    await obs.send(f"JOIN {channel}")
    await obs.expect(lambda l: " 366 " in l and channel in l, timeout=10)

    # Valware logs in with persistence ON, joins, then disables persistence.
    a = await _login(host, port, account, password, with_persistence=True)
    await a.send(f"JOIN {channel}")
    await a.expect(lambda l: " 366 " in l and channel in l, timeout=10)
    # Observer should see Valware JOIN.
    await obs.expect(
        lambda l: f" JOIN " in l and channel in l and "valware" in l.lower(),
        timeout=10,
    )

    # Switch persistence OFF on Valware's account.
    await a.send("PERSISTENCE SET OFF")
    # Tolerate any reply -- the cmd ack varies.
    await asyncio.sleep(0.3)

    # Valware disconnects (simulates page refresh / connection drop).
    await _close(a)

    # Observer must see QUIT for Valware (because persistence was off).
    await obs.expect(
        lambda l: " QUIT " in l and "valware" in l.lower(),
        timeout=10,
    )

    # Valware reconnects.  Note: with_persistence still True client-side;
    # the SERVER preference is OFF so the bug condition applies.
    b = await _login(host, port, account, password, with_persistence=True)

    # CRITICAL: the observer must see a JOIN line for Valware in the
    # channel they were previously in.  Pre-fix this never arrives.
    try:
        line = await obs.expect(
            lambda l: " JOIN " in l and "valware" in l.lower() and channel in l,
            timeout=8,
        )
    except TimeoutError:
        await _close(b)
        await _close(obs)
        pytest.fail(
            "Bug present: observer did not see JOIN broadcast for "
            "Valware's reconnect after persistence was disabled. "
            "Most-recent observer lines: " + "\n".join(obs.lines[-12:])
        )

    await _close(b)
    await _close(obs)
    assert channel in line, f"unexpected JOIN target: {line}"
