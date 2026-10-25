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
#include "project/run.h"

#include "debug/debugger.h"
#include "editor/editor.h"
#include "editor/tabs.h"
#include "lsp/json.h"
#include "project/langenv.h"
#include "project/toolchain.h"
#include "window/terminal.h"
#include <json-glib/json-glib.h>

#include <stdio.h>
#include <string.h>

#define C_FLAGS "-std=c17 -Wall -Wextra -g -fsanitize=address,undefined"
#define CXX_FLAGS "-std=c++20 -Wall -Wextra -g -fsanitize=address,undefined"

typedef enum {
  PROJECT_NONE,
  PROJECT_TASKS, /* .t-code/tasks.json or t-code.json */
  PROJECT_CMAKE,
  PROJECT_MESON,
  PROJECT_MAKE,
  PROJECT_FILE, /* a C / C++ file alone */
  PROJECT_SCRIPT,
  PROJECT_RUSTC, /* a Rust file alone */
  PROJECT_CARGO,
  PROJECT_GO,
  PROJECT_NODE,
  PROJECT_MAVEN,
  PROJECT_GRADLE,
} ProjectKind;

typedef enum {
  GOAL_BUILD,
  GOAL_RUN,
  GOAL_DEBUG, /* build, then hand the program to the debugger */
  GOAL_TEST,
} Goal;

typedef struct {
  const char *ext;
  const char *run;   /* the interpreter (its first word is the program), NULL: the file runs itself */
  const char *check; /* Build: checks the syntax without running it, or NULL */
} Script;

/* clang-format off */
static const Script scripts[] = {
  { ".py",   "python3", "python3 -c 'import sys; compile(open(sys.argv[1], \"rb\").read(), sys.argv[1], \"exec\")'" },
  { ".js",   "node",    "node --check" },
  { ".mjs",  "node",    "node --check" },
  { ".cjs",  "node",    "node --check" },
  { ".ts",   "node",    NULL }, /* Node 22.18 and newer run TypeScript */
  { ".sh",   "bash",    "bash -n" },
  { ".lua",  "lua",     "luac -p" },
  { ".rb",   "ruby",    "ruby -c" },
  { ".pl",   "perl",    "perl -c" },
  { ".php",  "php",     "php -l" },
  { ".go",   "go run",  "go vet" },
  { ".java", "java",    NULL }, /* JDK 11 and newer run one source file */
};
/* clang-format on */

typedef struct {
  ProjectKind kind;
  char *root;           /* the folder with the project file, or the file itself (FILE, SCRIPT, RUSTC) */
  const char *file;     /* the current file */
  const Script *script; /* PROJECT_SCRIPT: how it runs */
  char *python;         /* a Python file's project root, or NULL */
  char *interpreter;    /* the Python it runs with (toolchain_python), or NULL: python3 */
  char *cargo_flags;    /* PROJECT_CARGO: the picked features and target (" --features a") */
  char *task_name;      /* PROJECT_TASKS: profile or task name */
} Project;

static void project_clear(Project *p)
{
  g_free(p->root);
  g_free(p->python);
  g_free(p->interpreter);
  g_free(p->cargo_flags);
  g_free(p->task_name);
}

/* The nearest directory, from start upwards, that holds one of names. */
static char *find_up(const char *start, const char *const *names)
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

/* How file runs when it is a script: by its extension, or itself when it has none, is executable and
 * starts with "#!"; NULL when it is not one. */
static const Script *script_of(const char *file)
{
  static const Script itself = { NULL, NULL, NULL };
  const char *name = file ? strrchr(file, '/') : NULL;
  const char *ext = name ? strrchr(name, '.') : NULL;
  char head[2] = { 0 };

  if (!name)
    return NULL;
  for (size_t i = 0; ext && i < G_N_ELEMENTS(scripts); i++)
    if (strcmp(ext, scripts[i].ext) == 0)
      return &scripts[i];
  if (ext || !g_file_test(file, G_FILE_TEST_IS_EXECUTABLE) || g_file_test(file, G_FILE_TEST_IS_DIR))
    return NULL;
  FILE *f = fopen(file, "rb");
  size_t got = f ? fread(head, 1, 2, f) : 0;
  if (f)
    fclose(f);
  return got == 2 && head[0] == '#' && head[1] == '!' ? &itself : NULL;
}

/* The program a script's interpreter is ("go" for "go run"); the caller frees it. */
static char *script_program(const Script *script)
{
  return g_strndup(script->run, strcspn(script->run, " "));
}

/* Whether file's extension is one of exts. */
static gboolean has_ext(const char *file, const char *const *exts)
{
  const char *name = file ? strrchr(file, '/') : NULL;
  const char *ext = name ? strrchr(name, '.') : NULL;

  return ext && g_strv_contains(exts, ext);
}

/* A Python file's project root: the nearest folder upwards with a project file or a venv. */
static char *python_root(const char *start)
{
  static const char *const marks[] = { "pyproject.toml", "setup.py",         "setup.cfg",       "requirements.txt",
                                       "Pipfile",        ".venv/bin/python", "venv/bin/python", NULL };

  return find_up(start, marks);
}

/* A file of another language than C / C++ in its project (Cargo, Go, Node, Maven, Gradle); FALSE when
 * it has none. */
static gboolean find_language_project(Project *p, const char *start)
{
  static const char *const rust[] = { ".rs", NULL }, *const go[] = { ".go", NULL };
  static const char *const node[] = { ".js", ".mjs", ".cjs", ".ts", ".mts", ".cts", ".jsx", ".tsx", NULL };
  static const char *const jvm[] = { ".java", ".kt", NULL };
  static const char *const cargo[] = { "Cargo.toml", NULL }, *const gomod[] = { "go.mod", NULL };
  static const char *const package[] = { "package.json", NULL }, *const pom[] = { "pom.xml", NULL };
  static const char *const gradle[] = { "build.gradle", "build.gradle.kts", "settings.gradle", "settings.gradle.kts",
                                        NULL };

  if (has_ext(p->file, rust) && (p->root = find_up(start, cargo)))
    p->kind = PROJECT_CARGO;
  else if (has_ext(p->file, go) && (p->root = find_up(start, gomod)))
    p->kind = PROJECT_GO;
  else if (has_ext(p->file, node) && (p->root = find_up(start, package)))
    p->kind = PROJECT_NODE;
  else if (has_ext(p->file, jvm) && (p->root = find_up(start, pom)))
    p->kind = PROJECT_MAVEN;
  else if (has_ext(p->file, jvm) && (p->root = find_up(start, gradle)))
    p->kind = PROJECT_GRADLE;
  else if (has_ext(p->file, rust)) {
    p->kind = PROJECT_RUSTC;
    p->root = g_strdup(p->file);
  }
  return p->kind != PROJECT_NONE;
}

typedef struct {
  char *name;
  char *build;
  char *run;
  char *debug;
  char *test;
  char *args;
  char *env;
  char *cwd;
} TaskProfile;

static void task_profile_clear(TaskProfile *tp)
{
  g_free(tp->name);
  g_free(tp->build);
  g_free(tp->run);
  g_free(tp->debug);
  g_free(tp->test);
  g_free(tp->args);
  g_free(tp->env);
  g_free(tp->cwd);
}

static char *parse_task_args(JsonObject *obj)
{
  if (!json_object_has_member(obj, "args"))
    return NULL;
  JsonNode *node = json_object_get_member(obj, "args");
  if (JSON_NODE_HOLDS_ARRAY(node)) {
    JsonArray *arr = json_node_get_array(node);
    guint len = json_array_get_length(arr);
    GString *s = g_string_new("");
    for (guint i = 0; i < len; i++) {
      const char *arg = json_array_get_string_element(arr, i);
      char *quoted = g_shell_quote(arg ? arg : "");
      if (s->len > 0)
        g_string_append_c(s, ' ');
      g_string_append(s, quoted);
      g_free(quoted);
    }
    return g_string_free(s, FALSE);
  }
  if (JSON_NODE_HOLDS_VALUE(node)) {
    const char *str = json_node_get_string(node);
    return str ? g_strdup(str) : NULL;
  }
  return NULL;
}

static char *parse_task_env(JsonObject *obj)
{
  if (!json_object_has_member(obj, "env"))
    return NULL;
  JsonNode *node = json_object_get_member(obj, "env");
  if (!JSON_NODE_HOLDS_OBJECT(node))
    return NULL;
  JsonObject *env_obj = json_node_get_object(node);
  GList *members = json_object_get_members(env_obj);
  GString *s = g_string_new("");
  for (GList *l = members; l; l = l->next) {
    const char *key = l->data;
    const char *val = json_object_get_string_member(env_obj, key);
    char *quoted = g_shell_quote(val ? val : "");
    if (s->len > 0)
      g_string_append_c(s, ' ');
    g_string_append_printf(s, "%s=%s", key, quoted);
    g_free(quoted);
  }
  g_list_free(members);
  return g_string_free(s, FALSE);
}

static gboolean load_task_profile_from_obj(JsonObject *obj, const char *name, TaskProfile *tp)
{
  tp->name = g_strdup(name);
  if (json_object_has_member(obj, "build"))
    tp->build = g_strdup(json_object_get_string_member(obj, "build"));
  if (json_object_has_member(obj, "run"))
    tp->run = g_strdup(json_object_get_string_member(obj, "run"));
  if (json_object_has_member(obj, "debug"))
    tp->debug = g_strdup(json_object_get_string_member(obj, "debug"));
  if (json_object_has_member(obj, "test"))
    tp->test = g_strdup(json_object_get_string_member(obj, "test"));
  if (json_object_has_member(obj, "cwd"))
    tp->cwd = g_strdup(json_object_get_string_member(obj, "cwd"));
  tp->args = parse_task_args(obj);
  tp->env = parse_task_env(obj);
  return tp->build != NULL || tp->run != NULL || tp->debug != NULL || tp->test != NULL;
}

static gboolean load_task_profile(const char *root, TaskProfile *tp)
{
  char *paths[] = { g_build_filename(root, ".t-code", "tasks.json", NULL), g_build_filename(root, "t-code.json", NULL),
                    NULL };
  char *chosen = NULL;
  for (int i = 0; paths[i]; i++) {
    if (g_file_test(paths[i], G_FILE_TEST_IS_REGULAR)) {
      chosen = paths[i];
      break;
    }
  }
  if (!chosen) {
    for (int i = 0; paths[i]; i++)
      g_free(paths[i]);
    return FALSE;
  }

  JsonParser *parser = json_parser_new();
  gboolean ok = FALSE;
  if (json_parser_load_from_file(parser, chosen, NULL)) {
    JsonNode *rnode = json_parser_get_root(parser);
    if (rnode && JSON_NODE_HOLDS_OBJECT(rnode)) {
      JsonObject *root_obj = json_node_get_object(rnode);
      if (json_object_has_member(root_obj, "profiles")) {
        JsonNode *pnode = json_object_get_member(root_obj, "profiles");
        if (JSON_NODE_HOLDS_OBJECT(pnode)) {
          JsonObject *profiles_obj = json_node_get_object(pnode);
          const char *target =
              json_object_has_member(root_obj, "default") ? json_object_get_string_member(root_obj, "default") : NULL;
          JsonObject *target_obj = NULL;
          if (target && json_object_has_member(profiles_obj, target)) {
            target_obj = json_object_get_object_member(profiles_obj, target);
          } else {
            GList *m = json_object_get_members(profiles_obj);
            if (m) {
              target = m->data;
              target_obj = json_object_get_object_member(profiles_obj, target);
              g_list_free(m);
            }
          }
          if (target_obj)
            ok = load_task_profile_from_obj(target_obj, target ? target : "default", tp);
        }
      }
      if (!ok)
        ok = load_task_profile_from_obj(root_obj, "default", tp);
    }
  }
  g_object_unref(parser);
  for (int i = 0; paths[i]; i++)
    g_free(paths[i]);
  return ok;
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
    TaskProfile tp = { 0 };
    if (load_task_profile(p.root, &tp)) {
      p.task_name = g_strdup(tp.name);
      task_profile_clear(&tp);
    }
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
static char *send_verb(const char *verb, const char *what)
{
  char *exe = g_file_read_link("/proc/self/exe", NULL);
  char *quoted = g_shell_quote(exe ? exe : "t-code");
  char *command = g_strdup_printf("%s --send \"%s \"%s", quoted, verb, what);

  g_free(quoted);
  g_free(exe);
  return command;
}
/* The folder a task profile's commands run in: its cwd (relative to root, the tasks file's folder)
 * or root. */
static char *task_folder(const char *root, const TaskProfile *tp)
{
  if (!tp->cwd)
    return g_strdup(root);
  return g_path_is_absolute(tp->cwd) ? g_strdup(tp->cwd) : g_build_filename(root, tp->cwd, NULL);
}

/* The shell command that builds, runs or debugs according to .t-code/tasks.json (or t-code.json). */
static char *tasks_command(const char *root, Goal goal)
{
  TaskProfile tp = { 0 };
  if (!load_task_profile(root, &tp))
    return goal == GOAL_TEST ? g_strdup("echo 't-code: could not read .t-code/tasks.json'; false")
                             : g_strdup("echo 't-code: could not read .t-code/tasks.json'");

  char *workdir = task_folder(root, &tp);
  char *qdir = g_shell_quote(workdir);
  char *cmd = NULL;

  if (goal == GOAL_BUILD) {
    if (tp.build)
      cmd = g_strdup_printf("cd %s && %s", qdir, tp.build);
    else
      cmd = g_strdup("echo 'Build OK (no build command configured in tasks.json)'");
  } else if (goal == GOAL_TEST) {
    cmd = tp.test && *tp.test ? g_strdup_printf("cd %s && %s%s%s", qdir, tp.env && *tp.env ? tp.env : "",
                                                tp.env && *tp.env ? " " : "", tp.test)
                              : g_strdup("echo 't-code: no test command configured in tasks.json'; false");
  } else if (goal == GOAL_RUN) {
    const char *run_cmd = tp.run ? tp.run : tp.debug;
    if (run_cmd) {
      GString *exec_line = g_string_new("");
      g_string_append_printf(exec_line, "cd %s && ", qdir);
      if (tp.env && *tp.env)
        g_string_append_printf(exec_line, "%s ", tp.env);
      g_string_append(exec_line, run_cmd);
      if (tp.args && *tp.args)
        g_string_append_printf(exec_line, " %s", tp.args);

      if (tp.build)
        cmd = g_strdup_printf("cd %s && %s && %s", qdir, tp.build, exec_line->str);
      else
        cmd = g_strdup(exec_line->str);
      g_string_free(exec_line, TRUE);
    } else {
      cmd = g_strdup("echo 't-code: no run command configured in tasks.json'");
    }
  } else if (goal == GOAL_DEBUG) {
    const char *dbg_exe = tp.debug ? tp.debug : tp.run;
    if (dbg_exe) {
      char *full_exe = g_path_is_absolute(dbg_exe) ? g_strdup(dbg_exe) : g_build_filename(workdir, dbg_exe, NULL);
      char *qexe = g_shell_quote(full_exe);
      char *debug = send_verb("debug", qexe);
      if (tp.build)
        cmd = g_strdup_printf("cd %s && %s && %s", qdir, tp.build, debug);
      else
        cmd = g_strdup(debug);
      g_free(debug);
      g_free(qexe);
      g_free(full_exe);
    } else {
      cmd = g_strdup("echo 't-code: no debug target configured in tasks.json'");
    }
  }

  g_free(qdir);
  g_free(workdir);
  task_profile_clear(&tp);
  return cmd ? cmd : g_strdup("true");
}

static char *if_installed(const char *program, char *what);
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

/* A Python file's module name when it is inside its root's packages ("app.cli" for app/cli.py with
 * app/__init__.py; "app" for app/__main__.py), else NULL: it then runs as a file. Relative imports
 * (from .core import x) work only when it runs as a module. */
static char *python_module(const char *root, const char *file)
{
  size_t len = strlen(root);
  char *at, *module;
  gboolean package;

  if (!g_str_has_prefix(file, root) || file[len] != '/')
    return NULL;
  at = g_path_get_dirname(file);
  package = strlen(at) > len;           /* a file in the root itself is in no package */
  while (package && strlen(at) > len) { /* every folder up to the root has __init__.py */
    char *init = g_build_filename(at, "__init__.py", NULL), *parent = g_path_get_dirname(at);
    package = g_file_test(init, G_FILE_TEST_IS_REGULAR);
    g_free(init);
    g_free(at);
    at = parent;
  }
  g_free(at);
  if (!package)
    return NULL;
  module = g_strndup(file + len + 1, strlen(file + len + 1) - strlen(".py"));
  g_strdelimit(module, "/", '.');
  if (g_str_has_suffix(module, ".__main__"))
    module[strlen(module) - strlen(".__main__")] = '\0';
  return module;
}

/* what (taken), run only when program is installed. */
static char *if_installed(const char *program, char *what)
{
  char *cmd = g_strdup_printf("if command -v %s >/dev/null 2>&1; then %s; else echo 't-code: %s is not installed'; fi",
                              program, what, program);

  g_free(what);
  return cmd;
}

/* The shell command that runs a script with its interpreter in its folder (Build: checks it). A
 * Python file runs from its project's root, which goes into PYTHONPATH (its imports of the project's
 * packages work from any folder), with the project's venv when it has one. */
static char *script_command(const Project *p, Goal goal)
{
  const Script *script = p->script;
  char *dir = p->python ? g_strdup(p->python) : g_path_get_dirname(p->file);
  char *qdir = g_shell_quote(dir), *qfile = g_shell_quote(p->file);
  const char *venv = p->interpreter;
  char *qvenv = venv ? g_shell_quote(venv) : NULL;
  char *program = script->run ? script_program(script) : NULL;
  /* The interpreter and the syntax check, which starts with it: the venv's python replaces it. */
  char *run = qvenv ? g_strdup(qvenv) : g_strdup(script->run);
  char *check =
      script->check && qvenv ? g_strconcat(qvenv, script->check + strlen(script->run), NULL) : g_strdup(script->check);
  char *path = p->python ? g_strdup_printf("PYTHONPATH=%s${PYTHONPATH:+:$PYTHONPATH} ", qdir) : g_strdup("");
  char *what;

  if (goal == GOAL_DEBUG) /* the debugger does not ask (project_can_debug) */
    what = g_strdup("echo 't-code: debugging is for C and C++ programs; Ctrl+F5 runs this file'; false");
  else if (goal == GOAL_RUN) /* in a subshell: the shell stays in its own directory */
  {
    char *module = p->python ? python_module(p->python, p->file) : NULL,
         *qmodule = module ? g_shell_quote(module) : NULL;
    if (module)
      what = g_strdup_printf("(cd %s && %s%s -m %s)", qdir, path, run, qmodule);
    else
      what = script->run ? g_strdup_printf("(cd %s && %s%s %s)", qdir, path, run, qfile)
                         : g_strdup_printf("(cd %s && %s)", qdir, qfile);
    g_free(qmodule);
    g_free(module);
  } else if (check)
    what = g_strdup_printf("(cd %s && %s %s) && echo 't-code: no syntax errors'", qdir, check, qfile);
  else
    what = g_strdup("echo 't-code: nothing to build, Ctrl+F5 runs this file'");
  if (program && !venv && goal != GOAL_DEBUG)
    what = if_installed(program, what);
  g_free(path);
  g_free(check);
  g_free(run);
  g_free(program);
  g_free(qvenv);
  g_free(qfile);
  g_free(qdir);
  g_free(dir);
  return what;
}

/* A Rust file alone: compiled into build/ next to it (and run in its folder). */
static char *rustc_command(const char *file, Goal goal)
{
  char *dir = g_path_get_dirname(file), *base = g_path_get_basename(file);
  *strrchr(base, '.') = '\0';
  char *out_dir = g_build_filename(dir, "build", NULL), *out = g_build_filename(out_dir, base, NULL);
  char *qdir = g_shell_quote(dir), *qfile = g_shell_quote(file), *qout_dir = g_shell_quote(out_dir);
  char *qout = g_shell_quote(out);
  char *cmd = goal == GOAL_RUN ? g_strdup_printf("mkdir -p %s && rustc -g -o %s %s && (cd %s && %s)", qout_dir, qout,
                                                 qfile, qdir, qout)
                               : g_strdup_printf("mkdir -p %s && rustc -g -o %s %s", qout_dir, qout, qfile);

  g_free(qout);
  g_free(qout_dir);
  g_free(qfile);
  g_free(qdir);
  g_free(out);
  g_free(out_dir);
  g_free(base);
  g_free(dir);
  return if_installed("rustc", cmd);
}

/* Where cargo runs for a crate: the crate itself, or its workspace's root when the crate has nothing
 * to run (a library: cargo run there fails, the workspace's root runs its program). */
static char *cargo_root(const char *crate)
{
  char *main_rs = g_build_filename(crate, "src", "main.rs", NULL), *bin = g_build_filename(crate, "src", "bin", NULL);
  char *toml = g_build_filename(crate, "Cargo.toml", NULL), *text = NULL, *root = NULL;
  gboolean runs = g_file_test(main_rs, G_FILE_TEST_EXISTS) || g_file_test(bin, G_FILE_TEST_IS_DIR) ||
                  (g_file_get_contents(toml, &text, NULL, NULL) && strstr(text, "[[bin]]"));

  g_free(text);
  char *dir = runs ? NULL : g_path_get_dirname(crate);
  while (dir && !root) { /* upwards to the workspace's Cargo.toml */
    char *path = g_build_filename(dir, "Cargo.toml", NULL), *parent = g_path_get_dirname(dir);
    text = NULL;
    if (g_file_get_contents(path, &text, NULL, NULL) && strstr(text, "[workspace]"))
      root = g_strdup(dir);
    if (strcmp(parent, dir) == 0) /* the top */
      g_clear_pointer(&parent, g_free);
    g_free(text);
    g_free(path);
    g_free(dir);
    dir = parent;
  }
  g_free(dir);
  g_free(toml);
  g_free(bin);
  g_free(main_rs);
  return root ? root : g_strdup(crate);
}

/* Cargo's target for a file: "--bin name" in src/bin/, "--example name" in examples/, else "". */
static char *cargo_target(const char *root, const char *file)
{
  char *bin = g_build_filename(root, "src", "bin", NULL), *examples = g_build_filename(root, "examples", NULL);
  char *dir = g_path_get_dirname(file), *base = g_path_get_basename(file), *target = NULL;
  gboolean in_bin = g_str_has_prefix(file, bin) && file[strlen(bin)] == '/';
  gboolean in_examples = g_str_has_prefix(file, examples) && file[strlen(examples)] == '/';

  *strrchr(base, '.') = '\0';
  if (in_bin || in_examples) {
    /* src/bin/name.rs, or src/bin/name/main.rs */
    char *name = strcmp(base, "main") == 0 && strcmp(dir, in_bin ? bin : examples) != 0 ? g_path_get_basename(dir)
                                                                                        : g_strdup(base);
    char *quoted = g_shell_quote(name);
    target = g_strdup_printf(" %s %s", in_bin ? "--bin" : "--example", quoted);
    g_free(quoted);
    g_free(name);
  }
  g_free(base);
  g_free(dir);
  g_free(examples);
  g_free(bin);
  return target ? target : g_strdup("");
}

/* The Node package manager the project uses, by its lock file. */
static const char *node_manager(const char *root)
{
  static const char *const locks[][2] = {
    { "pnpm-lock.yaml", "pnpm" }, { "yarn.lock", "yarn" }, { "bun.lock", "bun" }, { "bun.lockb", "bun" }
  };

  for (size_t i = 0; i < G_N_ELEMENTS(locks); i++) {
    char *path = g_build_filename(root, locks[i][0], NULL);
    gboolean found = g_file_test(path, G_FILE_TEST_EXISTS);
    g_free(path);
    if (found)
      return locks[i][1];
  }
  return "npm";
}

/* The first of names that package.json's "scripts" has, or NULL; the caller frees it. */
static char *node_script(const char *root, const char *const *names)
{
  char *path = g_build_filename(root, "package.json", NULL);
  JsonParser *parser = json_parser_new();
  char *found = NULL;

  if (json_parser_load_from_file(parser, path, NULL)) {
    JsonNode *scripts = json_at(json_parser_get_root(parser), "scripts");
    JsonObject *object = scripts && JSON_NODE_HOLDS_OBJECT(scripts) ? json_node_get_object(scripts) : NULL;
    for (int i = 0; object && names[i] && !found; i++)
      if (json_object_has_member(object, names[i]))
        found = g_strdup(names[i]);
  }
  g_object_unref(parser);
  g_free(path);
  return found;
}

/* A Java class's full name from its package line and file name ("com.example.App"). */
static char *java_class(const char *file)
{
  char *text = NULL, *base = g_path_get_basename(file), *name;
  GMatchInfo *match = NULL;
  static GRegex *package;

  *strrchr(base, '.') = '\0';
  if (!package)
    package = g_regex_new("^\\s*package\\s+([\\w.]+)\\s*;", G_REGEX_MULTILINE, 0, NULL);
  if (g_file_get_contents(file, &text, NULL, NULL) && g_regex_match(package, text, 0, &match)) {
    char *dotted = g_match_info_fetch(match, 1);
    name = g_strdup_printf("%s.%s", dotted, base);
    g_free(dotted);
  } else {
    name = g_strdup(base);
  }
  g_match_info_free(match);
  g_free(text);
  g_free(base);
  return name;
}

/* The shell command for another language's project: Cargo, Go, Node, Maven or Gradle. */
static char *language_command(const Project *p, Goal goal)
{
  static const char *const starts[] = { "start", "dev", NULL }, *const builds[] = { "build", NULL },
                           *const tests[] = { "test", NULL };
  char *qroot = g_shell_quote(p->root), *dir = g_path_get_dirname(p->file), *qdir = g_shell_quote(dir);
  char *qfile = g_shell_quote(p->file), *cmd = NULL;
  const char *program = NULL;

  switch (p->kind) {
    case PROJECT_CARGO: {
      char *target = cargo_target(p->root, p->file), *where = cargo_root(p->root), *qwhere = g_shell_quote(where);
      program = "cargo";
      const char *flags = p->cargo_flags ? p->cargo_flags : "";
      cmd = goal == GOAL_TEST  ? g_strdup_printf("(cd %s && cargo test%s)", qwhere, flags)
            : goal == GOAL_RUN ? g_strdup_printf("(cd %s && cargo run%s%s)", qwhere, target, flags)
                               : g_strdup_printf("(cd %s && cargo build%s%s)", qwhere, target, flags);
      g_free(qwhere);
      g_free(where);
      g_free(target);
      break;
    }
    case PROJECT_GO: /* the file's package, which go finds the module of */
      program = "go";
      cmd = goal == GOAL_TEST  ? g_strdup_printf("(cd %s && go test ./...)", qroot)
            : goal == GOAL_RUN ? g_strdup_printf("(cd %s && go run .)", qdir)
                               : g_strdup_printf("(cd %s && go vet .)", qdir);
      break;
    case PROJECT_NODE: {
      const char *manager = node_manager(p->root);
      const char *const *wanted = goal == GOAL_TEST ? tests : goal == GOAL_RUN ? starts : builds;
      char *script = node_script(p->root, wanted);
      program = manager;
      if (script)
        cmd = goal == GOAL_TEST ? g_strdup_printf("(cd %s && %s test)", qroot, manager)
                                : g_strdup_printf("(cd %s && %s run %s)", qroot, manager, script);
      else if (goal == GOAL_RUN) {
        program = "node";
        cmd = g_strdup_printf("(cd %s && node %s)", qroot, qfile);
      } else if (goal == GOAL_TEST)
        cmd = g_strdup("echo 't-code: no test script in package.json'; false");
      else
        cmd = g_strdup("echo 't-code: no build script in package.json'");
      g_free(script);
      break;
    }
    case PROJECT_MAVEN: {
      program = "mvn";
      if (goal == GOAL_TEST)
        cmd = g_strdup_printf("(cd %s && mvn test)", qroot);
      else {
        char *class = java_class(p->file), *qclass = g_shell_quote(class);
        cmd = goal == GOAL_RUN
                  ? g_strdup_printf("(cd %s && mvn -q compile && java -cp target/classes %s)", qroot, qclass)
                  : g_strdup_printf("(cd %s && mvn -q compile)", qroot);
        g_free(qclass);
        g_free(class);
      }
      break;
    }
    case PROJECT_GRADLE: {
      char *wrapper = g_build_filename(p->root, "gradlew", NULL);
      gboolean own = g_file_test(wrapper, G_FILE_TEST_IS_EXECUTABLE);
      program = own ? NULL : "gradle";
      cmd = g_strdup_printf("(cd %s && %s %s)", qroot, own ? "./gradlew" : "gradle",
                            goal == GOAL_TEST  ? "test"
                            : goal == GOAL_RUN ? "run"
                                               : "build");
      g_free(wrapper);
      break;
    }
    default: break;
  }
  if (goal == GOAL_DEBUG) {
    g_free(cmd);
    cmd = g_strdup("echo 't-code: debugging is for C and C++ programs; Ctrl+F5 runs this file'; false");
  } else if (program) {
    cmd = if_installed(program, cmd);
  }
  g_free(qfile);
  g_free(qdir);
  g_free(dir);
  g_free(qroot);
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
  gboolean alone =
      p->kind == PROJECT_FILE || p->kind == PROJECT_SCRIPT || p->kind == PROJECT_RUSTC; /* root: the file */
  TaskProfile tp = { 0 };
  char *folder;

  if (alone)
    return g_path_get_dirname(p->root);
  if (p->kind == PROJECT_TASKS && load_task_profile(p->root, &tp)) {
    folder = task_folder(p->root, &tp);
    task_profile_clear(&tp);
    return folder;
  }
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
