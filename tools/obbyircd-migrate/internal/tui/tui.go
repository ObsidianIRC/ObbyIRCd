// Package tui implements the bubbletea-based interactive frontend for
// obbyircd-migrate. The UI is deliberately small: pick a source,
// point at the source DB, choose target paths, dry-run, then commit.
// Everything below the TUI is in package migrate.
package tui

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"

	tea "github.com/charmbracelet/bubbletea"
	"github.com/charmbracelet/lipgloss"

	"github.com/obsidianirc/obbyircd-migrate/internal/ir"
	"github.com/obsidianirc/obbyircd-migrate/internal/migrate"
	"github.com/obsidianirc/obbyircd-migrate/internal/writer"
)

type screen int

const (
	screenSource screen = iota
	screenSourcePath
	screenTargets
	screenPreview
	screenDone
)

// Model is the bubbletea state.
type Model struct {
	scr screen

	// Source picker
	sources    []migrate.Source
	sourceIdx  int

	// Path inputs
	srcPath      string
	obsidianPath string
	channelPath  string

	// Editing state for the current text input.
	editing      *string
	cursor       int

	// Targets screen field index (0=src, 1=obs, 2=chan)
	targetField  int

	// Outcome of the read+dry-run preview.
	bundle  *ir.Bundle
	report  *writer.Report
	preview string

	err error

	// Final commit results.
	commitReport *writer.Report
	commitErr    error
}

func InitialModel() Model {
	return Model{
		scr:     screenSource,
		sources: migrate.KnownSources(),
	}
}

func (m Model) Init() tea.Cmd { return nil }

func (m Model) Update(msg tea.Msg) (tea.Model, tea.Cmd) {
	switch msg := msg.(type) {
	case tea.KeyMsg:
		// Global keys
		switch msg.String() {
		case "ctrl+c", "ctrl+d":
			return m, tea.Quit
		}
		// If we're editing a text field, route input there first.
		if m.editing != nil {
			switch msg.Type {
			case tea.KeyEnter:
				m.editing = nil
				return m, nil
			case tea.KeyBackspace:
				if m.cursor > 0 && len(*m.editing) > 0 {
					*m.editing = (*m.editing)[:len(*m.editing)-1]
					m.cursor--
				}
				return m, nil
			case tea.KeyEsc:
				m.editing = nil
				return m, nil
			default:
				if msg.Type == tea.KeyRunes {
					*m.editing += string(msg.Runes)
					m.cursor += len(msg.Runes)
				} else if msg.Type == tea.KeySpace {
					*m.editing += " "
					m.cursor++
				}
				return m, nil
			}
		}
		// Per-screen keys
		switch m.scr {
		case screenSource:
			return m.updateSource(msg)
		case screenSourcePath:
			return m.updateSourcePath(msg)
		case screenTargets:
			return m.updateTargets(msg)
		case screenPreview:
			return m.updatePreview(msg)
		case screenDone:
			if msg.Type == tea.KeyEnter || msg.String() == "q" {
				return m, tea.Quit
			}
		}
	}
	return m, nil
}

func (m Model) updateSource(msg tea.KeyMsg) (tea.Model, tea.Cmd) {
	switch msg.String() {
	case "up", "k":
		if m.sourceIdx > 0 {
			m.sourceIdx--
		}
	case "down", "j":
		if m.sourceIdx < len(m.sources)-1 {
			m.sourceIdx++
		}
	case "enter":
		m.scr = screenSourcePath
		m.editing = &m.srcPath
		m.cursor = len(m.srcPath)
	case "q":
		return m, tea.Quit
	}
	return m, nil
}

func (m Model) updateSourcePath(msg tea.KeyMsg) (tea.Model, tea.Cmd) {
	switch msg.String() {
	case "enter":
		if m.srcPath == "" {
			m.editing = &m.srcPath
			return m, nil
		}
		// Auto-fill target paths to sensible defaults beside obbyircd's
		// PERMDATADIR layout if not yet set.
		if m.obsidianPath == "" {
			m.obsidianPath = filepath.Join(filepath.Dir(m.srcPath), "obsidian.db")
		}
		if m.channelPath == "" {
			m.channelPath = filepath.Join(filepath.Dir(m.srcPath), "channel.db")
		}
		m.scr = screenTargets
		m.targetField = 1
		m.editing = &m.obsidianPath
		m.cursor = len(m.obsidianPath)
	case "esc":
		m.scr = screenSource
	}
	return m, nil
}

func (m Model) updateTargets(msg tea.KeyMsg) (tea.Model, tea.Cmd) {
	switch msg.String() {
	case "up", "k":
		if m.targetField > 0 {
			m.targetField--
		}
	case "down", "j":
		if m.targetField < 2 {
			m.targetField++
		}
	case "e", "i":
		switch m.targetField {
		case 0:
			m.editing = &m.srcPath
		case 1:
			m.editing = &m.obsidianPath
		case 2:
			m.editing = &m.channelPath
		}
		if m.editing != nil {
			m.cursor = len(*m.editing)
		}
	case "enter":
		// Run the dry-run preview.
		bundle, rep, err := migrate.Run(migrate.PlanInputs{
			Source:       m.sources[m.sourceIdx],
			SourcePath:   m.srcPath,
			ObsidianPath: m.obsidianPath,
			ChannelPath:  m.channelPath,
			DryRun:       true,
			OnConflict:   writer.ConflictSkip,
		})
		m.err = err
		m.bundle = bundle
		m.report = rep
		m.preview = renderPreview(bundle, rep)
		m.scr = screenPreview
	case "esc":
		m.scr = screenSource
	}
	return m, nil
}

func (m Model) updatePreview(msg tea.KeyMsg) (tea.Model, tea.Cmd) {
	switch msg.String() {
	case "y", "Y", "enter":
		bundle, rep, err := migrate.Run(migrate.PlanInputs{
			Source:       m.sources[m.sourceIdx],
			SourcePath:   m.srcPath,
			ObsidianPath: m.obsidianPath,
			ChannelPath:  m.channelPath,
			DryRun:       false,
			OnConflict:   writer.ConflictSkip,
		})
		_ = bundle
		m.commitErr = err
		m.commitReport = rep
		m.scr = screenDone
	case "n", "N", "esc":
		m.scr = screenTargets
	}
	return m, nil
}

func (m Model) View() string {
	var b strings.Builder
	b.WriteString(titleStyle.Render("obbyircd-migrate"))
	b.WriteString("\n\n")
	switch m.scr {
	case screenSource:
		b.WriteString(headStyle.Render("Pick the source services daemon to migrate from:"))
		b.WriteString("\n\n")
		for i, s := range m.sources {
			cursor := "  "
			label := string(s)
			line := lipgloss.NewStyle()
			if i == m.sourceIdx {
				cursor = "▶ "
				line = line.Foreground(lipgloss.Color("#75e6da")).Bold(true)
			}
			b.WriteString(line.Render(cursor+label) + "\n")
		}
		b.WriteString("\n")
		b.WriteString(hintStyle.Render("↑/↓ select  ⏎ next  q quit"))
	case screenSourcePath:
		b.WriteString(headStyle.Render(fmt.Sprintf("Path to %s database:", m.sources[m.sourceIdx])))
		b.WriteString("\n")
		b.WriteString(hintStyle.Render(sourceHint(m.sources[m.sourceIdx])))
		b.WriteString("\n\n")
		b.WriteString(inputStyle.Render(m.srcPath + cursorChar(m.editing != nil)))
		b.WriteString("\n\n")
		b.WriteString(hintStyle.Render("⏎ next  esc back"))
	case screenTargets:
		b.WriteString(headStyle.Render("Target paths"))
		b.WriteString("\n\n")
		fields := []struct {
			label string
			value string
		}{
			{"Source DB:        ", m.srcPath},
			{"obsidian.db:      ", m.obsidianPath},
			{"channel.db:       ", m.channelPath},
		}
		for i, f := range fields {
			arrow := "  "
			row := lipgloss.NewStyle()
			if i == m.targetField {
				arrow = "▶ "
				row = row.Foreground(lipgloss.Color("#75e6da")).Bold(true)
			}
			val := f.value
			if val == "" {
				val = "(unset)"
			}
			b.WriteString(row.Render(arrow + f.label + val))
			b.WriteString("\n")
		}
		b.WriteString("\n")
		if m.editing != nil {
			b.WriteString(inputStyle.Render(*m.editing + "█"))
			b.WriteString("\n\n")
		}
		b.WriteString(hintStyle.Render("↑/↓ select  e/i edit  ⏎ dry-run  esc back"))
	case screenPreview:
		if m.err != nil {
			b.WriteString(errStyle.Render("Read error: " + m.err.Error()))
			b.WriteString("\n\n")
			b.WriteString(hintStyle.Render("esc back to targets"))
			break
		}
		b.WriteString(headStyle.Render("Dry-run preview"))
		b.WriteString("\n\n")
		b.WriteString(m.preview)
		b.WriteString("\n")
		b.WriteString(hintStyle.Render("y/⏎ commit  n/esc go back"))
	case screenDone:
		if m.commitErr != nil {
			b.WriteString(errStyle.Render("Commit failed: " + m.commitErr.Error()))
		} else {
			b.WriteString(okStyle.Render("Migration complete."))
			b.WriteString("\n\n")
			b.WriteString(renderCommitSummary(m.commitReport))
		}
		b.WriteString("\n\n")
		b.WriteString(hintStyle.Render("⏎/q quit"))
	}
	return b.String()
}

func sourceHint(s migrate.Source) string {
	switch s {
	case migrate.SourceAnope:
		return "e.g. /path/to/anope/data/anope.db (db_flatfile)"
	case migrate.SourceAtheme:
		return "e.g. /path/to/atheme/etc/services.db (opensex)"
	case migrate.SourceErgo:
		return "e.g. /path/to/ergo/ircd.db (BuntDB)"
	}
	return ""
}

func cursorChar(active bool) string {
	if active {
		return "█"
	}
	return ""
}

func renderPreview(b *ir.Bundle, rep *writer.Report) string {
	if b == nil {
		return "(no bundle)"
	}
	var s strings.Builder
	fmt.Fprintf(&s, "Source: %s   Exported: %s\n", b.Source, b.ExportedAt.Format("2006-01-02 15:04:05"))
	fmt.Fprintf(&s, "  Accounts: %d\n", len(b.Accounts))
	for i, a := range b.Accounts {
		if i >= 5 {
			fmt.Fprintf(&s, "    ... and %d more\n", len(b.Accounts)-5)
			break
		}
		fmt.Fprintf(&s, "    • %s  [%s]  %s\n", a.Name, a.Password.Scheme, ifEmpty(a.Email, "(no email)"))
	}
	fmt.Fprintf(&s, "  Channels: %d\n", len(b.Channels))
	for i, ch := range b.Channels {
		if i >= 5 {
			fmt.Fprintf(&s, "    ... and %d more\n", len(b.Channels)-5)
			break
		}
		fmt.Fprintf(&s, "    • %s  founder=%s  acl=%d  akicks=%d\n", ch.Name, ch.Founder, len(ch.ACL), len(ch.Akicks))
	}
	fmt.Fprintf(&s, "  Bans: %d   Memos: %d   Opers: %d\n", len(b.Bans), len(b.Memos), len(b.Opers))
	if rep != nil && len(rep.Warnings) > 0 {
		s.WriteString("\nWarnings:\n")
		for _, w := range rep.Warnings {
			fmt.Fprintf(&s, "  ! %s\n", w)
		}
	}
	return s.String()
}

func renderCommitSummary(rep *writer.Report) string {
	if rep == nil {
		return "(no report)"
	}
	var s strings.Builder
	inserted, skipped, failed := 0, 0, 0
	for _, a := range rep.Accounts {
		switch a.Action {
		case "inserted":
			inserted++
		case "skipped":
			skipped++
		case "failed":
			failed++
		}
	}
	fmt.Fprintf(&s, "Accounts: %d inserted, %d skipped, %d failed\n",
		inserted, skipped, failed)
	chInserted := 0
	for _, c := range rep.Channels {
		if c.Action == "inserted" {
			chInserted++
		}
	}
	fmt.Fprintf(&s, "Channels: %d written\n", chInserted)
	if rep.Bans > 0 {
		fmt.Fprintf(&s, "TKL: %d ban records noted (writer not yet implemented; see migration-report)\n", rep.Bans)
	}
	if len(rep.Warnings) > 0 {
		s.WriteString("\nWarnings:\n")
		for _, w := range rep.Warnings {
			fmt.Fprintf(&s, "  ! %s\n", w)
		}
	}
	return s.String()
}

func ifEmpty(s, fallback string) string {
	if s == "" {
		return fallback
	}
	return s
}

// Run launches the TUI. Used by cmd/obbyircd-migrate.
func Run() error {
	p := tea.NewProgram(InitialModel())
	_, err := p.Run()
	return err
}

// Styles
var (
	titleStyle = lipgloss.NewStyle().
			Foreground(lipgloss.Color("#a78bfa")).
			Bold(true).Padding(0, 1)
	headStyle = lipgloss.NewStyle().Bold(true)
	hintStyle = lipgloss.NewStyle().Foreground(lipgloss.Color("#888"))
	errStyle  = lipgloss.NewStyle().Foreground(lipgloss.Color("#f97373")).Bold(true)
	okStyle   = lipgloss.NewStyle().Foreground(lipgloss.Color("#86efac")).Bold(true)
	inputStyle = lipgloss.NewStyle().
			Border(lipgloss.RoundedBorder()).
			Padding(0, 1).
			Foreground(lipgloss.Color("#fafafa"))
)

// Suppress unused warnings on os import if it ever drops out.
var _ = os.Stderr
