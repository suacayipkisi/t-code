/*
 * Plugins (see plugins.h; the manifest and the protocol are docs/plugins.md's): finding them, their
 * commands in the palette and on keys, starting and stopping their programs.
 */
#define _DEFAULT_SOURCE /* kill(), setpgid() */
#include "plugins/plugins.h"

#include "application/identity.h"
#include "editor/tabs.h"
#include "popups/picker.h"
#include "window/statusbar.h"

#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include <signal.h>
#include <unistd.h>

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
  g_free(plugin->token);
  g_queue_clear_full(&plugin->output, g_free);
  if (plugin->commands)
    g_ptr_array_free(plugin->commands, TRUE);
  g_free(plugin);
}

Plugin *plugin_find(App *t, const char *id)
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

Plugin *plugin_by_token(App *t, const char *token)
{
  for (guint i = 0; t->plugins && token && *token && i < t->plugins->len; i++) {
    Plugin *p = g_ptr_array_index(t->plugins, i);
    if (strcmp(p->token, token) == 0)
      return p;
  }
  return NULL;
}

/* A string member of obj, or NULL when it is missing or not a string. */
static const char *string_member(JsonObject *obj, const char *name)
{
  JsonNode *node = json_object_get_member(obj, name);

  return node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING
             ? json_node_get_string(node)
             : NULL;
}

/* The program a plugin runs: plugin.json's "executable" (relative to its folder), else run.sh, plugin or a file
 * named like its folder; NULL when it is not there or cannot run. */
static char *plugin_executable(const char *plugin_dir, const char *dir_name, const char *configured)
{
  const char *defaults[] = { "run.sh", "plugin", dir_name, NULL };

  if (configured && *configured) {
    char *path = g_path_is_absolute(configured) ? g_strdup(configured) : g_build_filename(plugin_dir, configured, NULL);
    if (g_file_test(path, G_FILE_TEST_IS_EXECUTABLE))
      return path;
    g_free(path);
    return NULL;
  }
  for (int i = 0; defaults[i]; i++) {
    char *path = g_build_filename(plugin_dir, defaults[i], NULL);
    if (g_file_test(path, G_FILE_TEST_IS_EXECUTABLE))
      return path;
    g_free(path);
  }
  return NULL;
}

/* The plugin API version plugin.json asks for ("api": "tapi_N"): 0 when it names none, -1 when it is
 * not one this t-code speaks. */
static int plugin_api(JsonObject *obj)
{
  const char *api = string_member(obj, "api");
  char *end = NULL;
  gint64 number;

  if (!json_object_has_member(obj, "api"))
    return 0;
  if (!api || !g_str_has_prefix(api, "tapi_"))
    return -1;
  number = g_ascii_strtoll(api + 5, &end, 10);
  return end && end != api + 5 && !*end && number >= 1 && number <= TAPI_LATEST ? (int)number : -1;
}

/* plugin.json's "permissions": a list of "edit", "terminal", "network", "start" (others are left
 * out with a warning). */
static PluginPermissions plugin_permissions(JsonObject *obj, const char *name)
{
  static const char *const names[] = { "edit", "terminal", "network", "start" };
  JsonNode *node = json_object_get_member(obj, "permissions");
  JsonArray *list = node && JSON_NODE_HOLDS_ARRAY(node) ? json_node_get_array(node) : NULL;
  PluginPermissions permissions = 0;

  for (guint i = 0; list && i < json_array_get_length(list); i++) {
    JsonNode *item = json_array_get_element(list, i);
    const char *word = JSON_NODE_HOLDS_VALUE(item) && json_node_get_value_type(item) == G_TYPE_STRING
                           ? json_node_get_string(item)
                           : NULL;
    gboolean known = FALSE;
    for (guint n = 0; word && n < G_N_ELEMENTS(names); n++)
      if (strcmp(word, names[n]) == 0) {
        permissions |= 1 << n;
        known = TRUE;
      }
    if (!known)
      g_warning("Plugin %s: unknown permission '%s' left out", name, word ? word : "?");
  }
  return permissions;
}

/* A command's "keys": GTK's shortcut syntax (<Control><Alt>b), its label made from it. A "trigger"
 * (the shortcut before tapi_1) leaves "keys" a label; so does "keys" GTK cannot read (Ctrl+Alt+B). */
static void command_keys(PluginCommand *cmd, const char *keys, const char *trigger)
{
  guint key = 0;
  GdkModifierType mods = 0;

  if (trigger && *trigger) {
    cmd->trigger = g_strdup(trigger);
    cmd->keys = g_strdup(keys);
  } else if (keys && *keys && gtk_accelerator_parse(keys, &key, &mods) && key) {
    cmd->trigger = g_strdup(keys);
    cmd->keys = gtk_accelerator_get_label(key, mods);
  } else {
    cmd->keys = g_strdup(keys);
  }
}

/* The plugin plugin.json (obj) describes, or NULL: a warning says what is wrong with a broken one; one
 * with the id of a plugin found before is left out quietly. A plugin without commands or "start"
 * needs no program. */
static Plugin *plugin_from(App *t, JsonObject *obj, const char *plugin_dir, const char *dir_name)
{
  const char *name = string_member(obj, "name"), *id = string_member(obj, "id");
  JsonNode *commands = json_object_get_member(obj, "commands");
  JsonArray *list = commands && JSON_NODE_HOLDS_ARRAY(commands) ? json_node_get_array(commands) : NULL;
  int api = plugin_api(obj);
  PluginPermissions permissions;
  gboolean starts = FALSE;
  char *executable = NULL;

  if (!name || !*name) {
    g_warning("Plugin at %s: missing required 'name'", plugin_dir);
    return NULL;
  }
  if (commands && !list) {
    g_warning("Plugin %s: 'commands' is not a list", name);
    return NULL;
  }
  if (api < 0) {
    g_warning("Plugin %s asks for plugin API %s: it needs a newer t-code (this one speaks tapi_1 to tapi_%d)", name,
              string_member(obj, "api") ?: "?", TAPI_LATEST);
    return NULL;
  }
  if (!id || !*id)
    id = dir_name;
  if (plugin_find(t, id))
    return NULL;
  permissions = plugin_permissions(obj, name);
  if (json_object_has_member(obj, "start")) {
    if (g_strcmp0(string_member(obj, "start"), "project") != 0)
      g_warning("Plugin %s: \"start\" can only be \"project\"; left out", name);
    else if (api > 0 && !(permissions & PLUGIN_START))
      g_warning("Plugin %s: \"start\" needs \"start\" in \"permissions\": it runs only from its commands", name);
    else
      starts = TRUE;
  }
  if (((list && json_array_get_length(list) > 0) || starts) &&
      !(executable = plugin_executable(plugin_dir, dir_name, string_member(obj, "executable")))) {
    g_warning("Plugin %s (%s): no executable found in %s", name, id, plugin_dir);
    return NULL;
  }

  Plugin *p = g_new0(Plugin, 1);
  p->id = g_strdup(id);
  p->name = g_strdup(name);
  p->version = g_strdup(string_member(obj, "version") ?: "");
  p->description = g_strdup(string_member(obj, "description") ?: "");
  p->dir = g_strdup(plugin_dir);
  p->executable = executable;
  p->api = api;
  p->permissions = permissions;
  p->starts = starts;
  p->token = g_uuid_string_random();
  p->commands = g_ptr_array_new_with_free_func((GDestroyNotify)plugin_command_free);
  for (guint i = 0; list && i < json_array_get_length(list); i++) {
    JsonNode *node = json_array_get_element(list, i);
    JsonObject *command = JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
    const char *command_id = command ? string_member(command, "id") : NULL;
    const char *command_name = command ? string_member(command, "name") : NULL;
    if (!command_id || !command_name) {
      g_warning("Plugin %s: a command without 'id' or 'name' is left out", name);
      continue;
    }
    PluginCommand *cmd = g_new0(PluginCommand, 1);
    cmd->id = g_strdup(command_id);
    cmd->name = g_strdup(command_name);
    command_keys(cmd, string_member(command, "keys"), string_member(command, "trigger"));
    cmd->plugin = p;
    g_ptr_array_add(p->commands, cmd);
  }
  return p;
}

/* The plugin in plugin_dir (its plugin.json), when there is one that works. */
static void load_plugin_from_dir(App *t, const char *plugin_dir, const char *dir_name)
{
  char *json_path = g_build_filename(plugin_dir, "plugin.json", NULL);
  JsonParser *parser = json_parser_new();
  GError *error = NULL;
  JsonNode *root;
  Plugin *p;

  if (!g_file_test(json_path, G_FILE_TEST_IS_REGULAR)) {
    /* not a plugin's folder */
  } else if (!json_parser_load_from_file(parser, json_path, &error)) {
    g_warning("Plugin at %s: invalid JSON: %s", json_path, error->message);
  } else if ((root = json_parser_get_root(parser)) && JSON_NODE_HOLDS_OBJECT(root) &&
             (p = plugin_from(t, json_node_get_object(root), plugin_dir, dir_name))) {
    g_ptr_array_add(t->plugins, p);
  }
  g_clear_error(&error);
  g_object_unref(parser);
  g_free(json_path);
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
  char *user_dir = g_build_filename(g_get_user_config_dir(), TCODE_NAME, "plugins", NULL);
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

static void unload(App *t, gboolean quitting);

void plugins_stop(App *t)
{
  unload(t, TRUE);
}

/* Their start programs restart too: they get new tokens. */
void plugins_reload(App *t)
{
  unload(t, FALSE);
  plugins_init(t);
  refresh_shortcuts(t);
  plugins_start_project(t);
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

/* Running their programs ---------------------------------------------------------------------- */

enum {
  STOP_MS = 2000, /* a stopped program gets SIGTERM, then SIGKILL after this */
  QUIT_MS = 1000, /* the same while t-code quits: it waits this long */
};

struct PluginRun {
  App *app; /* NULL once its plugins were unloaded */
  char *plugin_id, *name;
  GSubprocess *proc;
  pid_t pid;             /* its process group: a script's children go with it */
  GDataInputStream *out; /* its stdout and stderr */
  GCancellable *cancel;  /* reading them */
  gboolean start;        /* a plugin's start program (else one command's run) */
  gboolean stopping;     /* t-code stopped it: its end is no failure */
  gboolean ended;
  guint kill_id; /* SIGKILL when SIGTERM was not enough */
  int refs;      /* the output reader and the wait */
};

static GList *runs; /* PluginRun *: every program started and not ended */

static void run_unref(PluginRun *run)
{
  if (--run->refs > 0)
    return;
  runs = g_list_remove(runs, run);
  g_clear_handle_id(&run->kill_id, g_source_remove);
  g_object_unref(run->out);
  g_object_unref(run->cancel);
  g_object_unref(run->proc);
  g_free(run->plugin_id);
  g_free(run->name);
  g_free(run);
}

/* Kept for the Plugins window, and on t-code's stderr like before. */
static void keep_line(PluginRun *run, const char *line)
{
  Plugin *p = run->app ? plugin_find(run->app, run->plugin_id) : NULL;

  g_printerr("%s: %s\n", run->plugin_id, line);
  if (!p)
    return;
  g_queue_push_tail(&p->output, g_strdup(line));
  while (p->output.length > PLUGIN_OUTPUT_LINES)
    g_free(g_queue_pop_head(&p->output));
}

static void on_output_line(GObject *source, GAsyncResult *result, gpointer data)
{
  PluginRun *run = data;
  char *line = g_data_input_stream_read_line_finish(G_DATA_INPUT_STREAM(source), result, NULL, NULL);
  char *text;

  if (!line) { /* the end, or unloaded */
    run_unref(run);
    return;
  }
  text = g_utf8_make_valid(line, -1);
  keep_line(run, text);
  g_free(text);
  g_free(line);
  g_data_input_stream_read_line_async(run->out, G_PRIORITY_DEFAULT, run->cancel, on_output_line, run);
}

static void on_run_ended(GObject *source G_GNUC_UNUSED, GAsyncResult *result, gpointer data)
{
  PluginRun *run = data;
  gboolean waited = g_subprocess_wait_finish(run->proc, result, NULL);
  gboolean ok = waited && g_subprocess_get_successful(run->proc);
  Plugin *p = run->app ? plugin_find(run->app, run->plugin_id) : NULL;

  run->ended = TRUE;
  g_clear_handle_id(&run->kill_id, g_source_remove);
  if (p && p->running == run) {
    p->running = NULL;
    p->state = ok ? PLUGIN_STOPPED : PLUGIN_FAILED;
  }
  if (!ok && !run->stopping && run->app) {
    if (waited && g_subprocess_get_if_exited(run->proc))
      g_warning("Plugin '%s' exited with status %d", run->name, g_subprocess_get_exit_status(run->proc));
    else if (waited)
      g_warning("Plugin '%s' was ended by signal %d", run->name, g_subprocess_get_term_sig(run->proc));
    char *message = g_strdup_printf("Plugin %s failed", run->name);
    status_say(run->app, message);
    g_free(message);
  }
  run_unref(run);
}

static void child_setup(gpointer data G_GNUC_UNUSED)
{
  setpgid(0, 0);
}

/* Starts p's program for command_id with the plugin's environment (docs/plugins.md, "How a plugin
 * runs"), without waiting for it; NULL when it cannot start (the status bar says so). */
static PluginRun *launch(App *t, Plugin *p, const char *command_id, gboolean start)
{
  GSubprocessLauncher *launcher =
      g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_MERGE);
  Tab *tab = current_tab(t);
  const char *active_file = (tab && tab->kind == TAB_EDITOR && tab->path) ? tab->path : "";
  char *api = g_strdup_printf("tapi_%d", TAPI_LATEST); /* what the socket speaks */
  char **env = g_get_environ();
  GError *error = NULL;

  if (t->paths.socket)
    env = g_environ_setenv(env, "TCODE_SOCKET", t->paths.socket, TRUE);
  env = g_environ_setenv(env, "TCODE_PLUGIN_ID", p->id, TRUE);
  env = g_environ_setenv(env, "TCODE_API", api, TRUE);
  env = g_environ_setenv(env, "TCODE_PLUGIN_TOKEN", p->token, TRUE);
  env = g_environ_setenv(env, "TCODE_COMMAND_ID", command_id, TRUE);
  env = g_environ_setenv(env, "TCODE_ACTIVE_FILE", active_file, TRUE);
  env = g_environ_setenv(env, "TCODE_PROJECT_ROOT", t->project.root ? t->project.root : "", TRUE);
  g_subprocess_launcher_set_environ(launcher, env);
  g_subprocess_launcher_set_cwd(launcher, p->dir);
  g_subprocess_launcher_set_child_setup(launcher, child_setup, NULL, NULL);
  g_strfreev(env);
  g_free(api);

  const char *argv[] = { p->executable, command_id, active_file, NULL };
  GSubprocess *proc = g_subprocess_launcher_spawnv(launcher, argv, &error);
  g_object_unref(launcher);
  if (!proc) {
    g_warning("Plugin '%s' launch failed: %s", p->name, error->message);
    char *message = g_strdup_printf("Plugin %s failed to launch", p->name);
    status_say(t, message);
    g_free(message);
    g_error_free(error);
    return NULL;
  }

  PluginRun *run = g_new0(PluginRun, 1);
  run->app = t;
  run->plugin_id = g_strdup(p->id);
  run->name = g_strdup(p->name);
  run->proc = proc;
  run->pid = (pid_t)atoi(g_subprocess_get_identifier(proc));
  run->out = g_data_input_stream_new(g_subprocess_get_stdout_pipe(proc));
  g_data_input_stream_set_newline_type(run->out, G_DATA_STREAM_NEWLINE_TYPE_ANY);
  run->cancel = g_cancellable_new();
  run->start = start;
  run->refs = 2;
  runs = g_list_prepend(runs, run);
  g_data_input_stream_read_line_async(run->out, G_PRIORITY_DEFAULT, run->cancel, on_output_line, run);
  g_subprocess_wait_async(proc, NULL, on_run_ended, run);
  return run;
}

/* A signal for the program and whatever it started. */
static void signal_run(PluginRun *run, int sig)
{
  if (kill(-run->pid, sig) < 0)
    kill(run->pid, sig);
}

static gboolean kill_late(gpointer data)
{
  PluginRun *run = data;

  run->kill_id = 0;
  signal_run(run, SIGKILL);
  return G_SOURCE_REMOVE;
}

static void stop_run(PluginRun *run)
{
  if (run->stopping || run->ended)
    return;
  run->stopping = TRUE;
  signal_run(run, SIGTERM);
  run->kill_id = g_timeout_add(STOP_MS, kill_late, run);
}

/* Whether the process is gone or only waits to be reaped (no main loop runs while t-code quits). */
static gboolean process_ended(pid_t pid)
{
  char path[64], *stat = NULL;
  gboolean ended;

  snprintf(path, sizeof path, "/proc/%d/stat", (int)pid);
  if (!g_file_get_contents(path, &stat, NULL, NULL))
    return TRUE;
  const char *paren = strrchr(stat, ')'); /* the name in parentheses may hold anything */
  ended = paren && paren[1] == ' ' && paren[2] == 'Z';
  g_free(stat);
  return ended;
}

/* t's plugins go: their start programs are stopped and nothing reads the programs' output any more.
 * Quitting, the stopped programs get QUIT_MS to go, then SIGKILL. */
static void unload(App *t, gboolean quitting)
{
  for (GList *l = runs; l; l = l->next) {
    PluginRun *run = l->data;
    if (run->app != t)
      continue;
    if (run->start)
      stop_run(run);
    g_cancellable_cancel(run->cancel);
    run->app = NULL;
  }
  if (t->plugins) {
    g_ptr_array_free(t->plugins, TRUE);
    t->plugins = NULL;
  }
  if (!quitting)
    return;
  for (gint64 until = g_get_monotonic_time() + QUIT_MS * 1000;; g_usleep(10000)) {
    gboolean left = FALSE;
    for (GList *l = runs; l; l = l->next) {
      PluginRun *run = l->data;
      left |= run->stopping && !run->ended && !process_ended(run->pid);
    }
    if (!left || g_get_monotonic_time() > until)
      break;
  }
  for (GList *l = runs; l; l = l->next) {
    PluginRun *run = l->data;
    if (run->stopping && !run->ended && !process_ended(run->pid))
      signal_run(run, SIGKILL);
  }
}

gboolean plugin_start(App *t, Plugin *p)
{
  if (!p->running && p->starts && p->executable) {
    p->running = launch(t, p, "start", TRUE);
    p->state = p->running ? PLUGIN_RUNNING : PLUGIN_FAILED;
  }
  return p->running != NULL;
}

void plugin_stop(Plugin *p)
{
  if (!p->running)
    return;
  stop_run(p->running);
  p->running = NULL;
  p->state = PLUGIN_STOPPED;
}

void plugins_start_project(App *t)
{
  if (!t->project.root || strcmp(t->project.root, g_get_home_dir()) == 0) /* home: no folder open */
    return;
  for (guint i = 0; t->plugins && i < t->plugins->len; i++)
    plugin_start(t, g_ptr_array_index(t->plugins, i));
}

void plugins_stop_project(App *t)
{
  for (guint i = 0; t->plugins && i < t->plugins->len; i++)
    plugin_stop(g_ptr_array_index(t->plugins, i));
}

void plugin_run_command(App *t, const char *plugin_id, const char *command_id)
{
  Plugin *p = plugin_find(t, plugin_id);
  gboolean found = FALSE;

  if (!p) {
    g_warning("Plugin '%s' not found", plugin_id);
    return;
  }
  for (guint i = 0; i < p->commands->len && !found; i++)
    found = g_strcmp0(((PluginCommand *)g_ptr_array_index(p->commands, i))->id, command_id) == 0;
  if (!found) {
    g_warning("Plugin '%s': command '%s' not found", plugin_id, command_id);
    return;
  }
  launch(t, p, command_id, FALSE);
}

void open_plugins_folder(App *t)
{
  char *dir = g_build_filename(g_get_user_config_dir(), TCODE_NAME, "plugins", NULL);
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
