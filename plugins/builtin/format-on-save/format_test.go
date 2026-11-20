package main

import (
	"net"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
)

func TestMinimalEdit(t *testing.T) {
	cases := []struct {
		old, new string
		want     Edit
	}{
		{"int  x;\n", "int x;\n", Edit{[4]int{1, 5, 1, 6}, ""}},
		{"a\nb\n", "a\nb\nc\n", Edit{[4]int{3, 1, 3, 1}, "c\n"}},
		{"çok  güzel\n", "çok güzel\n", Edit{[4]int{1, 5, 1, 6}, ""}}, // columns in characters
		{"x = é\n", "x = è\n", Edit{[4]int{1, 5, 1, 6}, "è"}},         // never half a character
		{"same", "same", Edit{[4]int{1, 5, 1, 5}, ""}},
		{"aaa", "aa", Edit{[4]int{1, 3, 1, 4}, ""}},
		{"", "new\n", Edit{[4]int{1, 1, 1, 1}, "new\n"}},
	}
	for _, c := range cases {
		if got := MinimalEdit(c.old, c.new); got != c.want {
			t.Errorf("MinimalEdit(%q, %q) = %+v, want %+v", c.old, c.new, got, c.want)
		}
	}
}

func write(t *testing.T, path, text string, mode os.FileMode) {
	t.Helper()
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, []byte(text), mode); err != nil {
		t.Fatal(err)
	}
}

func TestConfigured(t *testing.T) {
	root := t.TempDir()
	file := filepath.Join(root, "src", "deep", "a.c")
	write(t, file, "int x;\n", 0o644)
	clang := Formatters[0]
	if clang.Configured(file, root) {
		t.Error("no .clang-format, yet configured")
	}
	write(t, filepath.Join(root, ".clang-format"), "BasedOnStyle: LLVM\n", 0o644)
	if !clang.Configured(file, root) {
		t.Error(".clang-format at the root not found")
	}
	py := filepath.Join(root, "tool.py")
	write(t, filepath.Join(root, "pyproject.toml"), "[project]\nname = \"x\"\n", 0o644)
	if Formatters[2].Configured(py, root) {
		t.Error("black configured without [tool.black]")
	}
	write(t, filepath.Join(root, "pyproject.toml"), "[tool.black]\nline-length = 100\n", 0o644)
	if !Formatters[2].Configured(py, root) {
		t.Error("[tool.black] not seen")
	}
}

func TestSettings(t *testing.T) {
	t.Setenv("XDG_CONFIG_HOME", t.TempDir())
	s := LoadSettings()
	if s.IsOff("/p") {
		t.Fatal("off from the start")
	}
	if on, err := s.Toggle("/p"); err != nil || on {
		t.Fatalf("first toggle: on=%v err=%v", on, err)
	}
	if !LoadSettings().IsOff("/p") {
		t.Fatal("not kept off")
	}
	if on, _ := s.Toggle("/p"); !on || LoadSettings().IsOff("/p") {
		t.Fatal("not back on")
	}
}

// fakeTcode answers like t-code: hello, get-text with text (twice), edit and save; it records the
// requests.
func fakeTcode(t *testing.T, text string) (string, func() []string) {
	t.Helper()
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
		buf := make([]byte, 0, 4096)
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
				case "get-text":
					answer = "ok\n" + text + "\x00"
				}
				conn.Write([]byte(answer))
			}
		}
	}()
	return sock, func() []string {
		listener.Close()
		<-done
		mu.Lock()
		defer mu.Unlock()
		return requests
	}
}

// A fake gofmt (upper case) on PATH: the file is formatted with one edit, then saved.
func TestFormatActive(t *testing.T) {
	bin := t.TempDir()
	write(t, filepath.Join(bin, "gofmt"), "#!/bin/sh\ntr a-z A-Z\n", 0o755)
	t.Setenv("PATH", bin+":/usr/bin:/bin")
	sock, requests := fakeTcode(t, "package main\n")
	t.Setenv("TCODE_SOCKET", sock)
	t.Setenv("TCODE_PLUGIN_TOKEN", "secret")
	tc, err := Connect()
	if err != nil {
		t.Fatal(err)
	}
	if err := formatActive(tc, "/p/main.go", true); err != nil {
		t.Fatal(err)
	}
	tc.Close()
	got := requests()
	want := []string{"hello tapi_1 secret", "get-text ", "get-text ",
		`edit [{"range":[1,1,1,13],"text":"PACKAGE MAIN"}]`, "save ", "say Format: formatted with gofmt"}
	if strings.Join(got, "|") != strings.Join(want, "|") {
		t.Errorf("requests:\n%q\nwant\n%q", got, want)
	}
}
