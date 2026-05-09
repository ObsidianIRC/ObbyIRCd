// Package atheme reads Atheme's opensex services.db format.
//
// Format reference: modules/backend/corestorage.c. Each row is a
// space-separated record with a 2-4 char prefix:
//
//	MU  <id> <name> <pass> <email> <reg> <lastlogin> <flags> <language>
//	MN  <user> <nick> <reg> <lastseen>
//	AC  <user> <mask>
//	MCFP <user> <certfp>
//	MC  <name> <reg> <used> <flags> <mlock_on> <mlock_off> <mlock_limit> [mlock_key]
//	CA  <chan> <target> <flags> <tmodified> <setter>
//	KL  <id> <user> <host> <duration> <settime> <setby> <reason...spans>
//	XL  <id> <realname> <duration> <settime> <setby> <reason...spans>
//	QL  <id> <mask> <duration> <settime> <setby> <reason...spans>
//	SO  <account> <operclass> <flags> [password]
//	ME  <dest> <sender> <sent> <status> <text...spans>
//	MDU <user> <key> <value...spans>
//	MDC <chan> <key> <value...spans>
package atheme

import (
	"bufio"
	"fmt"
	"os"
	"strconv"
	"strings"
	"time"

	"github.com/obsidianirc/obbyircd-migrate/internal/ir"
)

// Read parses an Atheme services.db and returns an IR bundle.
func Read(path string) (*ir.Bundle, error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, fmt.Errorf("open %s: %w", path, err)
	}
	defer f.Close()

	b := &ir.Bundle{
		Version:    ir.Version,
		Source:     "atheme",
		ExportedAt: time.Now().UTC(),
	}

	type acctEntry struct {
		acc ir.Account
	}
	accounts := map[string]*acctEntry{} // by lower-case canonical name
	channels := map[string]*ir.Channel{} // by lower-case channel name

	sc := bufio.NewScanner(f)
	sc.Buffer(make([]byte, 0, 1<<20), 1<<22)

	for sc.Scan() {
		line := sc.Text()
		if line == "" {
			continue
		}
		fields := splitOpensexFields(line)
		if len(fields) == 0 {
			continue
		}
		switch fields[0] {
		case "MU":
			// MU <id> <name> <pass> <email> <reg> <lastlogin> <flags> <language>
			if len(fields) < 7 {
				continue
			}
			name := fields[2]
			a := &acctEntry{acc: ir.Account{
				Name:         name,
				CaseFolded:   strings.ToLower(name),
				Email:        ifNotStar(fields[4]),
				RegisteredAt: parseUnix(fields[5]),
				LastLoginAt:  parseUnix(fields[6]),
				Verified:     true,
			}}
			scheme, val := splitAthemePassword(fields[3])
			a.acc.Password = ir.Password{Scheme: scheme, Value: val}
			if len(fields) >= 8 {
				a.acc.Flags = athemeMUFlagLetters(fields[7])
			}
			accounts[strings.ToLower(name)] = a
		case "MN":
			// MN <user> <nick> <reg> <lastseen> -- adds an alias to MU.
			if len(fields) < 4 {
				continue
			}
			user := strings.ToLower(fields[1])
			nick := fields[2]
			if entry, ok := accounts[user]; ok {
				if !strings.EqualFold(nick, entry.acc.Name) {
					entry.acc.Aliases = append(entry.acc.Aliases, nick)
				}
			}
		case "MCFP":
			// MCFP <user> <certfp>
			if len(fields) < 3 {
				continue
			}
			if entry, ok := accounts[strings.ToLower(fields[1])]; ok {
				entry.acc.Certfps = append(entry.acc.Certfps, strings.ToLower(fields[2]))
			}
		case "MDU":
			// MDU <user> <key> <value-spans>
			if len(fields) < 4 {
				continue
			}
			if entry, ok := accounts[strings.ToLower(fields[1])]; ok {
				key := fields[2]
				val := fields[3]
				if entry.acc.Metadata == nil {
					entry.acc.Metadata = map[string]string{}
				}
				entry.acc.Metadata[key] = val
				// HostServ vhosts live as "private:usercloak" or
				// equivalent metadata in Atheme — capture them.
				switch {
				case key == "private:usercloak", strings.HasSuffix(key, ":vhost"):
					entry.acc.Vhost = val
				}
			}
		case "MC":
			// MC <name> <reg> <used> <flags> <mlock_on> <mlock_off> <mlock_limit> [mlock_key]
			if len(fields) < 7 {
				continue
			}
			name := fields[1]
			ch := &ir.Channel{
				Name:         name,
				RegisteredAt: parseUnix(fields[2]),
				Modes:        ir.ChannelModes{},
				ModeLock: ir.ChannelModeLock{
					Limit: parseInt(fields[6]),
				},
			}
			if len(fields) >= 8 {
				ch.ModeLock.Key = ifNotStar(fields[7])
			}
			// mlock_on/off are bitmasks from atheme; we don't have
			// the bit→letter table without pulling all of atheme's
			// chanmode registry. Leave unset; users can re-set
			// mode-locks in obbyircd. Channels still carry the
			// limit + key which is what most installs care about.
			channels[strings.ToLower(name)] = ch
		case "CA":
			// CA <chan> <target> <flags> <tmodified> <setter>
			if len(fields) < 5 {
				continue
			}
			ch, ok := channels[strings.ToLower(fields[1])]
			if !ok {
				continue
			}
			target := fields[2]
			// CA rows with the 'b' (CA_AKICK) flag are akicks rather
			// than ACL grants. Convert + skip the rest of the ACL
			// translation.
			if strings.ContainsRune(fields[3], 'b') {
				ak := ir.Akick{
					SetAt: parseUnix(fields[4]),
				}
				if len(fields) >= 6 {
					ak.SetBy = ifNotStar(fields[5])
				}
				if isHostmask(target) {
					ak.Mask = target
				} else {
					ak.Account = target
				}
				ch.Akicks = append(ch.Akicks, ak)
				continue
			}
			modes := translateAthemeChanacsFlags(fields[3])
			if modes == "" {
				continue
			}
			entry := ir.ACLEntry{
				Modes: modes,
				SetAt: parseUnix(fields[4]),
			}
			if len(fields) >= 6 {
				entry.SetBy = ifNotStar(fields[5])
			}
			// Atheme's "target" is either an entity name (account or
			// group — strings without '!' or '@') or a hostmask.
			if isHostmask(target) {
				entry.Mask = target
			} else {
				entry.Account = target
			}
			// Founder gets stamped on the channel, not just the ACL.
			if strings.Contains(modes, "q") && ch.Founder == "" && entry.Account != "" {
				ch.Founder = entry.Account
			}
			ch.ACL = append(ch.ACL, entry)
		case "KL":
			// KL <id> <user> <host> <duration> <settime> <setby> <reason>
			if len(fields) < 8 {
				continue
			}
			b.Bans = append(b.Bans, ir.Ban{
				Type:      "K",
				Mask:      fields[2] + "@" + fields[3],
				Reason:    fields[7],
				SetBy:     ifNotStar(fields[6]),
				SetAt:     parseUnix(fields[5]),
				ExpiresAt: expiry(fields[5], fields[4]),
			})
		case "XL":
			// XL <id> <gecos> <duration> <settime> <setby> <reason>
			if len(fields) < 7 {
				continue
			}
			b.Bans = append(b.Bans, ir.Ban{
				Type:      "X",
				Mask:      fields[2],
				Reason:    fields[6],
				SetBy:     ifNotStar(fields[5]),
				SetAt:     parseUnix(fields[4]),
				ExpiresAt: expiry(fields[4], fields[3]),
			})
		case "QL":
			// QL <id> <mask> <duration> <settime> <setby> <reason>
			if len(fields) < 7 {
				continue
			}
			b.Bans = append(b.Bans, ir.Ban{
				Type:      "Q",
				Mask:      fields[2],
				Reason:    fields[6],
				SetBy:     ifNotStar(fields[5]),
				SetAt:     parseUnix(fields[4]),
				ExpiresAt: expiry(fields[4], fields[3]),
			})
		case "ME":
			// ME <dest> <sender> <sent> <status> <text-spans>
			if len(fields) < 5 {
				continue
			}
			b.Memos = append(b.Memos, ir.Memo{
				Recipient: fields[1],
				Sender:    fields[2],
				SentAt:    parseUnix(fields[3]),
				Body:      fields[4],
			})
		case "SO":
			// SO <account> <operclass> <flags> [password]
			if len(fields) < 3 {
				continue
			}
			oper := ir.Oper{
				Account:   fields[1],
				Operclass: fields[2],
			}
			if len(fields) >= 4 {
				oper.Flags = fields[3]
			}
			b.Opers = append(b.Opers, oper)
		}
	}

	if err := sc.Err(); err != nil {
		return nil, err
	}

	for _, e := range accounts {
		b.Accounts = append(b.Accounts, e.acc)
	}
	for _, c := range channels {
		b.Channels = append(b.Channels, *c)
	}

	return b, nil
}

// splitOpensexFields splits a line into fields. The last field of any
// row is allowed to contain spaces (multiword), so we don't know its
// arity until we see the type. To keep things simple we split greedily
// into "max" fields per type, where max is the count of the
// fixed-prefix fields plus 1 multiword tail. Most rows have a
// well-known arity (see corestorage.c).
//
// Implementation: split on spaces, but if the type indicates a
// multiword tail, glue the trailing tokens back together.
func splitOpensexFields(line string) []string {
	parts := strings.SplitN(line, " ", 2)
	if len(parts) == 0 {
		return nil
	}
	typ := parts[0]
	rest := ""
	if len(parts) == 2 {
		rest = parts[1]
	}
	// maxFixed = total field count for this row INCLUDING the type
	// AND including the multiword tail (counted as one logical field).
	// hasTail says the last field consumes-to-EOL.
	maxFixed := 0
	hasTail := false
	switch typ {
	case "MU":
		maxFixed = 9 // MU id name pass email reg lastlogin flags language
	case "MN":
		maxFixed = 5 // MN user nick reg lastseen
	case "AC":
		maxFixed = 3 // AC user mask
	case "MCFP":
		maxFixed = 3 // MCFP user certfp
	case "MDU", "MDC":
		maxFixed = 4 // MDU user key value-spans
		hasTail = true
	case "MC":
		maxFixed = 9 // MC name reg used flags mlock_on mlock_off mlock_limit mlock_key
	case "CA":
		maxFixed = 6 // CA chan target flags tmodified setter
	case "KL":
		maxFixed = 8 // KL id user host duration settime setby reason
		hasTail = true
	case "XL", "QL":
		maxFixed = 7 // XL/QL id mask duration settime setby reason
		hasTail = true
	case "ME":
		maxFixed = 6 // ME dest sender sent status text
		hasTail = true
	case "SI":
		maxFixed = 5 // SI mask settime setby reason
		hasTail = true
	case "SO":
		maxFixed = 5 // SO account operclass flags [password] (password optional)
	case "NAM":
		maxFixed = 2
	default:
		return append([]string{typ}, strings.Fields(rest)...)
	}
	tokens := strings.Fields(rest)
	// data fields (not counting type) = maxFixed - 1.
	// non-tail data fields = maxFixed - 2 (when hasTail), else maxFixed - 1.
	if hasTail && len(tokens) >= maxFixed-1 {
		head := tokens[:maxFixed-2]
		tail := strings.Join(tokens[maxFixed-2:], " ")
		out := append([]string{typ}, head...)
		out = append(out, tail)
		return out
	}
	out := append([]string{typ}, tokens...)
	return out
}

// translateAthemeChanacsFlags maps Atheme's bitmask-flag-letter string
// (`flags.c::ca_flags`) onto obbyircd member modes.
func translateAthemeChanacsFlags(flags string) string {
	var modes strings.Builder
	seen := map[byte]bool{}
	add := func(c byte) {
		if !seen[c] {
			seen[c] = true
			modes.WriteByte(c)
		}
	}
	for _, c := range flags {
		switch c {
		case 'F':
			add('q') // CA_FOUNDER → +q owner
		case 'q':
			add('q') // CA_USEOWNER
		case 'a':
			add('a') // CA_USEPROTECT
		case 'o', 'O':
			add('o')
		case 'h', 'H':
			add('h')
		case 'v', 'V':
			add('v')
		}
	}
	return modes.String()
}

func athemeMUFlagLetters(s string) []string {
	if s == "" || s == "*" {
		return nil
	}
	known := map[byte]string{
		'h': "HOLD",
		'n': "NEVEROP",
		'o': "NOOP",
		'W': "WAITAUTH",
		's': "HIDEMAIL",
		'm': "NOMEMO",
		'p': "PRIVATE",
		'g': "NOGREET",
	}
	var out []string
	for i := 0; i < len(s); i++ {
		if v, ok := known[s[i]]; ok {
			out = append(out, v)
		}
	}
	return out
}

func splitAthemePassword(p string) (string, string) {
	if p == "" || p == "*" {
		return "reset-required", ""
	}
	if strings.HasPrefix(p, "$z$") {
		// Atheme PBKDF2v2 — both the older textual form
		// ($z$pbkdf2-<prf>$...) and the numeric-algo form
		// ($z$<algo>$<iter>$<salt>$<hash>[$<serverkey>]). The verifier
		// in account-registration.c handles both via pbkdf2v2_verify.
		return "pbkdf2v2", p
	}
	if strings.HasPrefix(p, "$2a$") || strings.HasPrefix(p, "$2b$") || strings.HasPrefix(p, "$2y$") {
		return "bcrypt", p
	}
	if strings.HasPrefix(p, "$argon2") {
		return "argon2id", p
	}
	if strings.HasPrefix(p, "$5$") {
		return "crypt-sha256", p
	}
	if strings.HasPrefix(p, "$6$") {
		return "crypt-sha512", p
	}
	if strings.HasPrefix(p, "$7$") {
		return "scrypt", p
	}
	// Raw md5/sha1 hex are length-fingerprintable but unsafe to keep.
	return "reset-required", p
}

func isHostmask(s string) bool {
	return strings.ContainsAny(s, "!@")
}

func ifNotStar(s string) string {
	if s == "*" {
		return ""
	}
	return s
}

func parseUnix(s string) time.Time {
	s = strings.TrimSpace(s)
	if s == "" || s == "0" || s == "*" {
		return time.Time{}
	}
	n, err := strconv.ParseInt(s, 10, 64)
	if err != nil || n <= 0 {
		return time.Time{}
	}
	return time.Unix(n, 0).UTC()
}

func parseInt(s string) int {
	n, _ := strconv.Atoi(strings.TrimSpace(s))
	return n
}

func expiry(settime, duration string) time.Time {
	st := parseUnix(settime)
	dur, _ := strconv.ParseInt(duration, 10, 64)
	if st.IsZero() || dur <= 0 {
		return time.Time{}
	}
	return st.Add(time.Duration(dur) * time.Second)
}
