#!/usr/bin/env python3
"""t-code-jdtls: starts the Eclipse JDT Language Server (jdtls) for t-code's Java plugin.

Java: Install Language Server copies this file to ~/.local/bin/t-code-jdtls, the program the
plugin's lsp entry names (so t-code shows the server as "not found" until then). It runs in the
project's folder, speaks LSP on stdin / stdout and needs nothing but Python 3's standard library.

    jdtls       $TCODE_JDTLS_HOME, the plugin's download (~/.local/share/t-code/jdtls/current),
                then the usual installs (Arch's /usr/share/java/jdtls, Homebrew, Mason, a jdtls
                on PATH)
    Java        the first of $TCODE_JDTLS_JAVA, $JAVA_HOME, java on PATH that is 21 or newer, else
                the newest one in /usr/lib/jvm
    workspace   ~/.cache/t-code-java/workspace/<project>-<hash>: jdtls' index of the project,
                emptied at the next start after Java: Clean Language Server Workspace
    memory      the serial collector and a 1 GB heap at most; $TCODE_JDTLS_JVM_ARGS replaces the JVM
                options below (a big project may want -Xmx2G)
    Lombok      a project whose build file names lombok gets the Lombok jar from ~/.m2 or
                ~/.gradle as a Java agent, so jdtls sees its generated methods
    folders     a project without Maven or Gradle gets "triggerFiles" in initialize: one file per
                source folder (src/ for src/com/example/App.java in package com.example), from
                which jdtls learns where the packages start; t-code's options are the same for
                every project. The launcher reads initialize, adds them, and execs jdtls with a
                cat passing the rest of stdin on.
"""
import glob
import json
import hashlib
import os
import platform
import re
import shlex
import shutil
import sys
import urllib.parse

MIN_JAVA = 21
# The serial collector: on a small project jdtls held 295 MB with it, 800 MB with the parallel one
# (measured on jdtls 1.63, JDK 25), with the same answers.
JVM_ARGS = ["-XX:+UseSerialGC", "-Xms32m", "-Xmx1G", "-Dsun.zip.disableMemoryMapping=true"]
BUILD_FILES = ("pom.xml", "build.gradle", "build.gradle.kts")
SKIP_DIRS = {"target", "build", "out", "bin", "node_modules"}
MAX_FILES = 5000
PACKAGE = re.compile(rb"^\s*package\s+([\w.]+)\s*;", re.M)


def data_home():
    return os.environ.get("XDG_DATA_HOME") or os.path.expanduser("~/.local/share")


def cache_home():
    return os.environ.get("XDG_CACHE_HOME") or os.path.expanduser("~/.cache")


def download_dir():
    """Where Java: Install Language Server unpacks jdtls (one folder per version, "current" the
    one in use)."""
    return os.path.join(data_home(), "t-code", "jdtls")


# jdtls ------------------------------------------------------------------------------------------


def launcher_jar(home):
    jars = sorted(glob.glob(os.path.join(home, "plugins", "org.eclipse.equinox.launcher_*.jar")))
    return jars[-1] if jars else None


def config_name():
    return "config_linux_arm" if platform.machine() in ("aarch64", "arm64") else "config_linux"


def jdtls_homes():
    homes = [os.environ.get("TCODE_JDTLS_HOME"), os.path.join(download_dir(), "current"), "/usr/share/java/jdtls",
             "/usr/lib/jdtls", "/usr/share/jdtls", "/opt/jdtls", "/usr/local/share/jdtls",
             "/home/linuxbrew/.linuxbrew/opt/jdtls/libexec", os.path.join(data_home(), "nvim/mason/packages/jdtls")]
    script = shutil.which("jdtls")
    if script:  # <home>/bin/jdtls
        homes.append(os.path.dirname(os.path.dirname(os.path.realpath(script))))
    return [h for h in homes if h]


def find_jdtls():
    """The folder jdtls is unpacked in (plugins/ and config_linux/ in it), or None."""
    for home in jdtls_homes():
        if launcher_jar(home) and os.path.isdir(os.path.join(home, config_name())):
            return os.path.realpath(home)
    return None


# Java -------------------------------------------------------------------------------------------


def java_version(java):
    """The major version of the JDK java belongs to, from its home's release file; 0 when unknown."""
    home = os.path.dirname(os.path.dirname(os.path.realpath(java)))
    try:
        with open(os.path.join(home, "release")) as f:
            text = f.read()
    except OSError:
        return 0
    match = re.search(r'^JAVA_VERSION="(\d+)(?:\.(\d+))?', text, re.M)
    if not match:
        return 0
    major = int(match.group(1))
    return int(match.group(2) or 0) if major == 1 else major  # "1.8.0" is 8


def java_candidates():
    found = []
    for java in (os.environ.get("TCODE_JDTLS_JAVA"),
                 os.environ.get("JAVA_HOME") and os.path.join(os.environ["JAVA_HOME"], "bin", "java"),
                 shutil.which("java")):
        if java and os.access(java, os.X_OK):
            found.append(java)
    return found


def find_java():
    """(java, its major version): the first preferred one that runs jdtls, else the newest JDK in
    /usr/lib/jvm, else the first preferred one (too old: jdtls will say so); (None, 0) without any."""
    preferred = java_candidates()
    for java in preferred:
        version = java_version(java)
        if version >= MIN_JAVA:
            return java, version
    installed = [j for j in glob.glob("/usr/lib/jvm/*/bin/java") if os.access(j, os.X_OK)]
    best = max(installed, key=java_version, default=None)
    if best and java_version(best) >= MIN_JAVA:
        return best, java_version(best)
    if preferred:
        return preferred[0], java_version(preferred[0])
    return (best, java_version(best)) if best else (None, 0)


# The project ------------------------------------------------------------------------------------


def workspace_dir(project):
    project = os.path.realpath(project)
    name = re.sub(r"[^\w.-]", "_", os.path.basename(project)) or "root"
    return os.path.join(cache_home(), "t-code-java", "workspace",
                        f"{name}-{hashlib.sha256(project.encode()).hexdigest()[:12]}")


def wipe_flag(project):
    """The file whose presence empties the project's workspace at jdtls' next start."""
    return workspace_dir(project) + ".wipe"


def uses_lombok(project):
    for name in BUILD_FILES:
        try:
            with open(os.path.join(project, name), encoding="utf-8", errors="replace") as f:
                if "lombok" in f.read():
                    return True
        except OSError:
            pass
    return False


def lombok_jar():
    home = os.path.expanduser("~")
    jars = glob.glob(os.path.join(home, ".m2/repository/org/projectlombok/lombok/*/lombok-*.jar"))
    jars += glob.glob(os.path.join(home, ".gradle/caches/modules-2/files-2.1/org.projectlombok/lombok/*/*/lombok-*.jar"))
    jars = [j for j in jars if not re.search(r"-(sources|javadoc)\.jar$", j)]

    def version(path):
        return [int(n) for n in re.findall(r"\d+", os.path.basename(path))]

    return max(jars, key=version, default=None)


def trigger_files(project, limit=20):
    """One .java file per source folder of a project without Maven or Gradle (their importers know
    the folders), as URIs; [] for the others."""
    if any(os.path.exists(os.path.join(project, n)) for n in BUILD_FILES + ("settings.gradle", "settings.gradle.kts")):
        return []
    roots, seen = {}, 0
    for folder, dirs, files in os.walk(project):
        dirs[:] = sorted(d for d in dirs if d not in SKIP_DIRS and not d.startswith("."))
        for name in sorted(f for f in files if f.endswith(".java")):
            seen += 1
            path = os.path.join(folder, name)
            try:
                with open(path, "rb") as f:
                    match = PACKAGE.search(f.read(8192))
            except OSError:
                continue
            tail = os.sep + match.group(1).decode().replace(".", os.sep) if match else ""
            root = folder[: -len(tail)] if tail and folder.endswith(tail) else folder
            roots.setdefault(root, path)
        if seen >= MAX_FILES or len(roots) >= limit:
            break
    return ["file://" + urllib.parse.quote(p) for p in roots.values()]


def read_message(fd):
    """One LSP message read from fd, nothing more: (header, body); None at EOF or on a bad header."""
    header = b""
    while not header.endswith(b"\r\n\r\n"):
        byte = os.read(fd, 1)
        if not byte or len(header) > 4096:
            return None
        header += byte
    match = re.search(rb"content-length:\s*(\d+)", header, re.I)
    if not match:
        return None
    body, size = b"", int(match.group(1))
    while len(body) < size:
        part = os.read(fd, size - len(body))
        if not part:
            return None
        body += part
    return header, body


def with_trigger_files(body, files):
    """initialize's body with the files in its initializationOptions; anything else as it was."""
    try:
        message = json.loads(body)
        if message.get("method") != "initialize":
            return body
        params = message.setdefault("params", {})
        options = params.get("initializationOptions") or {}
        options.setdefault("triggerFiles", files)
        params["initializationOptions"] = options
        return json.dumps(message, separators=(",", ":")).encode()
    except (ValueError, AttributeError):
        return body


def exec_with_first(argv, first):
    """Execs argv with stdin being first, then the rest of this stdin (passed on by a cat that is
    not jdtls' child), so jdtls keeps this process id and gets t-code's signals itself."""
    read_end, write_end = os.pipe()
    os.write(write_end, first)  # far less than a pipe's buffer: t-code's initialize is a few KB
    child = os.fork()
    if child == 0:
        if os.fork() == 0:
            os.close(read_end)
            os.dup2(write_end, 1)
            os.close(write_end)
            try:
                os.execvp("cat", ["cat"])
            finally:
                os._exit(127)
        os._exit(0)
    os.waitpid(child, 0)
    os.close(write_end)
    os.dup2(read_end, 0)
    os.close(read_end)
    os.execv(argv[0], argv)


def command(project, home, java):
    """jdtls' command line for project (the folder t-code runs it in)."""
    jvm = shlex.split(os.environ["TCODE_JDTLS_JVM_ARGS"]) if os.environ.get("TCODE_JDTLS_JVM_ARGS") else JVM_ARGS
    agent = lombok_jar() if uses_lombok(project) else None
    config = os.path.join(cache_home(), "t-code-java", "config-" + hashlib.sha256(home.encode()).hexdigest()[:12])
    return [java,
            "-Declipse.application=org.eclipse.jdt.ls.core.id1",
            "-Dosgi.bundles.defaultStartLevel=4",
            "-Declipse.product=org.eclipse.jdt.ls.core.product",
            "-Dosgi.checkConfiguration=true",
            "-Dosgi.sharedConfiguration.area=" + os.path.join(home, config_name()),
            "-Dosgi.sharedConfiguration.area.readOnly=true",
            "-Dosgi.configuration.cascaded=true",
            "-Dlog.level=ERROR",
            *jvm,
            *(["-javaagent:" + agent] if agent else []),
            "--add-modules=ALL-SYSTEM",
            "--add-opens", "java.base/java.util=ALL-UNNAMED",
            "--add-opens", "java.base/java.lang=ALL-UNNAMED",
            "-jar", launcher_jar(home),
            "-configuration", config,
            "-data", workspace_dir(project)]


def main():
    project = os.getcwd()
    home = find_jdtls()
    java, version = find_java()
    if not home:
        sys.exit("t-code-jdtls: jdtls not found: run Java: Install Language Server in t-code")
    if not java:
        sys.exit("t-code-jdtls: no Java found: install a JDK, version 21 or newer")
    if version and version < MIN_JAVA:
        sys.exit(f"t-code-jdtls: jdtls needs Java {MIN_JAVA} or newer, found {version} ({java})")
    if os.path.exists(wipe_flag(project)):
        shutil.rmtree(workspace_dir(project), ignore_errors=True)
        os.unlink(wipe_flag(project))
    os.makedirs(workspace_dir(project), exist_ok=True)
    argv = command(project, home, java)
    files = trigger_files(project)
    message = read_message(0) if files and shutil.which("cat") else None
    if not message:
        os.execv(argv[0], argv)
    body = with_trigger_files(message[1], files)
    exec_with_first(argv, b"Content-Length: %d\r\n\r\n" % len(body) + body)


if __name__ == "__main__":
    main()
