/*
 * The language servers t-code knows (client.c runs them): which one a file gets, its command line,
 * where its program is, and what it is told at the start.
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
} ServerKind;

/* The server an editor's file gets (by its language or name), or SERVER_NONE. */
ServerKind server_kind_for_tab(Tab *tab);

/* The server's program's name ("clangd", "pylsp"): for messages and the status bar. */
const char *server_name(ServerKind kind);

/* The language a server is restarted by ("python", "rust"; NULL for clangd: C or C++). */
const char *server_language(ServerKind kind);

/* The file's languageId for didOpen ("cpp", "javascriptreact", "jsonc", ...). */
const char *server_language_id(ServerKind kind, Tab *tab);

/* The server's command line (NULL-terminated, g_strfreev): its program where it is found (see
 * server_program), its flags, and clangd's --clang-tidy as the settings say. */
char **server_argv(App *t, ServerKind kind);

/* Whether the server can run: clangd is taken as there, the others must be found. */
gboolean server_installed(App *t, ServerKind kind);

/* The program's name when the server is not installed (an editor offers to install it), else NULL. */
const char *server_missing(App *t, ServerKind kind);

/* initializationOptions, and the settings sent after initialize and given when the server asks
 * (workspace/configuration); NULL for none. */
char *server_options(ServerKind kind, App *t);
char *server_settings(ServerKind kind, App *t);
