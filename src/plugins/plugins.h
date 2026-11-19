/*
 * Plugins (docs/plugins.md): programs t-code starts, found in the user's, the system's and the source
 * tree's plugins/ folders by their plugin.json. Their commands show in the command palette and may
 * have keys; a command starts the plugin's program without waiting for it, with $TCODE_SOCKET,
 * $TCODE_API and the rest in its environment. A plugin with "start": "project" runs while a project
 * is open. The last lines its programs print are kept for the Plugins window. A plugin may also bring
 * things without a program: language servers ("lsp", lsp/servers.c runs them), a Run command for
 * some files ("run"), GtkSourceView language specs and snippets.
 */
#pragma once

#include "app.h"

#include <string.h>

typedef struct Plugin Plugin;
typedef struct PluginRun PluginRun; /* a running program of a plugin (plugins.c) */

/* What a plugin's "start" program did last. */
typedef enum {
  PLUGIN_IDLE,    /* not started (or no "start") */
  PLUGIN_RUNNING, /* started, still running */
  PLUGIN_STOPPED, /* stopped by t-code (the project closed, Stop), or it exited with status 0 */
  PLUGIN_FAILED,  /* it exited with an error or a signal t-code did not send */
} PluginState;

/* Where a plugin was found. */
typedef enum {
  PLUGIN_FROM_USER,   /* ~/.config/t-code/plugins: the user's own, the only ones Uninstall removes */
  PLUGIN_FROM_SYSTEM, /* /usr/share/t-code/plugins */
  PLUGIN_FROM_TCODE,  /* plugins/ beside t-code's styles (the source tree, a portable run) */
} PluginSource;

/* The lines of output kept per plugin. */
#define PLUGIN_OUTPUT_LINES 200

/* What a plugin's manifest says it does ("permissions"): edit and terminal are enforced on its
 * socket connections, network is only declared, start lets it start with the project. */
typedef enum {
  PLUGIN_EDIT = 1 << 0,
  PLUGIN_TERMINAL = 1 << 1,
  PLUGIN_NETWORK = 1 << 2,
  PLUGIN_START = 1 << 3,
} PluginPermissions;

typedef struct PluginCommand {
  char *id;
  char *name;
  char *keys;    /* the label shown beside it, e.g. Ctrl+Alt+1 */
  char *trigger; /* its GTK shortcut, e.g. <Control><Alt>1, or NULL */
  Plugin *plugin;
} PluginCommand;

/* A language server a plugin declares ("lsp"): lsp/servers.c gives it a ServerKind of its own when
 * one of its files opens, and runs it like a built-in one. */
typedef struct PluginServer {
  char *name;          /* unique within the plugin; shown in the status bar */
  char **argv;         /* argv[0]: a program on PATH, or a path relative to the plugin's folder */
  char **extensions;   /* ".m", or whole file names ("CMakeLists.txt"); may be empty */
  char **languages;    /* GtkSourceView language ids ("c", "cpphdr"); may be empty, not both */
  char *language_id;   /* the languageId of its files; NULL: from GtkSourceView's ("c", "cpp", "python") */
  char **language_ids; /* or by file: pattern, id, pattern, id, ... (".jsx", "javascriptreact"); may be empty */
  char *options;       /* initializationOptions, JSON; NULL for none */
  char *settings;      /* workspace settings, JSON; NULL for none */
} PluginServer;

typedef struct Plugin {
  char *id;
  char *name;
  char *version;
  char *description;
  char *dir;
  char *executable; /* NULL for a plugin without commands */
  int api;          /* the "tapi_N" its plugin.json asks for; 0: none (a plugin from before versions) */
  PluginSource source;
  gboolean enabled; /* not in plugins.json's "disabled": its commands, keys, servers and start work */
  PluginPermissions permissions;
  char *token;         /* $TCODE_PLUGIN_TOKEN: its program says "hello tapi_1 <token>" to be known as it */
  GPtrArray *commands; /* PluginCommand * */
  gboolean starts;     /* "start": "project": runs while a project is open */
  PluginState state;
  PluginRun *running;   /* its start program while it runs */
  GQueue output;        /* char *: the last PLUGIN_OUTPUT_LINES lines its programs printed (stdout, stderr) */
  guint printed;        /* lines printed in all, to see that output changed */
  GPtrArray *servers;   /* PluginServer *: "lsp" */
  GHashTable *run;      /* "run": an extension (".m") or a file name -> one of its command ids */
  char *language_specs; /* "language-specs": a folder of .lang files (absolute), or NULL */
  char *snippets;       /* "snippets": a folder of .snippets files (absolute), or NULL */
} Plugin;

/* Whether name (a file's name) is one of patterns: ".ext" matches the end, anything else the whole
 * name. Inline: lsp/servers.c uses it without the rest of plugins.c. */
static inline gboolean plugin_name_matches(const char *name, char *const *patterns)
{
  for (int i = 0; name && patterns && patterns[i]; i++)
    if (patterns[i][0] == '.' ? g_str_has_suffix(name, patterns[i]) && strlen(name) > strlen(patterns[i])
                              : strcmp(name, patterns[i]) == 0)
      return TRUE;
  return FALSE;
}

/* The plugin with that id, or the one $TCODE_PLUGIN_TOKEN token was given to; NULL when none. */
Plugin *plugin_find(App *t, const char *id);
Plugin *plugin_by_token(App *t, const char *token);

/* Discovers plugins from user and system directories and populates t->plugins. */
void plugins_init(App *t);

/* Reloads all plugins. */
void plugins_reload(App *t);

/* Grows each time the plugins are loaded again (Plugin pointers from before are gone). */
guint plugins_generation(void);

/* Turns a plugin on or off, kept in ~/.config/t-code/plugins.json ("disabled": its ids). Off, its
 * start program stops and its commands, keys and language servers go; on, they come back. Its
 * snippets and language specs follow at the next start. */
void plugin_set_enabled(App *t, Plugin *p, gboolean enabled);

/* Removes a plugin from the user's folder (stopped first) and loads the plugins again; FALSE with
 * error for one found elsewhere or a folder that would not go. */
gboolean plugin_uninstall(App *t, Plugin *p, GError **error);

/* Frees all loaded plugins and commands. Their start programs get SIGTERM, and SIGKILL when they are
 * still there a second later (t-code is quitting: this waits for them). */
void plugins_stop(App *t);

/* The project opened: every enabled plugin with "start": "project" starts, with command id "start".
 * Nothing starts without a project (the home folder). */
void plugins_start_project(App *t);
/* The project closes: those programs get SIGTERM, and SIGKILL when they are still there 2 s later. */
void plugins_stop_project(App *t);

/* One plugin's start program (the Plugins window's Start / Stop). plugin_start says whether it runs. */
gboolean plugin_start(App *t, Plugin *p);
void plugin_stop(Plugin *p);

/* Scans a directory for plugins and adds them to t->plugins. */
void plugins_scan_dir(App *t, const char *dir_path);

/* Queries plugins for matching commands in the command palette. Returns match count. */
int plugins_palette_query(App *t, const char *text, guint generation);

/* Handles selection of a plugin payload (format "plugin:<plugin_id>:<command_id>"). */
gboolean plugins_palette_choose(App *t, const char *payload);

/* Adds manifest shortcuts to a window's shared shortcut controller. */
void plugins_add_shortcuts(GtkShortcutController *controller, App *t);

/* The plugin command Run (F5) hands file to ("run" in plugin.json), or NULL. */
PluginCommand *plugins_run_command_for(App *t, const char *file);

/* Runs a plugin command asynchronously in a subprocess. */
void plugin_run_command(App *t, const char *plugin_id, const char *command_id);

/* Opens the user plugins directory in desktop file manager. */
void open_plugins_folder(App *t);
