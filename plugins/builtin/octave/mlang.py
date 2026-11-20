"""MATLAB-language sessions for t-code (the matlab and octave plugins share this file; each plugin's
folder has its own copy, since a registry entry pins one folder).

A session runs in a terminal tab (the plugin's own). Run saves the file and sends one line to it: it
records the output in a diary file, runs the file inside try / catch and prints markers around it.
Run in Batch gives the same line to a one-off program in a shell tab instead: no session kept, its
memory freed when it ends. The plugin then waits for the end marker, reads the errors from the diary
and hands them to t-code as diagnostics. Each command is its own run of the plugin; the session's
tab id is kept in $XDG_RUNTIME_DIR, under the plugin's token. When t-code closes, the session gets
"exit" typed (it saves its history, MATLAB frees its license), then t-code ends what is left of it.

What differs between MATLAB and Octave is a Flavor: finding the program, its session and batch
command lines, the run line and the error report's format."""
import glob
import hashlib
import json
import os
import re
import shlex
import sys
import tempfile
import time
import uuid
from shutil import which

sys.dont_write_bytecode = True  # no __pycache__ in the plugin's folder
from tcode import Tcode, TcodeError  # noqa: E402

WAIT_SECONDS = 3600  # a run that takes longer than this gets no diagnostics
QUIT_SECONDS = 10  # how long Close / Restart give "exit" before the tab is closed


def quoted(text):
    """A MATLAB / Octave char array: 'it''s'."""
    return "'" + text.replace("'", "''") + "'"


def find_function(name, project, run_file):
    """The .m file a function name in a report stands for, or None (the language's own, like run)."""
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


class Flavor:
    id = ""  # the plugin's id: its runtime folder and the diagnostics' source
    name = ""  # shown in tab titles and messages
    env = ""  # an environment variable naming the program, for tests and unusual installs
    program = ""  # the program on PATH
    install_hint = ""

    def find(self):
        return os.environ.get(self.env) or which(self.program)

    def session_args(self):
        """The session's arguments after the program."""
        return []

    def batch_command(self, program, line):
        """A shell command that runs line once and exits."""
        raise NotImplementedError

    def run_line(self, path, diary, start, done):
        raise NotImplementedError

    def parse_report(self, text, run_file, project):
        """The errors in the diary text between the markers: {path: [diagnostic, ...]}."""
        raise NotImplementedError

    def diagnostic(self, line, column, message, severity="error"):
        return {"range": [line, column], "severity": severity, "message": message, "source": self.id}


class Matlab(Flavor):
    id = "matlab"
    name = "MATLAB"
    env = "TCODE_MATLAB"
    program = "matlab"
    install_hint = "put matlab on your PATH (or install it in /usr/local/MATLAB)"

    def find(self):
        found = super().find()
        if found:
            return found
        for pattern in ("/usr/local/MATLAB/*/bin/matlab", "/opt/MATLAB/*/bin/matlab", "~/MATLAB/*/bin/matlab"):
            hits = sorted(glob.glob(os.path.expanduser(pattern)))
            if hits:
                return hits[-1]  # the newest release
        return None

    def session_args(self):
        return ["-nodesktop", "-nosplash"]

    def batch_command(self, program, line):
        return f"{shlex.quote(program)} -batch {shlex.quote(line)}"

    def run_line(self, path, diary, start, done):
        folder, script = os.path.split(path)
        return (f"cd({quoted(folder)}); diary({quoted(diary)}); disp({quoted(start)}); "
                f"try, run({quoted(script)}); catch tcode_err, fprintf(2, '%s\\n', getReport(tcode_err, 'extended', "
                f"'hyperlinks', 'off')); end; disp({quoted(done)}); diary off; clear tcode_err")

    def parse_report(self, text, run_file, project):
        """"Error in foo (line 12)" frames under the message, "Error: File: ... Line: 3 Column: 5" for
        a syntax error."""
        found = {}
        lines = [line.rstrip() for line in text.split("\n")]
        message, first = None, True
        for i, line in enumerate(lines):
            syntax = re.match(r"Error: File: (.+?) Line: (\d+) Column: (\d+)", line)
            if syntax:
                following = next((l for l in lines[i + 1:] if l.strip()), "syntax error")
                path = syntax.group(1) if os.path.isabs(syntax.group(1)) else os.path.join(project, syntax.group(1))
                found.setdefault(path, []).append(
                    self.diagnostic(int(syntax.group(2)), int(syntax.group(3)), following.strip()))
                first = False
                continue
            frame = re.match(r"Error in (\S+) \(line (\d+)\)", line)
            if frame:
                path = find_function(frame.group(1), project, run_file)
                if path:
                    found.setdefault(path, []).append(
                        self.diagnostic(int(frame.group(2)), 1, message or "error", "error" if first else "info"))
                    if first:
                        message = f"{message or 'error'} (called from here)"
                    first = False
                continue
            if message is None and line.strip() and not line.startswith("Error using "):
                message = line.strip()
        return found


class Octave(Flavor):
    id = "octave"
    name = "Octave"
    env = "TCODE_OCTAVE"
    program = "octave"
    install_hint = "install GNU Octave (octave on your PATH)"

    def session_args(self):
        return ["--no-gui", "--quiet"]

    def batch_command(self, program, line):
        return f"{shlex.quote(program)} --no-gui --quiet --eval {shlex.quote(line)}"

    def run_line(self, path, diary, start, done):
        """Octave has no getReport and its diary leaves stderr out: the error and its stack go to
        stdout, like Octave's own report plus each frame's file."""
        folder, script = os.path.split(path)
        return (f"cd({quoted(folder)}); diary({quoted(diary)}); disp({quoted(start)}); "
                f"try, run({quoted(script)}); catch tcode_err, printf('error: %s\\n', tcode_err.message); "
                f"for tcode_k = 1:numel(tcode_err.stack), printf('    %s at line %d column %d (%s)\\n', "
                f"tcode_err.stack(tcode_k).name, tcode_err.stack(tcode_k).line, tcode_err.stack(tcode_k).column, "
                f"tcode_err.stack(tcode_k).file); end; end; disp({quoted(done)}); diary off; clear tcode_err tcode_k")

    def parse_report(self, text, run_file, project):
        """"error: <message>" and its frames "    helper at line 2 column 3 (/p/lib/helper.m)"; a syntax
        error names its file in the message. Frames outside the project (Octave's own) are left out."""
        found = {}
        message, frames = None, []
        for line in text.split("\n"):
            line = line.rstrip()
            error = re.match(r"error: (.*)", line)
            if error and message is None:
                message = error.group(1)
                continue
            frame = re.match(r"\s+\S+ at line (\d+) column (\d+) \((.+)\)$", line)
            if frame:
                frames.append((frame.group(3), int(frame.group(1)), int(frame.group(2))))
        if message is None:
            return found
        syntax = re.search(r"(?:syntax|parse) error near line (\d+)(?:, column (\d+))? (?:in|of) file (.+)$", message)
        if syntax:
            path = syntax.group(3).strip()
            found[path] = [self.diagnostic(int(syntax.group(1)), int(syntax.group(2) or 1), "syntax error")]
            return found
        inside = os.path.join(project, "")
        first = True
        for path, line, column in frames:
            if path != run_file and not path.startswith(inside):
                continue
            text = message if first else f"{message} (called from here)"
            found.setdefault(path, []).append(self.diagnostic(line, column, text, "error" if first else "info"))
            first = False
        return found


class Session:
    """The plugin's commands for one flavor."""

    def __init__(self, flavor):
        self.flavor = flavor

    # State: the tab ids, per plugin token ----------------------------------------------------------

    def runtime_dir(self):
        folder = os.path.join(os.environ.get("XDG_RUNTIME_DIR") or tempfile.gettempdir(), f"t-code-{self.flavor.id}")
        os.makedirs(folder, mode=0o700, exist_ok=True)
        return folder

    def state_file(self):
        token = os.environ.get("TCODE_PLUGIN_TOKEN", "")
        return os.path.join(self.runtime_dir(), hashlib.sha256(token.encode()).hexdigest()[:16] + ".json")

    def load_state(self):
        try:
            with open(self.state_file()) as f:
                state = json.load(f)
                return state if isinstance(state, dict) else {}
        except (OSError, ValueError):
            return {}

    def load_tab(self, key="tab"):
        """The terminal tab id kept under key: "tab" for the session, "batch" for Run in Batch's shell."""
        return self.load_state().get(key)

    def save_tab(self, tab, key="tab"):
        state = self.load_state()
        state[key] = tab
        with open(self.state_file(), "w") as f:
            json.dump(state, f)

    # The session -----------------------------------------------------------------------------------

    def find(self, t):
        program = self.flavor.find()
        if not program:
            t.request("say", f"{self.flavor.name} not found: {self.flavor.install_hint}")
        return program

    def open_session(self, t):
        """Opens the session's terminal tab: its id, or None when the program is missing."""
        program = self.find(t)
        if not program:
            return None
        args = " ".join([json.dumps(program)] + self.flavor.session_args())
        tab = t.request("terminal-open", f"{self.flavor.name} -- {args}")
        t.request("terminal-close-text", f"{tab} exit")
        self.save_tab(tab)
        return tab

    @staticmethod
    def tab_open(t, tab):
        """Whether the plugin's tab is still there (a request only an open tab of ours answers ok)."""
        try:
            t.request("terminal-close-text", f"{tab} exit")
            return True
        except TcodeError as e:
            if e.code != "bad-argument":
                raise
            return False

    def close_session(self, t):
        """Types exit into the session and waits for its tab to close by itself (history saved, a
        license freed); after QUIT_SECONDS the tab is closed. False when none was open."""
        tab = self.load_tab()
        self.save_tab(None)
        if not tab or not self.tab_open(t, tab):
            return False
        t.request("terminal-send", f"{tab} exit")
        deadline = time.monotonic() + QUIT_SECONDS
        while time.monotonic() < deadline:
            time.sleep(0.3)
            if not self.tab_open(t, tab):
                return True
        try:
            t.request("terminal-close", tab)
        except TcodeError:
            pass
        return True

    def send(self, t, line):
        """Types line into the session, opening one when there is none (or it was closed). False when
        the program is missing."""
        tab = self.load_tab()
        if tab:
            try:
                t.request("terminal-send", f"{tab} {line}")
                return True
            except TcodeError as e:
                if e.code != "bad-argument":  # gone: closed, or the plugins were reloaded
                    raise
        tab = self.open_session(t)
        if tab:
            t.request("terminal-send", f"{tab} {line}")
        return tab is not None

    # Runs ------------------------------------------------------------------------------------------

    @staticmethod
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

    def report_errors(self, path, diary, start, done):
        """Waits for the run's end marker in the diary, then hands the errors to t-code (on a new
        connection: the wait may be long) and says how the run went."""
        report = self.wait_for(diary, start, done)
        try:
            os.unlink(diary)
        except OSError:
            pass
        if report is None:
            return
        t = Tcode()
        found = self.flavor.parse_report(report, path, t.request("get-project"))
        for file, items in found.items():
            t.json("diagnostics-set", {"path": file, "items": items})
        errors = sum(1 for items in found.values() for d in items if d["severity"] == "error")
        how = "stopped with an error" if errors else "ran fine"
        t.request("say", f"{self.flavor.name}: {os.path.basename(path)} {how}")

    def prepare(self, t):
        """The current file saved and its diagnostics cleared, with a new diary and markers:
        (path, diary, start, done), or None when it is no .m file."""
        path = t.request("get-file")
        if not path.endswith(".m"):
            t.request("say", f"{self.flavor.name} runs .m files: open one first")
            return None
        t.request("save")
        t.request("diagnostics-clear")
        diary = os.path.join(self.runtime_dir(), f"{uuid.uuid4().hex}.diary")
        return path, diary, f"__tcode_start_{uuid.uuid4().hex[:8]}__", f"__tcode_done_{uuid.uuid4().hex[:8]}__"

    def run_file(self, t):
        run = self.prepare(t)
        if not run or not self.send(t, self.flavor.run_line(*run)):
            return
        t.close()  # the wait may be long: no connection held meanwhile
        self.report_errors(*run)

    def run_batch(self, t):
        """The file run once (no session: output only) in the plugin's shell tab, then its errors as
        diagnostics like Run's."""
        program = self.find(t)
        run = program and self.prepare(t)
        if not run:
            return
        line = self.flavor.batch_command(program, self.flavor.run_line(*run))
        tab = self.load_tab("batch")
        if tab and self.tab_open(t, tab):
            t.request("terminal-send", f"{tab} {line}")  # its shell is at its prompt by now
        else:  # the line as the new tab's program (a shell just started may drop what is typed), then a shell
            shell = shlex.quote(f'{line}; exec "${{SHELL:-sh}}"')
            self.save_tab(t.request("terminal-open", f"{self.flavor.name} Batch -- sh -c {shell}"), "batch")
        t.close()
        self.report_errors(*run)

    def run_selection(self, t):
        text = t.text("get-selection")
        if not text.strip():
            t.request("say", f"Select some {self.flavor.name} code first")
            return
        for line in text.split("\n"):
            if line.strip() and not self.send(t, line):
                return

    def stop(self, t):
        tab = self.load_tab()
        try:
            if tab:
                t.request("terminal-interrupt", tab)
                return
        except TcodeError:
            self.save_tab(None)
        t.request("say", f"No {self.flavor.name} session is open")

    def main(self, command):
        t = Tcode()
        try:
            if command == "run":
                self.run_file(t)
            elif command == "selection":
                self.run_selection(t)
            elif command == "batch":
                self.run_batch(t)
            elif command == "session":
                self.open_session(t)
            elif command == "close":
                if not self.close_session(t):
                    t.request("say", f"No {self.flavor.name} session is open")
            elif command == "restart":
                self.close_session(t)  # frees what the session's graphics held
                if self.open_session(t):
                    t.request("say", f"{self.flavor.name}: started again")
            elif command == "stop":
                self.stop(t)
        except TcodeError as e:
            if e.code == "no-editor":
                t.request("say", "Open a .m file first")
            else:
                t.request("say", f"{self.flavor.name} plugin: {e.message}")


def run(flavor):
    """The plugin's entry point: the command is its first argument."""
    try:
        Session(flavor).main(sys.argv[1] if len(sys.argv) > 1 else "")
    except (BrokenPipeError, ConnectionError):
        pass
