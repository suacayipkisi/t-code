/*
 * Plugins (see plugins.h; the manifest and the protocol are docs/plugins.md's): finding them, their
 * commands in the palette and on keys, starting and stopping their programs.
 */
#ifndef _DEFAULT_SOURCE /* a test including this file may have it already */
#define _DEFAULT_SOURCE /* kill(), setpgid(), realpath() */
#endif
#include "plugins/plugins.h"

#include "application/identity.h"
#include "application/processes.h"
#include "editor/tabs.h"
#include "files/fileops.h"
#include "plugins/tapi.h"
#include "lsp/client/client.h"
#include "popups/picker.h"
#include "window/palette.h"
#include "window/statusbar.h"
#include "window/theme.h"

#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include <signal.h>
#include <stdlib.h>
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

static void plugin_server_free(PluginServer *server)
{
  g_free(server->name);
  g_strfreev(server->argv);
  g_strfreev(server->extensions);
  g_strfreev(server->languages);
  g_free(server->language_id);
  g_strfreev(server->language_ids);
  g_free(server->options);
  g_free(server->settings);
  g_strfreev(server->contents);
  g_free(server);
}

static void plugin_free(Plugin *plugin)
{
  if (!plugin)
    return;
  g_free(plugin->id);
  g_free(plugin->name);
  g_free(plugin->version);
  g_free(plugin->description);
  g_free(plugin->provider);
  g_free(plugin->dir);
  g_free(plugin->executable);
  g_free(plugin->token);
  g_queue_clear_full(&plugin->output, g_free);
  if (plugin->commands)
    g_ptr_array_free(plugin->commands, TRUE);
  if (plugin->servers)
    g_ptr_array_free(plugin->servers, TRUE);
  if (plugin->needs)
    g_ptr_array_unref(plugin->needs);
  g_clear_pointer(&plugin->run, g_hash_table_unref);
  g_clear_pointer(&plugin->debug, g_hash_table_unref);
  g_free(plugin->language_specs);
  g_free(plugin->snippets);
  g_free(plugin->styles);
  registry_record_free(plugin->record);
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

/* Whether relative (a path in plugin.json) stays in the plugin's folder once its ".." and links are
 * followed: a registry plugin's tree hash covers only that folder. TRUE when it is not there (yet). */
static gboolean inside_plugin(const char *plugin_dir, const char *relative)
{
  char *path = g_build_filename(plugin_dir, relative, NULL);
  char *real = realpath(path, NULL), *top = realpath(plugin_dir, NULL);
  size_t n = top ? strlen(top) : 0;
  gboolean inside = !real || (top && strncmp(real, top, n) == 0 && (real[n] == '/' || real[n] == '\0'));

  if (!real) { /* not there: still no ".." out of it */
    char *tidy = g_canonicalize_filename(relative, "/plugin");
    inside = g_str_has_prefix(tidy, "/plugin/");
    g_free(tidy);
  }
  free(top);
  free(real);
  g_free(path);
  return inside;
}

/* The program a plugin runs: plugin.json's "executable" (relative to its folder and inside it, or
 * absolute), else run.sh, plugin or a file named like its folder; NULL when it is not there or cannot run. */
static char *plugin_executable(const char *plugin_dir, const char *dir_name, const char *configured)
{
  const char *defaults[] = { "run.sh", "plugin", dir_name, NULL };

  if (configured && *configured) {
    if (!g_path_is_absolute(configured) && !inside_plugin(plugin_dir, configured)) {
      g_warning("Plugin in %s: \"executable\" %s leaves the plugin's folder", plugin_dir, configured);
      return NULL;
    }
    char *path = g_path_is_absolute(configured) ? g_strdup(configured) : g_build_filename(plugin_dir, configured, NULL);
    if (g_file_test(path, G_FILE_TEST_IS_EXECUTABLE))
      return path;
    g_free(path);
    return NULL;
  }
  for (int i = 0; defaults[i]; i++) {
    char *path = g_build_filename(plugin_dir, defaults[i], NULL);
    if (inside_plugin(plugin_dir, defaults[i]) && g_file_test(path, G_FILE_TEST_IS_EXECUTABLE))
      return path;
    g_free(path);
  }
  return NULL;
}

/* The plugin API version plugin.json asks for ("api": "tapi_1", "tapi_1_1"; plugins/tapi.h): 0 when it
 * names none, -1 when it is not one this t-code speaks. */
static int plugin_api(JsonObject *obj)
{
  int version;

  if (!json_object_has_member(obj, "api"))
    return 0;
  version = tapi_parse(string_member(obj, "api"));
  return tapi_spoken(version) ? version : -1;
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

/* obj's member name as a NULL-terminated list of non-empty strings (g_strfreev): an empty one when it
 * is missing, NULL when it is something else. */
static char **string_list(JsonObject *obj, const char *name)
{
  JsonNode *node = json_object_get_member(obj, name);
  JsonArray *list = node && JSON_NODE_HOLDS_ARRAY(node) ? json_node_get_array(node) : NULL;
  GPtrArray *strings = g_ptr_array_new();

  if (node && !list) {
    g_ptr_array_free(strings, TRUE);
    return NULL;
  }
  for (guint i = 0; list && i < json_array_get_length(list); i++) {
    JsonNode *item = json_array_get_element(list, i);
    if (!JSON_NODE_HOLDS_VALUE(item) || json_node_get_value_type(item) != G_TYPE_STRING ||
        !*json_node_get_string(item)) {
      g_ptr_array_free(strings, TRUE);
      return NULL;
    }
    g_ptr_array_add(strings, g_strdup(json_node_get_string(item)));
  }
  g_ptr_array_add(strings, NULL);
  return (char **)g_ptr_array_free(strings, FALSE);
}

/* obj's member name, an object, as JSON text; NULL when it is missing. *bad is set when it is there
 * but not an object. */
static char *object_text(JsonObject *obj, const char *name, gboolean *bad)
{
  JsonNode *node = json_object_get_member(obj, name);

  if (!node)
    return NULL;
  if (!JSON_NODE_HOLDS_OBJECT(node)) {
    *bad = TRUE;
    return NULL;
  }
  return json_to_string(node, FALSE);
}

/* An object of strings ("language-id": file pattern -> id, "uri-contents": scheme -> request) as name,
 * value, name, value, ... (g_strfreev); NULL when a value is not a string. */
static char **string_pairs(JsonObject *map)
{
  GList *names = json_object_get_members(map);
  GPtrArray *pairs = g_ptr_array_new();

  for (GList *l = names; l; l = l->next) {
    const char *id = string_member(map, l->data);
    if (!id || !*id || !*(const char *)l->data) {
      g_ptr_array_set_free_func(pairs, g_free);
      g_ptr_array_free(pairs, TRUE);
      g_list_free(names);
      return NULL;
    }
    g_ptr_array_add(pairs, g_strdup(l->data));
    g_ptr_array_add(pairs, g_strdup(id));
  }
  g_list_free(names);
  g_ptr_array_add(pairs, NULL);
  return (char **)g_ptr_array_free(pairs, FALSE);
}

/* One entry of "lsp", or NULL with a warning saying what is wrong with it. */
static PluginServer *plugin_server(JsonObject *obj, const char *plugin, const char *plugin_dir)
{
  PluginServer *s = g_new0(PluginServer, 1);
  JsonNode *language_id = json_object_get_member(obj, "language-id");
  gboolean bad = FALSE;
  const char *problem = NULL;

  if (language_id && JSON_NODE_HOLDS_OBJECT(language_id))
    s->language_ids = string_pairs(json_node_get_object(language_id));
  else if (!language_id || string_member(obj, "language-id"))
    s->language_ids = g_new0(char *, 1);

  s->argv = string_list(obj, "argv");
  s->extensions = string_list(obj, "extensions");
  s->languages = string_list(obj, "languages");
  s->options = object_text(obj, "options", &bad);
  s->settings = object_text(obj, "settings", &bad);
  JsonNode *contents = json_object_get_member(obj, "uri-contents");
  s->contents = !contents                          ? g_new0(char *, 1)
                : JSON_NODE_HOLDS_OBJECT(contents) ? string_pairs(json_node_get_object(contents))
                                                   : NULL;
  if (!s->argv || !s->argv[0])
    problem = "needs \"argv\", a list of strings starting with the program";
  else if (strchr(s->argv[0], '/') && !g_path_is_absolute(s->argv[0]) && !inside_plugin(plugin_dir, s->argv[0]))
    problem = "names a program outside the plugin's folder (\"argv\"[0] goes up out of it)";
  else if (!s->extensions || !s->languages)
    problem = "\"extensions\" and \"languages\" are lists of strings";
  else if (!s->extensions[0] && !s->languages[0])
    problem = "says no files: give it \"extensions\" or \"languages\"";
  else if (bad)
    problem = "\"options\" and \"settings\" are JSON objects";
  else if (!s->language_ids)
    problem = "\"language-id\" is a string, or an object of strings by file (\".jsx\": \"javascriptreact\")";
  else if (!s->contents)
    problem =
        "\"uri-contents\" is an object of strings, the request by URI scheme (\"jdt\": \"java/classFileContents\")";
  if (problem) {
    g_warning("Plugin %s: a language server %s; left out", plugin, problem);
    plugin_server_free(s);
    return NULL;
  }
  s->name = string_member(obj, "name") && *string_member(obj, "name") ? g_strdup(string_member(obj, "name"))
                                                                      : g_path_get_basename(s->argv[0]);
  s->language_id = g_strdup(string_member(obj, "language-id"));
  return s;
}

/* "lsp": the language servers the plugin declares, the broken ones left out with a warning. */
static GPtrArray *plugin_servers(JsonObject *obj, const char *plugin, const char *plugin_dir)
{
  JsonNode *node = json_object_get_member(obj, "lsp");
  JsonArray *list = node && JSON_NODE_HOLDS_ARRAY(node) ? json_node_get_array(node) : NULL;
  GPtrArray *servers = g_ptr_array_new_with_free_func((GDestroyNotify)plugin_server_free);

  if (node && !list)
    g_warning("Plugin %s: \"lsp\" is not a list; left out", plugin);
  for (guint i = 0; list && i < json_array_get_length(list); i++) {
    JsonNode *item = json_array_get_element(list, i);
    PluginServer *s =
        JSON_NODE_HOLDS_OBJECT(item) ? plugin_server(json_node_get_object(item), plugin, plugin_dir) : NULL;
    gboolean twice = FALSE;
    if (!JSON_NODE_HOLDS_OBJECT(item))
      g_warning("Plugin %s: a language server is not an object; left out", plugin);
    for (guint j = 0; s && j < servers->len; j++)
      twice |= strcmp(((PluginServer *)g_ptr_array_index(servers, j))->name, s->name) == 0;
    if (twice) {
      g_warning("Plugin %s: two language servers named '%s': the second is left out", plugin, s->name);
      g_clear_pointer(&s, plugin_server_free);
    }
    if (s)
      g_ptr_array_add(servers, s);
  }
  return servers;
}

/* "run" (or "debug"): file extensions (or names) -> the command Run (F5) hands them to, one of p's own. */
static GHashTable *plugin_run(JsonObject *obj, Plugin *p, const char *member)
{
  JsonNode *node = json_object_get_member(obj, member);
  JsonObject *map = node && JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
  GHashTable *run = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  GList *names = map ? json_object_get_members(map) : NULL;

  if (node && !map)
    g_warning("Plugin %s: \"%s\" is not an object; left out", p->name, member);
  for (GList *l = names; l; l = l->next) {
    const char *pattern = l->data, *command = string_member(map, pattern);
    gboolean known = FALSE;
    for (guint i = 0; command && i < p->commands->len; i++)
      known |= strcmp(((PluginCommand *)g_ptr_array_index(p->commands, i))->id, command) == 0;
    if (!*pattern || strchr(pattern, '/') || !known)
      g_warning("Plugin %s: \"%s\" for '%s' does not name one of its commands; left out", p->name, member, pattern);
    else
      g_hash_table_insert(run, g_strdup(pattern), g_strdup(command));
  }
  g_list_free(names);
  return run;
}

/* A folder member ("language-specs", "snippets", "styles"): relative to the plugin's folder, inside it. NULL,
 * with a warning when it is given but is not such a folder. */
static char *plugin_folder(JsonObject *obj, const char *member, const char *plugin, const char *plugin_dir)
{
  const char *relative = string_member(obj, member);
  char *path;

  if (!json_object_has_member(obj, member))
    return NULL;
  path = relative && *relative && !g_path_is_absolute(relative) && !strstr(relative, "..")
             ? g_build_filename(plugin_dir, relative, NULL)
             : NULL;
  if (!path || !g_file_test(path, G_FILE_TEST_IS_DIR)) {
    g_warning("Plugin %s: \"%s\" is not a folder inside the plugin's; left out", plugin, member);
    g_clear_pointer(&path, g_free);
  }
  return path;
}

static PluginSource scanning; /* where plugins_scan_dir is looking */

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
    char *latest = tapi_name(TAPI_LATEST);
    g_warning("Plugin %s asks for plugin API %s: it needs a newer t-code (this one speaks tapi_1 to %s)", name,
              string_member(obj, "api") ?: "?", latest);
    g_free(latest);
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
  p->provider = g_strdup(string_member(obj, "provider") ?: "");
  p->dir = g_strdup(plugin_dir);
  p->executable = executable;
  p->api = api;
  p->source = scanning;
  p->enabled = TRUE;
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
  p->servers = plugin_servers(obj, name, plugin_dir);
  p->needs = plugin_needs_of(obj, name);
  p->run = plugin_run(obj, p, "run");
  p->debug = plugin_run(obj, p, "debug");
  p->language_specs = plugin_folder(obj, "language-specs", name, plugin_dir);
  p->snippets = plugin_folder(obj, "snippets", name, plugin_dir);
  p->styles = plugin_folder(obj, "styles", name, plugin_dir);
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

/* The plugins' "language-specs" folders go after GtkSourceView's own (which win on the same id).
 * Only once, at start-up: GtkSourceView reads its folders once, the first time a language is looked
 * up, so a reload does not bring new ones. */
static void add_language_specs(App *t)
{
  static gboolean added;
  GtkSourceLanguageManager *languages = gtk_source_language_manager_get_default();

  for (guint i = 0; !added && i < t->plugins->len; i++) {
    Plugin *p = g_ptr_array_index(t->plugins, i);
    if (p->enabled && p->language_specs)
      gtk_source_language_manager_append_search_path(languages, p->language_specs);
  }
  added = TRUE;
}

/* The enabled plugins' "styles" folders become color themes; the settings' theme is shown again when
 * it came, went or may have changed. At start-up (before any widget) only the palette is set. */
static void apply_styles(App *t, gboolean starting)
{
  GPtrArray *dirs = g_ptr_array_new();

  for (guint i = 0; i < t->plugins->len; i++) {
    Plugin *p = g_ptr_array_index(t->plugins, i);
    if (p->enabled && p->styles)
      g_ptr_array_add(dirs, p->styles);
  }
  g_ptr_array_add(dirs, NULL);
  if (palette_set_styles((const char *const *)dirs->pdata, t->settings.color_theme)) {
    if (starting)
      palette_init(t, t->settings.color_theme);
    else
      color_theme_apply(t, t->settings.color_theme);
  }
  g_ptr_array_free(dirs, TRUE);
}

/* Turned on or off ---------------------------------------------------------------------------- */

static guint loads; /* plugins_generation() */

guint plugins_generation(void)
{
  return loads;
}

static char *choices_file(void)
{
  return g_build_filename(g_get_user_config_dir(), TCODE_NAME, "plugins.json", NULL);
}

/* plugins.json's "disabled" ids (g_strfreev; an empty list when there is none). */
static char **disabled_ids(void)
{
  char *path = choices_file();
  JsonParser *parser = json_parser_new();
  JsonNode *root;
  char **ids = NULL;

  if (json_parser_load_from_file(parser, path, NULL) && (root = json_parser_get_root(parser)) &&
      JSON_NODE_HOLDS_OBJECT(root))
    ids = string_list(json_node_get_object(root), "disabled");
  g_object_unref(parser);
  g_free(path);
  return ids ? ids : g_new0(char *, 1);
}

/* Writes plugins.json again with ids as "disabled", keeping its other members. */
static void store_disabled(char **ids)
{
  char *path = choices_file(), *text;
  JsonParser *parser = json_parser_new();
  JsonNode *root = NULL;
  JsonObject *obj;
  JsonArray *list = json_array_new();
  GError *error = NULL;

  if (json_parser_load_from_file(parser, path, NULL) && json_parser_get_root(parser) &&
      JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser)))
    root = json_node_copy(json_parser_get_root(parser));
  if (!root) {
    root = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(root, json_object_new());
  }
  obj = json_node_get_object(root);
  for (int i = 0; ids[i]; i++)
    json_array_add_string_element(list, ids[i]);
  json_object_set_array_member(obj, "disabled", list);
  text = json_to_string(root, TRUE);
  if (!g_file_set_contents(path, text, -1, &error)) {
    g_warning("t-code: cannot save %s: %s", path, error->message);
    g_clear_error(&error);
  }
  g_free(text);
  json_node_unref(root);
  g_object_unref(parser);
  g_free(path);
}

void plugins_choose_disabled(const char *id, gboolean disabled)
{
  char **ids = disabled_ids();
  GPtrArray *kept = g_ptr_array_new();

  for (int i = 0; ids[i]; i++)
    if (strcmp(ids[i], id) != 0)
      g_ptr_array_add(kept, ids[i]);
  if (disabled)
    g_ptr_array_add(kept, (gpointer)id);
  g_ptr_array_add(kept, NULL);
  store_disabled((char **)kept->pdata);
  g_ptr_array_free(kept, TRUE);
  g_strfreev(ids);
}

gboolean plugin_unreviewed(Plugin *p)
{
  return p->record && !p->record->reviewed;
}

void plugin_warned(Plugin *p, RegistryWarned warning)
{
  if (!p->record)
    return;
  p->record->warned |= warning;
  registry_set_record(p->id, p->record);
}

/* Whether p's program still needs the third warning before it runs. */
static gboolean ask_first(Plugin *p)
{
  return !plugin_may_run(p);
}

void plugin_set_enabled(App *t, Plugin *p, gboolean enabled)
{
  if (p->enabled == enabled)
    return;
  plugins_choose_disabled(p->id, !enabled);
  p->enabled = enabled;
  if (!enabled)
    plugin_stop(p);
  else if (t->project.root && strcmp(t->project.root, g_get_home_dir()) != 0)
    plugin_start(t, p);
  refresh_shortcuts(t);
  lsp_reattach(t);
  if (p->styles)
    apply_styles(t, FALSE);
}

gboolean plugin_uninstall(App *t, Plugin *p, GError **error)
{
  char *dir, *id;
  gboolean done;

  if (p->source != PLUGIN_FROM_USER) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                "%s came with the system or with t-code, not from your plugins folder: nothing to uninstall here",
                p->name);
    return FALSE;
  }
  dir = g_strdup(p->dir);
  id = g_strdup(p->id);
  plugin_stop(p);
  done = files_delete(dir, error);
  if (done) { /* only once it is gone: a plugin left behind stays off and keeps its review state */
    plugins_choose_disabled(id, FALSE); /* not left in "disabled" */
    registry_set_record(id, NULL);
  }
  g_free(id);
  g_free(dir);
  plugins_reload(t);
  return done;
}

/* Loading ----------------------------------------------------------------------------------- */

void plugins_init(App *t)
{
  char **disabled = disabled_ids();

  if (!t->plugins)
    t->plugins = g_ptr_array_new_with_free_func((GDestroyNotify)plugin_free);
  loads++;

  /* 1. User directory: ~/.config/t-code/plugins */
  char *user_dir = g_build_filename(g_get_user_config_dir(), TCODE_NAME, "plugins", NULL);
  g_mkdir_with_parents(user_dir, 0755);
  scanning = PLUGIN_FROM_USER;
  plugins_scan_dir(t, user_dir);
  g_free(user_dir);

  /* 2. System directory: /usr/share/t-code/plugins */
  scanning = PLUGIN_FROM_SYSTEM;
  plugins_scan_dir(t, "/usr/share/t-code/plugins");

  /* 3. Also check beside styles for development / portable runs */
  if (t->paths.styles) {
    char *parent = g_path_get_dirname(t->paths.styles);
    char *beside = g_build_filename(parent, "plugins", NULL);
    scanning = PLUGIN_FROM_TCODE;
    if (g_strcmp0(beside, "/usr/share/t-code/plugins") != 0)
      plugins_scan_dir(t, beside);
    g_free(beside);
    g_free(parent);
  }
  scanning = PLUGIN_FROM_USER; /* plugins_scan_dir called alone (tests) */
  for (guint i = 0; i < t->plugins->len; i++) {
    Plugin *p = g_ptr_array_index(t->plugins, i);
    p->enabled = !g_strv_contains((const char *const *)disabled, p->id);
    if (p->source == PLUGIN_FROM_USER) /* the registry installs only there */
      p->record = registry_record(p->id);
    if (!*p->provider && p->record) {
      g_free(p->provider);
      p->provider = registry_owner(p->record->repository);
    }
  }
  g_strfreev(disabled);
  add_language_specs(t);
  apply_styles(t, loads == 1);
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
  lsp_reattach(t); /* their language servers may have come or gone */
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
    for (guint j = 0; p->enabled && j < p->commands->len; j++) {
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
    for (guint j = 0; plugin->enabled && j < plugin->commands->len; j++) {
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
  p->printed++;
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
 * runs"), without waiting for it; NULL when it cannot start (the status bar says so). file (NULL: the
 * active editor's) is its $2; test (NULL: none) goes into TCODE_TEST. */
static PluginRun *launch_with(App *t, Plugin *p, const char *command_id, gboolean start, const char *file,
                              const char *test, int port)
{
  GSubprocessLauncher *launcher =
      g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_MERGE);
  Tab *tab = current_tab(t);
  const char *active_file = file ? file : (tab && tab->kind == TAB_EDITOR && tab->path) ? tab->path : "";
  char *api = tapi_name(TAPI_LATEST); /* what the socket speaks */
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
  env = test ? g_environ_setenv(env, "TCODE_TEST", test, TRUE) : g_environ_unsetenv(env, "TCODE_TEST");
  char *port_text = port > 0 ? g_strdup_printf("%d", port) : NULL;
  env = port_text ? g_environ_setenv(env, "TCODE_DEBUG_PORT", port_text, TRUE)
                  : g_environ_unsetenv(env, "TCODE_DEBUG_PORT");
  g_free(port_text);
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
  /* NULL once GLib has already reaped a program that ended at once: no pid to signal then. */
  const char *identifier = g_subprocess_get_identifier(proc);
  run->pid = identifier ? (pid_t)atoi(identifier) : 0;
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

static PluginRun *launch(App *t, Plugin *p, const char *command_id, gboolean start)
{
  return launch_with(t, p, command_id, start, NULL, NULL, 0);
}

/* A signal for the program and whatever it started. */
static void signal_run(PluginRun *run, int sig)
{
  if (run->pid <= 0) /* kill(0, ...) would signal t-code's own process group */
    return;
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

  if (pid <= 0)
    return TRUE;
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
  if (!p->running && p->enabled && p->starts && p->executable && ask_first(p)) {
    plugins_confirm_run(t, p, NULL);
    return FALSE;
  }
  if (!p->running && p->enabled && p->starts && p->executable) {
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

/* The command of an enabled plugin whose "run" (or "debug") names file, or NULL. */
static PluginCommand *command_for(App *t, const char *file, gboolean debug)
{
  char *name = file ? g_path_get_basename(file) : NULL;
  PluginCommand *found = NULL;

  for (guint i = 0; name && t->plugins && i < t->plugins->len && !found; i++) {
    Plugin *p = g_ptr_array_index(t->plugins, i);
    GHashTableIter iter;
    gpointer pattern, command;
    GHashTable *map = debug ? p->debug : p->run;
    if (!map)
      continue;
    g_hash_table_iter_init(&iter, map);
    while (p->enabled && !found && g_hash_table_iter_next(&iter, &pattern, &command)) {
      char *const patterns[] = { pattern, NULL };
      for (guint c = 0; plugin_name_matches(name, patterns) && c < p->commands->len && !found; c++)
        if (strcmp(((PluginCommand *)g_ptr_array_index(p->commands, c))->id, command) == 0)
          found = g_ptr_array_index(p->commands, c);
    }
  }
  g_free(name);
  return found;
}

PluginCommand *plugins_run_command_for(App *t, const char *file)
{
  return command_for(t, file, FALSE);
}

PluginCommand *plugins_debug_command_for(App *t, const char *file)
{
  return command_for(t, file, TRUE);
}

gboolean plugin_debug_test(App *t, PluginCommand *command, const char *file, const char *test, int port)
{
  Plugin *p = command->plugin;

  if (!p->enabled)
    return FALSE;
  if (ask_first(p)) { /* its first run is asked about from its own commands */
    char *said = g_strdup_printf("%s has not run yet: run one of its commands first (it asks whether it may)", p->name);
    status_say(t, said);
    g_free(said);
    return FALSE;
  }
  return launch_with(t, p, command->id, FALSE, file, test, port) != NULL;
}

void plugin_run_command(App *t, const char *plugin_id, const char *command_id)
{
  Plugin *p = plugin_find(t, plugin_id);
  gboolean found = FALSE;

  if (!p) {
    g_warning("Plugin '%s' not found", plugin_id);
    return;
  }
  if (!p->enabled) {
    status_say(t, "That plugin is switched off: turn it on in Plugins: Manage Plugins");
    return;
  }
  if (ask_first(p)) {
    plugins_confirm_run(t, p, command_id);
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

/* The file manager answered: a failure says so in the status bar. */
static void on_folder_opened(GObject *launcher, GAsyncResult *result, gpointer data)
{
  GError *error = NULL;

  if (!gtk_file_launcher_launch_finish(GTK_FILE_LAUNCHER(launcher), result, &error) &&
      !g_error_matches(error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED)) {
    char *said = g_strdup_printf("The file manager would not open that folder: %s", error->message);
    status_say(data, said);
    g_free(said);
  }
  g_clear_error(&error);
}

/* GtkFileLauncher, not GtkUriLauncher: the portal's OpenURI turns down a folder's file:// URI. */
void plugin_open_folder(App *t, const char *dir)
{
  GFile *folder = g_file_new_for_path(dir);
  GtkFileLauncher *launcher = gtk_file_launcher_new(folder);

  gtk_file_launcher_launch(launcher, t->window, NULL, on_folder_opened, t);
  g_object_unref(launcher);
  g_object_unref(folder);
}

void open_plugins_folder(App *t)
{
  char *dir = g_build_filename(g_get_user_config_dir(), TCODE_NAME, "plugins", NULL);

  g_mkdir_with_parents(dir, 0755);
  plugin_open_folder(t, dir);
  g_free(dir);
}

void plugins_own_processes(GHashTable *owners)
{
  for (GList *l = runs; l; l = l->next) {
    PluginRun *run = l->data;
    if (!run->ended && run->app && run->pid > 0) {
      char *label = g_strdup_printf("Plugin: %s", run->name);
      procs_own(owners, run->pid, PROC_PLUGIN, label);
      g_free(label);
    }
  }
}

gboolean plugins_end_process(int pid)
{
  for (GList *l = runs; l; l = l->next) {
    PluginRun *run = l->data;
    if (run->pid == pid && !run->ended) {
      Plugin *p = run->app ? plugin_find(run->app, run->plugin_id) : NULL;
      if (p && p->running == run)
        plugin_stop(p);
      else
        stop_run(run);
      return TRUE;
    }
  }
  return FALSE;
}
