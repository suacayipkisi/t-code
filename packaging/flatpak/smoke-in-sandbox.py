#!/usr/bin/env python3
"""Runs through stdin inside the installed application's normal Flatpak sandbox."""

import os
from pathlib import Path
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time


def require(condition, message):
    if not condition:
        raise RuntimeError(message)
    print(f"ok: {message}", flush=True)


def run(*args, **kwargs):
    return subprocess.run(args, check=True, text=True, capture_output=True, **kwargs)


def wait_for(predicate, process, message):
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"t-code exited early with status {process.returncode}: {message}")
        if predicate():
            print(f"ok: {message}", flush=True)
            return
        time.sleep(0.1)
    raise RuntimeError(f"Timed out: {message}")


def clangd_running():
    for entry in Path("/proc").iterdir():
        if not entry.name.isdecimal():
            continue
        try:
            command = (entry / "cmdline").read_bytes().split(b"\0", 1)[0]
            if command and os.path.basename(command) == b"clangd":
                return True
        except (FileNotFoundError, PermissionError, ProcessLookupError):
            continue
    return False


def smoke(version, work):
    require(os.environ.get("FLATPAK_ID") == "io.github.suacayipkisi.tcode", "running inside io.github.suacayipkisi.tcode")
    require(run("t-code", "--version").stdout.strip() == f"t-code {version}", "installed version matches metadata")
    for path in (
        "/app/share/t-code/styles/dark-classic.xml",
        "/app/share/icons/hicolor/scalable/apps/io.github.suacayipkisi.tcode.svg",
        "/app/share/applications/io.github.suacayipkisi.tcode.desktop",
        "/app/share/metainfo/io.github.suacayipkisi.tcode.metainfo.xml",
    ):
        require(Path(path).is_file(), f"installed {path}")
    for command in ("clangd", "git", "gcc", "g++", "make", "cmake", "meson", "ninja", "gdb", "pkg-config"):
        require(shutil.which(command) is not None, f"sandbox provides {command}")
    run("clangd", "--version")

    project = work / "project"
    project.mkdir()
    main = project / "main.c"
    main.write_text('#include <stdio.h>\nint main(void) { puts("flatpak smoke"); return 0; }\n')
    other_files = []
    for name in ("a", "b", "c", "d"):
        path = project / f"{name}.c"
        path.write_text(f"int {name}(void) {{ return 0; }}\n")
        other_files.append(path)
    markdown = project / "README.md"
    markdown.write_text("# Flatpak smoke\n\nA native Markdown preview.\n")
    (project / "Makefile").write_text("all:\n\t$(CC) -g -o smoke main.c\n")
    run("git", "init", "--quiet", str(project))
    run("make", "--no-print-directory", "-C", str(project))
    require(run(str(project / "smoke")).stdout.strip() == "flatpak smoke", "SDK compiles and runs a C program")
    debugger = run("gdb", "--batch", "-nx", "-ex", "run", "--args", str(project / "smoke"), env={**os.environ, "LC_ALL": "C"})
    require("exited normally" in debugger.stdout, "GDB can debug a sandbox child process")

    log_path = work / "t-code.log"
    process = None
    try:
        with log_path.open("w+") as log:
            process = subprocess.Popen(
                ["t-code", str(project), *(str(path) for path in other_files), str(main)],
                cwd=project,
                stdout=log,
                stderr=subprocess.STDOUT,
            )
            control = Path(os.environ["XDG_RUNTIME_DIR"]) / f"t-code-{process.pid}.sock"
            wait_for(control.is_socket, process, "editor launches and creates its private control socket")
            wait_for(clangd_running, process, "editor starts clangd for the opened C files")
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
                client.settimeout(5)
                client.connect(str(control))
                client.sendall(b"preview " + os.fsencode(markdown) + b"\0")
            time.sleep(1)
            require(process.poll() is None, "editor stays running after the Markdown preview request")
            process.terminate()
            require(process.wait(timeout=10) == 0, "editor exits cleanly on SIGTERM")
            require(not control.exists(), "editor removes its control socket")
        text = log_path.read_text()
        require(re.search(r"CRITICAL|[Ss]egmentation fault|segfault", text) is None, "editor logs contain no GTK criticals or crashes")
    except BaseException:
        if log_path.exists():
            print("--- t-code log ---", file=sys.stderr)
            print(log_path.read_text(), file=sys.stderr)
        raise
    finally:
        if process is not None and process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("Expected the packaged version as the sole argument")
    with tempfile.TemporaryDirectory(prefix="flatpak-smoke-", dir=os.environ["XDG_CACHE_HOME"]) as directory:
        smoke(sys.argv[1], Path(directory))
    print("All Flatpak smoke checks passed", flush=True)
