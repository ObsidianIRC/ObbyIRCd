# ObbyIRCd e2e

Single-container smoke tests: build the image, boot one IRCd, assert
on real protocol I/O. Runs before the image is published anywhere.

## Run locally

```
docker build -t obbyircd:e2e -f docker/Dockerfile .
cd tests/e2e
uv venv && source .venv/bin/activate
uv pip install -e .
OBBYIRCD_IMAGE=obbyircd:e2e pytest -v
```

Each test spawns a fresh container with random ephemeral ports and
its own `/tmp/obbyircd-e2e-*` data root. No shared state.

## Scope

- Basic protocol: registration, ISUPPORT, JOIN, PRIVMSG echo
- IRCv3 CAP coverage of obby features

Stack-level tests (file uploads, voice bridge, web client) live in
[obby-stack](https://github.com/ObsidianIRC/obby-stack) under
`tests/e2e/`.
