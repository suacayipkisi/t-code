/*
 * Build (Ctrl+Shift+B), run (Ctrl+F5) and build for debugging (F5), in the shell pane, also on the
 * status bar buttons. The project is found from the current file upwards:
 *   CMakeLists.txt  configure + build in build/; run starts the project's executable there
 *                  (named after the project, else the newest one that is not a test)
 *   meson.build     setup + compile in build/; run starts the newest non-test executable
 *   Makefile        make; run uses "make run" when the Makefile has a run target
 * Other languages' files look for their own project files only (a tools/x.py in a C project runs
 * itself, not make; a .py next to a package.json is not npm's):
 *   .rs             Cargo.toml: cargo run (--bin / --example for src/bin/, examples/), else rustc
 *   .go             go.mod: go run . in the file's folder
 *   .js .ts ...     package.json: its start or dev script (npm, pnpm, yarn or bun by the lock file)
 *   .java .kt       pom.xml: mvn compile, then java on the file's class; build.gradle: gradle run
 *   .py             from the project's root (pyproject.toml, requirements.txt, a venv...), which goes
 *                   into PYTHONPATH, with the picked interpreter or its venv's python when it has one
 * Without one, a script (scripts[], or an executable file starting with "#!") is run by its
 * interpreter in its folder. Build checks the syntax when the language has a way to.
 * A build for debugging ends with "t-code --send 'debug <program>'" (or "debug-failed <dir>" when it
 * failed): the control socket hands the program to the debugger (for a Makefile, which builds who
 * knows what, the folder: the debugger asks which program, once).
 */
#include "project/run/run.h"

#include "editor/editor.h"
#include "editor/tabs.h"
#include "project/langenv.h"
#include "project/run/runner.h"
#include "project/toolchain.h"
#include "window/terminal.h"

#include <stdio.h>
#include <string.h>

#define C_FLAGS "-std=c17 -Wall -Wextra -g -fsanitize=address,undefined"
#define CXX_FLAGS "-std=c++20 -Wall -Wextra -g -fsanitize=address,undefined"

static void project_clear(Project *p)
{
  g_free(p->root);
  g_free(p->python);
  g_free(p->interpreter);
  g_free(p->cargo_flags);
  g_free(p->task_name);
}

/* The nearest directory, from start upwards, that holds one of names. */
char *find_up(const char *start, const char *const *names)
{
  char *dir = g_strdup(start);

  for (;;) {
    for (int i = 0; names[i]; i++) {
      char *path = g_build_filename(dir, names[i], NULL);
      gboolean found = g_file_test(path, G_FILE_TEST_IS_REGULAR);
      g_free(path);
      if (found)
        return dir;
    }
    char *parent = g_path_get_dirname(dir);
    gboolean top = strcmp(parent, dir) == 0;
    g_free(dir);
    dir = parent;
    if (top) {
      g_free(dir);
      return NULL;
    }
  }
}

/* Whether file is C or C++ source (*cxx: C++). */
static gboolean is_source(const char *file, gboolean *cxx)
{
  const char *ext = file ? strrchr(file, '.') : NULL;

  *cxx = ext && (strcmp(ext, ".cpp") == 0 || strcmp(ext, ".cc") == 0 || strcmp(ext, ".cxx") == 0);
  return ext && (*cxx || strcmp(ext, ".c") == 0);
}

/* Whether file's extension is one of exts. */
gboolean has_ext(const char *file, const char *const *exts)
{
  const char *name = file ? strrchr(file, '/') : NULL;
  const char *ext = name ? strrchr(name, '.') : NULL;

  return ext && g_strv_contains(exts, ext);
}

/* What to build, from the current file's folder upwards: tasks.json first; else another language's
 * file in its own project or alone, or a script; else CMake, Meson or Make, or else the current C /
 * C++ file alone. */
static Project find_project(App *t)
{
  static const char *const tasks[] = { ".t-code/tasks.json", "t-code.json", NULL };
  static const char *const cmake[] = { "CMakeLists.txt", NULL };
  static const char *const meson[] = { "meson.build", NULL };
  static const char *const make[] = { "Makefile", "makefile", "GNUmakefile", NULL };
  static const char *const python[] = { ".py", NULL };
  Tab *tab = current_tab(t);
  const char *file = tab && tab->kind == TAB_EDITOR ? tab->path : NULL;
  char *start = file ? g_path_get_dirname(file) : g_strdup(t->project.root);
  Project p = { .kind = PROJECT_NONE, .file = file };
  gboolean cxx;

  if ((p.root = find_up(start, tasks))) {
    p.kind = PROJECT_TASKS;
    p.task_name = tasks_name(p.root);
  } else if (find_language_project(&p, start)) {
    if (p.kind == PROJECT_CARGO) {
      char *features = toolchain_rust_features(t), *target = toolchain_rust_target(t);
      p.cargo_flags = langenv_cargo_flags(features, target);
      g_free(target);
      g_free(features);
    }
  } else if ((p.script = script_of(file))) {
    p.kind = PROJECT_SCRIPT;
    p.root = g_strdup(file);
    if (has_ext(file, python)) {
      p.python = python_root(start);
      p.interpreter = toolchain_python(t, p.python);
    }
  } else if ((p.root = find_up(start, cmake)))
    p.kind = PROJECT_CMAKE;
  else if ((p.root = find_up(start, meson)))
    p.kind = PROJECT_MESON;
  else if ((p.root = find_up(start, make)))
    p.kind = PROJECT_MAKE;
  else if (is_source(file, &cxx)) {
    p.kind = PROJECT_FILE;
    p.root = g_strdup(file);
  }
  g_free(start);
  return p;
}

/* The shell command that sends "<verb> <what>" (what already a shell word: '/a/b', "$exe") to this
 * t-code through the control socket. */
char *send_verb(const char *verb, const char *what)
{
  char *exe = g_file_read_link("/proc/self/exe", NULL);
  char *quoted = g_shell_quote(exe ? exe : "t-code");
  char *command = g_strdup_printf("%s --send \"%s \"%s", quoted, verb, what);

  g_free(quoted);
  g_free(exe);
  return command;
}
/* what (taken), run only when program is installed. */
char *if_installed(const char *program, char *what)
{
  char *cmd = g_strdup_printf("if command -v %s >/dev/null 2>&1; then %s; else echo 't-code: %s is not installed'; fi",
                              program, what, program);

  g_free(what);
  return cmd;
}

/* The shell command that configures and builds with CMake (and then runs or debugs the program). */
static char *cmake_command(const char *root, Goal goal)
{
  char *build_dir = g_build_filename(root, "build", NULL);
  char *src = g_shell_quote(root), *build = g_shell_quote(build_dir);
  char *cache = g_build_filename(build_dir, "CMakeCache.txt", NULL);
  /* Ninja only for a new build/: CMake refuses another generator than the one it was configured
     * with (e.g. by hand with Unix Makefiles). */
  char *ninja = g_file_test(cache, G_FILE_TEST_EXISTS) ? NULL : g_find_program_in_path("ninja");
  char *cmd =
      g_strdup_printf("cmake -S %s -B %s %s-DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON >/dev/null"
                      " && cmake --build %s",
                      src, build, ninja ? "-G Ninja " : "", build);

  if (goal == GOAL_TEST) {
    char *test = g_strdup_printf("%s && (cd %s && if ctest -N | grep -Eq 'Total Tests: [1-9][0-9]*';"
                                 " then ctest --output-on-failure;"
                                 " else echo 'No CTest tests configured'; false; fi)",
                                 cmd, build);
    g_free(cmd);
    cmd = if_installed("ctest", test);
  } else if (goal != GOAL_BUILD) {
    char *debug = goal == GOAL_DEBUG ? send_verb("debug", "\"$exe\"") : NULL;
    /* The executable named after the project (CMAKE_PROJECT_NAME) anywhere under build/
         * (targets may sit in build/bin, build/src); else the newest one that is not a test, else
         * the newest one. CMake's own checks and shared libraries are left out. */
    char *with_run = g_strdup_printf("%s && exe=$(cd %s && {"
                                     " find . -type f -executable -not -path '*/CMakeFiles/*'"
                                     " -name \"$(sed -n 's/^CMAKE_PROJECT_NAME:STATIC=//p' CMakeCache.txt)\";"
                                     " find . -type f -executable -not -path '*/CMakeFiles/*' -not -name '*.so*'"
                                     " -printf '%%T@ %%p\\n' | sort -nr | cut -d' ' -f2- | grep -vi test;"
                                     " find . -type f -executable -not -path '*/CMakeFiles/*' -not -name '*.so*'"
                                     " -printf '%%T@ %%p\\n' | sort -nr | cut -d' ' -f2-; } | head -1)"
                                     " && if [ -n \"$exe\" ]; then exe=%s/${exe#./} && %s;"
                                     " else echo 'No executable in build/'; fi",
                                     cmd, build, build, debug ? debug : "echo \"▶ $exe\" && \"$exe\"");
    g_free(debug);
    g_free(cmd);
    cmd = with_run;
  }
  g_free(ninja);
  g_free(cache);
  g_free(build);
  g_free(src);
  g_free(build_dir);
  return cmd;
}

/* Meson uses its conventional build/ tree; reuse it when configured, otherwise set it up first. */
static char *meson_command(const char *root, Goal goal)
{
  char *build_dir = g_build_filename(root, "build", NULL);
  char *src = g_shell_quote(root), *build = g_shell_quote(build_dir);
  char *setup = g_strdup_printf("if test -f %s/meson-private/coredata.dat; then"
                                " meson setup --reconfigure %s %s --buildtype=debug;"
                                " else meson setup %s %s --buildtype=debug; fi"
                                " && meson compile -C %s",
                                build, build, src, build, src, build);
  char *cmd = NULL;

  if (goal == GOAL_TEST)
    cmd = g_strdup_printf("%s && meson test -C %s --print-errorlogs", setup, build);
  else if (goal == GOAL_BUILD)
    cmd = g_strdup(setup);
  else {
    char *debug = goal == GOAL_DEBUG ? send_verb("debug", "\"$exe\"") : NULL;
    cmd = g_strdup_printf("%s && exe=$(cd %s && {"
                          " find . -type f -executable -not -path '*/meson-private/*'"
                          " -not -path '*/meson-info/*' -not -name '*.so*'"
                          " -printf '%%T@ %%p\\n' | sort -nr | cut -d' ' -f2- | grep -vi test;"
                          " find . -type f -executable -not -path '*/meson-private/*'"
                          " -not -path '*/meson-info/*' -not -name '*.so*'"
                          " -printf '%%T@ %%p\\n' | sort -nr | cut -d' ' -f2-; } | head -1)"
                          " && if [ -n \"$exe\" ]; then exe=%s/${exe#./} && %s;"
                          " else echo 'No executable in Meson build/'; false; fi",
                          setup, build, build, debug ? debug : "echo \"▶ $exe\" && \"$exe\"");
    g_free(debug);
  }
  char *available = if_installed("meson", cmd);
  g_free(setup);
  g_free(build);
  g_free(src);
  g_free(build_dir);
  return available;
}

/* The shell command that runs make (and then "make run" when the Makefile has it, or the debugger). */
static char *make_command(const char *root, Goal goal)
{
  char *dir = g_shell_quote(root);
  char *debug = goal == GOAL_DEBUG ? send_verb("debug", dir) : NULL;
  char *cmd;

  if (debug)
    cmd = g_strdup_printf("make --no-print-directory -C %s && %s", dir, debug);
  else if (goal == GOAL_TEST)
    cmd = g_strdup_printf("make --no-print-directory -C %s test", dir);
  else if (goal == GOAL_RUN)
    cmd = g_strdup_printf("make --no-print-directory -C %s"
                          " && if make -C %s -n run >/dev/null 2>&1; then make -s --no-print-directory -C %s run;"
                          " else echo \"Build OK (no 'run' target in Makefile)\"; fi",
                          dir, dir, dir);
  else
    cmd = g_strdup_printf("make --no-print-directory -C %s", dir);
  g_free(debug);
  g_free(dir);
  return cmd;
}

/* The shell command that compiles one file into build/ next to it (and then runs or debugs it). */
static char *file_command(const char *file, Goal goal)
{
  gboolean cxx;
  is_source(file, &cxx);

  char *dir = g_path_get_dirname(file);
  char *base = g_path_get_basename(file);
  *strrchr(base, '.') = '\0';
  char *out_dir = g_build_filename(dir, "build", NULL);
  char *out_path = g_build_filename(out_dir, base, NULL);
  char *qdir = g_shell_quote(dir), *qfile = g_shell_quote(file);
  char *qout_dir = g_shell_quote(out_dir), *qout = g_shell_quote(out_path);
  char *cmd = g_strdup_printf("mkdir -p %s && %s %s %s -o %s -lm", qout_dir, cxx ? "g++" : "gcc",
                              cxx ? CXX_FLAGS : C_FLAGS, qfile, qout);

  if (goal == GOAL_RUN) {
    /* In a subshell: the shell stays in its own directory. */
    char *with_run = g_strdup_printf("%s && (cd %s && %s)", cmd, qdir, qout);
    g_free(cmd);
    cmd = with_run;
  } else if (goal == GOAL_DEBUG) {
    char *debug = send_verb("debug", qout);
    char *with_debug = g_strdup_printf("%s && %s", cmd, debug);
    g_free(cmd);
    g_free(debug);
    cmd = with_debug;
  }
  g_free(qout);
  g_free(qout_dir);
  g_free(qfile);
  g_free(qdir);
  g_free(out_path);
  g_free(out_dir);
  g_free(base);
  g_free(dir);
  return cmd;
}

char *project_summary(App *t)
{
  Project p = find_project(t);
  char *name = p.root ? g_path_get_basename(p.root) : NULL;
  char *text;

  switch (p.kind) {
    case PROJECT_TASKS: text = g_strdup_printf("Task: %s", p.task_name ? p.task_name : name); break;
    case PROJECT_CMAKE: text = g_strdup_printf("CMake: %s", name); break;
    case PROJECT_MESON: text = g_strdup_printf("Meson: %s", name); break;
    case PROJECT_MAKE: text = g_strdup_printf("Make: %s", name); break;
    case PROJECT_FILE: text = g_strdup_printf("gcc: %s", name); break;
    case PROJECT_RUSTC: text = g_strdup_printf("rustc: %s", name); break;
    case PROJECT_CARGO: text = g_strdup_printf("Cargo: %s", name); break;
    case PROJECT_GO: text = g_strdup_printf("Go: %s", name); break;
    case PROJECT_NODE: text = g_strdup_printf("%s: %s", node_manager(p.root), name); break;
    case PROJECT_MAVEN: text = g_strdup_printf("Maven: %s", name); break;
    case PROJECT_GRADLE: text = g_strdup_printf("Gradle: %s", name); break;
    case PROJECT_SCRIPT: {
      char *program = p.interpreter   ? langenv_interpreter_name(p.interpreter)
                      : p.script->run ? script_program(p.script)
                                      : g_strdup("Run");
      text = g_strdup_printf("%s: %s", program, name);
      g_free(program);
      break;
    }
    default: text = g_strdup("Nothing to build"); break;
  }
  g_free(name);
  project_clear(&p);
  return text;
}

/* The commands use sh syntax (if/fi, var=$(...)); a shell that speaks something else (fish,
 * tcsh) gets them through sh -c. */
static char *for_user_shell(const char *command)
{
  static const char *const posix[] = { "sh", "bash", "zsh", "dash", "ksh", "mksh", "ash", "yash", NULL };
  const char *env = g_getenv("SHELL");
  char *path = env && *env ? g_strdup(env) : vte_get_user_shell(); /* as start_shell() picks it */
  char *shell = path ? g_path_get_basename(path) : NULL;
  gboolean ok = !shell || g_strv_contains(posix, shell);
  char *quoted = ok ? NULL : g_shell_quote(command);
  char *line = ok ? g_strdup(command) : g_strconcat("sh -c ", quoted, NULL);

  g_free(quoted);
  g_free(shell);
  g_free(path);
  return line;
}

/* The editors are saved: the command runs in the shell, after clearing it. */
static void run_saved(App *t, gpointer data)
{
  char *command = data;

  if (!t->closing) {
    char *line = for_user_shell(command);
    char *with_clear = g_strdup_printf("clear; %s", line);
    run_in_shell(t, with_clear);
    g_free(with_clear);
    g_free(line);
  }
  g_free(command);
}

/* The folder p's program runs in: a file's own folder when it is built alone, a task profile's cwd,
 * else the project's. */
static char *run_folder(App *t, const Project *p)
{
  if (p->kind == PROJECT_FILE || p->kind == PROJECT_SCRIPT || p->kind == PROJECT_RUSTC) /* root: the file */
    return g_path_get_dirname(p->root);
  if (p->kind == PROJECT_TASKS)
    return tasks_folder(p->root);
  return g_strdup(p->root ? p->root : t->project.root);
}

/* The command for goal; *cwd (may be NULL) gets the folder the program runs in. */
static char *goal_command(App *t, Goal goal, char **cwd)
{
  Project p = find_project(t);
  char *command;

  switch (p.kind) {
    case PROJECT_TASKS: command = tasks_command(p.root, goal); break;
    case PROJECT_CMAKE: command = cmake_command(p.root, goal); break;
    case PROJECT_MESON: command = meson_command(p.root, goal); break;
    case PROJECT_MAKE: command = make_command(p.root, goal); break;
    case PROJECT_FILE:
      command = goal == GOAL_TEST ? g_strdup("echo 't-code: no test runner detected for this file'; false")
                                  : file_command(p.root, goal);
      break;
    case PROJECT_SCRIPT:
      if (goal == GOAL_TEST && p.python) {
        const char *python = p.interpreter;
        char *qroot = g_shell_quote(p.python), *qpython = g_shell_quote(python ? python : "python3");
        char *test = g_strdup_printf("(cd %s && %s -m pytest)", qroot, qpython);
        command = python ? test : if_installed("python3", test);
        g_free(qpython);
        g_free(qroot);
      } else if (goal == GOAL_TEST)
        command = g_strdup("echo 't-code: no Python test project found'; false");
      else
        command = script_command(&p, goal);
      break;
    case PROJECT_RUSTC:
      command = goal == GOAL_TEST ? g_strdup("echo 't-code: no test runner detected for this file'; false")
                                  : rustc_command(p.root, goal);
      break;
    case PROJECT_CARGO:
    case PROJECT_GO:
    case PROJECT_NODE:
    case PROJECT_MAVEN:
    case PROJECT_GRADLE: command = language_command(&p, goal); break;
    default:
      command =
          goal == GOAL_TEST
              ? g_strdup("echo 't-code: no test runner detected for this project'; false")
              : g_strdup("echo 't-code: nothing to build here (open a C file, or add a Makefile or CMakeLists.txt)'");
  }
  gboolean alone = p.kind == PROJECT_FILE || p.kind == PROJECT_SCRIPT || p.kind == PROJECT_RUSTC; /* root: the file */
  if (cwd) /* where Run starts it too */
    *cwd = run_folder(t, &p);
  if (goal == GOAL_DEBUG && p.kind != PROJECT_NONE) { /* a failed build says so: the debugger stops waiting */
    char *dir = alone ? g_path_get_dirname(p.root) : g_strdup(p.root);
    char *qdir = g_shell_quote(dir), *failed = send_verb("debug-failed", qdir);
    char *with_failed = g_strdup_printf("%s || %s", command, failed);
    g_free(command);
    command = with_failed;
    g_free(failed);
    g_free(qdir);
    g_free(dir);
  }
  project_clear(&p);
  return command;
}

char *project_command(App *t, gboolean run)
{
  return goal_command(t, run ? GOAL_RUN : GOAL_BUILD, NULL);
}

char *project_test_command(App *t)
{
  return goal_command(t, GOAL_TEST, NULL);
}

gboolean project_can_debug(App *t)
{
  Project p = find_project(t);
  gboolean c = p.kind == PROJECT_NONE || p.kind == PROJECT_TASKS || p.kind == PROJECT_CMAKE ||
               p.kind == PROJECT_MESON || p.kind == PROJECT_MAKE || p.kind == PROJECT_FILE;

  project_clear(&p);
  return c;
}

void build_project(App *t, gboolean run)
{
  /* Every changed file is saved first. */
  save_all(t, run_saved, project_command(t, run));
}

void run_project_tests(App *t)
{
  save_all(t, run_saved, project_test_command(t));
}

char *project_run_folder(App *t)
{
  Project p = find_project(t);
  char *folder = run_folder(t, &p);

  project_clear(&p);
  return folder;
}

char *build_for_debug(App *t)
{
  char *cwd = NULL;

  save_all(t, run_saved, goal_command(t, GOAL_DEBUG, &cwd));
  return cwd;
}
