# obbyircd-migrate

Migrate accounts, channels, and bans from Ergo / Anope / Atheme into
obbyircd's persistent stores.

## Targets

| Source format | Reader | Status |
|---------------|--------|--------|
| Atheme `services.db` (opensex) | [internal/readers/atheme](internal/readers/atheme) | Working — fixture + live tested |
| Anope `anope.db` (db_flatfile) | [internal/readers/anope](internal/readers/anope) | Working — fixture tested |
| Ergo `ircd.db` (BuntDB) | [internal/readers/ergo](internal/readers/ergo) | Working — live-tested against a real ergochat/ergo instance |

| Target | Writer | Status |
|--------|--------|--------|
| `obsidian.db` (sqlite, Phase 0 schema) | [internal/writer/obsidian.go](internal/writer/obsidian.go) | Schema-aware via PRAGMA, writes Phase 0 columns when present |
| `channel.db` (UnrealDB v101) | [internal/writer/channeldb.go](internal/writer/channeldb.go) | Direct binary write; round-trip verified by mirror parser |
| `tkldb.db` (UnrealDB v4999) | [internal/writer/tkldb.go](internal/writer/tkldb.go) | K/G/Z/Q-line types; X-line dropped with warning |

## Usage

```bash
# Interactive (default with no args)
obbyircd-migrate

# Read source, emit IR JSON to stdout
obbyircd-migrate read --from atheme --in services.db

# Full migration: read source -> write all three target stores
obbyircd-migrate run \
  --from ergo \
  --in /var/lib/ergo/ircd.db \
  --obsidian /home/valware/obby/data/obsidian.db \
  --channel  /home/valware/obby/data/channel.db \
  --tkl      /home/valware/obby/data/tkldb.db \
  --on-conflict skip
```

`--dry-run` previews without writing. `--on-conflict` is one of
`skip|fail|merge`.

## How channel ACL is encoded

Source-side ACL grants and akicks become channel-level extbans
(matching the design in [migration-research/PLAN.md](../../migration-research/PLAN.md)
§1.3):

| Source | Encoded as |
|--------|-----------|
| `Founder` | `obsidianirc/channel-registration` ModData stamp + `+e ~automode:q:~account:<founder>` |
| Account ACL grant | `+e ~automode:<modes>:~account:<account>` |
| Mask ACL grant | `+e ~automode:<modes>:<n!u@h>` |
| Akick (account) | `+b ~account:<account>` |
| Akick (mask) | `+b <n!u@h>` |

`<modes>` is a concatenation of letters from the active member-roles
config (default: q/a/o/h/v).

## Password compatibility

The writer copies hashes verbatim and stores the family in
`password_scheme`:

| Source format | obbyircd `password_scheme` |
|---------------|---------------------------|
| `$argon2id$...`           | `argon2id` (verifies natively today) |
| `$2a$/$2b$/$2y$...`       | `bcrypt` (verifier work in-progress) |
| `$z$pbkdf2-sha512$...`    | `pbkdf2v2` (verifier work in-progress) |
| `$5$/$6$...` POSIX crypt  | `crypt-sha256` / `crypt-sha512` |
| Raw md5/sha1, plain       | `reset-required` (forces reset on first login) |

SCRAM-SHA-256 from Ergo transfers verbatim into `accounts.scram_*`
columns and works immediately for SASL SCRAM auth.

## Design

```
  ┌────────────┐     ┌──────────────┐     ┌────────────┐
  │  reader    │ ──> │ intermediate │ ──> │  writer    │
  │  (per src) │     │     (IR)     │     │ (per dest) │
  └────────────┘     └──────────────┘     └────────────┘
```

The IR ([internal/ir/ir.go](internal/ir/ir.go)) is the contract.
Readers and writers evolve independently.

## Layout

```
cmd/obbyircd-migrate/      CLI: tui (default), read, run
internal/
  ir/          IR types (Bundle, Account, Channel, Ban, Memo, Oper)
  readers/
    anope/     OBJECT/DATA flatfile parser
    atheme/    opensex (MU/MN/MC/CA/KL/XL/QL/...) parser
    ergo/      BuntDB key-walker
  writer/
    obsidian.go       sqlite writer, schema-introspecting
    channeldb.go      UnrealDB v101 binary writer
    channeldb_parser.go   round-trip parser used in tests
    tkldb.go          UnrealDB v4999 binary writer
  migrate/    reader+writer orchestration
  tui/        bubbletea TUI
```

## Tests

`go test ./...` — fixture-driven tests for each reader, end-to-end
tests for each writer (with a mirror parser to round-trip channel.db
binary output against the same logic `src/modules/channeldb.c` uses).

## Known limitations

- The bcrypt / pbkdf2v2 / crypt-sha256 / crypt-sha512 verifier paths
  in `account-registration.c` are still TODO (PLAN.md §6.3 follow-up).
  Migrated accounts using those schemes will be readable by the
  writer (column populated, hash preserved), but obbyircd can't yet
  *verify* against them — only argon2id works for live login.
- X-line (gecos ban) doesn't have a native UnrealIRCd equivalent.
  The TKL writer drops X-lines with a warning; create a manual
  spamfilter rule if needed.
- Memos are not currently written anywhere — the IR carries them but
  no obsidian.db `memos` table exists yet (deferred per PLAN.md §6.2).
