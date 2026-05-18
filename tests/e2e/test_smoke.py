from lib.irc import IrcClient


async def test_welcome(ircd):
    port = ircd.host_port(6697)
    async with IrcClient("127.0.0.1", port, nick="smoke") as c:
        await c.register()
        line = await c.expect(lambda l: " 001 " in l, timeout=30)
        assert "smoke" in line


async def test_network_name(ircd):
    port = ircd.host_port(6697)
    async with IrcClient("127.0.0.1", port, nick="netname") as c:
        await c.register()
        await c.expect(lambda l: "NETWORK=TestNet" in l, timeout=30)


async def test_join_and_privmsg_echo(ircd):
    port = ircd.host_port(6697)
    async with IrcClient("127.0.0.1", port, nick="speaker") as c1, \
               IrcClient("127.0.0.1", port, nick="listener") as c2:
        await c1.register()
        await c2.register()
        await c1.expect(lambda l: " 001 " in l)
        await c2.expect(lambda l: " 001 " in l)
        await c1.send("JOIN #room")
        await c2.send("JOIN #room")
        await c2.expect(lambda l: "JOIN" in l and "#room" in l and "listener" in l)
        await c1.send("PRIVMSG #room :hello")
        line = await c2.expect(lambda l: "PRIVMSG #room :hello" in l, timeout=10)
        assert "speaker" in line
