#!/usr/bin/env python3
"""ShellCheck for t-code. Started with the project ($1 = "start"), it listens for files being opened
and saved; a shell script (.sh, .bash, .ksh, .bats, or a #! line naming sh / bash / dash / ksh) goes
through `shellcheck -f json1` and its findings become diagnostics (SC2086 and friends). "check" checks
the active file once."""
import json
import os
import re
import shutil
import subprocess
import sys

sys.dont_write_bytecode = True  # no __pycache__ in the plugin's folder
from tcode import Tcode, TcodeError  # noqa: E402

EXTENSIONS = (".sh", ".bash", ".ksh", ".bats")
SHEBANG = re.compile(r"^#!\s*\S*(/|env\s+)(ba|da|k)?sh\b")
SEVERITY = {"error": "error", "warning": "warning", "info": "info", "style": "hint"}


def is_script(path):
    if path.endswith(EXTENSIONS):
        return True
    try:
        with open(path, "rb") as f:
            first = f.readline(200).decode("utf-8", "replace")
    except OSError:
        return False
    return bool(SHEBANG.match(first))


def character_column(line, column):
    """ShellCheck counts a tab as 8 columns; t-code counts characters."""
    visual = 1
    for index, char in enumerate(line):
        if visual >= column:
            return index + 1
        visual = (visual + 8 - (visual - 1) % 8) if char == "\t" else visual + 1
    return len(line) + 1


def diagnostics(path, findings):
    """ShellCheck's json1 comments as t-code diagnostics."""
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            lines = f.read().split("\n")
    except OSError:
        lines = []

    def place(line, column):
        text = lines[line - 1] if 0 < line <= len(lines) else ""
        return [line, character_column(text, column)]

    items = []
    for c in findings.get("comments", []):
        items.append({
            "range": place(c["line"], c["column"]) + place(c.get("endLine", c["line"]), c.get("endColumn", c["column"])),
            "severity": SEVERITY.get(c.get("level"), "warning"),
            "message": c.get("message", ""),
            "code": f"SC{c['code']}",
            "source": "shellcheck",
        })
    return items


def check(t, path, told):
    """Checks path; told: whether "not installed" was said already (said once per run)."""
    program = shutil.which("shellcheck")
    if not program:
        if not told:
            t.request("say", "ShellCheck is not installed (dnf / apt / pacman install shellcheck): nothing to check")
        return True
    folder = os.path.dirname(path)
    result = subprocess.run([program, "-f", "json1", "-x", os.path.basename(path)], cwd=folder, capture_output=True,
                            text=True, timeout=60)
    try:
        findings = json.loads(result.stdout or "{}")
    except ValueError:
        t.request("say", f"ShellCheck said something odd: {result.stderr.strip()[:100]}")
        return told
    t.json("diagnostics-set", {"path": path, "items": diagnostics(path, findings)})
    return told


def listen():
    requests, told = Tcode(), False
    active = requests.request("get-file")  # opened before this started: its did_open is gone
    if active and is_script(active):
        told = check(requests, active, told)
    for event in Tcode().events():
        path = event.get("path") or ""
        if event.get("event") in ("did_open", "did_save") and path and is_script(path):
            told = check(requests, path, told)
        elif event.get("event") == "did_close" and path:
            requests.request("diagnostics-clear", path)


def check_active():
    t = Tcode()
    path = t.request("get-file")
    if not path:
        t.request("say", "Open a shell script first")
    elif not is_script(path):
        t.request("say", "That does not look like a shell script (no .sh, no #!/bin/sh)")
    else:
        t.request("save")
        check(t, path, False)
        t.request("say", "ShellCheck had a look")


if __name__ == "__main__":
    try:
        if sys.argv[1:2] == ["start"]:
            listen()
        else:
            check_active()
    except (TcodeError, BrokenPipeError, ConnectionError):
        pass  # t-code went away or the project closed: nothing left to do
