package writer

import (
	"bytes"
	"encoding/binary"
	"os"
	"path/filepath"
	"testing"
	"time"

	"github.com/obsidianirc/obbyircd-migrate/internal/ir"
)

func TestWriteTKLDB(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "tkldb.db")
	now := time.Date(2026, 5, 1, 0, 0, 0, 0, time.UTC)
	bundle := &ir.Bundle{
		Bans: []ir.Ban{
			{Type: "K", Mask: "*bot@evil.example", Reason: "spam", SetBy: "Alice", SetAt: now},
			{Type: "Z", Mask: "*@1.2.3.0/24", Reason: "abuse", SetBy: "Alice", SetAt: now},
			{Type: "Q", Mask: "evilbot", Reason: "name reserved", SetBy: "Alice", SetAt: now},
			{Type: "X", Mask: "*evil corp*", Reason: "gecos ban — should drop", SetBy: "Alice", SetAt: now},
		},
	}
	rep := &Report{}
	if err := WriteTKLDB(path, bundle, Options{}, rep); err != nil {
		t.Fatalf("WriteTKLDB: %v", err)
	}
	// Three bans encoded (X dropped with warning).
	if rep.Bans != 3 {
		t.Errorf("rep.Bans = %d, want 3", rep.Bans)
	}
	wantWarn := false
	for _, w := range rep.Warnings {
		if bytes.Contains([]byte(w), []byte("X-line")) {
			wantWarn = true
		}
	}
	if !wantWarn {
		t.Errorf("expected an X-line drop warning, got %v", rep.Warnings)
	}

	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	if len(data) < 16 {
		t.Fatalf("tkldb too short: %d bytes", len(data))
	}
	magic := binary.LittleEndian.Uint32(data[0:4])
	version := binary.LittleEndian.Uint32(data[4:8])
	count := binary.LittleEndian.Uint64(data[8:16])
	if magic != tkldbMagic {
		t.Errorf("magic = %x, want %x", magic, tkldbMagic)
	}
	if version != tkldbVersion {
		t.Errorf("version = %d, want %d", version, tkldbVersion)
	}
	if count != 3 {
		t.Errorf("count = %d, want 3", count)
	}

	// Check the first byte of each TKL is the expected letter:
	// header 16 bytes, first record starts at 16 with the letter byte.
	if data[16] != 'G' {
		t.Errorf("first ban letter = %c, want G", data[16])
	}

	// Check the body contains expected strings.
	for _, s := range []string{"*bot", "evil.example", "spam", "1.2.3.0/24", "evilbot"} {
		if !bytes.Contains(data, []byte(s)) {
			t.Errorf("expected %q in tkldb body", s)
		}
	}
	if bytes.Contains(data, []byte("evil corp")) {
		t.Error("X-line gecos string should NOT be in tkldb body")
	}
}
