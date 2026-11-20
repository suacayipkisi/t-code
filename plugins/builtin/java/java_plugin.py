#!/usr/bin/env python3
"""Java for t-code: the Eclipse JDT Language Server (jdtls), Run (F5 too), Build, Test and Clean for
Maven, Gradle and plain folders, with compiler errors, failed tests and exceptions as diagnostics.

The language server is plugin.json's lsp entry: ~/.local/bin/t-code-jdtls, this folder's
jdtls_launcher.py, copied there by Java: Install Language Server, which downloads jdtls when it is
not installed already. Started with the project, the plugin offers that install the first time a
.java file opens; with the server ready it exits at once.

Run, Build, Test and Clean type one line into the plugin's "Java" terminal tab: `sh <script>`, a
script in $XDG_RUNTIME_DIR/t-code-java that builds, runs and keeps the build's output and the
program's stderr. The plugin waits for the script's end, then turns javac's and Maven's errors,
failed tests and the exception's stack frames in the project into diagnostics.

Debug (F5 on a .java file, plugin.json's "debug") goes through java-debug, a jdtls bundle (Java:
Install Debugger downloads it from Maven Central; the launcher hands it to jdtls): over the socket's
lsp-command the plugin asks jdtls for the main class, its classpath, a build and a debug adapter's
port, then hands that port to t-code's debugger with debug-start. The program runs in the Debug
panel's terminal (runInTerminal)."""
import hashlib
import json
import os
import re
import shlex
import shutil
import sys
import tarfile
import tempfile
import time
import urllib.request
import uuid

sys.dont_write_bytecode = True  # no __pycache__ in the plugin's folder
import jdtls_launcher as launcher  # noqa: E402
from tcode import Tcode, TcodeError  # noqa: E402

MILESTONES = "https://download.eclipse.org/jdtls/milestones/"
DEBUG_REPOSITORY = "https://repo1.maven.org/maven2/com/microsoft/java/com.microsoft.java.debug.plugin/"
GRADLE_FILES = ("build.gradle", "build.gradle.kts")
SETTINGS_FILES = ("settings.gradle", "settings.gradle.kts")
SKIP_DIRS = {".git", ".hg", ".svn", "target", "build", "out", "bin", ".gradle", ".idea", ".mvn", "node_modules"}
MAX_FILES = 20000
WAIT_SECONDS = 24 * 3600  # a server program may run all day; a newer run or a closed tab ends the wait
PACKAGE = re.compile(r"^\s*package\s+([\w.]+)\s*;", re.M)
MAIN = re.compile(r"\bvoid\s+main\s*\(")
JAVAC = re.compile(r"^(.+?\.java):(\d+): (error|warning): (.*)$")
MAVEN = re.compile(r"^\[(ERROR|WARNING)\] (.+?\.java):\[(\d+),(\d+)\] (.*)$")
SUREFIRE = re.compile(r"^\[ERROR\]\s+([\w$.]+)\.([\w$]+):(\d+)\s+(.*)$")
GRADLE_FAILED = re.compile(r"^(\S+) > (.+) FAILED$")
GRADLE_AT = re.compile(r"^\s+(\S+) at ([\w$]+\.java):(\d+)$")
FRAME = re.compile(r"^\s+at (?:.*/)?([\w.$]+)\.([\w$<>]+)\(([\w$]+\.java):(\d+)\)")
THROWN = re.compile(r"^(?:Exception in thread \"[^\"]*\" |Caused by: )?((?:[\w$]+\.)+[\w$]+)(?:: (.*))?$")
GRADLE_INIT = """\
// Written by t-code's Java plugin: the classpath Run starts the program with.
allprojects {
    pluginManager.withPlugin("java") {
        def mainPath = sourceSets.main.runtimeClasspath
        def testPath = sourceSets.test.runtimeClasspath
        def mainOut = layout.buildDirectory.file("tcode-classpath-main.txt").get().asFile
        def testOut = layout.buildDirectory.file("tcode-classpath-test.txt").get().asFile
        tasks.register("tcodeClasspath") {
            dependsOn "classes", "testClasses"
            doLast {
                mainOut.text = mainPath.asPath
                testOut.text = testPath.asPath
            }
        }
    }
}
"""


# State ------------------------------------------------------------------------------------------


def runtime_dir():
    folder = os.path.join(os.environ.get("XDG_RUNTIME_DIR") or tempfile.gettempdir(), "t-code-java")
    os.makedirs(folder, mode=0o700, exist_ok=True)
    return folder


def state_file(project):
    return os.path.join(runtime_dir(), hashlib.sha256(project.encode()).hexdigest()[:16] + ".json")


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


def settings_file():
    config = os.environ.get("XDG_CONFIG_HOME") or os.path.expanduser("~/.config")
    return os.path.join(config, "t-code-java", "settings.json")


def load_settings():
    try:
        with open(settings_file()) as f:
            settings = json.load(f)
            return settings if isinstance(settings, dict) else {}
    except (OSError, ValueError):
        return {}


def save_settings(settings):
    os.makedirs(os.path.dirname(settings_file()), exist_ok=True)
    with open(settings_file(), "w") as f:
        json.dump(settings, f)


# Sources ----------------------------------------------------------------------------------------


def read(path):
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def package_of(text):
    match = PACKAGE.search(text)
    return match.group(1) if match else ""


def class_of(path, text):
    """A source file's class: "com.example.App" for com/example/App.java with its package line."""
    stem = os.path.splitext(os.path.basename(path))[0]
    package = package_of(text)
    return f"{package}.{stem}" if package else stem


def has_main(text):
    return bool(MAIN.search(text))


def source_root(path, text):
    """The folder the file's package starts in: src/ for src/com/example/App.java in package
    com.example; the file's own folder when the folders do not match the package."""
    folder = os.path.dirname(path)
    package = package_of(text)
    tail = os.sep + package.replace(".", os.sep)
    return folder[: -len(tail)] if package and folder.endswith(tail) else folder


def java_files(top):
    """The .java files under top, build outputs and hidden folders left out (at most MAX_FILES)."""
    found = []
    for folder, dirs, files in os.walk(top):
        dirs[:] = sorted(d for d in dirs if d not in SKIP_DIRS and not d.startswith("."))
        found += [os.path.join(folder, f) for f in sorted(files) if f.endswith(".java")]
        if len(found) >= MAX_FILES:
            break
    return found


def find_up(start, names, stop=None):
    """The nearest folder from start upwards holding one of names (not above stop), or None."""
    folder = start
    while True:
        if any(os.path.exists(os.path.join(folder, n)) for n in names):
            return folder
        parent = os.path.dirname(folder)
        if parent == folder or folder == stop:
            return None
        folder = parent


# The project ------------------------------------------------------------------------------------


class Build:
    """How a file is built: kind "maven", "gradle" or "plain", the folder commands run in, the tool
    (mvnw / gradlew when the project has one) and whether the file is a test's."""

    def __init__(self, kind, root, tool=None, source=None, test=False):
        self.kind, self.root, self.tool, self.source, self.test = kind, root, tool, source, test


def detect(path, project):
    """The Build of a .java file (or of the project, path empty)."""
    folder = os.path.dirname(path) if path else project
    stop = project if (folder + os.sep).startswith(project.rstrip(os.sep) + os.sep) else None
    test = (os.sep + os.path.join("src", "test") + os.sep) in path
    pom, gradle = find_up(folder, ["pom.xml"], stop), find_up(folder, GRADLE_FILES + SETTINGS_FILES, stop)
    if pom and (not gradle or len(pom) >= len(gradle)):
        wrapper = find_up(pom, ["mvnw"], stop)
        tool = os.path.join(wrapper, "mvnw") if wrapper and os.access(os.path.join(wrapper, "mvnw"), os.X_OK) else "mvn"
        return Build("maven", pom, tool, test=test)
    if gradle:
        top = gradle
        while True:  # the build's top: the highest folder with a settings file, up to the project
            above = find_up(os.path.dirname(top), SETTINGS_FILES, stop) if top != stop else None
            if not above or above == top:
                break
            top = above
        wrapper = os.path.join(top, "gradlew")
        return Build("gradle", gradle, wrapper if os.access(wrapper, os.X_OK) else "gradle", test=test)
    source = source_root(path, read(path)) if path else project
    return Build("plain", source, source=source)


def tool_missing(build):
    """The program the build needs but is not installed, or None."""
    program = build.tool if build.kind != "plain" else "javac"
    return None if os.path.isabs(program) or shutil.which(program) else program


def classes_dir(build):
    """Where a plain folder's classes go: outside the project, per source folder."""
    digest = hashlib.sha256(build.source.encode()).hexdigest()[:12]
    return os.path.join(launcher.cache_home(), "t-code-java", "classes", digest)


def plain_classpath(build, project):
    """A plain folder's jars: lib/ next to the sources or in the project, as a classpath wildcard."""
    for folder in (os.path.join(build.source, "lib"), os.path.join(os.path.dirname(build.source), "lib"),
                   os.path.join(project, "lib")):
        if os.path.isdir(folder):
            return os.path.join(folder, "*")
    return None


def mvn(build, *args):
    return " ".join([shlex.quote(build.tool), "-B", *args])


def gradle(build, *args):
    return " ".join([shlex.quote(build.tool), "--console=plain", *args])


def build_line(build, project, run_dir):
    """The shell line that compiles everything (for Build)."""
    if build.kind == "maven":
        return mvn(build, "-q", "test-compile" if build.test else "compile")
    if build.kind == "gradle":
        return gradle(build, "-q", "testClasses" if build.test else "classes")
    listing = os.path.join(run_dir, "sources.txt")
    with open(listing, "w") as f:
        f.write("\n".join(shlex.quote(p) for p in java_files(build.source)) + "\n")
    return javac(build, project, "@" + listing)


def jdk_tool(name):
    """javac, or the java of the same JDK as the javac on PATH (they may be two JDKs: a class file
    from a newer javac does not load in an older java)."""
    compiler = shutil.which("javac")
    tool = os.path.join(os.path.dirname(os.path.realpath(compiler)), name) if compiler else None
    return tool if tool and os.access(tool, os.X_OK) else name


def javac(build, project, sources):
    cp = plain_classpath(build, project)
    return " ".join([shlex.quote(jdk_tool("javac")), "-g", "-encoding", "UTF-8", "-d", shlex.quote(classes_dir(build)), "-sourcepath",
                     shlex.quote(build.source), *(["-cp", shlex.quote(cp)] if cp else []), shlex.quote(sources)])


def run_lines(build, project, main_file, run_dir):
    """(build line, run line) that compile what main_file needs and start its class."""
    name = shlex.quote(class_of(main_file, read(main_file)))
    if build.kind == "maven":
        cp_file = os.path.join(run_dir, "classpath.txt")
        scope = ["-Dmdep.includeScope=test"] if build.test else []
        line = mvn(build, "-q", "test-compile" if build.test else "compile", "dependency:build-classpath",
                   "-Dmdep.outputFile=" + shlex.quote(cp_file), *scope)
        classes = "target/test-classes:target/classes" if build.test else "target/classes"
        return line, f'java -cp "{classes}:$(cat {shlex.quote(cp_file)})" {name}'
    if build.kind == "gradle":
        init = os.path.join(run_dir, "classpath.gradle")
        with open(init, "w") as f:
            f.write(GRADLE_INIT)
        cp_file = "build/tcode-classpath-test.txt" if build.test else "build/tcode-classpath-main.txt"
        return gradle(build, "-q", "-I", shlex.quote(init), "tcodeClasspath"), f'java -cp "$(cat {cp_file})" {name}'
    cp = [classes_dir(build), build.source] + ([plain_classpath(build, project)] if plain_classpath(build, project) else [])
    return javac(build, project, main_file), f"{shlex.quote(jdk_tool('java'))} -cp {shlex.quote(':'.join(cp))} {name}"


def test_line(build, test_file):
    """Runs the tests: only test_file's class when it is a test, else all of them."""
    name = class_of(test_file, read(test_file)) if test_file and build.test else None
    if build.kind == "maven":
        only = [f"-Dtest={shlex.quote(name.rsplit('.', 1)[-1])}", "-Dsurefire.failIfNoSpecifiedTests=false"] if name else []
        return mvn(build, "test", *only)
    return gradle(build, "test", *(["--tests", shlex.quote(name)] if name else []))


def script_text(root, run_dir, first, then=None):
    """A script running first (its output kept in build.log), then, when first worked, then (its
    stderr kept in run.log; its stdout and stdin stay the terminal's). It writes the status of the
    last step, then "done", and removes itself. Ctrl+C ends it as done too."""
    q = {k: shlex.quote(os.path.join(run_dir, k))
         for k in ("build.log", "build.status", "run.log", "status", "done", "run.sh")}
    lines = [
        "# Written by t-code's Java plugin for one run; it removes itself.",
        f"cd {shlex.quote(root)} || exit 1",
        f"trap 'echo 130 > {q['status']}; : > {q['done']}; rm -f {q['run.sh']}; exit 130' INT",
        f"{{ {first}; echo $? > {q['build.status']}; }} 2>&1 | tee {q['build.log']}",
    ]
    if then:
        lines += [
            f'if [ "$(cat {q["build.status"]})" = 0 ]; then',
            f"  {{ {{ {then}; echo $? > {q['status']}; }} 2>&1 1>&3 3>&- | tee {q['run.log']} >&2; }} 3>&1",
            "else",
            f"  cp {q['build.status']} {q['status']}",
            "fi",
        ]
    else:
        lines.append(f"cp {q['build.status']} {q['status']}")
    lines += [f": > {q['done']}", f"rm -f {q['run.sh']}"]
    return "\n".join(lines) + "\n"


# Reading the output -----------------------------------------------------------------------------


class Sources:
    """Finds a class's file in the project: by its package's folders under the source roots, else
    anywhere in the project."""

    def __init__(self, project, roots):
        self.project, self.roots, self.files = project, roots, None

    def find(self, class_name, file_name):
        outer = class_name.split("$", 1)[0]
        package = outer.rsplit(".", 1)[0] if "." in outer else ""
        relative = os.path.join(*package.split("."), file_name) if package else file_name
        for root in self.roots:
            candidate = os.path.join(root, relative)
            if os.path.isfile(candidate):
                return candidate
        if self.files is None:
            self.files = java_files(self.project)
        suffix = os.sep + relative
        return next((f for f in self.files if f.endswith(suffix)), None)


def add(found, path, line, column, severity, message, source):
    found.setdefault(os.path.normpath(path), []).append(
        {"range": [line, column], "severity": severity, "message": message, "source": source})


def parse_build(text, root, sources):
    """javac's (also through Gradle), Maven's, Surefire's and Gradle's test errors: {path: [diagnostic]}."""
    found = {}
    lines = text.split("\n")
    for i, line in enumerate(lines):
        line = line.rstrip("\r")
        javac_error = JAVAC.match(line)
        maven_error = MAVEN.match(line)
        surefire = SUREFIRE.match(line)
        failed = GRADLE_FAILED.match(line)
        if javac_error:
            path, number, kind, message = javac_error.groups()
            column, extra = 1, []
            for following in lines[i + 1:i + 6]:
                if JAVAC.match(following):
                    break
                if following.strip() == "^" or re.match(r"^\s*\^\s*$", following):
                    column = following.index("^") + 1
                elif re.match(r"^\s+(symbol|location):", following):
                    extra.append(" ".join(following.split()))
            message += f" ({'; '.join(extra)})" if extra else ""
            add(found, os.path.join(root, path), int(number), column, kind, message, "javac")
        elif maven_error:
            kind, path, number, column, message = maven_error.groups()
            add(found, os.path.join(root, path), int(number), int(column), kind.lower(), message, "javac")
        elif surefire and not surefire.group(1).startswith("Tests"):
            name, method, number, message = surefire.groups()
            path = sources.find(name, name.rsplit(".", 1)[-1].split("$")[0] + ".java")
            if path:
                add(found, path, int(number), 1, "error", f"{method}: {message.lstrip('» ')}", "test")
        elif failed and i + 1 < len(lines) and GRADLE_AT.match(lines[i + 1]):
            name, method = failed.groups()
            error, file_name, number = GRADLE_AT.match(lines[i + 1]).groups()
            path = sources.find(name, file_name)
            if path:
                add(found, path, int(number), 1, "error", f"{method} failed: {error.rsplit('.', 1)[-1]}", "test")
    return found


def parse_exceptions(text, sources):
    """The stack traces in the program's stderr: for each exception (and each "Caused by"), its
    first frame in the project an error with the exception's message, the frames calling it info."""
    found = {}
    lines = text.split("\n")
    thrown, first = None, True
    for i, line in enumerate(lines):
        line = line.rstrip("\r")
        frame = FRAME.match(line)
        if frame and thrown:
            name, _method, file_name, number = frame.groups()
            path = sources.find(name, file_name)
            if path:
                if first:
                    add(found, path, int(number), 1, "error", thrown, "java")
                else:
                    add(found, path, int(number), 1, "info", f"called from here: {thrown}", "java")
                first = False
            continue
        header = THROWN.match(line.strip()) if line and not line[0].isspace() else None
        starts = line.startswith(("Exception in thread", "Caused by: "))
        next_is_frame = i + 1 < len(lines) and FRAME.match(lines[i + 1])
        if header and (starts or next_is_frame):
            kind, message = header.groups()
            simple = kind.rsplit(".", 1)[-1]
            thrown, first = (f"{simple}: {message}" if message else simple), True
        elif not frame and not line.strip().startswith("..."):
            thrown = None
    return found


# The terminal tab -------------------------------------------------------------------------------


def tab_open(t, tab):
    """Whether the plugin's tab is still there (terminal-close-text without text only answers ok
    for an open tab of ours, and changes nothing)."""
    try:
        t.request("terminal-close-text", tab)
        return True
    except TcodeError as e:
        if e.code != "bad-argument":
            raise
        return False


def in_terminal(t, project, line):
    """Types line into the plugin's "Java" tab (a shell, so it stays after the run), opening one
    when there is none or it was closed."""
    state = load_state(project)
    tab = state.get("tab")
    if tab and tab_open(t, tab):
        t.request("terminal-send", f"{tab} {line}")
        return
    shell = shlex.quote(f'{line}; exec "${{SHELL:-sh}}"')  # a shell just started may drop what is typed
    state["tab"] = t.request("terminal-open", f"Java -- sh -c {shell}")
    save_state(project, state)


def new_run(project):
    """A folder for one run's files; the newest run is the one the project's state names."""
    run_id = uuid.uuid4().hex[:12]
    run_dir = os.path.join(runtime_dir(), run_id)
    os.makedirs(run_dir, mode=0o700)
    state = load_state(project)
    state["run"] = run_id
    save_state(project, state)
    return run_id, run_dir


def wait_for(project, run_id, run_dir):
    """True once the run's script is done; False when a newer run started, its tab closed, or after
    WAIT_SECONDS. No connection is held meanwhile."""
    deadline = time.monotonic() + WAIT_SECONDS
    checked = time.monotonic()
    while time.monotonic() < deadline:
        if os.path.exists(os.path.join(run_dir, "done")):
            return True
        time.sleep(0.3)
        state = load_state(project)
        if state.get("run") != run_id:
            return False
        if time.monotonic() - checked > 5:
            checked = time.monotonic()
            t = Tcode()
            try:
                if not state.get("tab") or not tab_open(t, state["tab"]):
                    return False
            finally:
                t.close()
    return False


def roots_of(build, project):
    if build.kind == "plain":
        return [build.source]
    return [os.path.join(build.root, "src", "main", "java"), os.path.join(build.root, "src", "test", "java")]


def report(project, build, run_dir, what):
    """The run's errors as diagnostics, and how it went in the status bar."""
    try:
        status = read(os.path.join(run_dir, "status")).strip()
        build_log, run_log = read(os.path.join(run_dir, "build.log")), read(os.path.join(run_dir, "run.log"))
    finally:
        shutil.rmtree(run_dir, ignore_errors=True)
    sources = Sources(project, roots_of(build, project))
    found = parse_build(build_log, build.root, sources)
    for path, items in parse_exceptions(run_log, sources).items():
        found.setdefault(path, []).extend(items)
    t = Tcode()
    for path, items in found.items():
        t.json("diagnostics-set", {"path": path, "items": items})
    errors = sum(1 for items in found.values() for d in items if d["severity"] == "error")
    if status == "0":
        t.request("say", f"Java: {what} done")
    elif status == "130":
        t.request("say", f"Java: {what} stopped")
    else:
        count = f" ({errors} error{'s' if errors != 1 else ''})" if errors else ""
        t.request("say", f"Java: {what} failed{count}")
    t.close()


# Commands ---------------------------------------------------------------------------------------


def current_java_file(t):
    """The active editor's .java file, saved; "" when the active tab is none."""
    path = t.request("get-file")
    if not path.endswith(".java"):
        return ""
    try:
        t.request("save")
    except TcodeError:
        pass
    return path


def choose_main(t, project, build, path):
    """The file whose main Run starts: the current one when it has one, else the only one in its
    sources, else the one the user picks (the last pick offered first). None when there is none."""
    if path and has_main(read(path)):
        return path
    top = build.source if build.kind == "plain" else build.root
    mains = [f for f in java_files(top) if has_main(read(f))]
    if not mains:
        t.request("say", "Java: no main method found to run")
        return None
    if len(mains) == 1:
        return mains[0]
    state = load_state(project)
    if state.get("main") in mains:
        mains.remove(state["main"])
        mains.insert(0, state["main"])
    items = [{"label": class_of(f, read(f)), "detail": os.path.relpath(f, project)} for f in mains]
    try:
        index = int(t.json("pick", {"title": "Run which main class?", "items": items}).split(" ", 1)[0])
    except TcodeError as e:
        if e.code == "canceled":
            return None
        raise
    state = load_state(project)
    state["main"] = mains[index]
    save_state(project, state)
    return mains[index]


def start_run(t, project, build, run, what, first, then=None):
    """Types the run's script into the Java tab, waits for its end and reports its errors."""
    run_id, run_dir = run
    script = os.path.join(run_dir, "run.sh")
    with open(script, "w") as f:
        f.write(script_text(build.root, run_dir, first, then))
    t.request("diagnostics-clear")
    in_terminal(t, project, f"sh {shlex.quote(script)}")
    t.close()  # the run may take long: no connection held meanwhile
    if wait_for(project, run_id, run_dir):
        report(project, build, run_dir, what)
    else:
        shutil.rmtree(run_dir, ignore_errors=True)


def command_build(t, project, what):
    """Run, Build, Test or Clean for the current file's build (the project's without one)."""
    path = current_java_file(t)
    build = detect(path, project)
    if build.kind == "plain" and not path:
        t.request("say", "Java: open a .java file first (no pom.xml or build.gradle here)")
        return
    if build.kind == "plain" and what in ("test", "clean"):
        if what == "clean":
            shutil.rmtree(classes_dir(build), ignore_errors=True)
        t.request("say", "Java: compiled classes removed" if what == "clean"
                  else "Java: tests run through Maven or Gradle (no pom.xml or build.gradle here)")
        return
    if tool_missing(build):
        t.request("say", f"Java: {tool_missing(build)} not found: install it first")
        return
    main_file = choose_main(t, project, build, path) if what == "run" else None
    if what == "run" and not main_file:
        return
    run = new_run(project)  # its folder also holds the files the lines below refer to
    if what == "run":
        first, then = run_lines(build, project, main_file, run[1])
        start_run(t, project, build, run, f"running {os.path.basename(main_file)}", first, then)
    elif what == "build":
        start_run(t, project, build, run, "build", build_line(build, project, run[1]))
    elif what == "test":
        start_run(t, project, build, run, "tests", test_line(build, path))
    else:
        start_run(t, project, build, run, "clean",
                  mvn(build, "-q", "clean") if build.kind == "maven" else gradle(build, "-q", "clean"))


def stop(t, project):
    tab = load_state(project).get("tab")
    if tab and tab_open(t, tab):
        t.request("terminal-interrupt", tab)
    else:
        t.request("say", "Java: nothing is running")


def clean_workspace(t, project):
    flag = launcher.wipe_flag(project)
    os.makedirs(os.path.dirname(flag), exist_ok=True)
    with open(flag, "w"):
        pass
    t.request("say", "Java: jdtls starts from scratch next time: run Restart Language Servers")


# Debugging --------------------------------------------------------------------------------------


def lsp(t, path, command, *arguments):
    """workspace/executeCommand on jdtls (the server of path's editor), its result decoded."""
    return json.loads(t.json("lsp-command", {"command": command, "arguments": list(arguments), "path": path}))


def pick_main_class(t, project, path, mains):
    """The main class to debug among jdtls' (dicts with mainClass, projectName, filePath): the current
    file's, else the only one, else the one picked (the last pick first). None: none or canceled."""
    if not mains:
        t.request("say", "Java: no main method found to debug")
        return None
    for main in mains:
        if main.get("filePath") and os.path.realpath(main["filePath"]) == os.path.realpath(path):
            return main
    if len(mains) == 1:
        return mains[0]
    state = load_state(project)
    mains = sorted(mains, key=lambda m: m.get("mainClass") != state.get("debug-main"))
    items = [{"label": m.get("mainClass", "?"),
              "detail": os.path.relpath(m["filePath"], project) if m.get("filePath") else m.get("projectName", "")}
             for m in mains]
    try:
        index = int(t.json("pick", {"title": "Debug which main class?", "items": items}).split(" ", 1)[0])
    except TcodeError as e:
        if e.code == "canceled":
            return None
        raise
    state = load_state(project)
    state["debug-main"] = mains[index].get("mainClass")
    save_state(project, state)
    return mains[index]


def launch_arguments(main, paths, java, project):
    """java-debug's launch arguments: the class, its classpath and module path, run in the Debug
    panel's terminal."""
    modules, classes = (list(paths) + [[], []])[:2] if isinstance(paths, list) else ([], [])
    arguments = {"mainClass": main["mainClass"], "projectName": main.get("projectName", ""),
                 "modulePaths": modules or [], "classPaths": classes or [], "cwd": project,
                 "console": "integratedTerminal", "args": "", "vmArgs": "", "encoding": "UTF-8",
                 "stopOnEntry": False}
    if isinstance(java, str) and java:
        arguments["javaExec"] = java
    return arguments


def debug(t, project):
    """F5 on a .java file: java-debug starts the main class under t-code's debugger."""
    path = current_java_file(t)
    if not path:
        t.request("say", "Java: open a .java file first")
        return
    if not launcher.debug_bundles():
        items = [{"label": "Install java-debug", "detail": "the Java debugger for jdtls, about 3 MB"},
                 {"label": "Not now"}]
        try:
            if t.json("pick", {"title": "Java: debugging needs java-debug", "items": items}).startswith("0 "):
                install_debugger(t)
        except TcodeError:
            pass
        return
    try:
        main = pick_main_class(t, project, path, lsp(t, path, "vscode.java.resolveMainClass") or [])
        if not main:
            return
        built = lsp(t, path, "vscode.java.buildWorkspace",
                    json.dumps({"mainClass": main["mainClass"], "projectName": main.get("projectName", ""),
                                "isFullBuild": False}))
        if built in ("FAILED", "WITH_ERROR", "CANCELLED", 0, 2, 3):  # BuildWorkspaceStatus: 1 is SUCCEED
            t.request("say", "Java: the build has errors: see the Problems panel")
            return
        paths = lsp(t, path, "vscode.java.resolveClasspath", main["mainClass"], main.get("projectName", ""))
        try:
            java = lsp(t, path, "vscode.java.resolveJavaExecutable", main["mainClass"], main.get("projectName", ""))
        except TcodeError:
            java = None
        port = lsp(t, path, "vscode.java.startDebugSession")
    except TcodeError as e:
        if e.code == "failed":  # jdtls does not know the commands: java-debug is not loaded yet
            t.request("say", "Java: jdtls has not loaded java-debug: run Restart Language Servers, then F5 again")
            return
        raise
    if not isinstance(port, int):
        t.request("say", "Java: java-debug gave no port to debug with")
        return
    t.json("debug-start", {"name": "Java", "adapter-id": "java", "connect": f"127.0.0.1:{port}",
                           "request": "launch", "arguments": launch_arguments(main, paths, java, project),
                           "shown": main["mainClass"]})


def install_debugger(t):
    """Downloads java-debug's newest release from Maven Central (its SHA-512, else SHA-1, checked)
    into the launcher's folder; jdtls loads it when it starts again."""
    t.request("status-set", "jdtls java-debug: looking for the newest release…")
    try:
        metadata = fetch(DEBUG_REPOSITORY + "maven-metadata.xml").decode("utf-8", "replace")
        release = re.search(r"<release>([^<]+)</release>", metadata)
        versions = re.findall(r"<version>([^<]+)</version>", metadata)
        version = release.group(1) if release else (versions[-1] if versions else None)
        if not version or not re.fullmatch(r"[\w.-]+", version):
            raise OSError("no java-debug release found on Maven Central")
        name = f"com.microsoft.java.debug.plugin-{version}.jar"
        url = f"{DEBUG_REPOSITORY}{version}/{name}"
        t.request("status-set", f"jdtls java-debug {version}: downloading")
        data = fetch(url, timeout=60)
        for suffix, algorithm in ((".sha512", "sha512"), (".sha1", "sha1")):
            try:
                expected = fetch(url + suffix).decode().split()[0].lower()
                break
            except OSError:
                expected = None
        if not expected or hashlib.new(algorithm, data).hexdigest() != expected:
            raise OSError("the download's checksum does not match: try again")
        folder = launcher.debug_dir()
        os.makedirs(folder, exist_ok=True)
        temporary = os.path.join(folder, name + ".new")
        with open(temporary, "wb") as f:
            f.write(data)
        os.replace(temporary, os.path.join(folder, name))
        for old in os.listdir(folder):
            if old != name and old.startswith("com.microsoft.java.debug.plugin-"):
                os.unlink(os.path.join(folder, old))
    except OSError as e:
        t.request("status-clear", "jdtls")
        t.request("say", f"Java: couldn't download java-debug: {getattr(e, 'reason', e)}")
        return False
    t.request("status-clear", "jdtls")
    install_launcher()  # one that hands the bundle to jdtls
    try:
        t.request("lsp-restart")  # jdtls loads bundles when it starts
        t.request("say", f"Java: java-debug {version} installed; F5 debugs once jdtls has read the project")
    except TcodeError:
        t.request("say", f"Java: java-debug {version} installed: run Restart Language Servers, then F5")
    return True


# Installing the language server -----------------------------------------------------------------


def launcher_path():
    return os.path.expanduser("~/.local/bin/t-code-jdtls")


def launcher_source():
    with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "jdtls_launcher.py"), "rb") as f:
        return f.read()


def launcher_current():
    try:
        with open(launcher_path(), "rb") as f:
            return f.read() == launcher_source()
    except OSError:
        return False


def install_launcher():
    path = launcher_path()
    os.makedirs(os.path.dirname(path), exist_ok=True)
    temporary = path + ".new"
    with open(temporary, "wb") as f:
        f.write(launcher_source())
    os.chmod(temporary, 0o755)
    os.replace(temporary, path)


def fetch(url, timeout=30):
    with urllib.request.urlopen(url, timeout=timeout) as answer:
        return answer.read()


def version_key(text):
    return [int(n) for n in text.split(".")]


def latest_release():
    """(version, file name) of jdtls' newest milestone."""
    index = fetch(MILESTONES).decode("utf-8", "replace")
    versions = sorted(set(re.findall(r"milestones/(\d+(?:\.\d+)+)", index)), key=version_key)
    for version in reversed(versions[-3:]):  # the newest may be listed before its files are there
        try:
            name = fetch(f"{MILESTONES}{version}/latest.txt").decode().strip()
        except OSError:
            continue
        if name.endswith(".tar.gz"):
            return version, name
    raise OSError("no jdtls release found at download.eclipse.org")


def download(t):
    """Downloads jdtls' newest milestone into the plugin's folder, checks its SHA-256 and unpacks it:
    the folder, now also ".../current"."""
    t.request("status-set", "jdtls jdtls: looking for the newest release…")
    version, name = latest_release()
    url = f"{MILESTONES}{version}/{name}"
    expected = fetch(url + ".sha256").decode().split()[0].lower()
    target = launcher.download_dir()
    os.makedirs(target, exist_ok=True)
    digest = hashlib.sha256()
    with tempfile.TemporaryDirectory(dir=target) as tmp:
        archive = os.path.join(tmp, name)
        with urllib.request.urlopen(url, timeout=60) as answer, open(archive, "wb") as f:
            total = int(answer.headers.get("Content-Length") or 0)
            done, shown = 0, 0.0
            while True:
                block = answer.read(1 << 16)
                if not block:
                    break
                f.write(block)
                digest.update(block)
                done += len(block)
                if time.monotonic() - shown > 0.5:
                    shown = time.monotonic()
                    size = f"{done >> 20} / {total >> 20} MB" if total else f"{done >> 20} MB"
                    t.request("status-set", f"jdtls jdtls {version}: downloading {size}")
        if digest.hexdigest() != expected:
            raise OSError("the download's SHA-256 does not match: try again")
        t.request("status-set", f"jdtls jdtls {version}: unpacking")
        unpacked = os.path.join(tmp, "jdtls")
        with tarfile.open(archive) as tar:
            if hasattr(tarfile, "data_filter"):
                tar.extractall(unpacked, filter="data")
            else:
                tar.extractall(unpacked)  # noqa: S202 (Python before 3.12: the archive's SHA-256 was checked)
        folder = os.path.join(target, version)
        shutil.rmtree(folder, ignore_errors=True)
        os.replace(unpacked, folder)
    link = os.path.join(target, "current")
    os.symlink(version, link + ".new")
    os.replace(link + ".new", link)
    for old in os.listdir(target):  # earlier versions
        if old not in (version, "current") and re.fullmatch(r"\d+(\.\d+)+", old):
            shutil.rmtree(os.path.join(target, old), ignore_errors=True)
    t.request("status-clear", "jdtls")
    return folder, version


def install(t):
    java, version = launcher.find_java()
    if not java or version < launcher.MIN_JAVA:
        found = f" (found Java {version})" if java and version else ""
        t.request("say", f"Java: jdtls needs a JDK {launcher.MIN_JAVA} or newer{found}: install one first")
        return False
    home, how = launcher.find_jdtls(), "found"
    if not home:
        try:
            home, release = download(t)
            how = f"{release} downloaded"
        except (OSError, tarfile.TarError) as e:
            t.request("status-clear", "jdtls")
            t.request("say", f"Java: couldn't download jdtls: {getattr(e, 'reason', e)}")
            return False
    install_launcher()
    try:
        t.request("lsp-refresh")  # the open .java files get the server now
    except TcodeError:
        pass
    t.request("say", f"Java: jdtls {how}; it takes a moment to read the project the first time")
    return True


def server_ready():
    return launcher_current() and launcher.find_jdtls() is not None


def offer_install():
    """Started with the project: with the server ready, nothing to do. A jdtls already installed
    elsewhere only needs the launcher. Else, the first .java file that opens asks once."""
    if server_ready():
        return
    if os.path.exists(launcher_path()) or launcher.find_jdtls():
        if launcher.find_jdtls():
            install_launcher()  # a new plugin version brought a new launcher, or jdtls came from a package
            t = Tcode()
            try:
                t.request("lsp-refresh")
            except TcodeError:
                pass
        return
    if load_settings().get("never-offer"):
        return
    for event in Tcode().events():
        if event.get("event") in ("did_open", "active_changed") and str(event.get("path", "")).endswith(".java"):
            break
    else:
        return
    t = Tcode()
    items = [{"label": "Install jdtls", "detail": "Eclipse JDT Language Server, about 50 MB"},
             {"label": "Not now"}, {"label": "Don't ask again"}]
    try:
        index = int(t.json("pick", {"title": "Java: completion, errors and navigation need jdtls", "items": items})
                    .split(" ", 1)[0])
    except TcodeError:
        return
    if index == 0:
        install(t)
    elif index == 2:
        settings = load_settings()
        settings["never-offer"] = True
        save_settings(settings)
        t.request("say", "Java: run Java: Install Language Server when you want jdtls")


def main(command):
    if command == "start":
        offer_install()
        return
    t = Tcode()
    try:
        project = t.request("get-project")
        if command in ("run", "build", "test", "clean"):
            command_build(t, project, command)
        elif command == "stop":
            stop(t, project)
        elif command == "install":
            install(t)
        elif command == "debug":
            debug(t, project)
        elif command == "install-debugger":
            install_debugger(t)
        elif command == "clean-workspace":
            clean_workspace(t, project)
    except TcodeError as e:
        Tcode().request("say", f"Java plugin: {e.message}")


if __name__ == "__main__":
    try:
        main(sys.argv[1] if len(sys.argv) > 1 else "")
    except (BrokenPipeError, ConnectionError):
        pass
