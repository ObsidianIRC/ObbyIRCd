# AGENTS.md

How to write code in this repo without getting a PR review rant.

This file is the working guide for AI agents and humans. For *what* the
project is and *how it's deployed*, read [`ARCHITECTURE.md`](ARCHITECTURE.md)
first. For the canonical upstream style rules, read
[`doc/coding-guidelines`](doc/coding-guidelines) — this file restates and
extends them with the fork-specific bits.

## 1. Orientation

- **Lineage**: fork of UnrealIRCd 6 (`unreal60_dev`). Periodically merged
  from `upstream/unreal60_dev`. ObsidianIRC modules and the hosted backend
  live in-tree, not as third-party drops.
- **License**: GPLv2, inherited from UnrealIRCd. Preserve upstream
  credit; rule #15 of `doc/coding-guidelines` is non-negotiable.
- **Languages**:
  - C (ircd core + modules) — UnrealIRCd house style, see §3
  - Go (`tools/obbyircd-migrate/`; the hosted-backend Go source lives in its own repo, [ObsidianIRC/hosted-backend](https://github.com/ObsidianIRC/hosted-backend))
  - Embedded JavaScript via Duktape (`obbyscript`, `conf/scripts/*.js`)
  - Embedded Python 3 via CPython (`obbypy`, `conf/scripts/python/*.py`)
  - Shell (`docker/docker-entrypoint.sh`, `Config`, `extras/startup/`)

## 2. Where things live

```
src/                    core ircd C source
src/modules/            205 modules; ~20 ObsidianIRC-specific
include/                public headers; include "unrealircd.h" gets you everything
include/obsidian.h      fork-specific types (Account, Metadata, TwoFACredential)
doc/coding-guidelines   authoritative C style
doc/conf/               default config files + example.conf
doc/conf/scripts/       obbyscript example .js shipped to operators
tools/obbyircd-migrate/ Go CLI: Ergo/Anope/Atheme → obbyircd
docker/                 single-stage Alpine Dockerfile + entrypoint + conf template
compose.yaml            Coolify deploy: obbyircd (built here) + obby-api (mattfly/obby-api) + obby web (mattfly/obby, opt-in)
```

The "where is X" answer is almost always *the upstream UnrealIRCd
location*. The Obby delta is concentrated in `src/modules/`,
`include/obsidian.h`, `tools/obbyircd-migrate/`, `docker/`, and
`compose.yaml`. The hosted-backend source lives in its own repo;
this repo only references its published image.

## 3. C code — UnrealIRCd house style

Tabs, width 8, Allman braces. `/* */` comments only.
`#include "unrealircd.h"` pulls in everything; don't sprinkle individual
headers.

```c
if (something == 1)
{
    moo; /* one-line comments after code are fine */
    cow(go(moo));
}
```

Not K&R, not `// comments`, never spaces for indentation. Rule #11, #12,
#16 of `doc/coding-guidelines`.

### 3.1 Module skeleton

```c
/*
 *   IRC - Internet Relay Chat, src/modules/example.c
 *   (C) <year> The ObsidianIRC Team
 *
 *   See file AUTHORS in IRC package for additional names of
 *   the programmers.
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 1, or (at your option)
 *   any later version.
 */

#include "unrealircd.h"

CMD_FUNC(cmd_example);

#define MSG_EXAMPLE "EXAMPLE"

ModuleHeader MOD_HEADER
  = {
        "example",
        "1.0",
        "command /example",
        "ObsidianIRC Team",
        "unrealircd-6",
    };

MOD_TEST() { return MOD_SUCCESS; }

MOD_INIT()
{
    CommandAdd(modinfo->handle, MSG_EXAMPLE, cmd_example, MAXPARA, CMD_USER);
    MARK_AS_OFFICIAL_MODULE(modinfo);
    return MOD_SUCCESS;
}

MOD_LOAD()   { return MOD_SUCCESS; }
MOD_UNLOAD() { return MOD_SUCCESS; }

CMD_FUNC(cmd_example)
{
    sendnumeric(client, RPL_TEXT, "hello");
}
```

The `modversion` field (5th MOD_HEADER entry) **must** be
`"unrealircd-6"` — the loader rejects everything else.

Mirror the file name to the module name. Mirror the module name to the
`MOD_HEADER.name` string. Mirror that to the `loadmodule "name";`
config line. They must all agree.

### 3.2 Lifecycle — what goes where

| Callback | Use for |
|----------|---------|
| `MOD_TEST` | `HOOKTYPE_CONFIGTEST`, `HOOKTYPE_CONFIGPOSTTEST`. Pre-register mod-data slots, snomasks. |
| `MOD_INIT` | `CommandAdd`, hooks (all except CONFIGTEST/CONFIGPOSTTEST), `ClientCapabilityAdd`, `CmodeAdd`, `UmodeAdd`, `ModDataAdd`, `MARK_AS_OFFICIAL_MODULE`. |
| `MOD_LOAD` | `CommandOverrideAdd` (load-order safe here, **not** in MOD_INIT). Cross-module setup. Check `ModuleGetError(modinfo->handle)`. |
| `MOD_UNLOAD` | Free everything `safe_alloc`/`safe_strdup`'d. Walk and free linked lists. NULL their heads. The loader auto-detaches the things you registered via `*Add(modinfo->handle, ...)`. |

`CommandOverrideAdd` in `MOD_INIT` is a real bug — module load order
breaks it. Use `MOD_LOAD`. Likewise CONFIGTEST hooks **must** be in
`MOD_TEST` or they never see the validating pass.

### 3.3 Memory

```c
safe_alloc(size)                      /* zero-init, replaces malloc/calloc */
safe_free(ptr)                        /* NULL-safe, sets ptr=NULL after */
safe_strdup(dst, src)                 /* frees old dst first; pure assignment */
safe_strldup(dst, src, max)           /* bounded strdup */
safe_alloc_sensitive(size)            /* mlock'd, guard-paged (libsodium) */
safe_free_sensitive(ptr)              /* wipes memory on free */
safe_strdup_sensitive(dst, src)       /* for passwords, keys, SASL secrets */
```

Never `malloc`, never `calloc`, never `strdup`. Rule #19.

Prefer a stack buffer with `strlcpy`/`ircsnprintf` to `safe_strdup`
for short-lived strings — syzop's recurring critique. Heap allocation
is for things that genuinely outlive the function.

For sensitive material (passwords, tokens, keys) use the `_sensitive`
variants — they `mlock`/wipe on free.

### 3.4 String I/O

```c
strlcpy(dst, src, sizeof dst)
strlcat(dst, src, sizeof dst)
ircsnprintf(buf, sizeof buf, "%s %d", a, n)   /* fast path */
snprintf  (buf, sizeof buf, "%s ...", ...)    /* full printf */
sendnumeric(client, ERR_NOSUCHCHANNEL, name)
sendnumericfmt(client, num, fmt, ...)
```

Never `strcpy`, `strcat`, `sprintf`, `ircsprintf`. Rule #17.

`ircsnprintf` is preferred over `snprintf` for simple format strings;
it falls back to `snprintf` for anything beyond `%s %d %c %u %lu`-ish
specifiers without uncommon prefixes. Rule #18.

Format-string safety: **never** pass user input as the format string.

```c
sendto_one(client, NULL, "%s", user_input);    /* GOOD */
sendto_one(client, NULL, user_input);          /* SECURITY BUG */
```

Every `sendto_*`, `config_error`, `sendnumericfmt`,
`ircsnprintf` is `__attribute__((format(printf, ...)))` — the compiler
will catch many mistakes, but only with `-Wformat-security`.

### 3.5 Buffer sizes

```c
HOSTLEN    63   NICKLEN    30   CHANNELLEN  32
BUFSIZE   512                              /* raw line, do NOT change */
MAXLINELENGTH_USER  (MAXTAGSIZE + BUFSIZE) /* w/ message tags */
ISUPPORTLEN  (BUFSIZE - HOSTLEN - NICKLEN - 39)
```

Stack buffers carry `+1` for the NUL: `char nick[NICKLEN+1]`. `BUFSIZE`
is *not* the IRCv3 message-tags-aware budget — `MAXLINELENGTH_USER`
is. The token you add to a 005/ISUPPORT line eats from `ISUPPORTLEN`
*after* `strlen("YOURTOKEN=")`.

### 3.6 Hooks

```c
HookAdd(modinfo->handle, HOOKTYPE_LOCAL_JOIN, 0, my_join);
```

Priority: lower runs earlier; `-1000000` is "first, hard"; `+1000000`
is "last, hard". Default 0.

Returns:
- `HOOK_CONTINUE` (0) — pass through
- `HOOK_DENY` (1) — block the action (for decision hooks)
- `HOOK_ALLOW` (-1) — short-circuit accept

Decision hooks (`HOOKTYPE_CAN_JOIN`, `_CAN_KICK`, `_CAN_SEND_*`,
`_PRE_LOCAL_*`, `_IS_HANDSHAKE_FINISHED`, ...): return the decision,
do not mutate state to undo what the caller half-did. The caller
cleans up based on your return.

Notification hooks (`HOOKTYPE_LOCAL_JOIN`, `_REMOTE_JOIN`,
`_LOCAL_CONNECT`, `_TKL_ADD`, ...): return 0. The event already
happened.

**Never free hook arguments.** `safe_strdup` if you need to retain
them.

For new hook types: add `#define HOOKTYPE_FOO N` (next free) in
`include/modules.h`, add a `ValidateHook` line, add a typedef for the
function pointer. PRs #324 (`HOOKTYPE_MOTD`) and #313 (`HOOKTYPE_CAN_USE_NICK`)
are the canonical recent examples.

### 3.7 Command handlers

```c
CMD_FUNC(cmd_example);   /* expands to the canonical signature */
/* = void cmd_example(Client *client, MessageTag *recv_mtags,
                      int parc, const char *parv[]) */
```

`parv[0]` is the command name. `parv[parc]` is `NULL`. Always check
`parc < N` before reading `parv[N-1]` and `BadPtr(parv[N])` before
using a parameter.

Permission checks: `IsOper(client)`, `IsULine(client)`,
`MyUser(client)`, `IsRegistered(client)`. For fine-grained
operclass enforcement use `ValidatePermissionsForPath("foo:bar",
client, victim, channel, NULL)` — never roll your own opclass logic.

Propagate incoming message tags through outgoing sends via
`recv_mtags`. If you create new tags, pair `new_message()` with
`free_message_tags()` (or `safe_free_message_tags()`).

### 3.8 Config parsing

Three callbacks:

```c
/* MOD_TEST() registers these two */
HookAdd(handle, HOOKTYPE_CONFIGTEST,     0, my_configtest);
HookAdd(handle, HOOKTYPE_CONFIGPOSTTEST, 0, my_configposttest);
/* MOD_INIT() registers this one */
HookAdd(handle, HOOKTYPE_CONFIGRUN,      0, my_configrun);
```

`configtest` validates and *reports errors* via `config_error()` with
the exact format `"%s:%i: <message>"` including `cep->file->filename`
and `cep->line_number`. The Coolify UI parses these messages.

`config_warn` for non-fatal advisories (no error counter bump).
`config_status` for informational console messages.

Return `0` from your handler if you don't claim the block; return
non-zero (with errors counted via `*errs`) if you do.

`configrun` assumes input is already validated — no error reporting
here, just `safe_strdup` into your globals.

UnrealIRCd 6 field names: `ConfigEntry->name`, `->value`, `->items`,
`->file`, `->line_number`. Older U5 code uses `ce_varname`/`ce_vardata`/
`ce_entries` — modernize on touch.

### 3.9 Logging

Use the structured logger, not `snprintf` into a single string:

```c
unreal_log(ULOG_INFO, "subsystem", "EVENT_ID", client,
           "$client.details did $action",
           log_data_string("action", "blah"));
```

`$client` expands to the name; `$client.details` expands to "name
[ip]" or "name@ip"-style depending on what's available — use it for
audit messages where the IP matters. Picking the wrong expansion is
a real review pushback (PR #180).

### 3.10 Networking

UnrealIRCd is single-threaded event-driven. **Never block**:

| Use case | API |
|----------|-----|
| DNS | `unrealdns_doclient(client)` etc. — *not* `gethostbyname` |
| HTTP / outbound | `OutgoingWebRequest` (see `src/url.c`); `safe_strdup` into the request, no static storage |
| Timers | `EventAdd(handle, "name", cb, NULL, ms, count)` |
| Files | non-blocking, or `file_get_contents()` for small reads |
| Subprocesses | don't |

Comparison: `strcasecmp`/`strncasecmp` (ASCII-locale-independent).
`find_client`/`find_channel` handle rfc1459 nick mapping
(`{}|^` ↔ `[]\\~`) internally — never roll your own.

IPv6: `struct sockaddr_storage`, use `src/socket.c` helpers, never
assume `struct in_addr`. The CIDR/mask helpers
(`default-ipv6-clone-mask` machinery) are recent — reuse them.

Capability test in hot paths: `HasCapabilityFast(client, CAP_X)` —
the bit is already known, skip the hash lookup.

### 3.11 ObbyIRCd-specific patterns

- `include/obsidian.h` is the home of fork-wide types
  (`Account`, `Metadata`, `TwoFACredential`, the persistence ghost
  record). Include it from any module that touches the account/db
  layer.
- SQLite handle accessors live there too — don't open new connections
  per module, use the shared one.
- Custom hook: `HOOKTYPE_ACCOUNT_REGISTER` (id 133). Fires after a
  successful account registration. Used by account-recovery,
  authtoken, metadata-db.
- `obbyscript` and `obbypy` have feature parity. Don't add a hook to
  one without the other — the script API is meant to be symmetric.

## 4. Python — for `obbypy` scripts and tooling

Per repo-wide preference:

- **Imports at the top of the file.** Never local to a function or
  scope unless there's a documented circular-import workaround.
- **No bare `except Exception:`.** Catch the specific exceptions that
  can actually occur. `except` without a class, or `except Exception`,
  is rejected.
- Comments explain *why*, not *what*. Don't paraphrase the code in
  English. If the WHY is obvious, no comment.
- Type hints encouraged for module-level functions.
- Match the obbyscript JS surface when adding an obbypy API — the two
  scripting engines are siblings.

## 5. Go — for `tools/obbyircd-migrate/`

(The hosted-backend Go code lives in its own repo,
[ObsidianIRC/hosted-backend](https://github.com/ObsidianIRC/hosted-backend);
the rules below apply there too but enforce them on its PRs, not here.)

- `gofmt`/`goimports` clean — CI gate when added.
- `CGO_ENABLED=1` is required because the binary links `mattn/go-sqlite3`.
- Errors: wrap with `fmt.Errorf("doing X: %w", err)`. No bare
  `return err` past three call frames.
- No `panic` outside `main()` for fatal startup.
- For the migration tool: extend the IR (`internal/ir/ir.go`) when
  adding a new field — never carry source-specific shapes through to
  the writer. The IR is the contract.
- For the hosted backend: JWT and TURN secrets are environment-driven
  (`JWT_SECRET`, `VOICE_TURN_SECRET`); never hard-code defaults that
  could leak into production.

## 6. Shell — `docker/docker-entrypoint.sh`, scripts

- `set -eu` at the top. Add `-o pipefail` only on bash (entrypoint
  uses `/bin/sh` for Alpine compatibility — `pipefail` is bash-only).
- Quote every variable expansion: `"$var"`, `"$@"`.
- ShellCheck-clean.
- The entrypoint is one of the most-touched files — test it in a
  fresh container before claiming changes work. Empty conf volume +
  populated conf volume are both code paths.

## 7. Build, test, validate

```bash
./Config -quick        # non-interactive build, defaults to ~/obby
make -j$(nproc)
make install
./bin/obbyircd -c      # configtest only; non-zero exit on error
./bin/obbyircd -F      # run in foreground

# In Docker (the canonical CI-equivalent):
docker compose build
docker compose up -d
docker compose logs -f obbyircd
```

CI runs `extras/build-tests/nix/build` + `run-tests` on every PR
against `unreal60_dev`. There is **no in-tree unit-test harness** for
modules — coders are expected to test manually (rule #10). For
non-trivial changes:

1. Build with `-DDEBUGMODE` for extra runtime assertions
2. Spin up two instances, link them, exercise the path
3. Valgrind / ASAN if you allocate non-trivially
4. Rehash twice in a row; no leaks, no double-frees

Compile warning policy: clean on `-Wall -Wextra -Wformat
-Wformat-security`. New warnings will get flagged in review.

## 8. PR etiquette

- **Single-purpose PRs.** Re-indents and feature work in the same PR
  get the indent reverted by the reviewer. Whitespace-only fixes
  ship as their own PR (rule #6).
- For upstream UnrealIRCd: file a bug at <https://bugs.unrealircd.org>
  first, wait for head-coder acknowledgement, then PR with the bug#
  in the commit message (rule #3). For ObbyIRCd-specific work,
  GitHub issues + PRs on this repo are fine.
- Commit subject: imperative or descriptive, 50–72 chars, no period,
  cite bug# if relevant. No emoji.
- No `Signed-off-by:` lines unless you want to — UnrealIRCd has no DCO.
- Never touch `version.c.SH` or `version.h` unless you're a head
  coder (rule #13). Credits go via the bug tracker.
- **No protocol changes without prior discussion** (rule #14). This is
  the single most rejected class of patch.
- **No re-indenting unrelated lines** in a feature PR. Whitespace
  churn is its own PR.
- During RC stage (RC1 → final release) only the head coder commits
  directly; everyone else asks first, even for one-liners (rule #7).

## 9. Concrete don'ts

Reviewer-tested. Skip these, save a round-trip.

1. **Don't `strcpy` / `strcat` / `sprintf` / `ircsprintf`.** Rule #17.
2. **Don't `malloc` / `calloc` / `strdup`.** Rule #19. Use `safe_*`.
3. **Don't heap-allocate short-lived strings.** Stack + `strlcpy` /
   `ircsnprintf` is the canonical choice.
4. **Don't leave allocations un-freed on early-return paths.** Either
   `safe_free` before each `return`, or restructure to a single exit.
5. **Don't `// comment`.** Use `/* */`. Rule #11.
6. **Don't use spaces or mix tabs+spaces.** Tabs, width 8. Rule #16.
7. **Don't K&R-brace `if (x) {`.** Allman. Rule #12.
8. **Don't re-indent unrelated code in a feature PR.** Rule #6.
9. **Don't write `Unrealircd`.** It's `UnrealIRCd`.
10. **Don't mutate state from a decision hook** (e.g.
    `sub1_from_channel` from `can_join`). Return the decision; let the
    caller clean up.
11. **Don't `CommandOverrideAdd()` from `MOD_INIT()`.** It belongs in
    `MOD_LOAD()`.
12. **Don't register `HOOKTYPE_CONFIGTEST` outside `MOD_TEST()`.**
13. **Don't tighten parse rules on protocol input without
    discussion.** The intentional liberal-acceptance is what lets us
    evolve the protocol without desync.
14. **Don't broadcast unnecessarily on rehash.** Incremental only.
15. **Don't pass user data as a format string.**
16. **Don't roll your own oper-permission check.** Use
    `ValidatePermissionsForPath`.
17. **Don't free hook arguments.** Caller owns them.
18. **Don't bypass the bug tracker for non-trivial upstream work.**

## 10. Repo hygiene (this fork)

These are the user-level preferences enforced on touch in this repo,
in addition to the upstream rules above:

- **Write code as if it always was that way.** No patch-history
  comments. No `/* was X, now Y */`. No commented-out blocks of "old
  way for reference". No `/* removed Z */`. The PR description holds
  the change story; the source holds the ideal.
- **Comments explain WHY, not WHAT.** Skip the obvious. If a future
  reader would understand the line without the comment, drop the
  comment.
- **Lint clean on touch.** Errors I introduce → fix before commit.
  Errors in files I touched → fix in the same commit. Don't ship
  dirty diffs.
- **Don't manually patch state to mask a real bug.** If the fix is in
  the entrypoint or the migration writer or the conf template, wipe
  the bad state and let the canonical path recreate it. Manual
  `chown`/`chmod`/`rm` repairs contaminate the test.
- **Read git freely, never mutate.** `git status`, `diff`, `log`,
  `show`, `blame` are fine. `git add`, `commit`, `push`, `reset`,
  `checkout`, `merge`, `rebase`, `stash` only on explicit request.
- **Verify before claiming.** `ssh h4ks uname -m` before claiming
  arch. Read the file before claiming what it does. Run the build
  before claiming it compiles.

## 11. Quick file-path reference

- Canonical style rules: [`doc/coding-guidelines`](doc/coding-guidelines)
- Module API: [`include/modules.h`](include/modules.h)
- Send / format / log helpers: [`include/h.h`](include/h.h)
- Type definitions, buffer sizes: [`include/struct.h`](include/struct.h)
- Fork-wide types: [`include/obsidian.h`](include/obsidian.h)
- Tiny module skeletons (copy-paste): `src/modules/{away,help,motd,admin}.c`
- Config-block example: [`src/modules/restrict-commands.c`](src/modules/restrict-commands.c)
- Upstream Module API wiki: <https://www.unrealircd.org/docs/Dev:Module_API>
- Upstream API Doxygen: <https://www.unrealircd.org/api/>
- Upstream contribution flow: <https://www.unrealircd.org/docs/Contributing>
- Upstream bug tracker: <https://bugs.unrealircd.org>
