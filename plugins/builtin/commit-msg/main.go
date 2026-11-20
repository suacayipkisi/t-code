// Commit message helper for t-code, in Go (the standard library only).
//
//	write  looks at the staged files and the repository's last commits, offers the commit type
//	       the change looks like first (feat, fix, docs, test, ci, build, ...) with its scope
//	       (src/lsp/... -> lsp), asks for the subject and fills Source Control's message box:
//	       "fix(lsp): <subject>". A repository whose history is not Conventional Commits gets the
//	       subject alone.
package main

import (
	"errors"
	"fmt"
	"os"
	"strconv"
	"strings"
)

func project(t *Tcode) string {
	if root := os.Getenv("TCODE_PROJECT_ROOT"); root != "" {
		return root
	}
	root, _ := t.Request("get-project", "")
	return root
}

type pickItem struct {
	Label  string `json:"label"`
	Detail string `json:"detail,omitempty"`
}

// chooseType: the picker with the guess first; "" when the user closed it.
func chooseType(t *Tcode, guess, scope string) (string, error) {
	items := []pickItem{{Prefix(guess, scope), "a guess from the staged files"}}
	names := []string{guess}
	for _, kind := range Types {
		if kind.Name != guess {
			items = append(items, pickItem{Prefix(kind.Name, scope), kind.Detail})
			names = append(names, kind.Name)
		}
	}
	answer, err := t.JSON("pick", map[string]any{"title": "Commit type", "items": items})
	var te *TcodeError
	if errors.As(err, &te) && te.Code == "canceled" {
		return "", nil
	}
	if err != nil {
		return "", err
	}
	index, err := strconv.Atoi(strings.SplitN(answer, " ", 2)[0])
	if err != nil || index < 0 || index >= len(names) {
		return "", fmt.Errorf("odd pick answer %q", answer)
	}
	return names[index], nil
}

func write() error {
	t, err := Connect()
	if err != nil {
		return err
	}
	defer t.Close()
	top, err := Git(project(t), "rev-parse", "--show-toplevel")
	if err != nil || top == "" {
		t.Say("Commit: this project is not a git repository")
		return nil
	}
	changes, err := Staged(top)
	if err != nil {
		return err
	}
	if len(changes) == 0 {
		t.Say("Commit: nothing is staged yet: stage the files first")
		return nil
	}
	log, _ := Git(top, "log", "--format=%s", "-n", "100")
	var subjects []string
	if log != "" {
		subjects = strings.Split(log, "\n")
	}
	isConventional, used := History(subjects)
	prefix := ""
	if isConventional {
		scope := GuessScope(changes, used)
		kind, err := chooseType(t, GuessType(changes), scope)
		if err != nil || kind == "" {
			return err
		}
		prefix = Prefix(kind, scope)
	}
	question := "Commit subject"
	if prefix != "" {
		question = "Subject after " + strings.TrimSpace(prefix)
	}
	subject, err := t.Request("ask", question)
	var te *TcodeError
	if errors.As(err, &te) && te.Code == "canceled" {
		subject, err = "", nil // the prefix alone goes in: type the rest in the box
	}
	if err != nil {
		return err
	}
	if _, err := t.Request("scm-message", prefix+strings.TrimSpace(subject)); err != nil {
		return err
	}
	t.Say("Commit: the message is in Source Control")
	return nil
}

func main() {
	command := ""
	if len(os.Args) > 1 {
		command = os.Args[1]
	}
	var err error
	if command == "write" {
		err = write()
	} else {
		err = fmt.Errorf("unknown command %q", command)
	}
	var closed *TcodeError
	if err != nil && !(errors.As(err, &closed) && closed.Code == "closed") {
		fmt.Fprintf(os.Stderr, "commit-msg: %s: %v\n", command, err)
		os.Exit(1)
	}
}
