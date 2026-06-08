#!/usr/bin/env bash
# diagnose-sasl-fail.sh -- inspect why a migrated account can't SASL-login.
#
# Reads obsidian.db (the obbyircd account store), prints scheme + hash
# format + scram presence per account, and (with --probe) runs the same
# verifier obbyircd uses against a supplied plaintext password to
# narrow the failure to data vs. libcrypt vs. daemon code.
#
# Usage:
#   diagnose-sasl-fail.sh [-d PATH] <username>
#   diagnose-sasl-fail.sh [-d PATH] --probe <username> <password>
#   diagnose-sasl-fail.sh [-d PATH] --all

set -euo pipefail

DB="$(dirname "$0")/../data/obsidian.db"
MODE=info
PROBE_PASS=

while [ $# -gt 0 ]; do
  case "$1" in
    -d|--db) DB="$2"; shift 2 ;;
    --all)   MODE=all; shift ;;
    --probe) MODE=probe; PROBE_PASS="$3"; USER="$2"; shift 3 ;;
    -h|--help)
      grep '^#' "$0" | sed 's/^# \{0,1\}//'
      exit 0 ;;
    *)
      USER="$1"; shift ;;
  esac
done

if [ ! -f "$DB" ]; then
  echo "ERROR: obsidian.db not found at $DB. Use -d PATH." >&2
  exit 2
fi

# Sniff a hash format by the leading bytes -- the same heuristics the
# obbyircd verifier uses.
sniff_hash() {
  case "$1" in
    \$argon2*) echo argon2 ;;
    \$2a\$*|\$2b\$*|\$2y\$*) echo bcrypt ;;
    \$5\$*)  echo crypt-sha256 ;;
    \$6\$*)  echo crypt-sha512 ;;
    \$z\$pbkdf2-*) echo pbkdf2v2 ;;
    "") echo "(empty)" ;;
    *) echo "unknown (first6=${1:0:6})" ;;
  esac
}

dump_one() {
  local row="$1"
  IFS='|' read -r name scheme phash scram_salt twofa <<<"$row"
  scheme="${scheme:-(null)}"
  scram="$([ -n "$scram_salt" ] && echo yes || echo no)"
  twofa="$([ "$twofa" = "1" ] && echo yes || echo no)"
  fmt="$(sniff_hash "$phash")"

  echo "account:       $name"
  echo "  password_scheme: $scheme"
  echo "  hash format:     $fmt"
  echo "  hash prefix:     ${phash:0:8}..."
  echo "  scram present:   $scram"
  echo "  2FA enabled:     $twofa"

  # Sanity warnings.
  if [ "$scheme" = "bcrypt" ] && [ "$fmt" != "bcrypt" ]; then
    echo "  WARN: scheme=bcrypt but hash doesn't look like bcrypt"
  fi
  if [ "$scheme" = "(null)" ] && [ "$fmt" != "argon2" ]; then
    echo "  WARN: scheme is NULL but hash isn't argon2 -- verifier falls back"
    echo "        to libcrypt for \$2/\$5/\$6 hashes; if libcrypt lacks"
    echo "        bcrypt support, these accounts can't PLAIN-login"
  fi
  if [ "$scheme" = "reset-required" ]; then
    echo "  NOTE: this account must reset via /RECOVER -- migration"
    echo "        couldn't carry the source hash"
  fi
}

case "$MODE" in
  info)
    [ -n "${USER:-}" ] || { echo "usage: $0 <username>" >&2; exit 2; }
    row=$(sqlite3 "$DB" "SELECT name||'|'||IFNULL(password_scheme,'')||'|'||password||'|'||IFNULL(scram_salt,'')||'|'||IFNULL(twofa_enabled,0) FROM accounts WHERE lower(name)=lower('$USER') LIMIT 1")
    [ -n "$row" ] || { echo "no such account: $USER"; exit 1; }
    dump_one "$row"
    ;;

  all)
    sqlite3 "$DB" "SELECT name||'|'||IFNULL(password_scheme,'')||'|'||password||'|'||IFNULL(scram_salt,'')||'|'||IFNULL(twofa_enabled,0) FROM accounts ORDER BY id" | \
    while read -r row; do
      dump_one "$row"
      echo
    done
    ;;

  probe)
    row=$(sqlite3 "$DB" "SELECT IFNULL(password_scheme,'')||'|'||password FROM accounts WHERE lower(name)=lower('$USER') LIMIT 1")
    [ -n "$row" ] || { echo "no such account: $USER"; exit 1; }
    IFS='|' read -r scheme phash <<<"$row"
    fmt="$(sniff_hash "$phash")"
    echo "scheme: ${scheme:-(null)}"
    echo "format: $fmt"

    # Drive the libcrypt path the same way obbyircd does.
    case "$fmt" in
      bcrypt|crypt-sha256|crypt-sha512)
        python3 - "$phash" "$PROBE_PASS" <<'PY'
import sys, ctypes, ctypes.util
lib = ctypes.CDLL(ctypes.util.find_library("crypt") or "libcrypt.so.1", use_errno=True)
class crypt_data(ctypes.Structure):
    _fields_ = [("buf", ctypes.c_char * 32768)]
lib.crypt_r.restype = ctypes.c_char_p
lib.crypt_r.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.POINTER(crypt_data)]
d = crypt_data()
out = lib.crypt_r(sys.argv[2].encode(), sys.argv[1].encode(), ctypes.byref(d))
if not out:
    import os
    print(f"crypt_r returned NULL (errno={ctypes.get_errno()}: {os.strerror(ctypes.get_errno())})")
    print("--> libcrypt on this host does NOT recognise the hash format.")
    print("--> install libxcrypt or libxcrypt-compat to add bcrypt support.")
    sys.exit(1)
out = out.decode()
match = out == sys.argv[1]
print(f"crypt_r returned: {out}")
print(f"matches stored:   {match}")
sys.exit(0 if match else 1)
PY
        ;;
      argon2)
        echo "(argon2 verify needs the libargon2 binding -- not implemented in this script)"
        ;;
      *)
        echo "(no probe available for format=$fmt)"
        ;;
    esac
    ;;
esac
