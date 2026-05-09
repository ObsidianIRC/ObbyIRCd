package anope

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

const fixture = `OBJECT NickCore
ID 1
DATA display Alice
DATA email alice@example.com
DATA pass bcrypt:$2a$10$abcdefghijklmnopqrstuv1234567890ABCDEFGHIJ
DATA registered 1620000000
DATA uniqueid 1
DATA language en_US
OBJECT NickCore
ID 2
DATA display BobUnverified
DATA pass plain:hunter2
DATA registered 1700000000
DATA uniqueid 2
OBJECT NickAlias
DATA nick AliceAlt
DATA ncid 1
DATA registered 1620000000
DATA vhost_host alice.users.example.net
DATA vhost_ident alice
DATA vhost_time 1620100000
OBJECT ChannelInfo
DATA name #staff
DATA founderid 1
DATA registered 1620000000
DATA last_topic Welcome to staff
DATA last_topic_setter Alice
DATA last_topic_time 1620500000
DATA uniqueid 100
OBJECT ChanAccess
DATA ci 100
DATA ncid 2
DATA mask BobUnverified
DATA data VOICE
DATA creator Alice
DATA created 1620100000
OBJECT AutoKick
DATA ci 100
DATA mask *!*@spam.example.com
DATA reason persistent spammer
DATA creator Alice
DATA addtime 1620200000
OBJECT XLine
DATA mask *@1.2.3.0/24
DATA reason known abuse
DATA by Alice
DATA created 1620300000
DATA expires 1830300000
`

func TestReadAnopeFixture(t *testing.T) {
	dir := t.TempDir()
	dbPath := filepath.Join(dir, "anope.db")
	if err := os.WriteFile(dbPath, []byte(fixture), 0o600); err != nil {
		t.Fatal(err)
	}

	b, err := Read(dbPath)
	if err != nil {
		t.Fatalf("Read: %v", err)
	}

	if len(b.Accounts) != 2 {
		t.Errorf("expected 2 accounts, got %d", len(b.Accounts))
	}

	var alice *struct {
		Vhost   string
		Aliases []string
		Pwd     string
	}
	for _, a := range b.Accounts {
		if strings.EqualFold(a.Name, "Alice") {
			alice = &struct {
				Vhost   string
				Aliases []string
				Pwd     string
			}{a.Vhost, a.Aliases, a.Password.Scheme}
		}
	}
	if alice == nil {
		t.Fatalf("Alice not in bundle")
	}
	if alice.Pwd != "bcrypt" {
		t.Errorf("Alice password scheme = %q, want bcrypt", alice.Pwd)
	}
	if alice.Vhost != "alice@alice.users.example.net" {
		t.Errorf("Alice vhost = %q, want alice@alice.users.example.net", alice.Vhost)
	}
	if len(alice.Aliases) != 1 || alice.Aliases[0] != "AliceAlt" {
		t.Errorf("Alice aliases = %v, want [AliceAlt]", alice.Aliases)
	}

	if len(b.Channels) != 1 {
		t.Fatalf("expected 1 channel, got %d", len(b.Channels))
	}
	ch := b.Channels[0]
	if ch.Name != "#staff" {
		t.Errorf("channel name = %q, want #staff", ch.Name)
	}
	if !strings.EqualFold(ch.Founder, "Alice") {
		t.Errorf("channel founder = %q, want Alice (case-insensitive)", ch.Founder)
	}
	if len(ch.ACL) != 1 || ch.ACL[0].Account == "" || ch.ACL[0].Modes != "v" {
		t.Errorf("channel ACL = %+v, want one entry mapping BobUnverified -> v", ch.ACL)
	}
	if len(ch.Akicks) != 1 || ch.Akicks[0].Mask != "*!*@spam.example.com" {
		t.Errorf("akicks = %+v, want [*!*@spam.example.com]", ch.Akicks)
	}

	if len(b.Bans) != 1 || b.Bans[0].Mask != "*@1.2.3.0/24" {
		t.Errorf("bans = %+v, want [*@1.2.3.0/24]", b.Bans)
	}
}
