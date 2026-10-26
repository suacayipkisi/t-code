"""Run through the installed app's normal command chain and confinement profile."""

import configparser
import os
from pathlib import Path
import shutil
import socket
import subprocess
import tempfile
import time


SNAP = Path(os.environ["SNAP"])
APP = SNAP / "usr/bin/t-code"
VERSION = os.environ["SNAP_VERSION"]


def require(condition, message):
    if not condition:
        raise RuntimeError(message)
    print(f"ok: {message}", flush=True)


def run(*args, cwd=None):
    result = subprocess.run(args, cwd=cwd, text=True, capture_output=True, timeout=60)
    if result.returncode:
        detail = (result.stderr or result.stdout).strip()
        raise RuntimeError(f"command {args!r} exited {result.returncode}: {detail}")
    return result.stdout


def request(path, command, terminator=None):
    with socket.socket(socket.AF_UNIX) as client:
        client.settimeout(5)
        client.connect(str(path))
        client.sendall(command.encode() + b"\n")
        if terminator is None:
            return ""
        result = bytearray()
        while not result.endswith(terminator):
            chunk = client.recv(65536)
            if not chunk:
                raise RuntimeError(f"socket closed before reply to {command}")
            result.extend(chunk)
        return result[: -len(terminator)].decode()


def wait_for(predicate, app, message):
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        if app.poll() is not None:
            raise RuntimeError(f"t-code exited with {app.returncode}: {message}")
        if predicate():
            return
        time.sleep(0.1)
    raise RuntimeError(f"timed out: {message}")


def clangd_children(pid):
    found = []
    for entry in Path("/proc").iterdir():
        if not entry.name.isdecimal():
            continue
        try:
            status = (entry / "status").read_text()
            parent = next(line.split()[1] for line in status.splitlines() if line.startswith("PPid:"))
            command = (entry / "cmdline").read_bytes().split(b"\0")[0].decode()
            if int(parent) == pid and Path(command).name.startswith("clangd"):
                found.append(entry)
        except (OSError, StopIteration, UnicodeError):
            continue
    return found


def main():
    require(run(str(APP), "--version").strip() == f"t-code {VERSION}", "installed version")
    for name in ("clangd", "git", "cc", "c++", "clang", "make", "cmake", "meson", "ninja", "pkgconf", "gdb", "python3", "rg"):
        tool = shutil.which(name)
        require(tool is not None and Path(tool).resolve().is_relative_to(SNAP), f"bundled {name}")
    for name in ("cc", "c++", "gcc", "g++", "clang", "clang++"):
        tool = shutil.which(name)
        require(tool is not None and Path(tool).resolve() == SNAP / "bin/gcc", f"{name} selects the sysroot wrapper")
    desktop = configparser.ConfigParser(interpolation=None)
    desktop.read(SNAP / "usr/share/applications/io.github.suacayipkisi.tcode.desktop")
    require(desktop["Desktop Entry"]["Exec"] == "t-code-ide %F", "desktop file opening")
    require(desktop["Desktop Entry"]["StartupWMClass"] == "io.github.suacayipkisi.tcode", "desktop app ID")
    require((SNAP / "usr/share/icons/hicolor/scalable/apps/io.github.suacayipkisi.tcode.svg").is_file(), "packaged icon")
    require((SNAP / "usr/share/t-code/styles/dark-classic.xml").is_file(), "packaged editor style")
    require(Path(os.environ["XDG_CONFIG_HOME"]).is_relative_to(Path(os.environ["SNAP_USER_COMMON"])), "private plugin configuration")
    runtime = Path(os.environ["XDG_RUNTIME_DIR"])
    require(runtime.name == "snap.t-code-ide", "snap-specific XDG runtime socket directory")

    # A nonhidden host-home project exercises the actual home interface, including nested .git.
    home = Path(os.environ["SNAP_REAL_HOME"])
    with tempfile.TemporaryDirectory(prefix="t-code-snap-smoke-", dir=home) as temporary:
        project = Path(temporary) / "project"
        project.mkdir()
        source = project / "main.c"
        source.write_text('#include <stdio.h>\nint main(void) { puts("snap-c"); return 0; }\n')
        cpp = project / "main.cpp"
        cpp.write_text('#include <iostream>\nint main() { std::cout << "snap-cpp\\n"; }\n')
        run("cc", "-std=c23", str(source), "-o", str(project / "main"))
        require(run(str(project / "main")).strip() == "snap-c", "compile and run C with bundled libc headers")
        run("c++", str(cpp), "-o", str(project / "main-cpp"))
        require(run(str(project / "main-cpp")).strip() == "snap-cpp", "compile and run C++ with bundled standard library")
        run("clang", "-std=c23", str(source), "-o", str(project / "main-clang"))
        require(run(str(project / "main-clang")).strip() == "snap-c", "compile and run with bundled Clang")
        run("git", "init", "-q", str(project))
        run("git", "-C", str(project), "add", "main.c")
        run("git", "-C", str(project), "-c", "user.name=Snap smoke", "-c", "user.email=smoke@example.invalid", "commit", "-qm", "Smoke fixture")
        require(bool(run("git", "-C", str(project), "rev-parse", "HEAD").strip()), "Git operates on nested project .git")
        (project / "Makefile").write_text("all:\n\t$(CC) main.c -o make-main\n")
        run("make", "-C", str(project))
        require(run(str(project / "make-main")).strip() == "snap-c", "Make builds with bundled compiler")
        (project / "CMakeLists.txt").write_text("cmake_minimum_required(VERSION 3.22)\nproject(snap_smoke C)\nadd_executable(cmake-main main.c)\n")
        run("cmake", "-S", str(project), "-B", str(project / "cmake-build"), "-G", "Ninja")
        run("cmake", "--build", str(project / "cmake-build"))
        require(run(str(project / "cmake-build/cmake-main")).strip() == "snap-c", "CMake and Ninja build")
        (project / "meson.build").write_text("project('snap-smoke', 'c')\nexecutable('smoke-meson', 'main.c')\n")
        run("meson", "setup", str(project / "meson-build"), str(project))
        run("meson", "compile", "-C", str(project / "meson-build"))
        require(run(str(project / "meson-build/smoke-meson")).strip() == "snap-c", "Meson uses bundled Python")
        require("snap-python-ok" in run("gdb", "--batch", "-ex", "python print('snap-python-ok')"), "GDB has working embedded Python (not ptrace)")

        preview = project / "README.md"
        preview.write_text("# Snap smoke\n\nNative Markdown preview.\n")
        log = project / "app.log"
        with log.open("w+") as output:
            app = subprocess.Popen([str(APP), str(project), str(source)], cwd=project, stdout=output, stderr=subprocess.STDOUT)
            control = runtime / f"t-code-{app.pid}.sock"
            try:
                wait_for(control.exists, app, "control socket creation")
                wait_for(lambda: request(control, "get-file", b"\n") == str(source), app, "file argument opens in editor")
                require(request(control, "get-project", b"\n") == str(project), "project argument opens")
                require(request(control, "get-text", b"\0") == source.read_text(), "control socket reads editor buffer")
                request(control, "append // saved from snap smoke")
                request(control, "save")
                wait_for(lambda: "// saved from snap smoke" in source.read_text(), app, "socket saves project file")
                wait_for(lambda: len(clangd_children(app.pid)) == 1, app, "one bundled clangd starts")
                require(len(clangd_children(app.pid)) == 1, "one clangd serves opened C file")
                request(control, f"preview {preview}")
                request(control, "say Snap smoke passed")
                require(request(control, "get-project", b"\n") == str(project), "Markdown preview leaves IPC responsive")
                app.terminate()
                require(app.wait(timeout=10) == 0, "clean SIGTERM shutdown")
                require(not control.exists(), "control socket removed on shutdown")
                output.seek(0)
                text = output.read()
                require(not any(word in text for word in ("CRITICAL", "Segmentation fault", "cannot listen")), "no GTK criticals or socket setup errors")
            except BaseException:
                output.flush()
                print(log.read_text(), flush=True)
                raise
            finally:
                if app.poll() is None:
                    app.terminate()
                    try:
                        app.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        app.kill()
                        app.wait()
    print("Snap smoke passed", flush=True)


if __name__ == "__main__":
    main()
