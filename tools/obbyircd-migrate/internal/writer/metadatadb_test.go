package writer

import (
	"encoding/binary"
	"io"
	"os"
	"path/filepath"
	"testing"
	"time"

	"github.com/obsidianirc/obbyircd-migrate/internal/ir"
)

func TestWriteMetadataDB_RoundTrip(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "metadata.db")

	b := &ir.Bundle{
		Accounts: []ir.Account{
			{Name: "alice", Metadata: map[string]string{
				"avatar":   "https://cdn.example/a.png",
				"realname": "Alice Liddell",
			}},
			{Name: "bob", Metadata: map[string]string{
				"display-name": "Bobby",
			}},
		},
		Channels: []ir.Channel{
			{Name: "#general", Metadata: map[string]string{
				"description": "main channel",
			}},
		},
	}
	rep := &Report{}
	if err := WriteMetadataDB(path, b, Options{}, rep); err != nil {
		t.Fatalf("write: %v", err)
	}

	fd, err := os.Open(path)
	if err != nil {
		t.Fatalf("open: %v", err)
	}
	defer fd.Close()

	var version uint32
	if err := binary.Read(fd, binary.LittleEndian, &version); err != nil {
		t.Fatalf("version: %v", err)
	}
	if version != metadataDBVersion {
		t.Fatalf("version = %d, want %d", version, metadataDBVersion)
	}
	var count uint64
	if err := binary.Read(fd, binary.LittleEndian, &count); err != nil {
		t.Fatalf("count: %v", err)
	}
	if count != 4 {
		t.Fatalf("count = %d, want 4", count)
	}

	type entry struct {
		owner    string
		lastSeen int64
		key      string
		value    string
	}
	var got []entry
	for i := uint64(0); i < count; i++ {
		var magic uint32
		if err := binary.Read(fd, binary.LittleEndian, &magic); err != nil {
			t.Fatalf("magic[%d]: %v", i, err)
		}
		if magic != magicEntryStart {
			t.Fatalf("entry %d: bad start magic %#x", i, magic)
		}
		owner := readUnrealStr(t, fd)
		var ls int64
		if err := binary.Read(fd, binary.LittleEndian, &ls); err != nil {
			t.Fatalf("last_seen[%d]: %v", i, err)
		}
		key := readUnrealStr(t, fd)
		value := readUnrealStr(t, fd)
		var end uint32
		if err := binary.Read(fd, binary.LittleEndian, &end); err != nil {
			t.Fatalf("end[%d]: %v", i, err)
		}
		if end != magicEntryEnd {
			t.Fatalf("entry %d: bad end magic %#x", i, end)
		}
		got = append(got, entry{owner, ls, key, value})
	}

	if rest, _ := io.ReadAll(fd); len(rest) != 0 {
		t.Fatalf("trailing bytes: %d", len(rest))
	}

	want := map[string]entry{
		"alice|avatar":         {"alice", -1, "avatar", "https://cdn.example/a.png"},
		"alice|realname":       {"alice", -1, "realname", "Alice Liddell"},
		"bob|display-name":     {"bob", -1, "display-name", "Bobby"},
		"#general|description": {"#general", 0, "description", "main channel"},
	}
	if len(got) != len(want) {
		t.Fatalf("got %d entries, want %d", len(got), len(want))
	}
	now := time.Now().Unix()
	for _, e := range got {
		exp, ok := want[e.owner+"|"+e.key]
		if !ok {
			t.Fatalf("unexpected entry: %+v", e)
		}
		if e.value != exp.value {
			t.Fatalf("value: got %q, want %q", e.value, exp.value)
		}
		if exp.lastSeen == 0 {
			if e.lastSeen != 0 {
				t.Fatalf("channel last_seen: got %d, want 0", e.lastSeen)
			}
		} else {
			if e.lastSeen < now-5 || e.lastSeen > now+5 {
				t.Fatalf("account last_seen out of window: %d (now=%d)", e.lastSeen, now)
			}
		}
	}
}

func TestWriteMetadataDB_EmptyBundle(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "metadata.db")
	rep := &Report{}
	if err := WriteMetadataDB(path, &ir.Bundle{}, Options{}, rep); err != nil {
		t.Fatalf("write: %v", err)
	}
	if _, err := os.Stat(path); !os.IsNotExist(err) {
		t.Fatalf("empty bundle should not create file; got err=%v", err)
	}
	if rep.Metadata != 0 {
		t.Fatalf("rep.Metadata = %d, want 0", rep.Metadata)
	}
}

func TestWriteMetadataDB_DryRun(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "metadata.db")
	b := &ir.Bundle{
		Accounts: []ir.Account{
			{Name: "alice", Metadata: map[string]string{"k": "v"}},
		},
	}
	rep := &Report{}
	if err := WriteMetadataDB(path, b, Options{DryRun: true}, rep); err != nil {
		t.Fatalf("write: %v", err)
	}
	if _, err := os.Stat(path); !os.IsNotExist(err) {
		t.Fatalf("dry-run should not create file; got err=%v", err)
	}
	if rep.Metadata != 1 {
		t.Fatalf("rep.Metadata = %d, want 1", rep.Metadata)
	}
}

func TestWriteMetadataDB_FilePerms(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "metadata.db")
	b := &ir.Bundle{
		Accounts: []ir.Account{{Name: "alice", Metadata: map[string]string{"k": "v"}}},
	}
	if err := WriteMetadataDB(path, b, Options{}, &Report{}); err != nil {
		t.Fatalf("write: %v", err)
	}
	info, err := os.Stat(path)
	if err != nil {
		t.Fatalf("stat: %v", err)
	}
	if perm := info.Mode().Perm(); perm != 0o600 {
		t.Fatalf("mode = %o, want 0o600", perm)
	}
}

func readUnrealStr(t *testing.T, r io.Reader) string {
	t.Helper()
	var n uint16
	if err := binary.Read(r, binary.LittleEndian, &n); err != nil {
		t.Fatalf("strlen: %v", err)
	}
	if n == 0xffff || n == 0 {
		return ""
	}
	buf := make([]byte, n)
	if _, err := io.ReadFull(r, buf); err != nil {
		t.Fatalf("str: %v", err)
	}
	return string(buf)
}
