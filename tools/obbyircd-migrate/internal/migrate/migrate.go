// Package migrate ties readers + writers + reporting together. It's
// the layer the CLI and the TUI both call into so neither has to know
// about reader/writer internals.
package migrate

import (
	"errors"
	"fmt"

	"github.com/obsidianirc/obbyircd-migrate/internal/ir"
	anopeRead "github.com/obsidianirc/obbyircd-migrate/internal/readers/anope"
	athemeRead "github.com/obsidianirc/obbyircd-migrate/internal/readers/atheme"
	ergoRead "github.com/obsidianirc/obbyircd-migrate/internal/readers/ergo"
	"github.com/obsidianirc/obbyircd-migrate/internal/writer"
)

type Source string

const (
	SourceAtheme Source = "atheme"
	SourceAnope  Source = "anope"
	SourceErgo   Source = "ergo"
)

func KnownSources() []Source { return []Source{SourceAtheme, SourceAnope, SourceErgo} }

// Read dispatches to the right reader and returns the IR bundle.
func Read(src Source, path string) (*ir.Bundle, error) {
	switch src {
	case SourceAtheme:
		return athemeRead.Read(path)
	case SourceAnope:
		return anopeRead.Read(path)
	case SourceErgo:
		return ergoRead.Read(path)
	}
	return nil, fmt.Errorf("unknown source %q", src)
}

// PlanInputs is everything Run needs to migrate.
type PlanInputs struct {
	Source       Source
	SourcePath   string
	ObsidianPath string // path to write obsidian.db (sqlite). Optional.
	ChannelPath  string // path to write channel.db (UnrealDB). Optional.
	TKLPath      string // path to write tkldb.db (UnrealDB). Optional.
	MetadataPath string // path to write metadata.db (k4be format). Optional.
	DryRun       bool
	OnConflict   writer.ConflictPolicy
}

// Run is the top-level orchestrator: read source → write IR → run
// writers. Returns the bundle (so the caller can show / save it) and
// the post-write report.
func Run(in PlanInputs) (*ir.Bundle, *writer.Report, error) {
	if in.SourcePath == "" {
		return nil, nil, errors.New("source path is required")
	}
	bundle, err := Read(in.Source, in.SourcePath)
	if err != nil {
		return nil, nil, fmt.Errorf("read %s: %w", in.Source, err)
	}
	rep := &writer.Report{}
	opts := writer.Options{DryRun: in.DryRun, OnConflict: in.OnConflict}
	if in.ObsidianPath != "" && len(bundle.Accounts) > 0 {
		if err := writer.WriteObsidian(in.ObsidianPath, bundle, opts, rep); err != nil {
			return bundle, rep, fmt.Errorf("write obsidian: %w", err)
		}
	}
	if in.ChannelPath != "" && len(bundle.Channels) > 0 {
		if err := writer.WriteChannelDB(in.ChannelPath, bundle, opts, rep); err != nil {
			return bundle, rep, fmt.Errorf("write channel.db: %w", err)
		}
	}
	if in.TKLPath != "" && len(bundle.Bans) > 0 {
		if err := writer.WriteTKLDB(in.TKLPath, bundle, opts, rep); err != nil {
			return bundle, rep, fmt.Errorf("write tkldb.db: %w", err)
		}
	} else {
		rep.Bans = len(bundle.Bans)
	}
	if in.MetadataPath != "" {
		if err := writer.WriteMetadataDB(in.MetadataPath, bundle, opts, rep); err != nil {
			return bundle, rep, fmt.Errorf("write metadata.db: %w", err)
		}
	}
	return bundle, rep, nil
}
