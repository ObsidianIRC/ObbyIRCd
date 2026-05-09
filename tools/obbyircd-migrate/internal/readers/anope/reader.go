// Package anope reads Anope's db_flatfile format
// (modules/database/db_flatfile.cpp). The format is line-based:
//
//	OBJECT TypeName
//	ID <uint64>
//	DATA <key> <value>
//	DATA <key> <value>
//	...
//
// An object terminates when any non-"DATA " line appears (typically
// the next "OBJECT " line). Values can contain spaces — they span to
// EOL after the first space following the key.
package anope

import (
	"bufio"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"strconv"
	"strings"
	"time"

	"github.com/obsidianirc/obbyircd-migrate/internal/ir"
)

// Read parses an Anope DB and returns an IR bundle. Auto-detects
// between db_flatfile (legacy, "OBJECT TypeName / ID n / DATA k v")
// and db_json (Anope 2.1+ default). Only object types we care about
// are extracted (NickCore, NickAlias, ChannelInfo, ChanAccess,
// AutoKick, XLine). Unknown types are skipped silently.
func Read(path string) (*ir.Bundle, error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, fmt.Errorf("open %s: %w", path, err)
	}
	defer f.Close()

	// Sniff first non-whitespace byte to decide format.
	buf := make([]byte, 8)
	n, _ := f.Read(buf)
	for i := 0; i < n; i++ {
		if buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\n' || buf[i] == '\r' {
			continue
		}
		if buf[i] == '{' {
			_, _ = f.Seek(0, io.SeekStart)
			return readJSON(f)
		}
		break
	}
	_, _ = f.Seek(0, io.SeekStart)

	objs, err := scanObjects(f)
	if err != nil {
		return nil, err
	}

	b := &ir.Bundle{
		Version:    ir.Version,
		Source:     "anope",
		ExportedAt: time.Now().UTC(),
	}

	// Index NickCores by uniqueid so NickAlias can resolve back to a
	// display name. Index also by name so ChannelInfo founderid can
	// resolve.
	cores := map[string]*ncEntry{} // by lowercased display name
	coresByID := map[string]*ncEntry{}
	for _, o := range objs {
		if o.kind != "NickCore" {
			continue
		}
		acc := nickCoreToAccount(o)
		if acc == nil {
			continue
		}
		entry := &ncEntry{acc: *acc, id: o.fields["uniqueid"]}
		cores[strings.ToLower(acc.Name)] = entry
		if entry.id != "" {
			coresByID[entry.id] = entry
		}
	}

	// NickAliases attach to NickCores by ncid.
	for _, o := range objs {
		if o.kind != "NickAlias" {
			continue
		}
		nick := o.fields["nick"]
		ncid := o.fields["ncid"]
		if nick == "" || ncid == "" {
			continue
		}
		entry, ok := coresByID[ncid]
		if !ok {
			continue
		}
		// Skip the alias if it duplicates the canonical name.
		if !strings.EqualFold(nick, entry.acc.Name) {
			entry.acc.Aliases = append(entry.acc.Aliases, nick)
		}
		// NickAlias also carries vhost.
		if vh := o.fields["vhost_host"]; vh != "" {
			user := o.fields["vhost_ident"]
			if user != "" {
				entry.acc.Vhost = user + "@" + vh
			} else {
				entry.acc.Vhost = vh
			}
			entry.acc.VhostSetAt = parseUnix(o.fields["vhost_time"])
		}
	}

	// Flatten cores into the bundle.
	for _, e := range cores {
		b.Accounts = append(b.Accounts, e.acc)
	}

	// Channels — first pass build the list, second pass attach ACL/
	// akick rows.
	type chEntry struct {
		ch ir.Channel
		id string // uniqueid (uint64 string), used by ChanAccess.ci
	}
	channels := map[string]*chEntry{} // by lower-cased channel name
	channelsByID := map[string]*chEntry{}
	for _, o := range objs {
		if o.kind != "ChannelInfo" {
			continue
		}
		ch := channelInfoToChannel(o, coresByID)
		if ch == nil {
			continue
		}
		entry := &chEntry{ch: *ch, id: o.fields["uniqueid"]}
		channels[strings.ToLower(ch.Name)] = entry
		if entry.id != "" {
			channelsByID[entry.id] = entry
		}
	}

	for _, o := range objs {
		switch o.kind {
		case "ChanAccess":
			ci := o.fields["ci"]
			ch, ok := channelsByID[ci]
			if !ok {
				continue
			}
			modes := translateAnopeAccessFlags(o.fields["data"], o.fields["mask"])
			if modes == "" {
				continue
			}
			entry := ir.ACLEntry{
				Modes: modes,
				SetBy: o.fields["creator"],
				SetAt: parseUnix(o.fields["created"]),
			}
			if ncid := o.fields["ncid"]; ncid != "" {
				if nc, ok2 := coresByID[ncid]; ok2 {
					entry.Account = nc.acc.Name
				}
			}
			if entry.Account == "" {
				entry.Mask = o.fields["mask"]
			}
			ch.ch.ACL = append(ch.ch.ACL, entry)
		case "AutoKick":
			ci := o.fields["ci"]
			ch, ok := channelsByID[ci]
			if !ok {
				continue
			}
			a := ir.Akick{
				Mask:   o.fields["mask"],
				Reason: o.fields["reason"],
				SetBy:  o.fields["creator"],
				SetAt:  parseUnix(o.fields["addtime"]),
			}
			if ncid := o.fields["ncid"]; ncid != "" {
				if nc, ok2 := coresByID[ncid]; ok2 {
					a.Account = nc.acc.Name
					a.Mask = ""
				}
			}
			if a.Account == "" && a.Mask == "" {
				continue
			}
			ch.ch.Akicks = append(ch.ch.Akicks, a)
		case "XLine":
			b.Bans = append(b.Bans, xlineToBan(o))
		}
	}

	for _, e := range channels {
		b.Channels = append(b.Channels, e.ch)
	}

	return b, nil
}

// nickCoreToAccount maps an Anope NickCore record to an IR Account.
func nickCoreToAccount(o object) *ir.Account {
	display := o.fields["display"]
	if display == "" {
		return nil
	}
	scheme, value := splitAnopePassword(o.fields["pass"])
	a := &ir.Account{
		Name:         display,
		CaseFolded:   strings.ToLower(display),
		Email:        o.fields["email"],
		Password:     ir.Password{Scheme: scheme, Value: value},
		RegisteredAt: parseUnix(o.fields["registered"]),
		LastLoginAt:  parseUnix(o.fields["lastmail"]),
		Verified:     true, // Anope NickCore records are by definition past verification
	}
	return a
}

// ncEntry represents a NickCore + its uniqueid, indexed both by
// lower-cased display name and by uniqueid string.
type ncEntry struct {
	acc ir.Account
	id  string
}

// channelInfoToChannel maps an Anope ChannelInfo record to IR.
func channelInfoToChannel(o object, cores map[string]*ncEntry) *ir.Channel {
	name := o.fields["name"]
	if name == "" || name[0] != '#' {
		return nil
	}
	founder := ""
	if fid := o.fields["founderid"]; fid != "" {
		if nc, ok := cores[fid]; ok {
			founder = nc.acc.Name
		}
	}
	successor := ""
	if sid := o.fields["successorid"]; sid != "" {
		if nc, ok := cores[sid]; ok {
			successor = nc.acc.Name
		}
	}
	return &ir.Channel{
		Name:         name,
		RegisteredAt: parseUnix(o.fields["registered"]),
		Founder:      founder,
		Successor:    successor,
		Topic:        o.fields["last_topic"],
		TopicSetBy:   o.fields["last_topic_setter"],
		TopicSetAt:   parseUnix(o.fields["last_topic_time"]),
		Modes:        ir.ChannelModes{},
	}
}

// translateAnopeAccessFlags maps Anope's ACL "data" string (flag
// letters: VOICE/HOP/AOP/SOP-style or +VsoO etc.) to obbyircd member
// modes. Anope's actual format is configurable per-server; default is
// xOP/Flags. The simplest readable form is the one fred-style "F"/"f"
// flags, which we map straight.
func translateAnopeAccessFlags(data, mask string) string {
	// Anope ChanAccess.data is opaque per privilege provider. Common
	// providers:
	//   * "Flags" provider: data is a flag-letter string like "Vof"
	//   * "AccessXOP" provider: data is one of "VOICE","HOP","AOP","SOP","QOP"
	//   * "Levels" provider: data is a numeric privilege value
	d := strings.TrimSpace(data)
	if d == "" {
		return ""
	}
	// XOP shortcuts
	switch strings.ToUpper(d) {
	case "VOICE":
		return "v"
	case "HOP":
		return "h"
	case "AOP":
		return "o"
	case "SOP":
		return "ao"
	case "QOP":
		return "qao"
	}
	// Numeric — map by threshold (rough approximation; the migration
	// tool will warn). Anope levels: 0=member, 3=voice, 4=halfop,
	// 5=op, 10=admin, 9999=founder.
	if n, err := strconv.Atoi(d); err == nil {
		switch {
		case n >= 9999:
			return "q"
		case n >= 10:
			return "a"
		case n >= 5:
			return "o"
		case n >= 4:
			return "h"
		case n >= 3:
			return "v"
		}
		return ""
	}
	// Flags: collect any of the known privilege letters. Anope flag
	// letters are not 1:1 with mode letters; we use a conservative
	// map.
	var modes strings.Builder
	for _, c := range d {
		switch c {
		case 'v', 'V':
			modes.WriteByte('v')
		case 'h', 'H':
			modes.WriteByte('h')
		case 'o', 'O':
			modes.WriteByte('o')
		case 'a', 'A':
			modes.WriteByte('a')
		case 'q', 'Q', 'F':
			modes.WriteByte('q')
		}
	}
	// Dedup while preserving order.
	seen := map[byte]bool{}
	var out strings.Builder
	for i := 0; i < modes.Len(); i++ {
		c := modes.String()[i]
		if seen[c] {
			continue
		}
		seen[c] = true
		out.WriteByte(c)
	}
	return out.String()
}

func xlineToBan(o object) ir.Ban {
	mask := o.fields["mask"]
	// Determine type from "type" if present, else fall back to mask shape.
	typ := strings.ToUpper(o.fields["type"])
	if typ == "" {
		// XLine objects are usually akill (gline equivalent).
		typ = "G"
	}
	return ir.Ban{
		Type:      typ,
		Mask:      mask,
		Reason:    o.fields["reason"],
		SetBy:     o.fields["by"],
		SetAt:     parseUnix(o.fields["created"]),
		ExpiresAt: parseUnix(o.fields["expires"]),
	}
}

// splitAnopePassword turns Anope's `<scheme>:<value>` password
// representation into IR (scheme, value).
func splitAnopePassword(p string) (string, string) {
	if p == "" {
		return "reset-required", ""
	}
	colon := strings.IndexByte(p, ':')
	if colon < 0 {
		return "reset-required", p
	}
	scheme := strings.ToLower(p[:colon])
	val := p[colon+1:]
	switch scheme {
	case "bcrypt":
		return "bcrypt", val
	case "plain":
		return "plain", val
	case "hmac-sha256":
		return "hmac-sha256", val
	case "hmac-sha512":
		return "hmac-sha512", val
	case "md5", "sha1", "sha256", "sha2":
		return "reset-required", val
	}
	return "reset-required", val
}

func parseUnix(s string) time.Time {
	s = strings.TrimSpace(s)
	if s == "" {
		return time.Time{}
	}
	n, err := strconv.ParseInt(s, 10, 64)
	if err != nil || n <= 0 {
		return time.Time{}
	}
	return time.Unix(n, 0).UTC()
}

type object struct {
	kind   string
	fields map[string]string
}

// scanObjects walks the file once, splitting into OBJECT records.
func scanObjects(rdr interface {
	Read(p []byte) (n int, err error)
}) ([]object, error) {
	scanner := bufio.NewScanner(rdr)
	scanner.Buffer(make([]byte, 0, 1<<20), 1<<22)
	var (
		objs   []object
		cur    *object
	)
	for scanner.Scan() {
		line := scanner.Text()
		switch {
		case strings.HasPrefix(line, "OBJECT "):
			if cur != nil {
				objs = append(objs, *cur)
			}
			cur = &object{
				kind:   strings.TrimSpace(strings.TrimPrefix(line, "OBJECT ")),
				fields: map[string]string{},
			}
		case strings.HasPrefix(line, "ID "):
			if cur != nil {
				cur.fields["uniqueid"] = strings.TrimSpace(line[3:])
			}
		case strings.HasPrefix(line, "DATA "):
			if cur == nil {
				continue
			}
			rest := line[5:]
			sp := strings.IndexByte(rest, ' ')
			if sp < 0 {
				cur.fields[rest] = ""
			} else {
				cur.fields[rest[:sp]] = rest[sp+1:]
			}
		default:
			// Empty or stray line — Anope's reader closes the current
			// object on any non-DATA, non-OBJECT, non-ID line.
			if cur != nil {
				objs = append(objs, *cur)
				cur = nil
			}
		}
	}
	if cur != nil {
		objs = append(objs, *cur)
	}
	return objs, scanner.Err()
}

/* ============================================================
 * Anope db_json reader (Anope 2.1+ default; flatfile is deprecated).
 *
 * The JSON file is shaped:
 *   { "data": { "NickCore":[...], "NickAlias":[...],
 *               "ChannelInfo":[...], "AutoKick":[...] } }
 *
 * NickCore/NickAlias use uint64 uniqueid/ncid; ChannelInfo carries
 * founderid as a uint64 referencing NickCore.uniqueid. AutoKick.ci
 * is the channel name string (not the uniqueid).
 * ============================================================ */

func readJSON(r io.Reader) (*ir.Bundle, error) {
	var doc struct {
		Data map[string]json.RawMessage `json:"data"`
	}
	if err := json.NewDecoder(r).Decode(&doc); err != nil {
		return nil, fmt.Errorf("anope json decode: %w", err)
	}
	b := &ir.Bundle{
		Version:    ir.Version,
		Source:     "anope",
		ExportedAt: time.Now().UTC(),
	}

	type ncJ struct {
		Display    string `json:"display"`
		Email      string `json:"email"`
		Pass       string `json:"pass"`
		Registered int64  `json:"registered"`
		LastMail   int64  `json:"lastmail"`
		Uniqueid   uint64 `json:"uniqueid"`
	}
	type naJ struct {
		Nick             string `json:"nick"`
		Ncid             uint64 `json:"ncid"`
		Registered       int64  `json:"registered"`
		LastSeen         int64  `json:"last_seen"`
		VhostHost        string `json:"vhost_host"`
		VhostIdent       string `json:"vhost_ident"`
		VhostTime        int64  `json:"vhost_time"`
	}
	type ciJ struct {
		Name           string `json:"name"`
		Founderid      uint64 `json:"founderid"`
		Successorid    uint64 `json:"successorid"`
		Registered     int64  `json:"registered"`
		LastTopic      string `json:"last_topic"`
		LastTopicSetter string `json:"last_topic_setter"`
		LastTopicTime  int64  `json:"last_topic_time"`
	}
	type akJ struct {
		Ci      string `json:"ci"`
		Mask    string `json:"mask"`
		Reason  string `json:"reason"`
		Creator string `json:"creator"`
		Addtime int64  `json:"addtime"`
	}

	cores := map[uint64]*ir.Account{} // by uniqueid

	if raw, ok := doc.Data["NickCore"]; ok {
		var rows []ncJ
		_ = json.Unmarshal(raw, &rows)
		for _, r := range rows {
			scheme, value := splitAnopePassword(r.Pass)
			a := &ir.Account{
				Name:         r.Display,
				CaseFolded:   strings.ToLower(r.Display),
				Email:        r.Email,
				Password:     ir.Password{Scheme: scheme, Value: value},
				RegisteredAt: parseUnixInt(r.Registered),
				LastLoginAt:  parseUnixInt(r.LastMail),
				Verified:     true,
			}
			cores[r.Uniqueid] = a
		}
	}
	if raw, ok := doc.Data["NickAlias"]; ok {
		var rows []naJ
		_ = json.Unmarshal(raw, &rows)
		for _, r := range rows {
			a, found := cores[r.Ncid]
			if !found {
				continue
			}
			if !strings.EqualFold(r.Nick, a.Name) {
				a.Aliases = append(a.Aliases, r.Nick)
			}
			if r.VhostHost != "" {
				if r.VhostIdent != "" {
					a.Vhost = r.VhostIdent + "@" + r.VhostHost
				} else {
					a.Vhost = r.VhostHost
				}
				a.VhostSetAt = parseUnixInt(r.VhostTime)
			}
		}
	}

	channels := map[string]*ir.Channel{}    // by lower-case name
	channelByName := map[string]*ir.Channel{}
	if raw, ok := doc.Data["ChannelInfo"]; ok {
		var rows []ciJ
		_ = json.Unmarshal(raw, &rows)
		for _, r := range rows {
			ch := &ir.Channel{
				Name:         r.Name,
				RegisteredAt: parseUnixInt(r.Registered),
				Topic:        r.LastTopic,
				TopicSetBy:   r.LastTopicSetter,
				TopicSetAt:   parseUnixInt(r.LastTopicTime),
				Modes:        ir.ChannelModes{},
			}
			if a, ok := cores[r.Founderid]; ok {
				ch.Founder = a.Name
				ch.ACL = append(ch.ACL, ir.ACLEntry{
					Account: a.Name,
					Modes:   "qao", // Anope founder == full perms
				})
			}
			if a, ok := cores[r.Successorid]; ok && r.Successorid != 0 {
				ch.Successor = a.Name
			}
			channels[strings.ToLower(r.Name)] = ch
			channelByName[r.Name] = ch
		}
	}
	if raw, ok := doc.Data["AutoKick"]; ok {
		var rows []akJ
		_ = json.Unmarshal(raw, &rows)
		for _, r := range rows {
			ch, ok := channelByName[r.Ci]
			if !ok {
				continue
			}
			ch.Akicks = append(ch.Akicks, ir.Akick{
				Mask:   r.Mask,
				Reason: r.Reason,
				SetBy:  r.Creator,
				SetAt:  parseUnixInt(r.Addtime),
			})
		}
	}

	for _, a := range cores {
		b.Accounts = append(b.Accounts, *a)
	}
	for _, c := range channels {
		b.Channels = append(b.Channels, *c)
	}
	return b, nil
}

func parseUnixInt(t int64) time.Time {
	if t <= 0 {
		return time.Time{}
	}
	return time.Unix(t, 0).UTC()
}
