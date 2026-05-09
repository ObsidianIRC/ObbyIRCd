package writer

import (
	"bytes"
	"database/sql"
	"encoding/binary"
	"os"
	"path/filepath"
	"testing"
	"time"

	_ "modernc.org/sqlite"

	"github.com/obsidianirc/obbyircd-migrate/internal/ir"
)

// makeObsidianSchema creates an obsidian.db with the v0 schema
// (matches src/modules/account-registration.c at HEAD). The writer
// should successfully insert accounts using only the always-present
// columns; Phase 0 columns are omitted on purpose.
func makeObsidianSchema(t *testing.T, path string) {
	t.Helper()
	db, err := sql.Open("sqlite", path)
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()
	_, err = db.Exec(`CREATE TABLE accounts (
		id INTEGER PRIMARY KEY AUTOINCREMENT,
		name TEXT NOT NULL COLLATE NOCASE,
		email TEXT,
		password TEXT,
		time_registered INTEGER,
		verified INTEGER DEFAULT 0,
		verify_code TEXT,
		verify_expires INTEGER,
		scram_salt TEXT,
		scram_iterations INTEGER,
		scram_stored_key TEXT,
		scram_server_key TEXT,
		twofa_enabled INTEGER DEFAULT 0
	)`)
	if err != nil {
		t.Fatal(err)
	}
}

func TestWriteObsidianMemos(t *testing.T) {
	dir := t.TempDir()
	dbPath := filepath.Join(dir, "obsidian.db")
	db, err := sql.Open("sqlite", dbPath)
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()
	_, err = db.Exec(`
		CREATE TABLE accounts (id INTEGER PRIMARY KEY AUTOINCREMENT,
			name TEXT NOT NULL COLLATE NOCASE, password TEXT,
			time_registered INTEGER, verified INTEGER DEFAULT 0);
		CREATE TABLE memos (id INTEGER PRIMARY KEY AUTOINCREMENT,
			recipient_id INTEGER NOT NULL, sender TEXT NOT NULL,
			body TEXT NOT NULL, sent_at INTEGER NOT NULL,
			read_at INTEGER DEFAULT 0);
	`)
	if err != nil {
		t.Fatal(err)
	}

	now := time.Date(2026, 5, 1, 0, 0, 0, 0, time.UTC)
	bundle := &ir.Bundle{
		Accounts: []ir.Account{{
			Name: "Alice", Verified: true,
			Password: ir.Password{Scheme: "bcrypt", Value: "$2a$10$x"},
		}},
		Memos: []ir.Memo{
			{Recipient: "Alice", Sender: "Bob", Body: "hi alice", SentAt: now},
			{Recipient: "Charlie", Sender: "Bob", Body: "lost", SentAt: now}, // unknown recipient → skipped
		},
	}
	rep := &Report{}
	if err := WriteObsidian(dbPath, bundle, Options{}, rep); err != nil {
		t.Fatalf("WriteObsidian: %v", err)
	}
	var rows int
	_ = db.QueryRow(`SELECT COUNT(*) FROM memos`).Scan(&rows)
	if rows != 1 {
		t.Errorf("memos written = %d, want 1", rows)
	}
	var sender, body string
	_ = db.QueryRow(`SELECT sender, body FROM memos`).Scan(&sender, &body)
	if sender != "Bob" || body != "hi alice" {
		t.Errorf("memo row = (%s, %q)", sender, body)
	}
	gotWarn := false
	for _, w := range rep.Warnings {
		if bytes.Contains([]byte(w), []byte("Charlie")) {
			gotWarn = true
		}
	}
	if !gotWarn {
		t.Errorf("expected warning about unknown recipient, got %v", rep.Warnings)
	}
}

func TestWriteObsidianHappyPath(t *testing.T) {
	dir := t.TempDir()
	dbPath := filepath.Join(dir, "obsidian.db")
	makeObsidianSchema(t, dbPath)

	now := time.Date(2026, 5, 1, 0, 0, 0, 0, time.UTC)
	bundle := &ir.Bundle{
		Version: ir.Version,
		Source:  "atheme",
		Accounts: []ir.Account{
			{
				Name:         "Alice",
				CaseFolded:   "alice",
				Email:        "alice@example.com",
				Password:     ir.Password{Scheme: "bcrypt", Value: "$2a$10$abcdef"},
				RegisteredAt: now,
				Verified:     true,
				SCRAM: &ir.SCRAM{
					Salt:       "c2FsdA==",
					Iterations: 4096,
					StoredKey:  "c3RvcmVk",
					ServerKey:  "c2VydmVy",
				},
			},
		},
	}
	rep := &Report{}
	if err := WriteObsidian(dbPath, bundle, Options{}, rep); err != nil {
		t.Fatalf("WriteObsidian: %v", err)
	}
	if len(rep.Accounts) != 1 || rep.Accounts[0].Action != "inserted" {
		t.Errorf("report = %+v", rep.Accounts)
	}

	// Confirm the row landed.
	db, err := sql.Open("sqlite", dbPath)
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()
	var name, password string
	var iters int
	if err := db.QueryRow(`SELECT name, password, COALESCE(scram_iterations, 0) FROM accounts WHERE lower(name) = 'alice'`).Scan(&name, &password, &iters); err != nil {
		t.Fatalf("query: %v", err)
	}
	if name != "Alice" {
		t.Errorf("name = %q", name)
	}
	if password != "$2a$10$abcdef" {
		t.Errorf("password = %q", password)
	}
	if iters != 4096 {
		t.Errorf("scram_iterations = %d, want 4096", iters)
	}

	// Re-running with the same bundle on a populated DB should skip.
	rep2 := &Report{}
	if err := WriteObsidian(dbPath, bundle, Options{}, rep2); err != nil {
		t.Fatalf("re-run: %v", err)
	}
	if rep2.Accounts[0].Action != "skipped" {
		t.Errorf("re-run action = %q, want skipped", rep2.Accounts[0].Action)
	}
}

func TestWriteObsidianDryRun(t *testing.T) {
	dir := t.TempDir()
	dbPath := filepath.Join(dir, "obsidian.db")
	makeObsidianSchema(t, dbPath)

	bundle := &ir.Bundle{
		Accounts: []ir.Account{{
			Name:     "Alice",
			Password: ir.Password{Scheme: "bcrypt", Value: "$2a$10$xyz"},
			Verified: true,
		}},
	}
	rep := &Report{}
	if err := WriteObsidian(dbPath, bundle, Options{DryRun: true}, rep); err != nil {
		t.Fatalf("dry-run: %v", err)
	}
	if rep.Accounts[0].Action != "would-insert" {
		t.Errorf("dry-run action = %q", rep.Accounts[0].Action)
	}
	// Confirm no row was created.
	db, _ := sql.Open("sqlite", dbPath)
	defer db.Close()
	var count int
	_ = db.QueryRow(`SELECT COUNT(*) FROM accounts`).Scan(&count)
	if count != 0 {
		t.Errorf("dry-run wrote %d rows", count)
	}
}

func TestWriteChannelDBRoundTrip(t *testing.T) {
	dir := t.TempDir()
	dbPath := filepath.Join(dir, "channel.db")

	now := time.Date(2026, 5, 1, 0, 0, 0, 0, time.UTC)
	bundle := &ir.Bundle{
		Version: ir.Version,
		Source:  "atheme",
		Channels: []ir.Channel{{
			Name:         "#staff",
			RegisteredAt: now,
			Founder:      "Alice",
			Topic:        "Welcome to staff",
			TopicSetBy:   "Alice",
			TopicSetAt:   now,
			Modes:        ir.ChannelModes{Set: "nt"},
			ACL: []ir.ACLEntry{
				{Account: "Bob", Modes: "o"},
				{Mask: "*!*@helper.example", Modes: "h"},
			},
			Akicks: []ir.Akick{{
				Account: "Eve",
				Reason:  "abuse",
				SetBy:   "Alice",
				SetAt:   now,
			}},
		}},
	}

	rep := &Report{}
	if err := WriteChannelDB(dbPath, bundle, Options{}, rep); err != nil {
		t.Fatalf("WriteChannelDB: %v", err)
	}
	if len(rep.Channels) != 1 || rep.Channels[0].Action != "inserted" {
		t.Errorf("report = %+v", rep.Channels)
	}

	// Read back the binary and verify version/count and the
	// per-channel header fields.
	data, err := os.ReadFile(dbPath)
	if err != nil {
		t.Fatalf("read db: %v", err)
	}
	if len(data) < 12 {
		t.Fatalf("file too short: %d bytes", len(data))
	}
	version := binary.LittleEndian.Uint32(data[0:4])
	count := binary.LittleEndian.Uint64(data[4:12])
	if version != channelDBVersion {
		t.Errorf("version = %d, want %d", version, channelDBVersion)
	}
	if count != 1 {
		t.Errorf("count = %d, want 1", count)
	}

	// Magic start should appear right after the count.
	magic := binary.LittleEndian.Uint32(data[12:16])
	if magic != magicChannelStart {
		t.Errorf("magic = %x, want %x", magic, magicChannelStart)
	}

	// The output should contain Alice's automode extban for the
	// founder grant; spot-check that the encoded extban string is
	// in the file body.
	wantExtban := "~automode:q:~account:Alice"
	if !bytes.Contains(data, []byte(wantExtban)) {
		t.Errorf("expected %q in channel.db body", wantExtban)
	}
	wantOpExtban := "~automode:o:~account:Bob"
	if !bytes.Contains(data, []byte(wantOpExtban)) {
		t.Errorf("expected %q in channel.db body", wantOpExtban)
	}
	wantAkickBan := "~account:Eve"
	if !bytes.Contains(data, []byte(wantAkickBan)) {
		t.Errorf("expected %q in channel.db body", wantAkickBan)
	}

	// Round-trip parse with our channeldb-format-mirror parser and
	// verify the structured output.
	version2, channels, err := ParseChannelDB(data)
	if err != nil {
		t.Fatalf("ParseChannelDB: %v", err)
	}
	if version2 != channelDBVersion {
		t.Errorf("parsed version = %d", version2)
	}
	if len(channels) != 1 {
		t.Fatalf("parsed %d channels, want 1", len(channels))
	}
	pc := channels[0]
	if pc.Name != "#staff" {
		t.Errorf("parsed name = %q", pc.Name)
	}
	if pc.RegisteredBy != "Alice" {
		t.Errorf("parsed registered_by = %q", pc.RegisteredBy)
	}
	if pc.RegisteredAt != now.Unix() {
		t.Errorf("parsed registered_at = %d", pc.RegisteredAt)
	}
	// Check the +e (excepts) list has the founder + ACL grants.
	wantBans := map[string]bool{
		"~automode:q:~account:Alice":           false,
		"~automode:o:~account:Bob":             false,
		"~automode:h:*!*@helper.example":       false,
	}
	for _, e := range pc.Excepts {
		if _, ok := wantBans[e.BanStr]; ok {
			wantBans[e.BanStr] = true
		}
	}
	for k, found := range wantBans {
		if !found {
			t.Errorf("expected except %q to be parsed", k)
		}
	}
	// Banlist (akicks) should have ~account:Eve.
	if len(pc.Banlist) != 1 || pc.Banlist[0].BanStr != "~account:Eve" {
		t.Errorf("banlist = %+v", pc.Banlist)
	}
	// Modes should be "+ntP" (we always force +P).
	if pc.Modes1 == "" || pc.Modes1[len(pc.Modes1)-1] != 'P' {
		t.Errorf("modes1 = %q (must end with P)", pc.Modes1)
	}
}
