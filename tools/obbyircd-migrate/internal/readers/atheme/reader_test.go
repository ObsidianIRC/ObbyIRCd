package atheme

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

const fixture = `GRVER 1
DBV 12
TS 1620000000
MU 1 Alice $2a$10$abcdefghijklmnopqrstuv1234567890ABCDEFGHIJ alice@example.com 1620000000 1620500000 * en_US
MU 2 Bob $z$pbkdf2-sha512$10000$saltsalt$hashhash bob@example.com 1620100000 1620600000 * en_US
MN Alice AliceAlt 1620100000 1620200000
MCFP Alice 0123456789abcdef0123456789abcdef01234567
AC Alice *!*@trusted.host
MDU Alice private:usercloak alice.users.example.net
MC #staff 1620000000 1620500000 * 0 0 0 *
CA #staff Alice F 1620000000 Alice
CA #staff Bob vV 1620100000 Alice
CA #staff *!*@helper.example oh 1620200000 Alice
KL 1 *bot @badhost.example 0 1620300000 Alice spambot please go away
XL 1 *evil corp* 0 1620400000 Alice gecos ban
QL 1 evilbot 0 1620500000 Alice nick reserved for badness
ME Alice Bob 1620600000 0 hello there friend
SO Alice netadmin *
`

func TestReadAthemeFixture(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "services.db")
	if err := os.WriteFile(path, []byte(fixture), 0o600); err != nil {
		t.Fatal(err)
	}
	b, err := Read(path)
	if err != nil {
		t.Fatalf("Read: %v", err)
	}

	if len(b.Accounts) != 2 {
		t.Errorf("expected 2 accounts, got %d", len(b.Accounts))
	}
	var alice, bob *struct {
		PwdScheme string
		Aliases   []string
		Certfps   []string
		Vhost     string
	}
	for _, a := range b.Accounts {
		switch strings.ToLower(a.Name) {
		case "alice":
			alice = &struct {
				PwdScheme string
				Aliases   []string
				Certfps   []string
				Vhost     string
			}{a.Password.Scheme, a.Aliases, a.Certfps, a.Vhost}
		case "bob":
			bob = &struct {
				PwdScheme string
				Aliases   []string
				Certfps   []string
				Vhost     string
			}{a.Password.Scheme, a.Aliases, a.Certfps, a.Vhost}
		}
	}
	if alice == nil || bob == nil {
		t.Fatal("missing accounts")
	}
	if alice.PwdScheme != "bcrypt" {
		t.Errorf("alice scheme = %q, want bcrypt", alice.PwdScheme)
	}
	if bob.PwdScheme != "pbkdf2v2" {
		t.Errorf("bob scheme = %q, want pbkdf2v2", bob.PwdScheme)
	}
	if len(alice.Aliases) != 1 || alice.Aliases[0] != "AliceAlt" {
		t.Errorf("alice aliases = %v, want [AliceAlt]", alice.Aliases)
	}
	if len(alice.Certfps) != 1 {
		t.Errorf("alice certfps = %v", alice.Certfps)
	}
	if alice.Vhost != "alice.users.example.net" {
		t.Errorf("alice vhost = %q", alice.Vhost)
	}

	if len(b.Channels) != 1 {
		t.Fatalf("expected 1 channel, got %d", len(b.Channels))
	}
	ch := b.Channels[0]
	if ch.Name != "#staff" {
		t.Errorf("channel name = %q", ch.Name)
	}
	if !strings.EqualFold(ch.Founder, "Alice") {
		t.Errorf("founder = %q, want Alice", ch.Founder)
	}
	if len(ch.ACL) != 3 {
		t.Errorf("ACL count = %d (want 3): %+v", len(ch.ACL), ch.ACL)
	}
	// Find each row and check encoding.
	saw := map[string]string{}
	for _, e := range ch.ACL {
		key := e.Account
		if key == "" {
			key = e.Mask
		}
		saw[key] = e.Modes
	}
	if saw["Alice"] != "q" {
		t.Errorf("Alice ACL = %q, want q", saw["Alice"])
	}
	if saw["Bob"] != "v" {
		t.Errorf("Bob ACL = %q, want v", saw["Bob"])
	}
	if saw["*!*@helper.example"] != "oh" {
		t.Errorf("hostmask ACL = %q, want oh", saw["*!*@helper.example"])
	}

	if len(b.Bans) != 3 {
		t.Errorf("expected 3 bans (KL/XL/QL), got %d: %+v", len(b.Bans), b.Bans)
	}
	if len(b.Memos) != 1 {
		t.Errorf("expected 1 memo, got %d", len(b.Memos))
	}
	if len(b.Opers) != 1 {
		t.Errorf("expected 1 oper, got %d", len(b.Opers))
	}
}
