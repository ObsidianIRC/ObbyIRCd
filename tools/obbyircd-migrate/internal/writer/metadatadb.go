// metadata.db writer (k4be metadata-db v100). Format defined in
// src/modules/metadata-db.c upstream.

package writer

import (
	"bytes"
	"errors"
	"fmt"
	"os"
	"sort"
	"time"

	"github.com/obsidianirc/obbyircd-migrate/internal/ir"
)

const metadataDBVersion uint32 = 100

// WriteMetadataDB writes account + channel metadata. Account entries
// get last_seen = now (no source-side timestamp); channel entries get
// last_seen = 0 (matches the upstream +P-channel writer).
func WriteMetadataDB(path string, b *ir.Bundle, opts Options, rep *Report) error {
	if path == "" {
		return errors.New("WriteMetadataDB: path is required")
	}
	if rep == nil {
		rep = &Report{}
	}

	type entry struct {
		owner    string
		lastSeen int64
		key      string
		value    string
	}
	var entries []entry

	now := time.Now().Unix()
	for _, a := range b.Accounts {
		for _, k := range sortedKeys(a.Metadata) {
			entries = append(entries, entry{a.Name, now, k, a.Metadata[k]})
		}
	}
	for _, c := range b.Channels {
		for _, k := range sortedKeys(c.Metadata) {
			entries = append(entries, entry{c.Name, 0, k, c.Metadata[k]})
		}
	}

	rep.Metadata = len(entries)

	if opts.DryRun || len(entries) == 0 {
		return nil
	}

	var buf bytes.Buffer
	if err := writeUint32LE(&buf, metadataDBVersion); err != nil {
		return err
	}
	if err := writeUint64LE(&buf, uint64(len(entries))); err != nil {
		return err
	}
	for _, e := range entries {
		if err := writeUint32LE(&buf, magicEntryStart); err != nil {
			return err
		}
		if err := writeStr(&buf, e.owner); err != nil {
			return err
		}
		if err := writeInt64LE(&buf, e.lastSeen); err != nil {
			return err
		}
		if err := writeStr(&buf, e.key); err != nil {
			return err
		}
		if err := writeStr(&buf, e.value); err != nil {
			return err
		}
		if err := writeUint32LE(&buf, magicEntryEnd); err != nil {
			return err
		}
	}

	tmp := path + ".new"
	if err := os.WriteFile(tmp, buf.Bytes(), 0o600); err != nil {
		return fmt.Errorf("write %s: %w", tmp, err)
	}
	if err := os.Rename(tmp, path); err != nil {
		return fmt.Errorf("rename %s -> %s: %w", tmp, path, err)
	}
	return nil
}

func sortedKeys(m map[string]string) []string {
	if len(m) == 0 {
		return nil
	}
	keys := make([]string, 0, len(m))
	for k := range m {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	return keys
}
