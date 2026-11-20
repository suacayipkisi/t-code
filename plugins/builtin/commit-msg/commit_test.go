package main

import (
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"sync"
	"testing"
)

func changes(list ...string) []Change {
	var out []Change
	for _, item := range list {
		status, p, _ := strings.Cut(item, " ")
		out = append(out, Change{status, p})
	}
	return out
}

func TestGuessType(t *testing.T) {
	cases := map[string][]Change{
		"docs":  changes("M README.md", "M docs/plugins.md"),
		"test":  changes("M tests/test_lsp.c", "A plugins/x/test_x.py"),
		"ci":    changes("M .github/workflows/ci.yml"),
		"build": changes("M Makefile", "M packaging/fedora/t-code.spec", "M .github/workflows/packages.yml"),
		"feat":  changes("A src/lsp/new.c", "M src/lsp/client.c"),
		"fix":   changes("M src/lsp/client.c", "M tests/test_lsp.c"),
	}
	for want, c := range cases {
		if got := GuessType(c); got != want {
			t.Errorf("GuessType(%v) = %s, want %s", c, got, want)
		}
	}
}

func TestScopes(t *testing.T) {
	for p, want := range map[string]string{
		"src/lsp/inline/hover.c":       "lsp",
		"src/plugins/manager.c":        "plugins",
		"plugins/builtin/java/run.py":  "java",
		"cmd/tool/main.go":             "tool",
		"docs/plugins.md":              "docs",
		"src/main.c":                   "",
		"Makefile":                     "",
		"internal/server/http/http.go": "server",
	} {
		if got := scopeOf(p); got != want {
			t.Errorf("scopeOf(%s) = %q, want %q", p, got, want)
		}
	}
	if s := GuessScope(changes("M src/lsp/a.c", "M src/lsp/b/c.c"), nil); s != "lsp" {
		t.Errorf("one area: %q", s)
	}
	if s := GuessScope(changes("M src/lsp/a.c", "M src/lsp/b.c", "M tests/t.c"), map[string]int{"lsp": 3}); s != "lsp" {
		t.Errorf("mostly lsp, known: %q", s)
	}
	if s := GuessScope(changes("M src/lsp/a.c", "M src/git/b.c"), map[string]int{"lsp": 3}); s != "" {
		t.Errorf("two areas, half each: %q", s)
	}
}

func TestHistory(t *testing.T) {
	ok, used := History([]string{"fix(lsp): a", "feat(lsp): b", "docs: c", "Merge branch x"})
	if !ok || used["lsp"] != 2 {
		t.Errorf("conventional history: %v %v", ok, used)
	}
	if ok, _ := History([]string{"Add a thing", "Fix the other", "fix: one"}); ok {
		t.Error("a free-form history taken for conventional")
	}
	if ok, _ := History(nil); !ok {
		t.Error("a new repository should get types")
	}
	if Prefix("fix", "lsp") != "fix(lsp): " || Prefix("docs", "") != "docs: " {
		t.Error("prefixes")
	}
}

func git(t *testing.T, dir string, args ...string) {
	t.Helper()
	cmd := exec.Command("git", append([]string{"-C", dir, "-c", "user.name=t", "-c", "user.email=t@t"}, args...)...)
	if out, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("git %v: %v\n%s", args, err, out)
	}
}

// A repository with a conventional history and a staged new file under src/lsp; a t-code that picks
// the first (guessed) type and answers the subject: the box gets "feat(lsp): hover for Java".
func TestWrite(t *testing.T) {
	repo := t.TempDir()
	git(t, repo, "init", "-q", "-b", "main")
	os.MkdirAll(filepath.Join(repo, "src", "lsp"), 0o755)
	os.WriteFile(filepath.Join(repo, "src", "lsp", "client.c"), []byte("int x;\n"), 0o644)
	git(t, repo, "add", "-A")
	git(t, repo, "commit", "-qm", "fix(lsp): first")
	os.WriteFile(filepath.Join(repo, "src", "lsp", "hover.c"), []byte("int y;\n"), 0o644)
	git(t, repo, "add", "-A")

	sock := filepath.Join(t.TempDir(), "t-code.sock")
	listener, err := net.Listen("unix", sock)
	if err != nil {
		t.Fatal(err)
	}
	var mu sync.Mutex
	var requests []string
	done := make(chan struct{})
	go func() {
		defer close(done)
		conn, err := listener.Accept()
		if err != nil {
			return
		}
		defer conn.Close()
		var buf []byte
		chunk := make([]byte, 4096)
		for {
			n, err := conn.Read(chunk)
			if err != nil {
				return
			}
			buf = append(buf, chunk[:n]...)
			for {
				end := strings.IndexByte(string(buf), 0)
				if end < 0 {
					break
				}
				request := string(buf[:end])
				buf = buf[end+1:]
				mu.Lock()
				requests = append(requests, request)
				mu.Unlock()
				answer := "ok\n"
				switch strings.SplitN(request, " ", 2)[0] {
				case "hello":
					answer = "ok tapi_1 t-code test\n"
				case "pick":
					answer = "ok 0 feat(lsp): \n"
				case "ask":
					answer = "ok hover for Java\n"
				}
				conn.Write([]byte(answer))
			}
		}
	}()
	t.Setenv("TCODE_SOCKET", sock)
	t.Setenv("TCODE_PLUGIN_TOKEN", "secret")
	t.Setenv("TCODE_PROJECT_ROOT", repo)
	if err := write(); err != nil {
		t.Fatal(err)
	}
	listener.Close()
	<-done
	if len(requests) != 5 {
		t.Fatalf("requests: %q", requests)
	}
	if !strings.HasPrefix(requests[1], `pick {"items":[{"label":"feat(lsp): ","detail":"a guess from the staged files"},{"label":"fix(lsp): "`) {
		t.Errorf("pick: %s", requests[1])
	}
	if requests[2] != "ask Subject after feat(lsp):" {
		t.Errorf("ask: %s", requests[2])
	}
	if requests[3] != "scm-message feat(lsp): hover for Java" {
		t.Errorf("message: %s", requests[3])
	}
}
