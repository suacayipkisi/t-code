/*
 * The language servers (see servers.h): clangd for C/C++, pylsp for Python, gopls for Go,
 * rust-analyzer for Rust, typescript-language-server for TypeScript and JavaScript, the JSON language
 * server, yaml-language-server, marksman for Markdown and neocmakelsp for CMake.
 */
#include "lsp/servers.h"

#include "lsp/json.h"
#include "project/langenv.h"
#include "project/toolchain.h"

#include <glib/gstdio.h>
#include <string.h>

#define MAX_JSON_BYTES (512 * 1024) /* a bigger JSON file is data, not something edited by hand */

#ifdef __GLIBC__
#define MALLOC_TRIM "--malloc-trim", /* gives freed memory back to the system */
#else
#define MALLOC_TRIM /* glibc's malloc_trim(): clangd built on musl has no such flag and exits on it */
#endif

#ifndef SERVER_ARGV /* tests run a fake server */
/* clang-format off */
static const char *const clangd_argv[] = { "clangd", "--background-index", "--pch-storage=disk", "-j=2",
                                           "--limit-results=50", MALLOC_TRIM NULL };
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

const char *server_name(ServerKind kind)
{
  return server_command(kind)[0];
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

  if (kind == SERVER_CLANGD) {
    const char *source_id = gtk_source_language_get_id(gtk_source_buffer_get_language(tab->buffer));
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

ServerKind server_kind_for_tab(Tab *tab)
{
  GtkSourceLanguage *language = tab && tab->kind == TAB_EDITOR ? gtk_source_buffer_get_language(tab->buffer) : NULL;
  const char *id = language ? gtk_source_language_get_id(language) : "";
  const char *path = tab ? tab->path : "";

  if (strcmp(id, "c") == 0 || strcmp(id, "chdr") == 0 || strcmp(id, "cpp") == 0 || strcmp(id, "cpphdr") == 0)
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

/* The program ------------------------------------------------------------------------------------ */

#ifndef SERVER_ARGV
/* The pylsp installed in the project's Python's venv (it sees the venv's packages), or NULL. */
static char *venv_pylsp(App *t)
{
  char *python = toolchain_python(t, NULL), *bin = python ? g_path_get_dirname(python) : NULL;
  char *pylsp = bin ? g_build_filename(bin, "pylsp", NULL) : NULL;

  if (pylsp && !g_file_test(pylsp, G_FILE_TEST_IS_EXECUTABLE))
    g_clear_pointer(&pylsp, g_free);
  g_free(bin);
  g_free(python);
  return pylsp;
}
#endif

/* Where the server's program is: the project's venv's pylsp, then PATH, then the folders language
 * installers put programs in (pipx, cargo install, go install), which are not on the PATH of a
 * t-code started from the desktop's menu. NULL when it is nowhere (and for the tests' fake server,
 * which runs as it is). */
static char *server_program(App *t, ServerKind kind)
{
#ifdef SERVER_ARGV
  (void)t, (void)kind;
  return NULL;
#else
  const char *name = server_name(kind);
  char *path = kind == SERVER_PYTHON ? venv_pylsp(t) : NULL;
  const char *gopath = g_getenv("GOPATH");
  char **gopaths = gopath && *gopath ? g_strsplit(gopath, ":", 2) : NULL;
  char *folders[] = {
    g_build_filename(g_get_home_dir(), ".local", "bin", NULL),
    g_build_filename(g_get_home_dir(), ".cargo", "bin", NULL),
    gopaths ? g_build_filename(gopaths[0], "bin", NULL) : g_build_filename(g_get_home_dir(), "go", "bin", NULL),
  };

  if (!path)
    path = g_find_program_in_path(name);
  for (size_t i = 0; i < G_N_ELEMENTS(folders); i++) {
    char *candidate = g_build_filename(folders[i], name, NULL);
    if (!path && g_file_test(candidate, G_FILE_TEST_IS_EXECUTABLE))
      path = g_steal_pointer(&candidate);
    g_free(candidate);
    g_free(folders[i]);
  }
  g_strfreev(gopaths);
  return path;
#endif
}

char **server_argv(App *t, ServerKind kind)
{
  const char *const *command = server_command(kind);
  GPtrArray *argv = g_ptr_array_new();
  char *program = server_program(t, kind);

  g_ptr_array_add(argv, program ? program : g_strdup(command[0]));
  for (int i = 1; command[i]; i++)
    g_ptr_array_add(argv, g_strdup(command[i]));
#ifndef SERVER_ARGV
  if (kind == SERVER_CLANGD)
    g_ptr_array_add(argv, g_strdup(t->settings.clang_tidy ? "--clang-tidy" : "--clang-tidy=false"));
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
  char *program = kind == SERVER_NONE ? NULL : server_program(t, kind); /* clangd too */
  gboolean found = program != NULL;

  g_free(program);
  return found || kind == SERVER_NONE ? NULL : server_name(kind);
#endif
}

gboolean server_installed(App *t, ServerKind kind)
{
#ifdef SERVER_ARGV
  (void)t;
  return kind != SERVER_NONE;
#else
  return kind == SERVER_CLANGD || (kind != SERVER_NONE && !server_missing(t, kind));
#endif
}

/* What it is told ------------------------------------------------------------------------------ */

/* The JSON server neither formats nor reads schemas other than local files; gopls' and TypeScript's
 * inlay hints; neocmakelsp formats and lints; rust-analyzer's Cargo features and target. */
char *server_options(ServerKind kind, App *t)
{
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
    return g_strdup("{\"format\":{\"enable\":true},\"lint\":{\"enable\":true},\"scan_cmake_in_package\":false,"
                    "\"semantic_token\":false}");
  if (kind != SERVER_RUST)
    return NULL;
  char *features = toolchain_rust_features(t), *target = toolchain_rust_target(t);
  char *options = langenv_rust_options(features, target);

  g_free(target);
  g_free(features);
  return options;
}

/* JSON's validation, against a file's own local "$schema" only (no downloads, no formatting); YAML's
 * validation, formatting and schemas (the SchemaStore catalog's); pylsp's Python for jedi. */
char *server_settings(ServerKind kind, App *t)
{
  if (kind == SERVER_JSON)
    return g_strdup("{\"json\":{\"validate\":{\"enable\":true},\"format\":{\"enable\":false},"
                    "\"schemaDownload\":{\"enable\":false},\"schemas\":[]}}");
  if (kind == SERVER_YAML)
    return g_strdup("{\"yaml\":{\"validate\":true,\"hover\":true,\"completion\":true,\"format\":{\"enable\":true},"
                    "\"schemaStore\":{\"enable\":true}}}");
  if (kind != SERVER_PYTHON)
    return NULL;
  char *python = toolchain_python(t, NULL), *quoted = python ? json_quote(python) : NULL;
  char *settings = quoted ? g_strdup_printf("{\"pylsp\":{\"plugins\":{\"jedi\":{\"environment\":%s}}}}", quoted) : NULL;

  g_free(quoted);
  g_free(python);
  return settings;
}
