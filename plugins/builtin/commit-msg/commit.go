package main

// Reading the staged change and the repository's history: the commit type it looks like, its
// scope, and whether the repository writes Conventional Commits at all.

import (
	"os/exec"
	"path"
	"regexp"
	"strings"
)

// Change is one staged file: its status letter (A, M, D, R...) and path.
type Change struct {
	Status string
	Path   string
}

// Types, in the order the picker offers them after the guess.
var Types = []struct{ Name, Detail string }{
	{"feat", "a new feature"},
	{"fix", "a bug fix"},
	{"docs", "documentation only"},
	{"test", "tests only"},
	{"refactor", "neither a fix nor a feature"},
	{"perf", "faster or smaller"},
	{"build", "the build or packaging"},
	{"ci", "the CI configuration"},
	{"style", "formatting, no change in meaning"},
	{"chore", "anything else"},
}

func isDoc(p string) bool {
	ext := strings.ToLower(path.Ext(p))
	return strings.HasPrefix(p, "docs/") || strings.HasPrefix(p, "doc/") || ext == ".md" || ext == ".rst" ||
		ext == ".adoc" || ext == ".txt" && strings.Contains(strings.ToLower(path.Base(p)), "readme")
}

func isTest(p string) bool {
	base := path.Base(p)
	return strings.HasPrefix(p, "tests/") || strings.HasPrefix(p, "test/") || strings.Contains(p, "/tests/") ||
		strings.Contains(p, "/test/") || strings.HasSuffix(base, "_test.go") || strings.HasPrefix(base, "test_") ||
		strings.Contains(base, ".test.") || strings.Contains(base, ".spec.")
}

func isCI(p string) bool {
	return strings.HasPrefix(p, ".github/workflows/") || p == ".gitlab-ci.yml" || strings.HasPrefix(p, ".circleci/") ||
		p == ".travis.yml" || strings.HasPrefix(p, ".woodpecker")
}

func isBuild(p string) bool {
	base := path.Base(p)
	switch base {
	case "Makefile", "CMakeLists.txt", "meson.build", "Cargo.toml", "Cargo.lock", "go.mod", "go.sum", "package.json",
		"package-lock.json", "pyproject.toml", "setup.py", "Dockerfile", "PKGBUILD", ".SRCINFO", "snapcraft.yaml":
		return true
	}
	return strings.HasPrefix(p, "packaging/") || strings.HasPrefix(p, "debian/") || strings.HasSuffix(base, ".cmake") ||
		strings.HasSuffix(base, ".spec")
}

func all(changes []Change, test func(string) bool) bool {
	for _, c := range changes {
		if !test(c.Path) {
			return false
		}
	}
	return len(changes) > 0
}

// GuessType: what the staged files look like.
func GuessType(changes []Change) string {
	switch {
	case all(changes, isDoc):
		return "docs"
	case all(changes, isTest):
		return "test"
	case all(changes, isCI):
		return "ci"
	case all(changes, func(p string) bool { return isBuild(p) || isCI(p) }):
		return "build"
	}
	for _, c := range changes {
		if strings.HasPrefix(c.Status, "A") && !isTest(c.Path) && !isDoc(c.Path) {
			return "feat"
		}
	}
	return "fix"
}

// scopeOf: the part of a path that names its area: src/lsp/client.c -> lsp, plugins/builtin/java/x
// -> java, src/plugins/manager.c -> plugins, cmd/tool/main.go -> tool, docs/x.md -> docs; src/main.c
// and a top-level file -> "".
func scopeOf(p string) string {
	parts := strings.Split(p, "/")
	if len(parts) < 2 {
		return ""
	}
	skip := map[string]bool{"src": true, "lib": true, "pkg": true, "cmd": true, "internal": true, "app": true,
		"plugins": true, "builtin": true, "packages": true, "crates": true}
	for i, part := range parts[:len(parts)-1] {
		// a container folder counts when it is the last one, below the top (src/plugins/x.c -> plugins)
		if !skip[part] || (i == len(parts)-2 && i > 0) {
			return part
		}
	}
	return ""
}

// GuessScope: the scope all staged files share, preferring one the history already used; "" when
// they spread over several areas.
func GuessScope(changes []Change, used map[string]int) string {
	counts := map[string]int{}
	for _, c := range changes {
		counts[scopeOf(c.Path)]++
	}
	if len(counts) == 1 {
		for scope := range counts {
			return scope
		}
	}
	best, bestUses := "", 0
	for scope, n := range counts {
		if scope != "" && n*2 > len(changes) && used[scope] > bestUses { // most files, and known
			best, bestUses = scope, used[scope]
		}
	}
	return best
}

var conventional = regexp.MustCompile(`^([a-z]+)(\(([^)]+)\))?!?: \S`)

// History reads subjects: whether most are Conventional Commits, and how often each scope came up.
func History(subjects []string) (bool, map[string]int) {
	used := map[string]int{}
	matching := 0
	for _, s := range subjects {
		if m := conventional.FindStringSubmatch(s); m != nil {
			matching++
			if m[3] != "" {
				used[m[3]]++
			}
		}
	}
	return len(subjects) == 0 || matching*2 >= len(subjects), used
}

// Prefix: "type(scope): ", "type: " without a scope.
func Prefix(kind, scope string) string {
	if scope == "" {
		return kind + ": "
	}
	return kind + "(" + scope + "): "
}

// Git: the output of git in dir, without its last line break.
func Git(dir string, args ...string) (string, error) {
	out, err := exec.Command("git", append([]string{"-C", dir}, args...)...).Output()
	return strings.TrimRight(string(out), "\n"), err
}

// Staged: the staged files of the repository at top.
func Staged(top string) ([]Change, error) {
	out, err := Git(top, "diff", "--cached", "--name-status", "--no-renames", "-z")
	if err != nil {
		return nil, err
	}
	fields := strings.Split(strings.TrimRight(out, "\x00"), "\x00")
	var changes []Change
	for i := 0; i+1 < len(fields); i += 2 {
		changes = append(changes, Change{fields[i], fields[i+1]})
	}
	return changes, nil
}
