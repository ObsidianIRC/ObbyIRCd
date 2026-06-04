// Package ergo reads ergochat/ergo's BuntDB ircd.db and extracts an
// IR bundle. Account state lives under "account.<field> <case-folded>"
// keys (text-prefixed); channel state lives under "1 <uuid>" keys
// where 1 == TableChannels and the value is JSON
// (`RegisteredChannel`).
package ergo

import (
	"encoding/base64"
	"encoding/json"
	"fmt"
	"strings"
	"time"

	"github.com/tidwall/buntdb"

	"github.com/obsidianirc/obbyircd-migrate/internal/ir"
)

// Read parses an Ergo ircd.db and returns an IR bundle.
func Read(path string) (*ir.Bundle, error) {
	db, err := buntdb.Open(path)
	if err != nil {
		return nil, fmt.Errorf("open ergo buntdb: %w", err)
	}
	defer db.Close()

	b := &ir.Bundle{
		Version:    ir.Version,
		Source:     "ergo",
		ExportedAt: time.Now().UTC(),
	}

	accounts := map[string]*ir.Account{} // by case-folded name

	err = db.View(func(tx *buntdb.Tx) error {
		// First pass: discover all accounts via account.exists.
		err := tx.AscendKeys("account.exists *", func(k, v string) bool {
			cf := strings.TrimPrefix(k, "account.exists ")
			accounts[cf] = &ir.Account{CaseFolded: cf}
			return true
		})
		if err != nil {
			return err
		}

		// Now hydrate each one. Helper that fetches a key by suffix.
		fetch := func(prefix, cf string) string {
			val, err := tx.Get(fmt.Sprintf("%s %s", prefix, cf))
			if err != nil {
				return ""
			}
			return val
		}

		for cf, a := range accounts {
			a.Name = fetch("account.name", cf)
			if a.Name == "" {
				a.Name = cf
			}
			if v := fetch("account.verified", cf); v == "1" {
				a.Verified = true
			}
			if regAt := fetch("account.registered.time", cf); regAt != "" {
				// Ergo stores Unix nanoseconds as a decimal string.
				if ns, ok := parseInt64(regAt); ok {
					a.RegisteredAt = time.Unix(0, ns).UTC()
				}
			}
			if creds := fetch("account.credentials", cf); creds != "" {
				ergoCredentialsToIR(creds, a)
			}
			if vh := fetch("account.vhost", cf); vh != "" {
				var info struct {
					ApprovedVHost string
					Enabled       bool
				}
				if err := json.Unmarshal([]byte(vh), &info); err == nil && info.Enabled {
					a.Vhost = info.ApprovedVHost
				}
			}
			if nicks := fetch("account.additionalnicks", cf); nicks != "" {
				var list []string
				if err := json.Unmarshal([]byte(nicks), &list); err == nil {
					for _, n := range list {
						if !strings.EqualFold(n, a.Name) {
							a.Aliases = append(a.Aliases, n)
						}
					}
				}
			}
			if susp := fetch("account.suspended", cf); susp != "" {
				var s struct {
					TimeCreated time.Time
					Duration    time.Duration
					OperName    string
					Reason      string
				}
				if err := json.Unmarshal([]byte(susp), &s); err == nil {
					sus := &ir.Suspension{Reason: s.Reason, By: s.OperName}
					if s.Duration > 0 {
						sus.Until = s.TimeCreated.Add(s.Duration)
					}
					a.Suspension = sus
				}
			}
			if md := fetch("account.metadata", cf); md != "" {
				var m map[string]string
				if err := json.Unmarshal([]byte(md), &m); err == nil && len(m) > 0 {
					a.Metadata = m
					if email, ok := m["email"]; ok {
						a.Email = email
					}
				}
			}
			// Pre-metadata-2 Ergo stashed realname + email in scattered
			// keys; account.metadata wins when present.
			if a.Metadata == nil {
				a.Metadata = map[string]string{}
			}
			if _, set := a.Metadata["realname"]; !set {
				if rn := fetch("account.realname", cf); rn != "" {
					a.Metadata["realname"] = rn
				}
			}
			if settings := fetch("account.settings", cf); settings != "" {
				var s struct{ Email string }
				if err := json.Unmarshal([]byte(settings), &s); err == nil && s.Email != "" {
					if a.Email == "" {
						a.Email = s.Email
					}
					if _, set := a.Metadata["email"]; !set {
						a.Metadata["email"] = s.Email
					}
				}
			}
			if len(a.Metadata) == 0 {
				a.Metadata = nil
			}
		}

		// Channels: TableChannels prefix is "1 ".
		err = tx.AscendKeys("1 *", func(k, v string) bool {
			ch := readErgoChannel(v)
			if ch != nil {
				b.Channels = append(b.Channels, *ch)
			}
			return true
		})
		return err
	})
	if err != nil {
		return nil, err
	}

	for _, a := range accounts {
		b.Accounts = append(b.Accounts, *a)
	}

	return b, nil
}

func ergoCredentialsToIR(jsonStr string, a *ir.Account) {
	type creds struct {
		Version        int    `json:"Version"`
		PassphraseHash []byte `json:"PassphraseHash"`
		Certfps        []string
		Salt           []byte
		Iters          int
		StoredKey      []byte
		ServerKey      []byte
	}
	var c creds
	if err := json.Unmarshal([]byte(jsonStr), &c); err != nil {
		return
	}
	if len(c.PassphraseHash) > 0 {
		hash := string(c.PassphraseHash)
		// Ergo always stores bcrypt as raw bytes (golang.org/x/crypto/bcrypt
		// returns "$2a$..." ASCII), so we can store it verbatim as bcrypt.
		if strings.HasPrefix(hash, "$2") {
			a.Password = ir.Password{Scheme: "bcrypt", Value: hash}
		} else {
			a.Password = ir.Password{
				Scheme: "reset-required",
				Value:  base64.StdEncoding.EncodeToString(c.PassphraseHash),
			}
		}
	} else {
		a.Password = ir.Password{Scheme: "reset-required"}
	}
	if c.Iters > 0 && len(c.StoredKey) > 0 {
		a.SCRAM = &ir.SCRAM{
			Salt:       base64.StdEncoding.EncodeToString(c.Salt),
			Iterations: c.Iters,
			StoredKey:  base64.StdEncoding.EncodeToString(c.StoredKey),
			ServerKey:  base64.StdEncoding.EncodeToString(c.ServerKey),
		}
	}
	if len(c.Certfps) > 0 {
		a.Certfps = append(a.Certfps, c.Certfps...)
	}
}

// readErgoChannel parses one TableChannels value (the JSON-serialised
// `RegisteredChannel`) into IR. Ergo's `modes.Mode` is `type Mode rune`
// so JSON serialises it as an integer (e.g. 'q' => 113), not a string;
// likewise for AccountToUMode values. We model both as int32 here.
func readErgoChannel(jsonStr string) *ir.Channel {
	type maskInfo struct {
		TimeCreated     time.Time
		CreatorNickmask string
		CreatorAccount  string
	}
	type registeredChannel struct {
		Name           string
		RegisteredAt   time.Time
		Founder        string
		Topic          string
		TopicSetBy     string
		TopicSetTime   time.Time
		Modes          []int32 // mode runes as integers
		Key            string
		Forward        string
		UserLimit      int
		AccountToUMode map[string]int32 // value is a single mode rune
		Bans           map[string]maskInfo
		Excepts        map[string]maskInfo
		Invites        map[string]maskInfo
		Metadata       map[string]string
	}
	var rc registeredChannel
	if err := json.Unmarshal([]byte(jsonStr), &rc); err != nil {
		return nil
	}
	if rc.Name == "" {
		return nil
	}
	ch := &ir.Channel{
		Name:         rc.Name,
		RegisteredAt: rc.RegisteredAt,
		Founder:      rc.Founder,
		Topic:        rc.Topic,
		TopicSetBy:   rc.TopicSetBy,
		TopicSetAt:   rc.TopicSetTime,
		Metadata:     rc.Metadata,
	}
	// Set: simple flag modes only. We DELIBERATELY drop modes that
	// would clash with obbyircd's chanmode set (Ergo's "C" "M" "T"
	// "U" don't all exist on the obbyircd side; "n"/"t"/"i"/"m"/"s"/"p"
	// do). Unknown modes fall through.
	var set strings.Builder
	known := "nimstpkfl" // letters obbyircd's chanmodes/* set understands
	for _, r := range rc.Modes {
		if r == 0 {
			continue
		}
		c := byte(r)
		if strings.ContainsRune(known, rune(c)) && !strings.ContainsRune(set.String(), rune(c)) {
			set.WriteByte(c)
		}
	}
	if rc.Key != "" && !strings.ContainsRune(set.String(), 'k') {
		set.WriteByte('k')
	}
	if rc.UserLimit > 0 && !strings.ContainsRune(set.String(), 'l') {
		set.WriteByte('l')
	}
	ch.Modes.Set = set.String()
	if rc.Key != "" || rc.UserLimit > 0 {
		ch.Modes.Params = map[string]string{}
		if rc.Key != "" {
			ch.Modes.Params["k"] = rc.Key
		}
		if rc.UserLimit > 0 {
			ch.Modes.Params["l"] = fmt.Sprintf("%d", rc.UserLimit)
		}
	}
	// AccountToUMode → ACL grants. Account names are stored
	// case-folded in Ergo, but obbyircd's IR target is canonical. We
	// preserve the case-folded form here; the writer will use it
	// as-is in the ~automode extban (account names compare via
	// NOCASE on obbyircd's side anyway).
	for acct, mode := range rc.AccountToUMode {
		if mode == 0 {
			continue
		}
		converted := ergoModeRuneToObby(mode)
		if converted == "" {
			continue
		}
		ch.ACL = append(ch.ACL, ir.ACLEntry{
			Account: acct,
			Modes:   converted,
		})
	}
	// Bans → akick (account if creator was account, else mask).
	for mask, info := range rc.Bans {
		ch.Akicks = append(ch.Akicks, ir.Akick{
			Mask:  mask,
			SetBy: info.CreatorNickmask,
			SetAt: info.TimeCreated,
		})
	}
	return ch
}

// ergoModeRuneToObby maps Ergo's mode-rune integer onto obbyircd's
// member-mode letters (q/a/o/h/v) for ACL extbans. Anything else
// returns "" (skipped).
func ergoModeRuneToObby(r int32) string {
	switch byte(r) {
	case 'q', 'a', 'o', 'h', 'v':
		return string(byte(r))
	}
	return ""
}

func parseInt64(s string) (int64, bool) {
	var n int64
	for _, c := range s {
		if c < '0' || c > '9' {
			return 0, false
		}
		n = n*10 + int64(c-'0')
	}
	if s == "" {
		return 0, false
	}
	return n, true
}
