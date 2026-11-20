// Format on save for t-code, in Go (the standard library only).
//
// Started with the project ($1 = "start"), it formats the active file when it is saved, when the
// project asks for its formatter (a .clang-format, pyproject's [tool.ruff] or [tool.black], a
// .prettierrc, Cargo.toml, an .editorconfig for shfmt; gofmt and zig fmt always): the formatted
// text goes in as one edit (one undo step, the cursor stays) and the file is saved again. Its own
// save comes back as did_save "by": "plugin" and is left alone.
//
//	format  formats the active file now, configured or not
//	toggle  turns format on save off or back on for this project
package main

import (
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"
)

// formatActive formats path, the active editor's file. explicit: the user asked (Format This File),
// so every outcome is said; on save only failures are.
func formatActive(t *Tcode, path string, explicit bool) error {
	f, known := FormatterFor(path)
	if f == nil {
		if explicit && known {
			t.Say("Format: install %s to format %s files", strings.Join(FormatterNames(path), " or "), filepath.Ext(path))
		} else if explicit {
			t.Say("Format: no formatter for %s files", filepath.Base(path))
		}
		return nil
	}
	text, err := t.Text("get-text")
	if err != nil {
		return err
	}
	formatted, err := Run(f, path, text)
	if err != nil {
		t.Say("Format: %s failed: %s", f.Name, err)
		return nil
	}
	if formatted == text {
		if explicit {
			t.Say("Format: already tidy (%s)", f.Name)
		}
		return nil
	}
	if now, err := t.Text("get-text"); err != nil || now != text {
		return err // typed meanwhile: the next save formats it
	}
	if _, err := t.JSON("edit", []Edit{MinimalEdit(text, formatted)}); err != nil {
		return err
	}
	if _, err := t.Request("save", ""); err != nil {
		return err
	}
	if explicit {
		t.Say("Format: formatted with %s", f.Name)
	}
	return nil
}

func project(t *Tcode) string {
	if root := os.Getenv("TCODE_PROJECT_ROOT"); root != "" {
		return root
	}
	root, _ := t.Request("get-project", "")
	return root
}

// onSave: whether a save of path formats it.
func onSave(path, root string) bool {
	f, _ := FormatterFor(path)
	return f != nil && !LoadSettings().IsOff(root) && f.Configured(path, root)
}

func listen() error {
	requests, err := Connect()
	if err != nil {
		return err
	}
	defer requests.Close()
	events, err := Connect()
	if err != nil {
		return err
	}
	defer events.Close()
	root := project(requests)
	active, err := requests.Request("get-file", "")
	if err != nil {
		return err
	}
	var failed error
	events.Events(func(e Event) {
		if failed != nil {
			return
		}
		switch e.Event {
		case "active_changed":
			active = e.Path
		case "did_save":
			if e.By != "plugin" && e.Path != "" && e.Path == active && onSave(e.Path, root) {
				failed = formatActive(requests, e.Path, false)
			}
		}
	})
	return failed
}

func formatNow() error {
	t, err := Connect()
	if err != nil {
		return err
	}
	defer t.Close()
	path, err := t.Request("get-file", "")
	if err != nil {
		return err
	}
	if path == "" {
		t.Say("Format: open a file first")
		return nil
	}
	return formatActive(t, path, true)
}

func toggle() error {
	t, err := Connect()
	if err != nil {
		return err
	}
	defer t.Close()
	root := project(t)
	settings := LoadSettings()
	on, err := settings.Toggle(root)
	if err != nil {
		return err
	}
	if on {
		t.Say("Format: on save again in %s (where the project configures a formatter)", filepath.Base(root))
	} else {
		t.Say("Format: not on save in %s any more (Format This File still works)", filepath.Base(root))
	}
	return nil
}

func main() {
	command := ""
	if len(os.Args) > 1 {
		command = os.Args[1]
	}
	var err error
	switch command {
	case "start":
		err = listen()
	case "format":
		err = formatNow()
	case "toggle":
		err = toggle()
	default:
		err = fmt.Errorf("unknown command %q", command)
	}
	var closed *TcodeError
	if err != nil && !(errors.As(err, &closed) && closed.Code == "closed") {
		fmt.Fprintf(os.Stderr, "format-on-save: %s: %v\n", command, err)
		os.Exit(1)
	}
}
