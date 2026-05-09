// Package writer turns an ir.Bundle into actual obbyircd state on
// disk. obsidian.db is sqlite (account-registration.c writes it);
// channel.db is the binary UnrealDB format (channeldb.c writes it).
package writer

import (
	"database/sql"
	"errors"
	"fmt"
	"strings"
	"time"

	_ "modernc.org/sqlite"

	"github.com/obsidianirc/obbyircd-migrate/internal/ir"
)

// ConflictPolicy controls what the writer does when a destination row
// would collide with an existing row.
type ConflictPolicy int

const (
	ConflictSkip ConflictPolicy = iota // default: leave existing row alone
	ConflictFail                       // abort the migration on first collision
	ConflictMerge                      // overwrite metadata-ish fields, keep identity
)

func (p ConflictPolicy) String() string {
	switch p {
	case ConflictSkip:
		return "skip"
	case ConflictFail:
		return "fail"
	case ConflictMerge:
		return "merge"
	}
	return "unknown"
}

// Options bundle the per-run knobs.
type Options struct {
	DryRun     bool
	OnConflict ConflictPolicy
}

// AccountReport summarises one account's outcome. The whole-run output
// is a slice of these so we can render a CSV/JSON for the operator.
type AccountReport struct {
	Name           string
	Action         string // inserted | skipped | merged | failed
	PasswordScheme string
	NeedsReset     bool
	Notes          string
}

type ChannelReport struct {
	Name   string
	Action string
	ACL    int
	Akicks int
	Notes  string
}

type Report struct {
	Accounts []AccountReport
	Channels []ChannelReport
	Bans     int
	Warnings []string
}

// WriteObsidian opens the obsidian.db sqlite at `path` and writes
// every account in the bundle. It introspects the schema with
// PRAGMA table_info before attempting to write any post-§6.1 column,
// so it works whether or not Phase 0 has shipped.
func WriteObsidian(path string, b *ir.Bundle, opts Options, rep *Report) error {
	if path == "" {
		return errors.New("WriteObsidian: path is required")
	}
	if rep == nil {
		rep = &Report{}
	}

	db, err := sql.Open("sqlite", path)
	if err != nil {
		return fmt.Errorf("open obsidian.db: %w", err)
	}
	defer db.Close()

	cols, err := tableColumns(db, "accounts")
	if err != nil {
		return fmt.Errorf("read accounts schema: %w", err)
	}
	if len(cols) == 0 {
		return errors.New("obsidian.db has no `accounts` table — is account-registration.c initialised?")
	}

	// Always-present columns we rely on (verified by the inspection).
	required := []string{"name", "password", "time_registered", "verified"}
	for _, c := range required {
		if !cols[c] {
			return fmt.Errorf("obsidian.db.accounts missing required column %q", c)
		}
	}

	// Optional columns — Phase 0 (§6.1) additions. Each is checked
	// individually so partial migrations work.
	hasEmail := cols["email"]
	hasScramSalt := cols["scram_salt"]
	hasScramIters := cols["scram_iterations"]
	hasScramStored := cols["scram_stored_key"]
	hasScramServer := cols["scram_server_key"]
	hasPwdScheme := cols["password_scheme"]
	hasVhost := cols["vhost"]
	hasVhostSetAt := cols["vhost_set_at"]
	hasSuspendedUntil := cols["suspended_until"]
	hasSuspendedReason := cols["suspended_reason"]
	hasSuspendedBy := cols["suspended_by"]
	hasFlags := cols["flags"]

	// Sibling tables for certfps / aliases / memos — also Phase 0+. Not required.
	hasCertfps, _ := tableExists(db, "account_certfps")
	hasAliases, _ := tableExists(db, "account_aliases")
	hasMemos, _ := tableExists(db, "memos")

	tx, err := db.Begin()
	if err != nil {
		return fmt.Errorf("begin tx: %w", err)
	}
	commit := false
	defer func() {
		if !commit {
			_ = tx.Rollback()
		}
	}()

	for _, a := range b.Accounts {
		ar := AccountReport{Name: a.Name, PasswordScheme: a.Password.Scheme}

		// Existence check on canonical name (NOCASE collation).
		var existingID int64
		err := tx.QueryRow(`SELECT id FROM accounts WHERE lower(name) = lower(?) LIMIT 1`, a.Name).Scan(&existingID)
		if err != nil && !errors.Is(err, sql.ErrNoRows) {
			rep.Warnings = append(rep.Warnings, fmt.Sprintf("account %s: lookup failed: %v", a.Name, err))
			ar.Action = "failed"
			ar.Notes = err.Error()
			rep.Accounts = append(rep.Accounts, ar)
			continue
		}
		if existingID != 0 {
			switch opts.OnConflict {
			case ConflictFail:
				return fmt.Errorf("account %q already exists in obsidian.db (--on-conflict=fail)", a.Name)
			case ConflictSkip:
				ar.Action = "skipped"
				ar.Notes = "exists"
				rep.Accounts = append(rep.Accounts, ar)
				continue
			case ConflictMerge:
				// Fall through to update path -- not yet implemented for v1.
				ar.Action = "skipped"
				ar.Notes = "merge not yet implemented"
				rep.Accounts = append(rep.Accounts, ar)
				continue
			}
		}

		// Build the INSERT dynamically based on which columns exist.
		colNames := []string{"name", "password", "time_registered", "verified"}
		placeholders := []string{"?", "?", "?", "?"}
		args := []any{a.Name, a.Password.Value, unixOrZero(a.RegisteredAt), boolToInt(a.Verified)}

		add := func(name string, val any) {
			colNames = append(colNames, name)
			placeholders = append(placeholders, "?")
			args = append(args, val)
		}
		if hasEmail && a.Email != "" {
			add("email", a.Email)
		}
		if a.SCRAM != nil {
			if hasScramSalt {
				add("scram_salt", a.SCRAM.Salt)
			}
			if hasScramIters {
				add("scram_iterations", a.SCRAM.Iterations)
			}
			if hasScramStored {
				add("scram_stored_key", a.SCRAM.StoredKey)
			}
			if hasScramServer {
				add("scram_server_key", a.SCRAM.ServerKey)
			}
		}
		if hasPwdScheme && a.Password.Scheme != "" {
			add("password_scheme", a.Password.Scheme)
		}
		if hasVhost && a.Vhost != "" {
			add("vhost", a.Vhost)
		}
		if hasVhostSetAt && !a.VhostSetAt.IsZero() {
			add("vhost_set_at", a.VhostSetAt.Unix())
		}
		if a.Suspension != nil {
			if hasSuspendedUntil {
				add("suspended_until", unixOrZero(a.Suspension.Until))
			}
			if hasSuspendedReason {
				add("suspended_reason", a.Suspension.Reason)
			}
			if hasSuspendedBy {
				add("suspended_by", a.Suspension.By)
			}
		}
		if hasFlags && len(a.Flags) > 0 {
			add("flags", strings.Join(a.Flags, ","))
		}

		ar.NeedsReset = a.Password.Scheme == "reset-required"

		if opts.DryRun {
			ar.Action = "would-insert"
			rep.Accounts = append(rep.Accounts, ar)
			continue
		}

		stmt := fmt.Sprintf("INSERT INTO accounts (%s) VALUES (%s)",
			strings.Join(colNames, ", "), strings.Join(placeholders, ", "))
		res, err := tx.Exec(stmt, args...)
		if err != nil {
			ar.Action = "failed"
			ar.Notes = err.Error()
			rep.Accounts = append(rep.Accounts, ar)
			continue
		}
		newID, _ := res.LastInsertId()

		if hasCertfps {
			for _, fp := range a.Certfps {
				if _, err := tx.Exec(
					`INSERT INTO account_certfps (account_id, fingerprint, added_at) VALUES (?, ?, ?)`,
					newID, strings.ToLower(fp), time.Now().Unix()); err != nil {
					rep.Warnings = append(rep.Warnings, fmt.Sprintf("certfp insert for %s: %v", a.Name, err))
				}
			}
		}
		if hasAliases {
			for _, al := range a.Aliases {
				if _, err := tx.Exec(
					`INSERT OR IGNORE INTO account_aliases (account_id, alias) VALUES (?, ?)`,
					newID, al); err != nil {
					rep.Warnings = append(rep.Warnings, fmt.Sprintf("alias insert for %s: %v", a.Name, err))
				}
			}
		}

		ar.Action = "inserted"
		rep.Accounts = append(rep.Accounts, ar)
	}

	// Memos: only write if the table exists. Resolve recipient name to
	// account id; warn + skip on misses (e.g. memo to a deleted account).
	if hasMemos {
		for _, m := range b.Memos {
			var recipientID int64
			err := tx.QueryRow(
				`SELECT id FROM accounts WHERE lower(name) = lower(?) LIMIT 1`,
				m.Recipient).Scan(&recipientID)
			if err != nil {
				rep.Warnings = append(rep.Warnings,
					fmt.Sprintf("memo for unknown recipient %q skipped", m.Recipient))
				continue
			}
			sentAt := unixOrZero(m.SentAt)
			readAt := int64(0)
			if !m.ReadAt.IsZero() {
				readAt = m.ReadAt.Unix()
			}
			if !opts.DryRun {
				if _, err := tx.Exec(
					`INSERT INTO memos (recipient_id, sender, body, sent_at, read_at) VALUES (?, ?, ?, ?, ?)`,
					recipientID, m.Sender, m.Body, sentAt, readAt); err != nil {
					rep.Warnings = append(rep.Warnings,
						fmt.Sprintf("memo for %s by %s: %v", m.Recipient, m.Sender, err))
				}
			}
		}
	} else if len(b.Memos) > 0 {
		rep.Warnings = append(rep.Warnings,
			fmt.Sprintf("%d memos in source dropped: target obsidian.db has no `memos` table (load the obbyircd memo module first)", len(b.Memos)))
	}

	if opts.DryRun {
		// Roll back so the dry-run touches nothing.
		return nil
	}
	if err := tx.Commit(); err != nil {
		return fmt.Errorf("commit: %w", err)
	}
	commit = true
	return nil
}

// tableColumns returns a set of lower-cased column names for the given
// table, or an empty set if the table doesn't exist.
func tableColumns(db *sql.DB, table string) (map[string]bool, error) {
	rows, err := db.Query("PRAGMA table_info(" + table + ")")
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	out := map[string]bool{}
	for rows.Next() {
		var cid int
		var name, typ string
		var notnull, pk int
		var dflt sql.NullString
		if err := rows.Scan(&cid, &name, &typ, &notnull, &dflt, &pk); err != nil {
			return nil, err
		}
		out[strings.ToLower(name)] = true
	}
	return out, nil
}

func tableExists(db *sql.DB, table string) (bool, error) {
	var n int
	err := db.QueryRow(
		`SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name=?`,
		table).Scan(&n)
	if err != nil {
		return false, err
	}
	return n > 0, nil
}

func boolToInt(b bool) int {
	if b {
		return 1
	}
	return 0
}

func unixOrZero(t time.Time) int64 {
	if t.IsZero() {
		return 0
	}
	return t.Unix()
}
