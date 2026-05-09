// Package ir defines the intermediate representation that every reader
// produces and the writer consumes. The IR is the contract between
// readers and the writer; readers never touch the writer's internals
// and vice versa.
//
// Format is JSON; field names use snake_case so a hand-crafted IR file
// is easy to write for a fixture or a one-off migration.
package ir

import "time"

const Version = 1

// Bundle is a single migration's full payload. One Bundle == one
// migration run; the writer is idempotent so re-running with the
// same Bundle on the same target is safe.
type Bundle struct {
	Version    int       `json:"version"`
	Source     string    `json:"source"` // "atheme" | "anope" | "ergo"
	ExportedAt time.Time `json:"exported_at"`

	Accounts []Account `json:"accounts,omitempty"`
	Channels []Channel `json:"channels,omitempty"`
	Bans     []Ban     `json:"bans,omitempty"`
	Memos    []Memo    `json:"memos,omitempty"`
	Opers    []Oper    `json:"opers,omitempty"`
}

// Account == one services account on the source side. Becomes one row
// in obsidian.db.accounts (plus rows in account_certfps and
// account_aliases when those tables exist).
type Account struct {
	Name         string    `json:"name"`         // canonical (display) account name
	CaseFolded   string    `json:"case_folded"`  // RFC1459-folded; used for collision detection
	Email        string    `json:"email,omitempty"`
	Password     Password  `json:"password"`
	SCRAM        *SCRAM    `json:"scram,omitempty"`
	RegisteredAt time.Time `json:"registered_at"`
	LastLoginAt  time.Time `json:"last_login_at,omitempty"`
	Verified     bool      `json:"verified"`

	Vhost     string `json:"vhost,omitempty"`
	VhostSetAt time.Time `json:"vhost_set_at,omitempty"`

	Suspension *Suspension `json:"suspension,omitempty"`
	Aliases    []string    `json:"aliases,omitempty"` // additional registered nicks pointing at this account
	Certfps    []string    `json:"certfps,omitempty"` // SHA1/SHA256 hex strings, lowercase
	Flags      []string    `json:"flags,omitempty"`   // canonical: HOLD,PRIVATE,NEVEROP,NOOP,...
	Metadata   map[string]string `json:"metadata,omitempty"`
}

// Password is intentionally a tagged structure so the writer knows
// whether to store-as-is, force-reset, or rehash. Scheme strings
// match obsidian.db's `password_scheme` column values.
type Password struct {
	Scheme string `json:"scheme"` // argon2id|bcrypt|pbkdf2v2|crypt-sha256|crypt-sha512|reset-required|plain
	Value  string `json:"value"`  // raw hash string (or plaintext for "plain", marked for re-hashing)
}

// SCRAM == SCRAM-SHA-256 SASL credentials, transferable from Ergo.
// Other sources don't have these.
type SCRAM struct {
	Salt       string `json:"salt"`        // base64
	Iterations int    `json:"iterations"`
	StoredKey  string `json:"stored_key"`  // base64
	ServerKey  string `json:"server_key"`  // base64
}

type Suspension struct {
	Until  time.Time `json:"until,omitempty"` // zero = forever
	Reason string    `json:"reason,omitempty"`
	By     string    `json:"by,omitempty"`
}

// Channel == one registered channel, becomes a +P entry in channel.db
// with the obsidianirc/channel-registration ModData populated and
// per-grant +e ~automode extbans synthesised from ACL rows.
type Channel struct {
	Name         string    `json:"name"`           // canonical with leading '#'
	RegisteredAt time.Time `json:"registered_at"`
	Founder      string    `json:"founder"`        // services account name (may match an Account)
	Successor    string    `json:"successor,omitempty"`

	Topic       string    `json:"topic,omitempty"`
	TopicSetBy  string    `json:"topic_set_by,omitempty"`
	TopicSetAt  time.Time `json:"topic_set_at,omitempty"`

	// Channel modes that should be set on creation. The writer turns
	// these into the modes1/modes2 string pair channeldb writes.
	Modes ChannelModes `json:"modes"`

	// Mode-lock from source mlock_on/mlock_off/limit/key.
	ModeLock ChannelModeLock `json:"mode_lock"`

	// ACL grants -- account-bound or mask-bound. Each becomes an
	// +e ~automode extban.
	ACL []ACLEntry `json:"acl,omitempty"`

	// Akick list: each becomes a +b ~account: or +b <mask> in the
	// channel's banlist.
	Akicks []Akick `json:"akicks,omitempty"`

	Metadata map[string]string `json:"metadata,omitempty"`
}

type ChannelModes struct {
	// Single-letter modes that take a parameter. The writer concatenates
	// them into modes1 + modes2 in channeldb format.
	Set     string            `json:"set,omitempty"`     // e.g. "ntsi"
	Params  map[string]string `json:"params,omitempty"`  // e.g. {"k":"key","l":"50"}
}

type ChannelModeLock struct {
	On     string `json:"on,omitempty"`     // "ntsi"
	Off    string `json:"off,omitempty"`    // "m"
	Limit  int    `json:"limit,omitempty"`  // +l value
	Key    string `json:"key,omitempty"`    // +k value
}

// ACLEntry: exactly one of Account or Mask is set. Modes is one or
// more letters from the channel-mode set (typically members modes
// q/a/o/h/v but writer doesn't restrict).
type ACLEntry struct {
	Account string    `json:"account,omitempty"` // services account (preferred)
	Mask    string    `json:"mask,omitempty"`    // n!u@h fallback
	Modes   string    `json:"modes"`             // e.g. "o", "ov", "q"
	SetBy   string    `json:"set_by,omitempty"`
	SetAt   time.Time `json:"set_at,omitempty"`
}

type Akick struct {
	Account   string    `json:"account,omitempty"`
	Mask      string    `json:"mask,omitempty"`
	Reason    string    `json:"reason,omitempty"`
	SetBy     string    `json:"set_by,omitempty"`
	SetAt     time.Time `json:"set_at,omitempty"`
	ExpiresAt time.Time `json:"expires_at,omitempty"`
}

// Ban == TKL row, written to tkldb.db (or a config snippet). Type is
// one of K/G/Z/Q/X (k-line/g-line/z-line/q-line/x-line).
type Ban struct {
	Type      string    `json:"type"`
	Mask      string    `json:"mask"`
	Reason    string    `json:"reason,omitempty"`
	SetBy     string    `json:"set_by,omitempty"`
	SetAt     time.Time `json:"set_at,omitempty"`
	ExpiresAt time.Time `json:"expires_at,omitempty"`
}

type Memo struct {
	Recipient string    `json:"recipient"`
	Sender    string    `json:"sender"`
	Body      string    `json:"body"`
	SentAt    time.Time `json:"sent_at"`
	ReadAt    time.Time `json:"read_at,omitempty"`
}

type Oper struct {
	Account   string `json:"account"`
	Operclass string `json:"operclass"`
	Flags     string `json:"flags,omitempty"`
}
