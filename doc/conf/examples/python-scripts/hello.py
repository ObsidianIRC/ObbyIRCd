"""Smoke-test script for obbypy.

Drop into <conf>/scripts/python/hello.py (or wherever obbypy {
scripts-dir ...; } points) and /REHASH.  Demonstrates a hook and a
command; the API surface lives in the built-in `obby` module.
"""

import obby


def on_connect(event):
    """Greet local users on connect."""
    client = event["client"]
    obby.send_notice(client, f"hello {client.name}, you connected from {client.ip}")
    return 0  # HOOK_CONTINUE


def hello_cmd(client, params):
    """/HELLO -- the bot waves back."""
    obby.send_notice(client, "hi from python!")
    if params:
        obby.send_notice(client, f"you said: {' '.join(params)}")


obby.register_hook("LOCAL_CONNECT", on_connect)
obby.register_command("HELLO", hello_cmd, params=0)
obby.log("hello.py loaded")
