#!/usr/bin/env python3
"""MATLAB for t-code: a MATLAB session in a terminal tab (the plugin's own), .m files run in it.

Run saves the file and sends one line to the session: it records the output in a diary file, runs
the file inside try / catch and prints markers around it. This program then waits for the end
marker, reads MATLAB's errors from the diary ("Error in foo (line 12)", "Error: File: ... Line: 3
Column: 5") and hands them to t-code as diagnostics. Each command is its own run of this program;
the session's tab id is kept in $XDG_RUNTIME_DIR, under the plugin's token."""
import glob
import hashlib
import json
import os
import re
import sys
import tempfile
import time
import uuid

sys.dont_write_bytecode = True  # no __pycache__ in the plugin's folder
from tcode import Tcode, TcodeError  # noqa: E402

WAIT_SECONDS = 3600  # a run that takes longer than this gets no diagnostics


def runtime_dir():
    folder = os.path.join(os.environ.get("XDG_RUNTIME_DIR") or tempfile.gettempdir(), "t-code-matlab")
    os.makedirs(folder, mode=0o700, exist_ok=True)
    return folder


def state_file():
    token = os.environ.get("TCODE_PLUGIN_TOKEN", "")
    return os.path.join(runtime_dir(), hashlib.sha256(token.encode()).hexdigest()[:16] + ".json")


def load_tab():
    try:
        with open(state_file()) as f:
            return json.load(f).get("tab")
    except (OSError, ValueError):
        return None


def save_tab(tab):
    with open(state_file(), "w") as f:
        json.dump({"tab": tab}, f)


def find_matlab():
    if os.environ.get("TCODE_MATLAB"):
        return os.environ["TCODE_MATLAB"]
    from shutil import which
    found = which("matlab")
    if found:
        return found
    for pattern in ("/usr/local/MATLAB/*/bin/matlab", "/opt/MATLAB/*/bin/matlab", "~/MATLAB/*/bin/matlab"):
        hits = sorted(glob.glob(os.path.expanduser(pattern)))
        if hits:
            return hits[-1]  # the newest release
    return None


def quoted(text):
    """A MATLAB char array: 'it''s'."""
    return "'" + text.replace("'", "''") + "'"


def open_session(t):
    """Opens the session's terminal tab: its id, or None when MATLAB is missing."""
    matlab = find_matlab()
    if not matlab:
        t.request("say", "MATLAB not found: put matlab on your PATH (or install it in /usr/local/MATLAB)")
        return None
    tab = t.request("terminal-open", f"MATLAB -- {json.dumps(matlab)} -nodesktop -nosplash")
    save_tab(tab)
    return tab


def send(t, line):
    """Types line into the session, opening one when there is none (or it was closed). False when
    MATLAB is missing."""
    tab = load_tab()
    if tab:
        try:
            t.request("terminal-send", f"{tab} {line}")
            return True
        except TcodeError as e:
            if e.code != "bad-argument":  # gone: closed, or the plugins were reloaded
                raise
    tab = open_session(t)
    if tab:
        t.request("terminal-send", f"{tab} {line}")
    return tab is not None


def run_line(path, diary, start, done):
    folder, script = os.path.split(path)
    return (f"cd({quoted(folder)}); diary({quoted(diary)}); disp({quoted(start)}); "
            f"try, run({quoted(script)}); catch tcode_err, fprintf(2, '%s\\n', getReport(tcode_err, 'extended', "
            f"'hyperlinks', 'off')); end; disp({quoted(done)}); diary off; clear tcode_err")


def find_function(name, project, run_file):
    """The .m file a name in MATLAB's report stands for, or None (MATLAB's own, like run)."""
    name = name.split(">")[0].split("/")[-1]
    stem = os.path.splitext(os.path.basename(run_file))[0]
    if name == stem:
        return run_file
    leaf = name.split(".")[-1] + ".m"
    for folder, dirs, files in os.walk(project):
        dirs[:] = [d for d in dirs if not d.startswith(".")]
        if leaf in files:
            return os.path.join(folder, leaf)
    return None


def parse_report(text, run_file, project):
    """MATLAB's errors in the diary text between the markers: {path: [diagnostic, ...]}."""
    found = {}
    lines = [line.rstrip() for line in text.split("\n")]

    def add(path, line, column, message, severity="error"):
        found.setdefault(path, []).append({"range": [line, column], "severity": severity, "message": message,
                                           "source": "matlab"})

    message, first = None, True
    for i, line in enumerate(lines):
        syntax = re.match(r"Error: File: (.+?) Line: (\d+) Column: (\d+)", line)
        if syntax:
            following = next((l for l in lines[i + 1:] if l.strip()), "syntax error")
            path = syntax.group(1) if os.path.isabs(syntax.group(1)) else os.path.join(project, syntax.group(1))
            add(path, int(syntax.group(2)), int(syntax.group(3)), following.strip())
            first = False
            continue
        frame = re.match(r"Error in (\S+) \(line (\d+)\)", line)
        if frame:
            path = find_function(frame.group(1), project, run_file)
            if path:
                add(path, int(frame.group(2)), 1, message or "error", "error" if first else "info")
                if first:
                    message = f"{message or 'error'} (called from here)"
                first = False
            continue
        if message is None and line.strip() and not line.startswith("Error using "):
            message = line.strip()
    return found


def wait_for(diary, start, done):
    """The diary's text between the markers, once the end marker is there (None after WAIT_SECONDS)."""
    deadline = time.monotonic() + WAIT_SECONDS
    while time.monotonic() < deadline:
        try:
            with open(diary, encoding="utf-8", errors="replace") as f:
                text = f.read()
        except OSError:
            text = ""
        if done in text:
            return text.split(start, 1)[-1].split(done, 1)[0]
        time.sleep(0.3)
    return None


def run_file(t):
    path = t.request("get-file")
    if not path.endswith(".m"):
        t.request("say", "MATLAB runs .m files: open one first")
        return
    t.request("save")
    t.request("diagnostics-clear")
    diary = os.path.join(runtime_dir(), f"{uuid.uuid4().hex}.diary")
    start, done = f"__tcode_start_{uuid.uuid4().hex[:8]}__", f"__tcode_done_{uuid.uuid4().hex[:8]}__"
    if not send(t, run_line(path, diary, start, done)):
        return
    t.close()  # the wait may be long: no connection held meanwhile
    report = wait_for(diary, start, done)
    try:
        os.unlink(diary)
    except OSError:
        pass
    if report is None:
        return
    t = Tcode()
    found = parse_report(report, path, t.request("get-project"))
    for file, items in found.items():
        t.json("diagnostics-set", {"path": file, "items": items})
    errors = sum(1 for items in found.values() for d in items if d["severity"] == "error")
    t.request("say", f"MATLAB: {os.path.basename(path)} {'stopped with an error' if errors else 'ran fine'}")


def run_selection(t):
    text = t.text("get-selection")
    if not text.strip():
        t.request("say", "Select some MATLAB code first")
        return
    for line in text.split("\n"):
        if line.strip() and not send(t, line):
            return


def main(command):
    t = Tcode()
    try:
        if command == "run":
            run_file(t)
        elif command == "selection":
            run_selection(t)
        elif command == "session":
            open_session(t)
        elif command in ("stop", "close"):
            tab = load_tab()
            try:
                if tab and command == "stop":
                    t.request("terminal-interrupt", tab)
                elif tab:
                    t.request("terminal-close", tab)
                    save_tab(None)
                else:
                    t.request("say", "No MATLAB session is open")
            except TcodeError:
                save_tab(None)
                t.request("say", "No MATLAB session is open")
    except TcodeError as e:
        if e.code == "no-editor":
            t.request("say", "Open a .m file first")
        else:
            t.request("say", f"MATLAB plugin: {e.message}")


if __name__ == "__main__":
    try:
        main(sys.argv[1] if len(sys.argv) > 1 else "")
    except (BrokenPipeError, ConnectionError):
        pass
