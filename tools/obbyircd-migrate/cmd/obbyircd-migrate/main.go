// Command obbyircd-migrate migrates accounts + channels from
// ergochat/Anope/Atheme into obbyircd's stores. The TUI is the
// default; the `read|write|run` subcommands offer scriptable
// non-interactive alternatives.
//
// Examples:
//
//	obbyircd-migrate                                                # launch TUI
//	obbyircd-migrate read --from atheme --in services.db --out ir.json
//	obbyircd-migrate run  --from anope  --in anope.db \
//	    --obsidian /var/lib/obby/data/obsidian.db \
//	    --channel  /var/lib/obby/data/channel.db \
//	    --dry-run
package main

import (
	"encoding/json"
	"flag"
	"fmt"
	"os"

	"github.com/obsidianirc/obbyircd-migrate/internal/migrate"
	"github.com/obsidianirc/obbyircd-migrate/internal/tui"
	"github.com/obsidianirc/obbyircd-migrate/internal/writer"
)

func main() {
	if len(os.Args) <= 1 {
		if err := tui.Run(); err != nil {
			fmt.Fprintln(os.Stderr, err)
			os.Exit(1)
		}
		return
	}
	switch os.Args[1] {
	case "read":
		runRead(os.Args[2:])
	case "run":
		runMigrate(os.Args[2:])
	case "tui":
		if err := tui.Run(); err != nil {
			fmt.Fprintln(os.Stderr, err)
			os.Exit(1)
		}
	case "-h", "--help", "help":
		usage()
	default:
		fmt.Fprintf(os.Stderr, "unknown subcommand %q\n\n", os.Args[1])
		usage()
		os.Exit(2)
	}
}

func runRead(args []string) {
	fs := flag.NewFlagSet("read", flag.ExitOnError)
	from := fs.String("from", "", "source: atheme|anope|ergo")
	in := fs.String("in", "", "path to source database")
	out := fs.String("out", "-", "path to write IR JSON ('-' for stdout)")
	if err := fs.Parse(args); err != nil {
		os.Exit(2)
	}
	if *from == "" || *in == "" {
		fs.Usage()
		os.Exit(2)
	}
	bundle, err := migrate.Read(migrate.Source(*from), *in)
	if err != nil {
		fmt.Fprintf(os.Stderr, "read: %v\n", err)
		os.Exit(1)
	}
	enc := json.NewEncoder(os.Stdout)
	enc.SetIndent("", "  ")
	var w *os.File = os.Stdout
	if *out != "-" && *out != "" {
		f, err := os.Create(*out)
		if err != nil {
			fmt.Fprintf(os.Stderr, "create %s: %v\n", *out, err)
			os.Exit(1)
		}
		defer f.Close()
		w = f
		enc = json.NewEncoder(w)
		enc.SetIndent("", "  ")
	}
	if err := enc.Encode(bundle); err != nil {
		fmt.Fprintf(os.Stderr, "encode: %v\n", err)
		os.Exit(1)
	}
}

func runMigrate(args []string) {
	fs := flag.NewFlagSet("run", flag.ExitOnError)
	from := fs.String("from", "", "source: atheme|anope|ergo")
	in := fs.String("in", "", "path to source database")
	obsidian := fs.String("obsidian", "", "obsidian.db output path (sqlite)")
	channel := fs.String("channel", "", "channel.db output path (UnrealDB v101)")
	tkl := fs.String("tkl", "", "tkldb.db output path (UnrealDB v4999)")
	metadata := fs.String("metadata", "", "metadata.db output path (k4be metadata-db format)")
	dryRun := fs.Bool("dry-run", false, "preview only, no writes")
	conflict := fs.String("on-conflict", "skip", "skip|fail|merge")
	if err := fs.Parse(args); err != nil {
		os.Exit(2)
	}
	if *from == "" || *in == "" {
		fs.Usage()
		os.Exit(2)
	}
	policy := writer.ConflictSkip
	switch *conflict {
	case "skip":
		policy = writer.ConflictSkip
	case "fail":
		policy = writer.ConflictFail
	case "merge":
		policy = writer.ConflictMerge
	default:
		fmt.Fprintf(os.Stderr, "--on-conflict must be skip|fail|merge\n")
		os.Exit(2)
	}

	bundle, rep, err := migrate.Run(migrate.PlanInputs{
		Source:       migrate.Source(*from),
		SourcePath:   *in,
		ObsidianPath: *obsidian,
		ChannelPath:  *channel,
		TKLPath:      *tkl,
		MetadataPath: *metadata,
		DryRun:       *dryRun,
		OnConflict:   policy,
	})
	if err != nil {
		fmt.Fprintf(os.Stderr, "run: %v\n", err)
		os.Exit(1)
	}
	fmt.Printf("read %d accounts, %d channels, %d bans from %s\n",
		len(bundle.Accounts), len(bundle.Channels), len(bundle.Bans), *from)
	inserted, skipped, failed := 0, 0, 0
	for _, a := range rep.Accounts {
		switch a.Action {
		case "inserted":
			inserted++
		case "skipped":
			skipped++
		case "failed":
			failed++
		case "would-insert":
			inserted++
		}
	}
	chInserted := 0
	for _, c := range rep.Channels {
		if c.Action == "inserted" || c.Action == "would-insert" {
			chInserted++
		}
	}
	fmt.Printf("accounts: %d inserted, %d skipped, %d failed\n", inserted, skipped, failed)
	fmt.Printf("channels: %d written\n", chInserted)
	if rep.Metadata > 0 {
		fmt.Printf("metadata: %d entries written\n", rep.Metadata)
	}
	if len(rep.Warnings) > 0 {
		fmt.Println("warnings:")
		for _, w := range rep.Warnings {
			fmt.Printf("  ! %s\n", w)
		}
	}
	if *dryRun {
		fmt.Println("dry-run; no changes written.")
	}
}

func usage() {
	fmt.Fprintf(os.Stderr, `obbyircd-migrate <command> [flags]

Commands:
  tui          launch interactive TUI (also the default with no args)
  read         parse a source DB and emit IR JSON to stdout / --out file
  run          full migration: read source + write obsidian.db + channel.db

Run '%s <command> -h' for command-specific flags.
`, os.Args[0])
}
