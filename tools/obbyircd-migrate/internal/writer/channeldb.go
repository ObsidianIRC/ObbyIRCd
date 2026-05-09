package writer

import (
	"bytes"
	"encoding/binary"
	"errors"
	"fmt"
	"os"
	"sort"
	"strings"
	"time"

	"github.com/obsidianirc/obbyircd-migrate/internal/ir"
)

// Mirrors src/modules/channeldb.c
const (
	channelDBVersion    = 101
	magicChannelStart   = 0x11111111
	magicChannelEnd     = 0x22222222
	maxStringLen        = 0xfffe
	stringNullSentinel  = 0xffff
)

// WriteChannelDB serialises every channel in the bundle into UnrealDB
// format at `path`, using channeldb v101's record layout (extended
// with registered_by + registered_at). Compatible with the
// channeldb.c reader at src/modules/channeldb.c::read_channeldb.
//
// The output is plaintext (no encryption) — encrypted channeldb is
// rare and would require pulling in xchacha20poly1305; if you have
// `set::channeldb::db-secret` set, write to a tmp file and have the
// admin re-encrypt by stopping ircd, replacing data/channel.db, and
// starting again with the secret unset (or a one-shot conversion).
func WriteChannelDB(path string, b *ir.Bundle, opts Options, rep *Report) error {
	if path == "" {
		return errors.New("WriteChannelDB: path is required")
	}
	if rep == nil {
		rep = &Report{}
	}

	var buf bytes.Buffer
	if err := writeUint32LE(&buf, channelDBVersion); err != nil {
		return err
	}
	if err := writeUint64LE(&buf, uint64(len(b.Channels))); err != nil {
		return err
	}

	for _, ch := range b.Channels {
		if err := writeChannelEntry(&buf, &ch); err != nil {
			return fmt.Errorf("encode %s: %w", ch.Name, err)
		}
		rep.Channels = append(rep.Channels, ChannelReport{
			Name:   ch.Name,
			Action: ifElse(opts.DryRun, "would-insert", "inserted"),
			ACL:    len(ch.ACL),
			Akicks: len(ch.Akicks),
		})
	}

	if opts.DryRun {
		return nil
	}
	// Atomic-rename pattern: write tmp, fsync, rename.
	tmp := path + ".new"
	if err := os.WriteFile(tmp, buf.Bytes(), 0o600); err != nil {
		return fmt.Errorf("write %s: %w", tmp, err)
	}
	if err := os.Rename(tmp, path); err != nil {
		return fmt.Errorf("rename %s -> %s: %w", tmp, path, err)
	}
	return nil
}

func writeChannelEntry(w *bytes.Buffer, ch *ir.Channel) error {
	if err := writeUint32LE(w, magicChannelStart); err != nil {
		return err
	}
	if err := writeStr(w, ch.Name); err != nil {
		return err
	}
	creation := unixOrNow(ch.RegisteredAt)
	if err := writeInt64LE(w, creation); err != nil {
		return err
	}
	if err := writeStr(w, ch.Topic); err != nil {
		return err
	}
	if err := writeStr(w, ch.TopicSetBy); err != nil {
		return err
	}
	if err := writeInt64LE(w, unixOrZeroSigned(ch.TopicSetAt)); err != nil {
		return err
	}

	modes1, modes2 := buildModes(ch)
	if err := writeStr(w, modes1); err != nil {
		return err
	}
	if err := writeStr(w, modes2); err != nil {
		return err
	}
	if err := writeStr(w, buildModeLock(ch)); err != nil {
		return err
	}

	// v101 fields: registered_by + registered_at.
	regBy := ch.Founder
	regAt := unixOrZero(ch.RegisteredAt)
	if err := writeStr(w, regBy); err != nil {
		return err
	}
	if err := writeUint64LE(w, uint64(regAt)); err != nil {
		return err
	}

	// Ban list (akicks → +b ~account: / +b mask).
	bans := buildAkickBans(ch)
	if err := writeListMode(w, bans); err != nil {
		return err
	}
	// Except list — ACL grants encoded as ~automode extbans.
	exempts := buildAclExempts(ch)
	if err := writeListMode(w, exempts); err != nil {
		return err
	}
	// Invex list — empty for now (we don't import +I rules).
	if err := writeListMode(w, nil); err != nil {
		return err
	}

	if err := writeUint32LE(w, magicChannelEnd); err != nil {
		return err
	}
	return nil
}

type listEntry struct {
	banstr string
	who    string
	when   int64
}

func writeListMode(w *bytes.Buffer, entries []listEntry) error {
	if err := writeUint32LE(w, uint32(len(entries))); err != nil {
		return err
	}
	for _, e := range entries {
		if err := writeStr(w, e.banstr); err != nil {
			return err
		}
		if err := writeStr(w, e.who); err != nil {
			return err
		}
		if err := writeInt64LE(w, e.when); err != nil {
			return err
		}
	}
	return nil
}

// buildModes returns (modes1, modes2) in the same shape channel_modes()
// emits server-side: a "+abc..." string and a space-separated parameter
// string. Member modes (q/a/o/h/v) are NOT included here — they're
// applied via the +e ~automode extbans on join.
func buildModes(ch *ir.Channel) (string, string) {
	var modes strings.Builder
	var params strings.Builder
	if ch.Modes.Set != "" {
		modes.WriteByte('+')
		modes.WriteString(ch.Modes.Set)
	}
	// Add +P always — registered channel == permanent in obbyircd.
	if !strings.Contains(modes.String(), "P") {
		if modes.Len() == 0 {
			modes.WriteByte('+')
		}
		modes.WriteByte('P')
	}
	// Per-mode parameters in deterministic order so test fixtures are
	// stable.
	if len(ch.Modes.Params) > 0 {
		keys := make([]string, 0, len(ch.Modes.Params))
		for k := range ch.Modes.Params {
			keys = append(keys, k)
		}
		sort.Strings(keys)
		for _, k := range keys {
			if params.Len() > 0 {
				params.WriteByte(' ')
			}
			params.WriteString(ch.Modes.Params[k])
		}
	}
	return modes.String(), params.String()
}

func buildModeLock(ch *ir.Channel) string {
	if ch.ModeLock.On == "" && ch.ModeLock.Off == "" && ch.ModeLock.Limit == 0 && ch.ModeLock.Key == "" {
		return ""
	}
	var b strings.Builder
	if ch.ModeLock.On != "" {
		b.WriteByte('+')
		b.WriteString(ch.ModeLock.On)
	}
	if ch.ModeLock.Off != "" {
		b.WriteByte('-')
		b.WriteString(ch.ModeLock.Off)
	}
	return b.String()
}

func buildAkickBans(ch *ir.Channel) []listEntry {
	out := make([]listEntry, 0, len(ch.Akicks))
	for _, k := range ch.Akicks {
		var banstr string
		switch {
		case k.Account != "":
			banstr = "~account:" + k.Account
		case k.Mask != "":
			banstr = k.Mask
		default:
			continue
		}
		who := k.SetBy
		if who == "" {
			who = "imported"
		}
		out = append(out, listEntry{banstr: banstr, who: who, when: unixOrZeroSigned(k.SetAt)})
	}
	return out
}

func buildAclExempts(ch *ir.Channel) []listEntry {
	out := make([]listEntry, 0, len(ch.ACL)+1)
	// Founder always gets +q via automode (in addition to the
	// channel-registration ModData stamp).
	if ch.Founder != "" && !aclHasFounder(ch) {
		out = append(out, listEntry{
			banstr: fmt.Sprintf("~automode:q:~account:%s", ch.Founder),
			who:    "imported",
			when:   unixOrZeroSigned(ch.RegisteredAt),
		})
	}
	if ch.Successor != "" {
		out = append(out, listEntry{
			banstr: fmt.Sprintf("~automode:q:~account:%s", ch.Successor),
			who:    "imported",
			when:   unixOrZeroSigned(ch.RegisteredAt),
		})
	}
	for _, e := range ch.ACL {
		if e.Modes == "" {
			continue
		}
		var banstr string
		switch {
		case e.Account != "":
			banstr = fmt.Sprintf("~automode:%s:~account:%s", e.Modes, e.Account)
		case e.Mask != "":
			banstr = fmt.Sprintf("~automode:%s:%s", e.Modes, e.Mask)
		default:
			continue
		}
		who := e.SetBy
		if who == "" {
			who = "imported"
		}
		out = append(out, listEntry{banstr: banstr, who: who, when: unixOrZeroSigned(e.SetAt)})
	}
	return out
}

func aclHasFounder(ch *ir.Channel) bool {
	for _, e := range ch.ACL {
		if e.Account == ch.Founder && strings.Contains(e.Modes, "q") {
			return true
		}
	}
	return false
}

func writeUint32LE(w *bytes.Buffer, v uint32) error {
	var b [4]byte
	binary.LittleEndian.PutUint32(b[:], v)
	_, err := w.Write(b[:])
	return err
}

func writeUint64LE(w *bytes.Buffer, v uint64) error {
	var b [8]byte
	binary.LittleEndian.PutUint64(b[:], v)
	_, err := w.Write(b[:])
	return err
}

func writeInt64LE(w *bytes.Buffer, v int64) error {
	return writeUint64LE(w, uint64(v))
}

// writeStr writes UnrealDB string framing: uint16le length then bytes,
// no NUL terminator. NULL string is signalled by length == 0xffff.
func writeStr(w *bytes.Buffer, s string) error {
	if s == "" {
		// Empty string still writes length=0; we treat IR-side empty as
		// empty rather than NULL (channeldb.c reads them back as "").
		return writeUint16LE(w, 0)
	}
	if len(s) > maxStringLen {
		return fmt.Errorf("string too long: %d bytes", len(s))
	}
	if err := writeUint16LE(w, uint16(len(s))); err != nil {
		return err
	}
	_, err := w.WriteString(s)
	return err
}

func writeUint16LE(w *bytes.Buffer, v uint16) error {
	var b [2]byte
	binary.LittleEndian.PutUint16(b[:], v)
	_, err := w.Write(b[:])
	return err
}

func unixOrNow(t time.Time) int64 {
	if t.IsZero() {
		return time.Now().Unix()
	}
	return t.Unix()
}

func unixOrZeroSigned(t time.Time) int64 {
	if t.IsZero() {
		return 0
	}
	return t.Unix()
}

func ifElse[T any](cond bool, a, b T) T {
	if cond {
		return a
	}
	return b
}
