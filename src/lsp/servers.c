/*
 * The language servers (see servers.h): clangd for C/C++ and Objective-C, pylsp for Python, gopls for Go,
 * rust-analyzer for Rust, typescript-language-server for TypeScript and JavaScript, the JSON language
 * server, yaml-language-server, marksman for Markdown and neocmakelsp for CMake; and the ones plugins
 * declare.
 */
#include "lsp/servers.h"

#include "lsp/json.h"
#include "plugins/plugins.h"
#include "project/langenv.h"
#include "project/toolchain.h"
#include "project/trust.h"

#include <glib/gstdio.h>
#include <string.h>
#include <unistd.h>

#define MAX_JSON_BYTES (512 * 1024) /* a bigger JSON file is data, not something edited by hand */

#ifndef SERVER_ARGV /* tests run a fake server */
/* clang-format off */
static const char *const clangd_argv[] = { "clangd", NULL }; /* its flags: server_clangd_flags */
static const char *const python_argv[] = { "pylsp", NULL };
static const char *const go_argv[] = { "gopls", NULL };
static const char *const rust_argv[] = { "rust-analyzer", NULL };
static const char *const typescript_argv[] = { "typescript-language-server", "--stdio", NULL };
static const char *const json_argv[] = { "vscode-json-language-server", "--stdio", NULL };
static const char *const yaml_argv[] = { "yaml-language-server", "--stdio", NULL };
static const char *const markdown_argv[] = { "marksman", "server", NULL };
static const char *const cmake_argv[] = { "neocmakelsp", "stdio", NULL };
/* clang-format on */
#else
static const char *const fake_argv[] = { SERVER_ARGV NULL };
#endif

/* clang-format off */
static const struct {
  const char *id, *title;
} builtin[SERVER_NONE] = {
  [SERVER_CLANGD] = { "clangd", "C, C++ and Objective-C" },
  [SERVER_PYTHON] = { "pylsp", "Python" },
  [SERVER_GO] = { "gopls", "Go" },
  [SERVER_RUST] = { "rust-analyzer", "Rust" },
  [SERVER_TYPESCRIPT] = { "typescript-language-server", "TypeScript and JavaScript" },
  [SERVER_JSON] = { "vscode-json-language-server", "JSON" },
  [SERVER_YAML] = { "yaml-language-server", "YAML" },
  [SERVER_MARKDOWN] = { "marksman", "Markdown" },
  [SERVER_CMAKE] = { "neocmakelsp", "CMake" },
};
/* clang-format on */

const char *server_id(ServerKind kind)
{
  return kind < SERVER_NONE ? builtin[kind].id : NULL;
}

const char *server_title(ServerKind kind)
{
  return kind < SERVER_NONE ? builtin[kind].title : NULL;
}

ServerKind server_by_id(const char *id)
{
  for (int kind = 0; id && kind < SERVER_NONE; kind++)
    if (strcmp(builtin[kind].id, id) == 0)
      return kind;
  return SERVER_NONE;
}

gboolean server_enabled(App *t, ServerKind kind)
{
  return kind >= SERVER_NONE || !(t->settings.servers_off & (1u << kind));
}

/* Plugins' servers ------------------------------------------------------------------------------- */

/* A plugin's server as it was last matched: a copy, so that a plugin reloaded or gone leaves no
 * dangling pointer behind in a running Lsp. */
typedef struct {
  char *key; /* "<plugin id>/<server name>" */
  char *name, *dir, *language_id, *options, *settings;
  char **argv, **language_ids, **contents;
} Declared;

static GPtrArray *declared; /* Declared *; the one at i is kind SERVER_PLUGIN + i (never removed) */

static Declared *declared_of(ServerKind kind)
{
  guint i = kind - SERVER_PLUGIN;

  return kind >= SERVER_PLUGIN && declared && i < declared->len ? g_ptr_array_index(declared, i) : NULL;
}

/* The kind for p's server s, its copy brought up to date. */
static ServerKind declare(Plugin *p, PluginServer *s)
{
  char *key = g_strconcat(p->id, "/", s->name, NULL);
  Declared *d = NULL;
  guint i;

  if (!declared)
    declared = g_ptr_array_new();
  for (i = 0; i < declared->len && !d; i++)
    if (strcmp(((Declared *)g_ptr_array_index(declared, i))->key, key) == 0)
      d = g_ptr_array_index(declared, i);
  if (d) {
    i--;
    g_free(key);
  } else {
    d = g_new0(Declared, 1);
    d->key = key;
    g_ptr_array_add(declared, d);
  }
  g_set_str(&d->name, s->name);
  g_set_str(&d->dir, p->dir);
  g_set_str(&d->language_id, s->language_id);
  g_set_str(&d->options, s->options);
  g_set_str(&d->settings, s->settings);
  g_strfreev(d->argv);
  d->argv = g_strdupv(s->argv);
  g_strfreev(d->language_ids);
  d->language_ids = g_strdupv(s->language_ids);
  g_strfreev(d->contents);
  d->contents = g_strdupv(s->contents);
  return SERVER_PLUGIN + i;
}

/* The first plugin server for path, whose GtkSourceView language is id; SERVER_NONE when none. */
static ServerKind plugin_kind(App *t, const char *path, const char *id)
{
  char *name = path ? g_path_get_basename(path) : NULL;
  ServerKind kind = SERVER_NONE;

  for (guint i = 0; t && t->plugins && i < t->plugins->len && kind == SERVER_NONE; i++) {
    Plugin *p = g_ptr_array_index(t->plugins, i);
    for (guint j = 0; p->enabled && p->servers && j < p->servers->len && kind == SERVER_NONE; j++) {
      PluginServer *s = g_ptr_array_index(p->servers, j);
      if (plugin_name_matches(name, s->extensions) || (*id && g_strv_contains((const char *const *)s->languages, id)))
        kind = declare(p, s);
    }
  }
  g_free(name);
  return kind;
}

/* The built-in ones ------------------------------------------------------------------------------ */

static const char *const *server_command(ServerKind kind)
{
#ifdef SERVER_ARGV
  (void)kind; /* every language's server is the fake one */
  return fake_argv;
#else
  switch (kind) {
    case SERVER_CLANGD: return clangd_argv;
    case SERVER_PYTHON: return python_argv;
    case SERVER_GO: return go_argv;
    case SERVER_RUST: return rust_argv;
    case SERVER_TYPESCRIPT: return typescript_argv;
    case SERVER_JSON: return json_argv;
    case SERVER_YAML: return yaml_argv;
    case SERVER_MARKDOWN: return markdown_argv;
    case SERVER_CMAKE: return cmake_argv;
    default: return NULL;
  }
#endif
}

gboolean server_restarts_cheaply(ServerKind kind)
{
  return kind < SERVER_NONE && kind != SERVER_GO && kind != SERVER_RUST;
}

const char *server_contents_request(ServerKind kind, const char *scheme)
{
  Declared *d = declared_of(kind);

  for (int i = 0; d && d->contents && scheme && d->contents[i] && d->contents[i + 1]; i += 2)
    if (strcmp(d->contents[i], scheme) == 0)
      return d->contents[i + 1];
  return NULL;
}

const char *server_name(ServerKind kind)
{
  Declared *d = declared_of(kind);

  return d ? d->name : server_command(kind)[0];
}

const char *server_language(ServerKind kind)
{
  switch (kind) {
    case SERVER_CLANGD: return NULL; /* C or C++ comes from GtkSourceView */
    case SERVER_PYTHON: return "python";
    case SERVER_GO: return "go";
    case SERVER_RUST: return "rust";
    case SERVER_TYPESCRIPT: return "typescript";
    case SERVER_JSON: return "json";
    case SERVER_YAML: return "yaml";
    case SERVER_MARKDOWN: return "markdown";
    case SERVER_CMAKE: return "cmake";
    default: return NULL;
  }
}

const char *server_language_id(ServerKind kind, Tab *tab)
{
  const char *path = tab->path ? tab->path : "";
  Declared *d = declared_of(kind);

  if (d && d->language_id)
    return d->language_id;
  for (int i = 0; d && d->language_ids[i] && d->language_ids[i + 1]; i += 2) {
    char *const pattern[] = { d->language_ids[i], NULL };
    char *name = g_path_get_basename(path);
    gboolean matches = plugin_name_matches(name, pattern);
    g_free(name);
    if (matches)
      return d->language_ids[i + 1];
  }
  if (d) { /* GtkSourceView's id, which is LSP's for most languages; its headers' are C's and C++'s */
    GtkSourceLanguage *language = gtk_source_buffer_get_language(tab->buffer);
    const char *id = language ? gtk_source_language_get_id(language) : "plaintext";
    return strcmp(id, "chdr") == 0 ? "c" : strcmp(id, "cpphdr") == 0 ? "cpp" : id;
  }
  if (kind == SERVER_CLANGD) {
    const char *source_id = gtk_source_language_get_id(gtk_source_buffer_get_language(tab->buffer));
    if (strcmp(source_id, "objc") == 0)
      return g_str_has_suffix(path, ".mm") ? "objective-cpp" : "objective-c";
    return g_str_has_prefix(source_id, "cpp") ? "cpp" : "c";
  }
  if (kind == SERVER_TYPESCRIPT) {
    gboolean react = g_str_has_suffix(path, "x"); /* .jsx, .tsx */
    if (g_str_has_suffix(path, ".js") || g_str_has_suffix(path, ".mjs") || g_str_has_suffix(path, ".cjs") ||
        g_str_has_suffix(path, ".jsx"))
      return react ? "javascriptreact" : "javascript";
    return react ? "typescriptreact" : "typescript";
  }
  if (kind == SERVER_JSON && g_str_has_suffix(path, ".jsonc"))
    return "jsonc";
  return server_language(kind);
}

/* Whether a JSON file gets the JSON server: not a generated one (a compile database, a lock file) nor
 * one bigger than MAX_JSON_BYTES, which the server would parse on every change for nothing. */
static gboolean json_worth_serving(const char *path)
{
  static const char *const generated[] = { "compile_commands.json", "package-lock.json", NULL };
  const char *name = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
  GStatBuf info;

  if (g_strv_contains(generated, name) || g_str_has_suffix(name, ".min.json"))
    return FALSE;
  return g_stat(path, &info) != 0 || info.st_size <= MAX_JSON_BYTES; /* not saved yet: small */
}

/* The built-in server for tab, on or off. */
static ServerKind builtin_kind(Tab *tab)
{
  GtkSourceLanguage *language = tab && tab->kind == TAB_EDITOR ? gtk_source_buffer_get_language(tab->buffer) : NULL;
  const char *id = language ? gtk_source_language_get_id(language) : "";
  const char *path = tab ? tab->path : "";

  if (strcmp(id, "c") == 0 || strcmp(id, "chdr") == 0 || strcmp(id, "cpp") == 0 || strcmp(id, "cpphdr") == 0 ||
      strcmp(id, "objc") == 0)
    return SERVER_CLANGD;
  if (g_str_has_suffix(path, ".py"))
    return SERVER_PYTHON;
  if (g_str_has_suffix(path, ".go"))
    return SERVER_GO;
  if (g_str_has_suffix(path, ".rs"))
    return SERVER_RUST;
  static const char *const scripts[] = { ".js", ".mjs", ".cjs", ".jsx", ".ts", ".mts", ".cts", ".tsx", NULL };
  for (int i = 0; scripts[i]; i++)
    if (g_str_has_suffix(path, scripts[i]))
      return SERVER_TYPESCRIPT;
  if (g_str_has_suffix(path, ".json") || g_str_has_suffix(path, ".jsonc"))
    return json_worth_serving(path) ? SERVER_JSON : SERVER_NONE;
  if (g_str_has_suffix(path, ".yaml") || g_str_has_suffix(path, ".yml"))
    return SERVER_YAML;
  if (g_str_has_suffix(path, ".md") || g_str_has_suffix(path, ".markdown"))
    return SERVER_MARKDOWN;
  if (g_str_has_suffix(path, "/CMakeLists.txt") || g_str_has_suffix(path, ".cmake"))
    return SERVER_CMAKE;
  return SERVER_NONE;
}

ServerKind server_kind_for_tab(Tab *tab)
{
  ServerKind kind = builtin_kind(tab), plugin;
  GtkSourceLanguage *language;

  if (!tab || tab->kind != TAB_EDITOR)
    return kind; /* a Markdown preview has a .md path too */
  if (kind != SERVER_NONE && !server_enabled(tab->app, kind))
    kind = SERVER_NONE;
  language = gtk_source_buffer_get_language(tab->buffer);
  plugin = plugin_kind(tab->app, tab->path, language ? gtk_source_language_get_id(language) : "");
  if (plugin == SERVER_NONE)
    return kind;
  return kind != SERVER_NONE && server_installed(tab->app, kind) ? kind : plugin;
}

/* The program ------------------------------------------------------------------------------------ */

#ifndef SERVER_ARGV
/* The pylsp installed in the project's Python's venv (it sees the venv's packages), or NULL. */
static char *venv_pylsp(App *t)
{
  char *python = toolchain_server_python(t), *bin = python ? g_path_get_dirname(python) : NULL;
  char *pylsp = bin ? g_build_filename(bin, "pylsp", NULL) : NULL;

  if (pylsp && !g_file_test(pylsp, G_FILE_TEST_IS_EXECUTABLE))
    g_clear_pointer(&pylsp, g_free);
  g_free(bin);
  g_free(python);
  return pylsp;
}
#endif

/* Where the program name is: PATH, then the folders language installers put programs in (pipx,
 * cargo install, go install), in case they are not on PATH even after the login shell's (see
 * application/shellenv.h). NULL when it is nowhere. */
static char *find_program(const char *name)
{
  char *path = g_find_program_in_path(name);
  const char *gopath = g_getenv("GOPATH"), *gobin = g_getenv("GOBIN"), *cargo = g_getenv("CARGO_HOME");
  char **gopaths = gopath && *gopath ? g_strsplit(gopath, ":", 2) : NULL;
  char *folders[] = {
    g_build_filename(g_get_home_dir(), ".local", "bin", NULL),
    cargo && *cargo ? g_build_filename(cargo, "bin", NULL) : g_build_filename(g_get_home_dir(), ".cargo", "bin", NULL),
    gobin && *gobin ? g_strdup(gobin) : NULL,
    gopaths ? g_build_filename(gopaths[0], "bin", NULL) : g_build_filename(g_get_home_dir(), "go", "bin", NULL),
  };

  for (size_t i = 0; i < G_N_ELEMENTS(folders); i++) {
    if (!folders[i])
      continue;
    char *candidate = g_build_filename(folders[i], name, NULL);
    if (!path && g_file_test(candidate, G_FILE_TEST_IS_EXECUTABLE))
      path = g_steal_pointer(&candidate);
    g_free(candidate);
    g_free(folders[i]);
  }
  g_strfreev(gopaths);
  return path;
}

/* Where the server's program is: a plugin's relative to its folder or found like the others; the
 * project's venv's pylsp, else find_program's. NULL when it is nowhere (and for the tests' fake
 * built-in server, which runs as it is). */
static char *server_program(App *t, ServerKind kind)
{
  Declared *d = declared_of(kind);

  if (d && strchr(d->argv[0], '/'))
    return g_path_is_absolute(d->argv[0]) ? g_strdup(d->argv[0]) : g_build_filename(d->dir, d->argv[0], NULL);
  if (d)
    return find_program(d->argv[0]);
#ifdef SERVER_ARGV
  (void)t;
  return NULL;
#else
  char *path = kind == SERVER_PYTHON ? venv_pylsp(t) : NULL;

  return path ? path : find_program(server_name(kind));
#endif
}

gboolean server_program_found(const char *argv0, const char *dir)
{
  char *program = strchr(argv0, '/')
                      ? (g_path_is_absolute(argv0) ? g_strdup(argv0) : g_build_filename(dir, argv0, NULL))
                      : find_program(argv0);
  gboolean found = program && g_file_test(program, G_FILE_TEST_IS_EXECUTABLE);

  g_free(program);
  return found;
}

/* clangd ------------------------------------------------------------------------------------------ */

char **server_clangd_flags(const App *t)
{
  GPtrArray *flags = g_ptr_array_new();
  char **extra = NULL;
  GError *error = NULL;

  g_ptr_array_add(flags,
                  g_strdup(t->settings.clangd.background_index ? "--background-index" : "--background-index=false"));
  if (t->settings.clangd.pch_on_disk)
    g_ptr_array_add(flags, g_strdup("--pch-storage=disk"));
  if (t->settings.clangd.threads > 0)
    g_ptr_array_add(flags, g_strdup_printf("-j=%d", t->settings.clangd.threads));
  if (t->settings.clangd.limit_results > 0)
    g_ptr_array_add(flags, g_strdup_printf("--limit-results=%d", t->settings.clangd.limit_results));
#ifdef __GLIBC__ /* clangd built on musl has no --malloc-trim (glibc's malloc_trim) and exits on it */
  g_ptr_array_add(flags, g_strdup(t->settings.clangd.malloc_trim ? "--malloc-trim" : "--malloc-trim=false"));
#endif
  g_ptr_array_add(flags, g_strdup(t->settings.clang_tidy ? "--clang-tidy" : "--clang-tidy=false"));
  if (t->settings.clangd.extra && *t->settings.clangd.extra) {
    if (g_shell_parse_argv(t->settings.clangd.extra, NULL, &extra, &error)) {
      for (int i = 0; extra[i]; i++)
        g_ptr_array_add(flags, extra[i]);
      g_free(extra);
    } else {
      g_warning("t-code: clangd's extra flags are left out: %s", error->message);
      g_error_free(error);
    }
  }
  g_ptr_array_add(flags, NULL);
  return (char **)g_ptr_array_free(flags, FALSE);
}

/* Adds name=value to the tunables unless they set name already (the user's own wins). */
static void add_tunable(GString *tunables, const char *name, int value)
{
  if (value <= 0 || strstr(tunables->str, name))
    return;
  g_string_append_printf(tunables, "%s%s=%d", tunables->len ? ":" : "", name, value);
}

char *server_clangd_tunables(const App *t, const char *inherited)
{
  GString *tunables = g_string_new(inherited ? inherited : "");
  gsize before = tunables->len;

  add_tunable(tunables, "glibc.malloc.arena_max", t->settings.clangd.arenas);
  add_tunable(tunables, "glibc.malloc.mmap_threshold", t->settings.clangd.mmap_threshold_kb * 1024);
  if (tunables->len == before)
    return g_string_free(tunables, TRUE), NULL;
  return g_string_free(tunables, FALSE);
}

/* Whether a folder named by a process id belongs to a process that is gone. */
static gboolean process_gone(const char *name)
{
  char *end;
  long pid = strtol(name, &end, 10);

  if (*end || pid <= 0)
    return FALSE; /* not one of ours */
  char *proc = g_strdup_printf("/proc/%ld", pid);
  gboolean gone = !g_file_test(proc, G_FILE_TEST_IS_DIR);
  g_free(proc);
  return gone;
}

/* Deletes the files in dir (preambles: no folders inside), then dir. */
static void remove_flat_dir(const char *dir)
{
  GDir *d = g_dir_open(dir, 0, NULL);
  const char *name;

  while (d && (name = g_dir_read_name(d))) {
    char *path = g_build_filename(dir, name, NULL);
    g_remove(path);
    g_free(path);
  }
  if (d)
    g_dir_close(d);
  g_rmdir(dir);
}

char *server_preamble_dir(const char *cache)
{
  char *top = g_build_filename(cache ? cache : g_get_user_cache_dir(), "t-code", "preambles", NULL);
  char *own = g_strdup_printf("%s/%d", top, (int)getpid());
  GDir *d = g_dir_open(top, 0, NULL);
  const char *name;

  while (d && (name = g_dir_read_name(d)))
    if (process_gone(name)) { /* a t-code that ended without its clangd cleaning up (killed, crashed) */
      char *path = g_build_filename(top, name, NULL);
      remove_flat_dir(path);
      g_free(path);
    }
  if (d)
    g_dir_close(d);
  g_free(top);
  if (g_mkdir_with_parents(own, 0700) != 0)
    return g_free(own), NULL;
  return own;
}

char **server_argv(App *t, ServerKind kind)
{
  Declared *d = declared_of(kind);
  const char *const *command = d ? (const char *const *)d->argv : server_command(kind);
  GPtrArray *argv = g_ptr_array_new();
  char *program = server_program(t, kind);

  g_ptr_array_add(argv, program ? program : g_strdup(command[0]));
  for (int i = 1; command[i]; i++)
    g_ptr_array_add(argv, g_strdup(command[i]));
#ifndef SERVER_ARGV
  if (kind == SERVER_CLANGD) {
    char **flags = server_clangd_flags(t);
    for (int i = 0; flags[i]; i++)
      g_ptr_array_add(argv, flags[i]);
    g_free(flags);
  }
#endif
  g_ptr_array_add(argv, NULL);
  return (char **)g_ptr_array_free(argv, FALSE);
}

const char *server_missing(App *t, ServerKind kind)
{
#ifdef SERVER_ARGV
  (void)t, (void)kind;
  return NULL; /* the fake server is always there */
#else
  char *program = kind >= SERVER_NONE ? NULL : server_program(t, kind); /* clangd too */
  gboolean found = program != NULL;

  g_free(program);
  return found || kind >= SERVER_NONE ? NULL : server_name(kind);
#endif
}

gboolean server_installed(App *t, ServerKind kind)
{
  if (kind >= SERVER_PLUGIN) /* its program is there (no install offers for a plugin's) */
    return declared_of(kind) && server_program_found(declared_of(kind)->argv[0], declared_of(kind)->dir);
#ifdef SERVER_ARGV
  (void)t;
  return kind != SERVER_NONE;
#else
  return kind == SERVER_CLANGD || (kind != SERVER_NONE && !server_missing(t, kind));
#endif
}

/* What it is told ------------------------------------------------------------------------------ */

/* The JSON server neither formats nor reads schemas other than local files; gopls' and TypeScript's
 * inlay hints; neocmakelsp formats (and lints with the project's own .neocmake.toml); rust-analyzer's
 * Cargo features and target. */
/* neocmakelsp's lint is mostly "line too long" past 80 columns (and command case), its limit set only
 * in a .neocmake.toml / .neocmakelint.toml in the folder it runs in: on when the project has one,
 * else every longer line was underlined. Its syntax errors come either way. */
static gboolean cmake_lints(App *t)
{
  static const char *const names[] = { ".neocmake.toml", ".neocmakelint.toml" };
  gboolean found = FALSE;

  for (guint i = 0; i < G_N_ELEMENTS(names) && t && t->project.root && !found; i++) {
    char *path = g_build_filename(t->project.root, names[i], NULL);
    found = g_file_test(path, G_FILE_TEST_IS_REGULAR);
    g_free(path);
  }
  return found;
}

/* Whether dir is root or a folder inside it. */
static gboolean inside(const char *dir, const char *root)
{
  size_t n = strlen(root);

  return strncmp(dir, root, n) == 0 && (dir[n] == '\0' || dir[n] == '/' || (n > 0 && root[n - 1] == '/'));
}

char *server_typescript_path(const char *file, const char *root)
{
  char *dir = file && root ? g_path_get_dirname(file) : NULL, *found = NULL;

  while (dir && !found && inside(dir, root)) {
    char *candidate = g_build_filename(dir, "node_modules", "typescript", "lib", "tsserver.js", NULL);
    if (g_file_test(candidate, G_FILE_TEST_IS_REGULAR))
      found = g_steal_pointer(&candidate);
    g_free(candidate);
    char *up = g_path_get_dirname(dir);
    gboolean top = strcmp(up, dir) == 0;
    g_free(dir);
    dir = up;
    if (top)
      g_clear_pointer(&dir, g_free);
  }
  g_free(dir);
  return found;
}

/* path with its links followed (npm links bin/'s name to the package's own file). */
static char *follow_links(const char *path)
{
  char *current = g_canonicalize_filename(path, NULL), *target;

  for (int i = 0; i < 16 && (target = g_file_read_link(current, NULL)); i++) {
    char *dir = g_path_get_dirname(current);
    g_free(current);
    current = g_canonicalize_filename(target, dir); /* an absolute target ignores dir */
    g_free(dir);
    g_free(target);
  }
  return current;
}

char *server_global_typescript(void)
{
  char *program = find_program("typescript-language-server");
  char *real = program ? follow_links(program) : NULL, *found = NULL;
  char *dir = real ? g_path_get_dirname(real) : NULL;
  /* node's lookup from the package: node_modules in each folder above it */
  while (dir && !found) {
    char *base = g_path_get_basename(dir);
    char *nested = g_build_filename(dir, "node_modules", "typescript", "lib", "tsserver.js", NULL);
    char *sibling =
        strcmp(base, "node_modules") == 0 ? g_build_filename(dir, "typescript", "lib", "tsserver.js", NULL) : NULL;
    if (g_file_test(nested, G_FILE_TEST_IS_REGULAR))
      found = g_steal_pointer(&nested);
    else if (sibling && g_file_test(sibling, G_FILE_TEST_IS_REGULAR))
      found = g_steal_pointer(&sibling);
    g_free(sibling);
    g_free(nested);
    g_free(base);
    char *up = g_path_get_dirname(dir);
    gboolean top = strcmp(up, dir) == 0;
    g_free(dir);
    dir = top ? (g_free(up), NULL) : up;
  }
  g_free(dir);
  g_free(real);
  g_free(program);
  return found;
}

char *server_options(ServerKind kind, App *t)
{
  if (kind >= SERVER_PLUGIN)
    return declared_of(kind) ? g_strdup(declared_of(kind)->options) : NULL;
  if (kind == SERVER_JSON)
    return g_strdup("{\"provideFormatter\":false,\"handledSchemaProtocols\":[\"file\"]}");
  if (kind == SERVER_GO) /* inlay hints, off unless asked for (it takes them only here) */
    return g_strdup("{\"hints\":{\"assignVariableTypes\":true,\"compositeLiteralFields\":true,"
                    "\"constantValues\":true,\"functionTypeParameters\":true,\"parameterNames\":true,"
                    "\"rangeVariableTypes\":true}}");
  if (kind == SERVER_TYPESCRIPT) /* the same */
    return g_strdup("{\"preferences\":{\"includeInlayParameterNameHints\":\"literals\","
                    "\"includeInlayVariableTypeHints\":true,\"includeInlayFunctionLikeReturnTypeHints\":true,"
                    "\"includeInlayEnumMemberValueHints\":true}}");
  if (kind == SERVER_CMAKE) /* the packages' own CMake files are not read through: memory */
    return g_strdup_printf("{\"format\":{\"enable\":true},\"lint\":{\"enable\":%s},\"scan_cmake_in_package\":false,"
                           "\"semantic_token\":false}",
                           cmake_lints(t) ? "true" : "false");
  if (kind != SERVER_RUST)
    return NULL;
  char *features = toolchain_rust_features(t), *target = toolchain_rust_target(t);
  char *options = langenv_rust_options(features, target, trust_project(t, "Rust build scripts and proc macros"));

  g_free(target);
  g_free(features);
  return options;
}

/* JSON's validation, against a file's own local "$schema" only (no downloads, no formatting); YAML's
 * validation, formatting and schemas (the SchemaStore catalog's); pylsp's Python for jedi. */
char *server_settings(ServerKind kind, App *t)
{
  if (kind >= SERVER_PLUGIN)
    return declared_of(kind) ? g_strdup(declared_of(kind)->settings) : NULL;
  if (kind == SERVER_JSON)
    return g_strdup("{\"json\":{\"validate\":{\"enable\":true},\"format\":{\"enable\":false},"
                    "\"schemaDownload\":{\"enable\":false},\"schemas\":[]}}");
  if (kind == SERVER_YAML)
    return g_strdup("{\"yaml\":{\"validate\":true,\"hover\":true,\"completion\":true,\"format\":{\"enable\":true},"
                    "\"schemaStore\":{\"enable\":true}}}");
  if (kind != SERVER_PYTHON)
    return NULL;
  char *python = toolchain_server_python(t), *quoted = python ? json_quote(python) : NULL;
  char *settings = quoted ? g_strdup_printf("{\"pylsp\":{\"plugins\":{\"jedi\":{\"environment\":%s}}}}", quoted) : NULL;

  g_free(quoted);
  g_free(python);
  return settings;
}
