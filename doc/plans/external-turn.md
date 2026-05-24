# External TURN — operator-controlled HMAC TURN server

## Problem

`obby-api` ships an embedded `pion/turn` and mints client TURN
credentials inside the SFU. That couples TURN to the SFU process,
wastes a TURN daemon when operators already run one (Coolify users
typically have a shared `coturn`), and gives no generic way to point
clients at an external TURN service.

## Goal

Operator declares an external TURN service once in `obbyircd.conf`
and clients automatically receive HMAC-authenticated credentials for
it. Generic — works with any TURN server speaking
draft-uberti-behave-turn-rest. No hosted-backend code change required.

## Wire today

```
Client                ObbyIRCd                 hosted-backend
──────                ────────                 ──────────────
JOIN ^vc        ────► voice-channels.c   ────► voice.go (SFU + pion/turn)
TAGMSG @+rtc=*        bridge over            - mintTurnCreds(account, ttl)
                      voice.sock (JSON)      - bundles TURN into envelope
                                             - sends "joined" frame back
TAGMSG @+rtc=env ◄────  emit_outbound_signal  ◄─── {op:"signal",to:"<nick>",
                        TAGMSGs to <nick>          payload:{type:"joined",
                                                            TURN:{...}}}
```

`username = "<unix_expiry>:<account>"`, `password =
base64(HMAC-SHA1(shared_secret, username))` — coturn-compatible
(`use-auth-secret` + `static-auth-secret`).

## Design

Single insertion point: `voice-channels.c` rewrites `payload.TURN` on
outbound `signal` frames when `voice::turn` is configured. No new IRC
commands. No client-side change.

### Conf block

```
voice {
    turn {
        url "turn:turn.example.com:3478?transport=udp";
        url "turn:turn.example.com:3478?transport=tcp";
        url "turns:turn.example.com:5349?transport=tcp";
        shared-secret "<coturn static-auth-secret>";
        ttl 21600;
    };
};
```

`url` (≥1, repeatable) and `shared-secret` required; `ttl` optional
(60..86400). Realm intentionally absent: coturn's realm is server-side
config, doesn't reach the client.

### Module changes (`voice-channels.c`)

1. `MOD_TEST` registers `HOOKTYPE_CONFIGTEST`; `MOD_INIT` registers
   `HOOKTYPE_CONFIGRUN`. Both watch `CONFIG_MAIN` for `voice` block.
2. Module state: `cfg_turn_urls[]`, `cfg_turn_secret`, `cfg_turn_ttl`.
3. In `process_bridge_frame`, before `emit_outbound_signal`: if
   `cfg_turn_secret != NULL` and `payload` has a `TURN` field, replace
   it with locally-minted creds. Fail closed: strip the field if mint
   can't produce a valid account.
4. `MOD_UNLOAD` frees the urls + secret.

HMAC: OpenSSL `HMAC(EVP_sha1(), ...)`. b64: `b64_encode()`.

### Conf template + entrypoint

`obbyircd.conf.template` gets `${VOICE_TURN_CONFIG}` placeholder.
`docker-entrypoint.sh` renders the block from env vars:

```sh
VOICE_TURN_EXTERNAL_URLS="turn:t.example:3478 turn:t.example:5349"
VOICE_TURN_SHARED_SECRET="<coturn static-auth-secret>"
VOICE_TURN_TTL=21600
```

URLs are single-quoted in the rendered conf (escaped=1 skips URL
pre-pass). Single quotes inside URLs refused at entrypoint — there's
no escape for them inside single-quoted parser values, and they're
not legal URL bytes. shared-secret is rendered double-quoted with `"`
and `\` escaped.

### Future (out of scope here)

- obby-stack `compose.yaml` + `.env.example` doc entries
- Helm chart values (`voice.turn.external.*`)
- hosted-backend env to skip its embedded `startTurnServer` entirely

## Tests

Tier 1 — `tests/e2e/test_voice_turn.py`:
- pass-through when unconfigured (placeholder TURN survives)
- rewrite when configured (urls/ttl/username/HMAC password verified)
- secret with `'` (entrypoint escape + parser interop)

Bridge stub runs in a sidecar container (`bridge_sidecar.py`) that
holds the unix-socket connection to the IRCd; test process drives it
via docker stdin/stdout — host-side unix sockets don't proxy reliably
on Docker Desktop.
