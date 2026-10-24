/*
 * Native Stage 1 Plugin System:
 * Out-of-process plugins discovered in user and system directories, configured via plugin.json.
 * Commands appear in the Command Palette and execute asynchronously with clean environment
 * ($TCODE_SOCKET, $TCODE_ACTIVE_FILE, etc.) without freezing the UI.
 */
#include "plugins/plugins.h"

#include "editor/tabs.h"
#include "popups/picker.h"
#include "window/statusbar.h"

#include <gio/gio.h>
#include <json-glib/json-glib.h>

#include "window/commands.h"

static void plugin_command_free(PluginCommand *cmd)
{
  if (!cmd)
    return;
  g_free(cmd->id);
  g_free(cmd->name);
  g_free(cmd->keys);
  g_free(cmd->trigger);
  g_free(cmd);
}

static void plugin_free(Plugin *plugin)
{
  if (!plugin)
    return;
  g_free(plugin->id);
  g_free(plugin->name);
  g_free(plugin->version);
  g_free(plugin->description);
  g_free(plugin->dir);
  g_free(plugin->executable);
  if (plugin->commands)
    g_ptr_array_free(plugin->commands, TRUE);
  g_free(plugin);
}

static Plugin *find_plugin(App *t, const char *id)
{
  if (!t->plugins || !id)
    return NULL;
  for (guint i = 0; i < t->plugins->len; i++) {
    Plugin *p = g_ptr_array_index(t->plugins, i);
    if (g_strcmp0(p->id, id) == 0)
      return p;
  }
  return NULL;
}

static void load_plugin_from_dir(App *t, const char *plugin_dir, const char *dir_name)
{
  char *json_path = g_build_filename(plugin_dir, "plugin.json", NULL);
  if (!g_file_test(json_path, G_FILE_TEST_IS_REGULAR)) {
    g_free(json_path);
    return;
  }

  JsonParser *parser = json_parser_new();
  GError *error = NULL;
  if (!json_parser_load_from_file(parser, json_path, &error)) {
    g_warning("Plugin at %s: invalid JSON: %s", json_path, error ? error->message : "unknown error");
    g_clear_error(&error);
    g_object_unref(parser);
    g_free(json_path);
    return;
  }
  g_free(json_path);

  JsonNode *root = json_parser_get_root(parser);
  if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
    g_object_unref(parser);
    return;
  }
  JsonObject *obj = json_node_get_object(root);

  if (!json_object_has_member(obj, "name") || !json_object_has_member(obj, "commands")) {
    g_warning("Plugin at %s: missing required 'name' or 'commands'", plugin_dir);
    g_object_unref(parser);
    return;
  }

  const char *name = json_object_get_string_member(obj, "name");
  if (!name || !*name) {
    g_object_unref(parser);
    return;
  }

  JsonNode *cmds_node = json_object_get_member(obj, "commands");
  if (!cmds_node || !JSON_NODE_HOLDS_ARRAY(cmds_node)) {
    g_object_unref(parser);
    return;
  }
  JsonArray *cmds_arr = json_node_get_array(cmds_node);
  guint n_cmds = json_array_get_length(cmds_arr);
  if (n_cmds == 0) {
    g_object_unref(parser);
    return;
  }

  const char *id = json_object_has_member(obj, "id") ? json_object_get_string_member(obj, "id") : dir_name;
  if (!id || !*id)
    id = dir_name;

  if (find_plugin(t, id)) {
    g_object_unref(parser);
    return;
  }

  const char *version = json_object_has_member(obj, "version") ? json_object_get_string_member(obj, "version") : NULL;
  const char *desc =
      json_object_has_member(obj, "description") ? json_object_get_string_member(obj, "description") : NULL;
  const char *exe_cfg =
      json_object_has_member(obj, "executable") ? json_object_get_string_member(obj, "executable") : NULL;

  char *executable = NULL;
  if (exe_cfg && *exe_cfg) {
    if (g_path_is_absolute(exe_cfg))
      executable = g_strdup(exe_cfg);
    else
      executable = g_build_filename(plugin_dir, exe_cfg, NULL);
  } else {
    const char *defaults[] = { "run.sh", "plugin", dir_name, NULL };
    for (int i = 0; defaults[i]; i++) {
      char *cand = g_build_filename(plugin_dir, defaults[i], NULL);
      if (g_file_test(cand, G_FILE_TEST_IS_EXECUTABLE)) {
        executable = cand;
        break;
      }
      g_free(cand);
    }
  }

  if (!executable || !g_file_test(executable, G_FILE_TEST_IS_EXECUTABLE)) {
    g_warning("Plugin %s (%s): no executable found at %s", name, id, executable ? executable : "(null)");
    g_free(executable);
    g_object_unref(parser);
    return;
  }

  Plugin *p = g_new0(Plugin, 1);
  p->id = g_strdup(id);
  p->name = g_strdup(name);
  p->version = g_strdup(version ? version : "");
  p->description = g_strdup(desc ? desc : "");
  p->dir = g_strdup(plugin_dir);
  p->executable = executable;
  p->commands = g_ptr_array_new_with_free_func((GDestroyNotify)plugin_command_free);

  for (guint i = 0; i < n_cmds; i++) {
    JsonNode *cnode = json_array_get_element(cmds_arr, i);
    if (!cnode || !JSON_NODE_HOLDS_OBJECT(cnode))
      continue;
    JsonObject *cobj = json_node_get_object(cnode);
    if (!json_object_has_member(cobj, "id") || !json_object_has_member(cobj, "name"))
      continue;
    const char *cid = json_object_get_string_member(cobj, "id");
    const char *cname = json_object_get_string_member(cobj, "name");
    const char *ckeys = json_object_has_member(cobj, "keys") ? json_object_get_string_member(cobj, "keys") : NULL;
    const char *trigger =
        json_object_has_member(cobj, "trigger") ? json_object_get_string_member(cobj, "trigger") : NULL;

    PluginCommand *cmd = g_new0(PluginCommand, 1);
    cmd->id = g_strdup(cid);
    cmd->name = g_strdup(cname);
    cmd->keys = ckeys ? g_strdup(ckeys) : NULL;
    cmd->trigger = trigger ? g_strdup(trigger) : NULL;
    cmd->plugin = p;
    g_ptr_array_add(p->commands, cmd);
  }

  g_object_unref(parser);

  if (p->commands->len == 0) {
    plugin_free(p);
    return;
  }

  g_ptr_array_add(t->plugins, p);
}

void plugins_scan_dir(App *t, const char *dir_path)
{
  if (!dir_path || !g_file_test(dir_path, G_FILE_TEST_IS_DIR))
    return;

  GDir *dir = g_dir_open(dir_path, 0, NULL);
  if (!dir)
    return;

  const char *name;
  while ((name = g_dir_read_name(dir)) != NULL) {
    if (name[0] == '.')
      continue;
    char *sub = g_build_filename(dir_path, name, NULL);
    if (g_file_test(sub, G_FILE_TEST_IS_DIR))
      load_plugin_from_dir(t, sub, name);
    g_free(sub);
  }
  g_dir_close(dir);
}

void plugins_init(App *t)
{
  if (!t->plugins)
    t->plugins = g_ptr_array_new_with_free_func((GDestroyNotify)plugin_free);

  /* 1. User directory: ~/.config/t-code/plugins */
  char *user_dir = g_build_filename(g_get_user_config_dir(), "t-code", "plugins", NULL);
  g_mkdir_with_parents(user_dir, 0755);
  plugins_scan_dir(t, user_dir);
  g_free(user_dir);

  /* 2. System directory: /usr/share/t-code/plugins */
  plugins_scan_dir(t, "/usr/share/t-code/plugins");

  /* 3. Also check beside styles for development / portable runs */
  if (t->paths.styles) {
    char *parent = g_path_get_dirname(t->paths.styles);
    char *beside = g_build_filename(parent, "plugins", NULL);
    if (g_strcmp0(beside, "/usr/share/t-code/plugins") != 0)
      plugins_scan_dir(t, beside);
    g_free(beside);
    g_free(parent);
  }
}

void plugins_stop(App *t)
{
  if (t->plugins) {
    g_ptr_array_free(t->plugins, TRUE);
    t->plugins = NULL;
  }
}

void plugins_reload(App *t)
{
  plugins_stop(t);
  plugins_init(t);
  refresh_shortcuts(t);
  status_say(t, "Plugins reloaded");
}

typedef struct {
  Plugin *plugin;
  PluginCommand *cmd;
  int score;
} PluginMatch;

static int plugin_match_cmp(gconstpointer a, gconstpointer b)
{
  const PluginMatch *x = a, *y = b;
  return y->score - x->score;
}

int plugins_palette_query(App *t, const char *text, guint generation)
{
  if (!t->plugins || t->plugins->len == 0)
    return 0;

  GArray *matches = g_array_new(FALSE, FALSE, sizeof(PluginMatch));

  for (guint i = 0; i < t->plugins->len; i++) {
    Plugin *p = g_ptr_array_index(t->plugins, i);
    for (guint j = 0; j < p->commands->len; j++) {
      PluginCommand *cmd = g_ptr_array_index(p->commands, j);
      int score = -1;
      if (!text || !*text) {
        score = 0;
      } else {
        int s1 = fuzzy_score(text, cmd->name);
        int s2 = fuzzy_score(text, p->name);
        score = (s1 > s2) ? s1 : s2;
      }
      if (score >= 0) {
        PluginMatch m = { p, cmd, score };
        g_array_append_val(matches, m);
      }
    }
  }

  g_array_sort(matches, plugin_match_cmp);

  int count = (int)matches->len;
  for (guint i = 0; i < matches->len; i++) {
    PluginMatch *m = &g_array_index(matches, PluginMatch, i);
    char *payload = g_strdup_printf("plugin:%s:%s", m->plugin->id, m->cmd->id);
    picker_add(t, generation, m->cmd->name, m->cmd->keys, payload);
    g_free(payload);
  }

  g_array_free(matches, TRUE);
  return count;
}

gboolean plugins_palette_choose(App *t, const char *payload)
{
  if (!payload || !g_str_has_prefix(payload, "plugin:"))
    return FALSE;

  const char *rest = payload + strlen("plugin:");
  const char *colon = strchr(rest, ':');
  if (!colon)
    return FALSE;

  char *plugin_id = g_strndup(rest, colon - rest);
  const char *command_id = colon + 1;
  plugin_run_command(t, plugin_id, command_id);
  g_free(plugin_id);
  return TRUE;
}
typedef struct {
  App *app;
  char *payload;
} PluginShortcut;

static void plugin_shortcut_free(gpointer data)
{
  PluginShortcut *shortcut = data;
  g_free(shortcut->payload);
  g_free(shortcut);
}

static gboolean plugin_shortcut_activate(GtkWidget *widget G_GNUC_UNUSED, GVariant *args G_GNUC_UNUSED, gpointer data)
{
  PluginShortcut *shortcut = data;
  return plugins_palette_choose(shortcut->app, shortcut->payload);
}

void plugins_add_shortcuts(GtkShortcutController *controller, App *t)
{
  GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

  for (guint i = 0; t->plugins && i < t->plugins->len; i++) {
    Plugin *plugin = g_ptr_array_index(t->plugins, i);
    for (guint j = 0; j < plugin->commands->len; j++) {
      PluginCommand *cmd = g_ptr_array_index(plugin->commands, j);
      GtkShortcutTrigger *trigger;
      PluginShortcut *shortcut;
      char *payload;

      if (!cmd->trigger || !*cmd->trigger)
        continue;
      if (g_hash_table_contains(seen, cmd->trigger)) {
        g_warning("Plugin shortcut '%s' is already assigned; skipping %s", cmd->trigger, cmd->name);
        continue;
      }
      trigger = gtk_shortcut_trigger_parse_string(cmd->trigger);
      if (!trigger) {
        g_warning("Plugin shortcut '%s' for %s is invalid", cmd->trigger, cmd->name);
        continue;
      }
      g_hash_table_add(seen, g_strdup(cmd->trigger));
      payload = g_strdup_printf("plugin:%s:%s", plugin->id, cmd->id);
      shortcut = g_new0(PluginShortcut, 1);
      shortcut->app = t;
      shortcut->payload = payload;
      gtk_shortcut_controller_add_shortcut(
          controller,
          gtk_shortcut_new(trigger, gtk_callback_action_new(plugin_shortcut_activate, shortcut, plugin_shortcut_free)));
    }
  }
  g_hash_table_destroy(seen);
}

typedef struct {
  App *app;
  char *plugin_name;
} PluginRunContext;

static void on_plugin_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
  GSubprocess *proc = G_SUBPROCESS(source);
  PluginRunContext *ctx = user_data;
  GError *error = NULL;

  if (!g_subprocess_wait_finish(proc, result, &error)) {
    g_warning("Plugin '%s' execution error: %s", ctx->plugin_name, error ? error->message : "unknown");
    char *msg = g_strdup_printf("Plugin %s failed", ctx->plugin_name);
    status_say(ctx->app, msg);
    g_free(msg);
    g_clear_error(&error);
  } else if (!g_subprocess_get_successful(proc)) {
    int status = g_subprocess_get_exit_status(proc);
    g_warning("Plugin '%s' exited with error status %d", ctx->plugin_name, status);
    char *msg = g_strdup_printf("Plugin %s failed", ctx->plugin_name);
    status_say(ctx->app, msg);
    g_free(msg);
  }

  g_free(ctx->plugin_name);
  g_free(ctx);
  g_object_unref(proc);
}

void plugin_run_command(App *t, const char *plugin_id, const char *command_id)
{
  Plugin *p = find_plugin(t, plugin_id);
  if (!p) {
    g_warning("Plugin '%s' not found", plugin_id);
    return;
  }

  gboolean cmd_found = FALSE;
  for (guint i = 0; i < p->commands->len; i++) {
    PluginCommand *cmd = g_ptr_array_index(p->commands, i);
    if (g_strcmp0(cmd->id, command_id) == 0) {
      cmd_found = TRUE;
      break;
    }
  }
  if (!cmd_found) {
    g_warning("Plugin '%s': command '%s' not found", plugin_id, command_id);
    return;
  }

  GSubprocessLauncher *launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_NONE);
  g_subprocess_launcher_set_cwd(launcher, p->dir);

  char **env = g_get_environ();
  if (t->paths.socket)
    env = g_environ_setenv(env, "TCODE_SOCKET", t->paths.socket, TRUE);
  env = g_environ_setenv(env, "TCODE_PLUGIN_ID", plugin_id, TRUE);
  env = g_environ_setenv(env, "TCODE_COMMAND_ID", command_id, TRUE);

  Tab *tab = current_tab(t);
  const char *active_file = (tab && tab->kind == TAB_EDITOR && tab->path) ? tab->path : "";
  env = g_environ_setenv(env, "TCODE_ACTIVE_FILE", active_file, TRUE);

  const char *project_root = t->project.root ? t->project.root : "";
  env = g_environ_setenv(env, "TCODE_PROJECT_ROOT", project_root, TRUE);

  g_subprocess_launcher_set_environ(launcher, env);
  g_strfreev(env);

  const char *argv[] = { p->executable, command_id, active_file, NULL };
  GError *error = NULL;
  GSubprocess *proc = g_subprocess_launcher_spawnv(launcher, argv, &error);
  g_object_unref(launcher);

  if (!proc) {
    g_warning("Plugin '%s' launch failed: %s", p->name, error ? error->message : "unknown error");
    char *msg = g_strdup_printf("Plugin %s failed to launch", p->name);
    status_say(t, msg);
    g_free(msg);
    g_clear_error(&error);
    return;
  }

  PluginRunContext *ctx = g_new0(PluginRunContext, 1);
  ctx->app = t;
  ctx->plugin_name = g_strdup(p->name);

  g_subprocess_wait_async(proc, NULL, on_plugin_finished, ctx);
}

void open_plugins_folder(App *t)
{
  char *dir = g_build_filename(g_get_user_config_dir(), "t-code", "plugins", NULL);
  g_mkdir_with_parents(dir, 0755);
  char *uri = g_filename_to_uri(dir, NULL, NULL);
  if (uri) {
    GtkUriLauncher *launcher = gtk_uri_launcher_new(uri);
    gtk_uri_launcher_launch(launcher, t->window, NULL, NULL, NULL);
    g_object_unref(launcher);
    g_free(uri);
  }
  g_free(dir);
}
