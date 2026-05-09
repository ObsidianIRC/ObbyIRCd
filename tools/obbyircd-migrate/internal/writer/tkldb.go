package writer

import (
	"bytes"
	"errors"
	"fmt"
	"os"
	"strings"

	"github.com/obsidianirc/obbyircd-migrate/internal/ir"
)

// Mirror of src/modules/tkldb.c.
const (
	tkldbMagic   = 0x10101010
	tkldbVersion = 4999
)

// WriteTKLDB serialises every IR ban into UnrealDB format at `path`,
// matching tkldb.c's write_tkldb / write_tkline framing. The output
// is plaintext (no encryption); if the running ircd uses
// `set::tkldb::db-secret` you'll need to re-encrypt manually.
//
// Mapping from IR ban types to UnrealIRCd TKL types:
//
//	IR "K" (kline)      -> 'G' (global gline) -- safer default than 'k'
//	IR "G" (gline)      -> 'G'
//	IR "Z" (zline)      -> 'Z' (global gzline)
//	IR "Q" (nick-line)  -> 'Q' (global qline)
//	IR "X" (gecos-line) -> dropped, recorded in rep.Warnings (UnrealIRCd
//	                       has no first-class gecos ban; closest is a
//	                       spamfilter rule which we don't auto-create).
func WriteTKLDB(path string, b *ir.Bundle, opts Options, rep *Report) error {
	if path == "" {
		return errors.New("WriteTKLDB: path is required")
	}
	if rep == nil {
		rep = &Report{}
	}

	type encodedTKL struct {
		letter   byte
		setBy    string
		setAt    int64
		expireAt int64
		// server-ban payload
		usermask string
		hostmask string
		reason   string
		// name-ban payload (alternate)
		isNameBan bool
		nameHold  string // "H" or "*"
		nameMask  string // the nick to reserve
	}

	var encoded []encodedTKL
	for _, ban := range b.Bans {
		typ := strings.ToUpper(ban.Type)
		var letter byte
		switch typ {
		case "K", "G":
			letter = 'G'
		case "Z":
			letter = 'Z'
		case "Q":
			letter = 'Q'
		case "X":
			rep.Warnings = append(rep.Warnings,
				fmt.Sprintf("X-line (gecos ban) for mask %q dropped: UnrealIRCd has no native gecos ban; "+
					"create a spamfilter manually if needed", ban.Mask))
			continue
		default:
			rep.Warnings = append(rep.Warnings, fmt.Sprintf("unknown ban type %q (mask %q) skipped", ban.Type, ban.Mask))
			continue
		}

		setBy := ban.SetBy
		if setBy == "" {
			setBy = "imported"
		}
		setAt := unixOrZeroSigned(ban.SetAt)
		expireAt := unixOrZeroSigned(ban.ExpiresAt)

		entry := encodedTKL{
			letter:   letter,
			setBy:    setBy,
			setAt:    setAt,
			expireAt: expireAt,
		}

		if letter == 'Q' {
			// Q-line: hold flag + name + reason. Atheme/Anope nick
			// quarantines are persistent ("H").
			entry.isNameBan = true
			entry.nameHold = "H"
			entry.nameMask = ban.Mask
			entry.reason = ban.Reason
		} else {
			// Server ban: split mask on '@' into user / host.
			user, host := splitMask(ban.Mask)
			entry.usermask = user
			entry.hostmask = host
			entry.reason = ban.Reason
		}
		encoded = append(encoded, entry)
	}

	rep.Bans = len(encoded)

	if opts.DryRun {
		return nil
	}

	var buf bytes.Buffer
	if err := writeUint32LE(&buf, tkldbMagic); err != nil {
		return err
	}
	if err := writeUint32LE(&buf, tkldbVersion); err != nil {
		return err
	}
	if err := writeUint64LE(&buf, uint64(len(encoded))); err != nil {
		return err
	}
	for _, e := range encoded {
		if err := buf.WriteByte(e.letter); err != nil {
			return err
		}
		if err := writeStr(&buf, e.setBy); err != nil {
			return err
		}
		if err := writeInt64LE(&buf, e.setAt); err != nil {
			return err
		}
		if err := writeInt64LE(&buf, e.expireAt); err != nil {
			return err
		}
		if e.isNameBan {
			if err := writeStr(&buf, e.nameHold); err != nil {
				return err
			}
			if err := writeStr(&buf, e.nameMask); err != nil {
				return err
			}
			if err := writeStr(&buf, e.reason); err != nil {
				return err
			}
		} else {
			if err := writeStr(&buf, e.usermask); err != nil {
				return err
			}
			if err := writeStr(&buf, e.hostmask); err != nil {
				return err
			}
			if err := writeStr(&buf, e.reason); err != nil {
				return err
			}
		}
	}
	tmp := path + ".new"
	if err := os.WriteFile(tmp, buf.Bytes(), 0o600); err != nil {
		return fmt.Errorf("write %s: %w", tmp, err)
	}
	if err := os.Rename(tmp, path); err != nil {
		return fmt.Errorf("rename %s -> %s: %w", tmp, path, err)
	}
	return nil
}

// splitMask splits a "user@host" or "user@host/cidr" mask into
// (user, host). If there's no '@', the whole thing becomes the host
// and user defaults to "*".
func splitMask(mask string) (user, host string) {
	at := strings.IndexByte(mask, '@')
	if at < 0 {
		return "*", mask
	}
	return mask[:at], mask[at+1:]
}
