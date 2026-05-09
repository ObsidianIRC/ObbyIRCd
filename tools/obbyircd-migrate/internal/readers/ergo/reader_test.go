package ergo

import (
	"encoding/json"
	"fmt"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/tidwall/buntdb"
)

// buildErgoFixture writes a small but realistic Ergo ircd.db to disk:
// two accounts (Alice, Bob) and one registered channel (#staff) with
// Alice as founder and Bob with op via AccountToUMode.
func buildErgoFixture(t *testing.T, dir string) string {
	t.Helper()
	path := filepath.Join(dir, "ircd.db")
	db, err := buntdb.Open(path)
	if err != nil {
		t.Fatalf("open buntdb: %v", err)
	}
	now := time.Date(2026, 5, 1, 0, 0, 0, 0, time.UTC)

	mustSet := func(tx *buntdb.Tx, k, v string) {
		if _, _, err := tx.Set(k, v, nil); err != nil {
			t.Fatalf("set %s: %v", k, err)
		}
	}

	// Account credentials: Ergo's bcrypt hash is stored as raw bytes
	// (which JSON-encodes to base64). We construct the JSON manually
	// here so we don't have to import golang.org/x/crypto/bcrypt.
	bcryptHash := "$2a$10$abcdefghijklmnopqrstuv1234567890ABCDEFGHIJ"
	credsAlice := map[string]any{
		"Version":        1,
		"PassphraseHash": []byte(bcryptHash),
		"Certfps":        []string{"0123456789abcdef0123456789abcdef01234567"},
		"Salt":           []byte("salt-alice"),
		"Iters":          4096,
		"StoredKey":      []byte("alice-stored-key"),
		"ServerKey":      []byte("alice-server-key"),
	}
	credsAliceJSON, _ := json.Marshal(credsAlice)

	credsBob := map[string]any{
		"Version":        1,
		"PassphraseHash": []byte(bcryptHash),
	}
	credsBobJSON, _ := json.Marshal(credsBob)

	if err := db.Update(func(tx *buntdb.Tx) error {
		// Alice
		mustSet(tx, "account.exists alice", "1")
		mustSet(tx, "account.verified alice", "1")
		mustSet(tx, "account.name alice", "Alice")
		mustSet(tx, "account.registered.time alice", fmt.Sprintf("%d", now.UnixNano()))
		mustSet(tx, "account.credentials alice", string(credsAliceJSON))
		mustSet(tx, "account.vhost alice", `{"ApprovedVHost":"alice.users.example.net","Enabled":true}`)
		mustSet(tx, "account.additionalnicks alice", `["AliceAlt"]`)
		mustSet(tx, "account.metadata alice", `{"email":"alice@example.com"}`)

		// Bob
		mustSet(tx, "account.exists bob", "1")
		mustSet(tx, "account.verified bob", "1")
		mustSet(tx, "account.name bob", "Bob")
		mustSet(tx, "account.registered.time bob", fmt.Sprintf("%d", now.UnixNano()))
		mustSet(tx, "account.credentials bob", string(credsBobJSON))

		// Channel — TableChannels is table id 1, key is "1 <uuid>".
		// Ergo's modes.Mode is `type Mode rune` so it serialises as
		// an integer in JSON: 110 == 'n', 116 == 't', 111 == 'o'.
		channelJSON := `{
			"Name":"#staff",
			"UUID":"abcdef01-0000-0000-0000-000000000001",
			"RegisteredAt":"2026-05-01T00:00:00Z",
			"Founder":"Alice",
			"Topic":"Welcome",
			"TopicSetBy":"Alice!alice@host",
			"TopicSetTime":"2026-05-01T01:00:00Z",
			"Modes":[110,116],
			"Key":"hunter2",
			"UserLimit":50,
			"AccountToUMode":{"Bob":111},
			"Bans":{"*!*@spam.example":{"TimeCreated":"2026-05-02T00:00:00Z","CreatorAccount":"Alice"}},
			"Excepts":{},
			"Invites":{},
			"Metadata":{"description":"staff channel"}
		}`
		mustSet(tx, "1 abcdef01-0000-0000-0000-000000000001", channelJSON)
		return nil
	}); err != nil {
		t.Fatalf("populate: %v", err)
	}
	if err := db.Close(); err != nil {
		t.Fatalf("close: %v", err)
	}
	return path
}

func TestReadErgoFixture(t *testing.T) {
	dir := t.TempDir()
	path := buildErgoFixture(t, dir)
	b, err := Read(path)
	if err != nil {
		t.Fatalf("Read: %v", err)
	}
	if len(b.Accounts) != 2 {
		t.Errorf("expected 2 accounts, got %d", len(b.Accounts))
	}
	var alice, bob *struct {
		Pwd     string
		Vhost   string
		Aliases []string
		Email   string
		HasSCRAM bool
		Certfps []string
	}
	for _, a := range b.Accounts {
		v := &struct {
			Pwd     string
			Vhost   string
			Aliases []string
			Email   string
			HasSCRAM bool
			Certfps []string
		}{a.Password.Scheme, a.Vhost, a.Aliases, a.Email, a.SCRAM != nil, a.Certfps}
		switch strings.ToLower(a.Name) {
		case "alice":
			alice = v
		case "bob":
			bob = v
		}
	}
	if alice == nil || bob == nil {
		t.Fatal("missing accounts")
	}
	if alice.Pwd != "bcrypt" {
		t.Errorf("alice scheme = %q, want bcrypt", alice.Pwd)
	}
	if alice.Vhost != "alice.users.example.net" {
		t.Errorf("alice vhost = %q", alice.Vhost)
	}
	if len(alice.Aliases) != 1 || alice.Aliases[0] != "AliceAlt" {
		t.Errorf("alice aliases = %v", alice.Aliases)
	}
	if alice.Email != "alice@example.com" {
		t.Errorf("alice email = %q", alice.Email)
	}
	if !alice.HasSCRAM {
		t.Error("alice should have SCRAM creds")
	}
	if len(alice.Certfps) != 1 {
		t.Errorf("alice certfps = %v", alice.Certfps)
	}
	if bob.HasSCRAM {
		t.Error("bob should NOT have SCRAM creds (none set)")
	}

	if len(b.Channels) != 1 {
		t.Fatalf("expected 1 channel, got %d", len(b.Channels))
	}
	ch := b.Channels[0]
	if ch.Name != "#staff" {
		t.Errorf("channel name = %q", ch.Name)
	}
	if ch.Founder != "Alice" {
		t.Errorf("founder = %q", ch.Founder)
	}
	if ch.Modes.Params["k"] != "hunter2" {
		t.Errorf("key = %q", ch.Modes.Params["k"])
	}
	if ch.Modes.Params["l"] != "50" {
		t.Errorf("limit = %q", ch.Modes.Params["l"])
	}
	if len(ch.ACL) != 1 || ch.ACL[0].Account != "Bob" || ch.ACL[0].Modes != "o" {
		t.Errorf("ACL = %+v, want [Bob -> o]", ch.ACL)
	}
	if len(ch.Akicks) != 1 {
		t.Errorf("akicks = %+v", ch.Akicks)
	}
}
