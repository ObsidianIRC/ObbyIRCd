from lib.irc import IrcClient

EXPECTED_CAPS = (
    "sasl",
    "message-tags",
    "server-time",
    "account-notify",
    "extended-join",
    "draft/chathistory",
    "draft/account-registration",
)


async def test_cap_ls_advertises_obby_set(ircd):
    port = ircd.host_port(6697)
    async with IrcClient("127.0.0.1", port, nick="caps") as c:
        await c.send("CAP LS 302")
        await c.register()
        for _ in range(8):
            try:
                await c.expect(lambda l: "CAP " in l and " LS " in l, timeout=5)
            except TimeoutError:
                break
        joined = " ".join(c.lines)
        missing = [t for t in EXPECTED_CAPS if t not in joined]
        assert not missing, f"missing CAPs: {missing}"
