"""obbypy: feature-tour script.

Drop into <conf>/scripts/python/ (or wherever obbypy { scripts-dir
...; } points) and /REHASH.  Exercises a few corners of the API to
prove the wiring; not a recipe for any specific real-world script.
"""

import obby

# ---- 1. Hooks ------------------------------------------------------

def on_connect(event):
    """Greet local users on connect."""
    client = event["client"]
    obby.send_notice(client, f"hello {client.name} from python")
    return 0  # HOOK_CONTINUE


def on_join(event):
    """Welcome users when they enter a channel."""
    client = event["client"]
    channel = event["channel"]
    obby.send_notice(
        client,
        f"welcome to {channel.name} (you bring the user count to {channel.users_count})",
    )
    return 0


def on_chanmsg(event):
    """Quiet logger of activity."""
    obby.log(
        f"{event['client'].name} -> {event['channel'].name}: {event['msg']}",
        level="debug",
    )
    return 0


obby.register_hook("LOCAL_CONNECT", on_connect)
obby.register_hook("LOCAL_JOIN", on_join)
obby.register_hook("CHANMSG", on_chanmsg)


# ---- 2. Commands ---------------------------------------------------

def hello_cmd(client, params):
    """/HELLO -- the bot waves back."""
    obby.send_notice(client, "hi from python")
    if params:
        obby.send_notice(client, f"you said: {' '.join(params)}")


def whois_cmd(client, params):
    """/PYWHOIS <nick> -- demonstrates lookups and attribute access."""
    if not params:
        obby.send_notice(client, "usage: /PYWHOIS <nick>")
        return
    target = obby.find_client(params[0])
    if not target:
        obby.send_notice(client, f"no such user: {params[0]}")
        return
    obby.send_notice(client, f"{target.name}: host={target.host} ip={target.ip} "
                             f"account={target.account or '-'} umodes=+{target.umodes}")


obby.register_command("HELLO", hello_cmd, params=0)
obby.register_command("PYWHOIS", whois_cmd, params=1)


# ---- 3. Timers (one-shot + interval) ------------------------------

# Fire once after 30s, just to show set_timeout works.  In real
# scripts you'd schedule periodic work via set_interval.
def heartbeat():
    obby.log("python heartbeat tick", level="debug")


obby.set_interval(heartbeat, 60_000)


# ---- 4. HTTP -------------------------------------------------------

def on_http_done(err, body):
    if err:
        obby.log(f"http err: {err}", level="warn")
    else:
        snippet = (body or "")[:80].replace("\n", " ")
        obby.log(f"http ok: {snippet}", level="debug")


# Fire once at module load; results go to the IRCd log.
obby.http_get("https://example.com/", on_http_done)


# ---- 5. ModData (per-client kv) -----------------------------------

obby.register_moddata(
    "py_join_count",
    obby.MODDATATYPE_CLIENT,
    sync=False,
)


def bump_join_count(event):
    """Track how many channels a user has joined this session."""
    c = event["client"]
    current = obby.get_moddata(c, "py_join_count") or "0"
    try:
        n = int(current)
    except ValueError:
        n = 0
    n += 1
    obby.set_moddata(c, "py_join_count", str(n))
    if n == 5:
        obby.send_notice(c, "you've joined 5 channels this session, busy!")
    return 0


obby.register_hook("LOCAL_JOIN", bump_join_count)


obby.log("obbypy hello.py loaded")
