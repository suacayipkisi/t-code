/*
 * The language servers t-code knows (client.c runs them): which one a file gets, its command line,
 * where its program is, and what it is told at the start. Next to the built-in ones, the servers
 * plugins declare ("lsp" in plugin.json) get a kind each, SERVER_PLUGIN and up, and run the same way.
 */
#pragma once

#include "app.h"

typedef enum {
  SERVER_CLANGD,
  SERVER_PYTHON,
  SERVER_GO,
  SERVER_RUST,
  SERVER_TYPESCRIPT, /* JavaScript too */
  SERVER_JSON,
  SERVER_YAML,
  SERVER_MARKDOWN,
  SERVER_CMAKE,
  SERVER_NONE,
  SERVER_PLUGIN = 100, /* and up: a plugin's server, the same number for it until t-code quits */
} ServerKind;

/* The server an editor's file gets (by its language or name), or SERVER_NONE. A built-in server
 * turned off in Settings gives way to a plugin's for the same files; a built-in one that is on wins,
 * unless it is not installed. */
ServerKind server_kind_for_tab(Tab *tab);

/* A built-in server's stable id ("clangd", "pylsp"; settings.ini's language-servers-off) and what
 * it is for ("C and C++"); server_by_id gives SERVER_NONE for an unknown id. */
const char *server_id(ServerKind kind);
const char *server_title(ServerKind kind);
ServerKind server_by_id(const char *id);

/* Whether a built-in server is on (Settings, Language Servers); plugins' servers always are. */
gboolean server_enabled(App *t, ServerKind kind);

/* The server's program's name ("clangd", "pylsp"): for messages and the status bar. */
const char *server_name(ServerKind kind);

/* The language a server is restarted by ("python", "rust"; NULL for clangd: C or C++). */
const char *server_language(ServerKind kind);

/* The file's languageId for didOpen ("cpp", "javascriptreact", "jsonc", ...). */
const char *server_language_id(ServerKind kind, Tab *tab);

/* The server's command line (NULL-terminated, g_strfreev): its program where it is found (see
 * server_program), its flags, and clangd's --clang-tidy as the settings say. */
char **server_argv(App *t, ServerKind kind);

/* Whether the server can run: clangd is taken as there, the others must be found (a plugin's server
 * whose program is missing serves nothing, quietly; the Plugins window says it is not found). */
gboolean server_installed(App *t, ServerKind kind);

/* Whether a plugin's server program is there: argv0 relative to the plugin's folder dir when it holds
 * a "/", else on PATH or in the folders language installers use (as for the built-in servers). */
gboolean server_program_found(const char *argv0, const char *dir);

/* The program's name when a built-in server is not installed (an editor offers to install it), else
 * NULL. */
const char *server_missing(App *t, ServerKind kind);

/* initializationOptions, and the settings sent after initialize and given when the server asks
 * (workspace/configuration); NULL for none. */
char *server_options(ServerKind kind, App *t);
char *server_settings(ServerKind kind, App *t);
