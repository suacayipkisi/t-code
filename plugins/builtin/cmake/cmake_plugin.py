#!/usr/bin/env python3
"""CMake for t-code: configure presets, Configure with CMake's errors as diagnostics, Build and Clean
in a terminal tab (the plugin's own). The language server is plugin.json's lsp entry.

Configure runs cmake itself and reads its output ("CMake Error at sub/CMakeLists.txt:4 (foo):" and
the indented lines under it). Each command is its own run of this program; the chosen preset, the
build folder CMake reported and the terminal tab's id are kept in $XDG_RUNTIME_DIR, per project."""
import hashlib
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True  # no __pycache__ in the plugin's folder
from tcode import Tcode, TcodeError  # noqa: E402

HEADER = re.compile(r"^CMake (Error|Warning|Deprecation Warning|Deprecation Error)(?: \(dev\))? at (.+?):(\d+)"
                    r"(?: \((\w+)\))?:$")
WRITTEN = re.compile(r"^-- Build files have been written to: (.+)$")
PRESET = re.compile(r'^\s+"(.+?)"(?: - (.*))?$')
# Lines CMake adds under a message that are not part of it.
TRAILERS = ("Call Stack (most recent call first):", "This warning is for project developers.",
            "This error is for project developers.")


def state_file(project):
    folder = os.path.join(os.environ.get("XDG_RUNTIME_DIR") or tempfile.gettempdir(), "t-code-cmake")
    os.makedirs(folder, mode=0o700, exist_ok=True)
    return os.path.join(folder, hashlib.sha256(project.encode()).hexdigest()[:16] + ".json")


def load_state(project):
    try:
        with open(state_file(project)) as f:
            state = json.load(f)
            return state if isinstance(state, dict) else {}
    except (OSError, ValueError):
        return {}


def save_state(project, state):
    with open(state_file(project), "w") as f:
        json.dump(state, f)


def parse_output(text, source_dir):
    """CMake's messages with a place: ({path: [diagnostic, ...]}, [messages without a place])."""
    found, general = {}, []
    lines = text.split("\n")
    i = 0
    while i < len(lines):
        line = lines[i].rstrip()
        i += 1
        if line.startswith("CMake Error: "):
            general.append(line[len("CMake Error: "):])
            continue
        header = HEADER.match(line)
        if not header:
            continue
        body = []
        while i < len(lines) and (not lines[i].strip() or lines[i].startswith(" ")):
            part = lines[i].strip()
            i += 1
            if part in TRAILERS:  # the call stack, then the end of the message
                while i < len(lines) and (not lines[i].strip() or lines[i].startswith(" ")):
                    i += 1
                break
            if part:
                body.append(part)
        kind, path, number, command = header.groups()
        path = path if os.path.isabs(path) else os.path.join(source_dir, path)
        message = " ".join(body) or (f"{command}() failed" if command else kind)
        found.setdefault(os.path.normpath(path), []).append(
            {"range": [int(number), 1], "severity": "error" if "Error" in kind else "warning", "message": message,
             "source": "cmake"})
    return found, general


def parse_presets(text):
    """`cmake --list-presets=configure`: [(name, display name or None), ...]."""
    return [(m.group(1), m.group(2)) for m in map(PRESET.match, text.split("\n")) if m]


def configure_argv(project, state):
    if state.get("preset"):
        return ["cmake", "-S", project, "--preset", state["preset"]]
    return ["cmake", "-S", project, "-B", os.path.join(project, "build"), "-DCMAKE_BUILD_TYPE=Debug",
            "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"]


def build_dir(project, state):
    return state.get("build_dir") or os.path.join(project, "build")


def has_cmake_project(t, project):
    if not os.path.exists(os.path.join(project, "CMakeLists.txt")):
        t.request("say", "No CMakeLists.txt in the project folder")
        return False
    if not shutil.which("cmake"):
        t.request("say", "cmake not found: install CMake first")
        return False
    return True


def configure(t, project):
    state = load_state(project)
    t.request("say", f"CMake: configuring{' (' + state['preset'] + ')' if state.get('preset') else ''}…")
    t.close()  # configuring may take a while: no connection held meanwhile
    result = subprocess.run(configure_argv(project, state), cwd=project, capture_output=True, text=True,
                            errors="replace")
    output = result.stdout + result.stderr
    for line in output.split("\n"):
        written = WRITTEN.match(line)
        if written:
            state["build_dir"] = written.group(1).strip()
    save_state(project, state)
    found, general = parse_output(output, project)

    t = Tcode()
    t.request("diagnostics-clear")
    for path, items in found.items():
        t.json("diagnostics-set", {"path": path, "items": items})
    errors = sum(1 for items in found.values() for d in items if d["severity"] == "error") + len(general)
    if result.returncode == 0:
        t.request("say", f"CMake: configured in {os.path.relpath(build_dir(project, state), project)}")
    elif general and not found:
        t.request("say", f"CMake: {general[0]}")
    else:
        t.request("say", f"CMake: configuring failed ({errors} error{'s' if errors != 1 else ''})")
    return t


def select_preset(t, project):
    result = subprocess.run(["cmake", "--list-presets=configure"], cwd=project, capture_output=True, text=True,
                            errors="replace")
    presets = parse_presets(result.stdout)
    if not presets:
        t.request("say", "No configure presets (CMakePresets.json) in this project")
        return
    state = load_state(project)
    items = [{"label": "No preset", "detail": "build/, Debug"}]
    items += [{"label": name, "detail": shown or ""} for name, shown in presets]
    try:
        index = int(t.json("pick", {"title": "Configure Preset", "items": items}).split(" ", 1)[0])
    except TcodeError as e:
        if e.code == "canceled":
            return
        raise
    state["preset"] = presets[index - 1][0] if index > 0 else None
    state.pop("build_dir", None)  # the next Configure says where the preset builds
    save_state(project, state)
    configure(t, project)


def in_terminal(t, project, line):
    """Types line into the plugin's "CMake" tab (a shell, so it stays after the command), opening one
    when there is none or it was closed."""
    state = load_state(project)
    if state.get("tab"):
        try:
            t.request("terminal-send", f"{state['tab']} {line}")
            return
        except TcodeError as e:
            if e.code != "bad-argument":  # gone: closed, or the plugins were reloaded
                raise
    state["tab"] = t.request("terminal-open", "CMake")
    save_state(project, state)
    t.request("terminal-send", f"{state['tab']} {line}")


def build(t, project, target=None):
    state = load_state(project)
    folder = build_dir(project, state)
    if not os.path.exists(os.path.join(folder, "CMakeCache.txt")):
        t = configure(t, project)
        state = load_state(project)
        folder = build_dir(project, state)
        if not os.path.exists(os.path.join(folder, "CMakeCache.txt")):
            return
    command = f"cmake --build {shlex.quote(folder)}" + (f" --target {target}" if target else "")
    in_terminal(t, project, command)


def main(command):
    t = Tcode()
    try:
        project = t.request("get-project")
        if not has_cmake_project(t, project):
            return
        if command == "configure":
            configure(t, project)
        elif command == "preset":
            select_preset(t, project)
        elif command == "build":
            build(t, project)
        elif command == "clean":
            build(t, project, "clean")
    except TcodeError as e:
        t.request("say", f"CMake plugin: {e.message}")


if __name__ == "__main__":
    try:
        main(sys.argv[1] if len(sys.argv) > 1 else "")
    except (BrokenPipeError, ConnectionError):
        pass
