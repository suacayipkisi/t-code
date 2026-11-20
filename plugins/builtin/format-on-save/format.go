package main

// Which formatter takes which file, whether the project asks for it on save, running it, and the
// one edit that turns the old text into the new.

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"time"
	"unicode/utf8"
)

// Formatter reads the text on stdin and writes the formatted text on stdout.
type Formatter struct {
	Name string
	Exts []string
	Argv func(path string) []string
	// Configured: whether the project asks for this formatter, so it runs on save. Format This File
	// runs it either way.
	Configured func(path, root string) bool
}

func always(string, string) bool { return true }

// configFile: one of names in the file's folder or above, up to the project's root.
func configFile(names ...string) func(path, root string) bool {
	return func(path, root string) bool { return findUp(filepath.Dir(path), root, names...) != "" }
}

// configText: one of names in the file's folder or above that holds needle.
func configText(name, needle string) func(path, root string) bool {
	return func(path, root string) bool {
		found := findUp(filepath.Dir(path), root, name)
		if found == "" {
			return false
		}
		data, err := os.ReadFile(found)
		return err == nil && bytes.Contains(data, []byte(needle))
	}
}

func either(checks ...func(path, root string) bool) func(path, root string) bool {
	return func(path, root string) bool {
		for _, check := range checks {
			if check(path, root) {
				return true
			}
		}
		return false
	}
}

// Formatters, the first one installed for a file's extension wins.
var Formatters = []Formatter{
	{"clang-format", []string{".c", ".h", ".cc", ".cpp", ".cxx", ".hpp", ".hh", ".hxx", ".m", ".mm"},
		func(p string) []string { return []string{"clang-format", "--assume-filename=" + p} },
		configFile(".clang-format", "_clang-format")},
	{"ruff", []string{".py", ".pyi"},
		func(p string) []string { return []string{"ruff", "format", "--quiet", "--stdin-filename", p, "-"} },
		either(configFile("ruff.toml", ".ruff.toml"), configText("pyproject.toml", "[tool.ruff"))},
	{"black", []string{".py", ".pyi"},
		func(p string) []string { return []string{"black", "--quiet", "--stdin-filename", p, "-"} },
		configText("pyproject.toml", "[tool.black]")},
	{"gofmt", []string{".go"}, func(string) []string { return []string{"gofmt"} }, always},
	{"rustfmt", []string{".rs"},
		func(string) []string { return []string{"rustfmt", "--emit", "stdout", "--edition", "2021"} },
		configFile("Cargo.toml", "rustfmt.toml", ".rustfmt.toml")},
	{"prettier", []string{".js", ".jsx", ".mjs", ".cjs", ".ts", ".tsx", ".json", ".css", ".scss", ".less", ".html",
		".vue", ".md", ".yaml", ".yml", ".graphql"},
		func(p string) []string { return []string{"prettier", "--stdin-filepath", p} },
		either(configFile(".prettierrc", ".prettierrc.json", ".prettierrc.yaml", ".prettierrc.yml", ".prettierrc.js",
			".prettierrc.cjs", ".prettierrc.mjs", ".prettierrc.toml", "prettier.config.js", "prettier.config.cjs",
			"prettier.config.mjs"), configText("package.json", `"prettier"`))},
	{"shfmt", []string{".sh", ".bash"},
		func(p string) []string { return []string{"shfmt", "-filename", p} }, configFile(".editorconfig")},
	{"zig fmt", []string{".zig"}, func(string) []string { return []string{"zig", "fmt", "--stdin"} }, always},
}

// FormatterFor: the first installed formatter for path's extension, and whether any formatter
// knows the extension at all.
func FormatterFor(path string) (f *Formatter, known bool) {
	ext := strings.ToLower(filepath.Ext(path))
	for i := range Formatters {
		for _, e := range Formatters[i].Exts {
			if e != ext {
				continue
			}
			known = true
			if _, err := exec.LookPath(Formatters[i].Argv(path)[0]); err == nil {
				return &Formatters[i], true
			}
		}
	}
	return nil, known
}

// Names of the formatters for path's extension, for "install one of".
func FormatterNames(path string) []string {
	ext := strings.ToLower(filepath.Ext(path))
	var names []string
	for _, f := range Formatters {
		for _, e := range f.Exts {
			if e == ext {
				names = append(names, f.Name)
			}
		}
	}
	return names
}

// findUp: the first of names in dir or a folder above it, not above root (when dir is inside it).
func findUp(dir, root string, names ...string) string {
	dir, root = filepath.Clean(dir), filepath.Clean(root)
	inside := root != "." && root != "" && (dir == root || strings.HasPrefix(dir, root+string(filepath.Separator)))
	for {
		for _, name := range names {
			candidate := filepath.Join(dir, name)
			if info, err := os.Stat(candidate); err == nil && !info.IsDir() {
				return candidate
			}
		}
		parent := filepath.Dir(dir)
		if parent == dir || (inside && dir == root) {
			return ""
		}
		dir = parent
	}
}

// Run formats text with f, in the file's folder (where its configuration is looked for).
func Run(f *Formatter, path, text string) (string, error) {
	ctx, cancel := context.WithTimeout(context.Background(), 20*time.Second)
	defer cancel()
	argv := f.Argv(path)
	cmd := exec.CommandContext(ctx, argv[0], argv[1:]...)
	if info, err := os.Stat(filepath.Dir(path)); err == nil && info.IsDir() {
		cmd.Dir = filepath.Dir(path) // an unsaved new file's folder may not exist yet
	}
	cmd.Stdin = strings.NewReader(text)
	var out, stderr bytes.Buffer
	cmd.Stdout, cmd.Stderr = &out, &stderr
	if err := cmd.Run(); err != nil {
		first, _, _ := strings.Cut(strings.TrimSpace(stderr.String()), "\n")
		if first == "" {
			first = err.Error()
		}
		return "", errors.New(first)
	}
	return out.String(), nil
}

// Edit is one replacement of the edit request: [line, column, line, column], 1-based, columns in
// characters.
type Edit struct {
	Range [4]int `json:"range"`
	Text  string `json:"text"`
}

// place: the line and column (1-based, characters) of byte offset at in text.
func place(text string, at int) (int, int) {
	before := text[:at]
	line := strings.Count(before, "\n") + 1
	start := strings.LastIndexByte(before, '\n') + 1
	return line, utf8.RuneCountInString(before[start:]) + 1
}

// MinimalEdit: the one replacement, between the common start and end of old and new (on character
// boundaries), that makes new out of old. The cursor and the undo history stay small that way.
func MinimalEdit(old, new string) Edit {
	prefix := 0
	for prefix < len(old) && prefix < len(new) && old[prefix] == new[prefix] {
		prefix++
	}
	for prefix > 0 && prefix < len(old) && !utf8.RuneStart(old[prefix]) {
		prefix--
	}
	suffix := 0
	for suffix < len(old)-prefix && suffix < len(new)-prefix && old[len(old)-1-suffix] == new[len(new)-1-suffix] {
		suffix++
	}
	for suffix > 0 && len(old)-suffix < len(old) && !utf8.RuneStart(old[len(old)-suffix]) {
		suffix--
	}
	l1, c1 := place(old, prefix)
	l2, c2 := place(old, len(old)-suffix)
	return Edit{[4]int{l1, c1, l2, c2}, new[prefix : len(new)-suffix]}
}

// Projects where format on save is off, kept in $XDG_CONFIG_HOME/t-code-format/settings.json.
type Settings struct {
	Off []string `json:"off"`
}

func settingsPath() string {
	base := os.Getenv("XDG_CONFIG_HOME")
	if base == "" {
		home, _ := os.UserHomeDir()
		base = filepath.Join(home, ".config")
	}
	return filepath.Join(base, "t-code-format", "settings.json")
}

func LoadSettings() Settings {
	var s Settings
	if data, err := os.ReadFile(settingsPath()); err == nil {
		json.Unmarshal(data, &s)
	}
	return s
}

func (s Settings) IsOff(project string) bool {
	for _, p := range s.Off {
		if p == project {
			return true
		}
	}
	return false
}

// Toggle turns format on save off or back on for project and saves the settings; true: now on.
func (s *Settings) Toggle(project string) (bool, error) {
	on := s.IsOff(project)
	kept := s.Off[:0]
	for _, p := range s.Off {
		if p != project {
			kept = append(kept, p)
		}
	}
	s.Off = kept
	if !on {
		s.Off = append(s.Off, project)
	}
	data, _ := json.MarshalIndent(s, "", "  ")
	if err := os.MkdirAll(filepath.Dir(settingsPath()), 0o755); err != nil {
		return on, err
	}
	return on, os.WriteFile(settingsPath(), append(data, '\n'), 0o644)
}
