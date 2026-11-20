package main

// A small client for t-code's plugin socket (tapi_1), Go's standard library only. See t-code's
// docs/plugins.md for the protocol.

import (
	"bufio"
	"bytes"
	"encoding/json"
	"fmt"
	"net"
	"os"
	"strings"
)

// TcodeError is an "error <code> <message>" answer; code "closed" when t-code went away.
type TcodeError struct{ Code, Message string }

func (e *TcodeError) Error() string { return e.Code + ": " + e.Message }

type Tcode struct {
	conn net.Conn
	r    *bufio.Reader
}

// Connect opens a tapi_1 connection to $TCODE_SOCKET and says hello with the plugin's token.
func Connect() (*Tcode, error) {
	conn, err := net.Dial("unix", os.Getenv("TCODE_SOCKET"))
	if err != nil {
		return nil, err
	}
	t := &Tcode{conn: conn, r: bufio.NewReader(conn)}
	if _, err := t.Request("hello", strings.TrimSpace("tapi_1 "+os.Getenv("TCODE_PLUGIN_TOKEN"))); err != nil {
		conn.Close()
		return nil, err
	}
	return t, nil
}

func (t *Tcode) Close() { t.conn.Close() }

// Request sends one request, ended by a NUL (an argument may hold line breaks), and returns the
// value after "ok".
func (t *Tcode) Request(command, argument string) (string, error) {
	if _, err := t.conn.Write([]byte(command + " " + argument + "\x00")); err != nil {
		return "", &TcodeError{"closed", err.Error()}
	}
	line, err := t.r.ReadString('\n')
	if err != nil {
		return "", &TcodeError{"closed", "t-code closed the connection"}
	}
	line = strings.TrimSuffix(line, "\n")
	if line == "ok" {
		return "", nil
	}
	if value, found := strings.CutPrefix(line, "ok "); found {
		return value, nil
	}
	rest := strings.TrimPrefix(line, "error ")
	code, message, _ := strings.Cut(rest, " ")
	return "", &TcodeError{code, message}
}

// Text sends get-text or get-selection: "ok", then the text up to a NUL byte.
func (t *Tcode) Text(command string) (string, error) {
	if _, err := t.Request(command, ""); err != nil {
		return "", err
	}
	text, err := t.r.ReadString(0)
	if err != nil {
		return "", &TcodeError{"closed", "t-code closed the connection"}
	}
	return strings.TrimSuffix(text, "\x00"), nil
}

// JSON sends a request whose argument is value as one line of JSON.
func (t *Tcode) JSON(command string, value any) (string, error) {
	var b bytes.Buffer
	enc := json.NewEncoder(&b)
	enc.SetEscapeHTML(false)
	if err := enc.Encode(value); err != nil {
		return "", err
	}
	return t.Request(command, strings.TrimSuffix(b.String(), "\n"))
}

// Event is one line of a subscribed connection.
type Event struct {
	Event string `json:"event"`
	Path  string `json:"path"`
	By    string `json:"by"`
}

// Events subscribes this connection and calls handle for each event until t-code goes away.
func (t *Tcode) Events(handle func(Event)) error {
	if _, err := t.Request("subscribe", ""); err != nil {
		return err
	}
	for {
		line, err := t.r.ReadString('\n')
		if err != nil {
			return nil
		}
		var e Event
		if json.Unmarshal([]byte(line), &e) == nil {
			handle(e)
		}
	}
}

// Say puts a short message in the status bar, ignoring failures.
func (t *Tcode) Say(format string, args ...any) { t.Request("say", fmt.Sprintf(format, args...)) }
