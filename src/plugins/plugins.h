/*
 * Plugins (docs/plugins.md): programs t-code starts, found in the user's, the system's and the source
 * tree's plugins/ folders by their plugin.json. Their commands show in the command palette and may
 * have keys; a command starts the plugin's program without waiting for it, with $TCODE_SOCKET,
 * $TCODE_API and the rest in its environment.
 */
#pragma once

#include "app.h"

typedef struct Plugin Plugin;

typedef struct PluginCommand {
  char *id;
  char *name;
  char *keys;    /* the label shown beside it, e.g. Ctrl+Alt+1 */
  char *trigger; /* its GTK shortcut, e.g. <Control><Alt>1, or NULL */
  Plugin *plugin;
} PluginCommand;

typedef struct Plugin {
  char *id;
  char *name;
  char *version;
  char *description;
  char *dir;
  char *executable;    /* NULL for a plugin without commands */
  int api;             /* the "tapi_N" its plugin.json asks for; 0: none (a plugin from before versions) */
  GPtrArray *commands; /* PluginCommand * */
} Plugin;

/* Discovers plugins from user and system directories and populates t->plugins. */
void plugins_init(App *t);

/* Reloads all plugins. */
void plugins_reload(App *t);

/* Frees all loaded plugins and commands. */
void plugins_stop(App *t);

/* Scans a directory for plugins and adds them to t->plugins. */
void plugins_scan_dir(App *t, const char *dir_path);

/* Queries plugins for matching commands in the command palette. Returns match count. */
int plugins_palette_query(App *t, const char *text, guint generation);

/* Handles selection of a plugin payload (format "plugin:<plugin_id>:<command_id>"). */
gboolean plugins_palette_choose(App *t, const char *payload);

/* Adds manifest shortcuts to a window's shared shortcut controller. */
void plugins_add_shortcuts(GtkShortcutController *controller, App *t);

/* Runs a plugin command asynchronously in a subprocess. */
void plugin_run_command(App *t, const char *plugin_id, const char *command_id);

/* Opens the user plugins directory in desktop file manager. */
void open_plugins_folder(App *t);
