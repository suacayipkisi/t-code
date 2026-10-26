/*
 * Building and running files of other languages than C / C++ (see runner.h): a script by its
 * interpreter (scripts[], or an executable file starting with "#!"), a Python file from its project's
 * root, a Rust file alone with rustc, and Cargo, Go, Node, Maven and Gradle projects.
 */
#include "project/run/runner.h"

#include "lsp/json.h"

#include <stdio.h>
#include <string.h>

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

/* How file runs when it is a script: by its extension, or itself when it has none, is executable and
 * starts with "#!"; NULL when it is not one. */
const Script *script_of(const char *file)
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
char *script_program(const Script *script)
{
  return g_strndup(script->run, strcspn(script->run, " "));
}

/* A Python file's project root: the nearest folder upwards with a project file or a venv. */
char *python_root(const char *start)
{
  static const char *const marks[] = { "pyproject.toml", "setup.py",         "setup.cfg",       "requirements.txt",
                                       "Pipfile",        ".venv/bin/python", "venv/bin/python", NULL };

  return find_up(start, marks);
}

/* A file of another language than C / C++ in its project (Cargo, Go, Node, Maven, Gradle); FALSE when
 * it has none. */
gboolean find_language_project(Project *p, const char *start)
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

/* The shell command that runs a script with its interpreter in its folder (Build: checks it). A
 * Python file runs from its project's root, which goes into PYTHONPATH (its imports of the project's
 * packages work from any folder), with the project's venv when it has one. */
char *script_command(const Project *p, Goal goal)
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
char *rustc_command(const char *file, Goal goal)
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
const char *node_manager(const char *root)
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
char *language_command(const Project *p, Goal goal)
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
