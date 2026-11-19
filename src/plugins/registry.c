/*
 * The plugin registry (see registry.h). Downloads go through curl in a GSubprocess (like the update
 * check: no HTTP library), archives are unpacked by tar into a fresh folder in the cache, and nothing
 * lands in the plugins folder before its tree hash matches the pin.
 */
#define _DEFAULT_SOURCE /* lstat() under -std=c23 */
#include "plugins/registry.h"

#include "application/identity.h"

#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include <sys/stat.h>

#define FETCH_SECONDS "20"    /* the index */
#define DOWNLOAD_SECONDS "90" /* a plugin's archive */

void registry_entry_free(RegistryEntry *entry)
{
  if (!entry)
    return;
  g_free(entry->id);
  g_free(entry->name);
  g_free(entry->description);
  g_free(entry->provider);
  g_free(entry->version);
  g_free(entry->api);
  g_free(entry->repository);
  g_free(entry->commit);
  g_free(entry->path);
  g_free(entry->tree);
  g_strfreev(entry->permissions);
  g_strfreev(entry->categories);
  g_free(entry);
}

/* The index -------------------------------------------------------------------------------------- */

static const char *text_of(JsonObject *obj, const char *name)
{
  JsonNode *node = json_object_get_member(obj, name);

  return node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING
             ? json_node_get_string(node)
             : NULL;
}

/* A list of strings, or an empty one. */
static char **texts_of(JsonObject *obj, const char *name)
{
  JsonNode *node = json_object_get_member(obj, name);
  JsonArray *list = node && JSON_NODE_HOLDS_ARRAY(node) ? json_node_get_array(node) : NULL;
  GPtrArray *texts = g_ptr_array_new();

  for (guint i = 0; list && i < json_array_get_length(list); i++) {
    JsonNode *item = json_array_get_element(list, i);
    if (JSON_NODE_HOLDS_VALUE(item) && json_node_get_value_type(item) == G_TYPE_STRING)
      g_ptr_array_add(texts, g_strdup(json_node_get_string(item)));
  }
  g_ptr_array_add(texts, NULL);
  return (char **)g_ptr_array_free(texts, FALSE);
}

static gboolean is_hex(const char *text, size_t length)
{
  if (!text || strlen(text) != length)
    return FALSE;
  for (size_t i = 0; i < length; i++)
    if (!g_ascii_isxdigit(text[i]) || g_ascii_isupper(text[i]))
      return FALSE;
  return TRUE;
}

/* An id is a folder's name: lowercase letters, digits and "-". */
static gboolean good_id(const char *id)
{
  if (!id || !*id || *id == '-')
    return FALSE;
  for (const char *c = id; *c; c++)
    if (!g_ascii_islower(*c) && !g_ascii_isdigit(*c) && *c != '-')
      return FALSE;
  return TRUE;
}

/* A path inside the repository: relative, no "..", no leading or doubled "/". */
static gboolean good_path(const char *path)
{
  char **parts;
  gboolean good = TRUE;

  if (!*path)
    return TRUE;
  if (*path == '/' || g_str_has_suffix(path, "/"))
    return FALSE;
  parts = g_strsplit(path, "/", -1);
  for (int i = 0; parts[i]; i++)
    good &= *parts[i] && strcmp(parts[i], ".") != 0 && strcmp(parts[i], "..") != 0;
  g_strfreev(parts);
  return good;
}

char *registry_owner(const char *repository)
{
  const char *owner = repository && g_str_has_prefix(repository, "https://github.com/")
                          ? repository + strlen("https://github.com/")
                          : NULL;
  const char *slash = owner ? strchr(owner, '/') : NULL;

  return slash ? g_strndup(owner, slash - owner) : g_strdup("");
}

static RegistryEntry *entry_from(JsonObject *obj)
{
  RegistryEntry *e = g_new0(RegistryEntry, 1);
  JsonNode *reviewed = json_object_get_member(obj, "reviewed");
  const char *repository = text_of(obj, "repository");
  const char *problem = NULL;

  e->id = g_strdup(text_of(obj, "id"));
  e->name = g_strdup(text_of(obj, "name") ?: text_of(obj, "id"));
  e->description = g_strdup(text_of(obj, "description") ?: "");
  e->version = g_strdup(text_of(obj, "version") ?: "");
  e->api = g_strdup(text_of(obj, "api") ?: "");
  e->provider = text_of(obj, "provider") ? g_strdup(text_of(obj, "provider")) : registry_owner(repository);
  e->repository = g_strdup(repository);
  e->commit = g_strdup(text_of(obj, "commit"));
  e->path = g_strdup(text_of(obj, "path") ?: "");
  e->tree = g_strdup(text_of(obj, "tree-sha256"));
  e->permissions = texts_of(obj, "permissions");
  e->categories = texts_of(obj, "categories");
  e->reviewed = reviewed && JSON_NODE_HOLDS_VALUE(reviewed) && json_node_get_value_type(reviewed) == G_TYPE_BOOLEAN &&
                json_node_get_boolean(reviewed);
  if (!good_id(e->id))
    problem = "its id is not lowercase letters, digits and -";
  else if (!repository ||
           !(g_str_has_prefix(repository, "https://github.com/") || g_str_has_prefix(repository, "file:///")))
    problem = "its repository is not https://github.com/<owner>/<repo>";
  else if (!is_hex(e->commit, 40))
    problem = "its commit is not 40 lowercase hex digits";
  else if (!is_hex(e->tree, 64))
    problem = "its tree-sha256 is not 64 lowercase hex digits";
  else if (!good_path(e->path))
    problem = "its path leaves the repository";
  if (problem) {
    g_warning("Registry: %s left out: %s", e->id ? e->id : "an entry", problem);
    registry_entry_free(e);
    return NULL;
  }
  return e;
}

GPtrArray *registry_parse(const char *json, GError **error)
{
  JsonParser *parser = json_parser_new();
  JsonNode *root, *plugins;
  JsonArray *list;
  GPtrArray *entries = NULL;

  if (!json_parser_load_from_data(parser, json, -1, error)) {
    g_object_unref(parser);
    return NULL;
  }
  root = json_parser_get_root(parser);
  plugins = root && JSON_NODE_HOLDS_OBJECT(root) ? json_object_get_member(json_node_get_object(root), "plugins") : NULL;
  list = plugins && JSON_NODE_HOLDS_ARRAY(plugins) ? json_node_get_array(plugins) : NULL;
  if (!list) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "that is not a plugin index (no \"plugins\" list)");
  } else {
    entries = g_ptr_array_new_with_free_func((GDestroyNotify)registry_entry_free);
    for (guint i = 0; i < json_array_get_length(list); i++) {
      JsonNode *item = json_array_get_element(list, i);
      RegistryEntry *e = JSON_NODE_HOLDS_OBJECT(item) ? entry_from(json_node_get_object(item)) : NULL;
      gboolean twice = FALSE;
      for (guint j = 0; e && j < entries->len; j++)
        twice |= strcmp(((RegistryEntry *)g_ptr_array_index(entries, j))->id, e->id) == 0;
      if (twice) {
        g_warning("Registry: %s is listed twice: the second is left out", e->id);
        g_clear_pointer(&e, registry_entry_free);
      }
      if (e)
        g_ptr_array_add(entries, e);
    }
  }
  g_object_unref(parser);
  return entries;
}

/* The tree hash ---------------------------------------------------------------------------------- */

/* Adds "<relative path>" for every regular file under dir to files; FALSE with error on anything else
 * (a symbolic link, a device). */
static gboolean list_files(const char *top, const char *relative, GPtrArray *files, GError **error)
{
  char *dir = relative ? g_build_filename(top, relative, NULL) : g_strdup(top);
  GDir *listing = g_dir_open(dir, 0, error);
  const char *name;
  gboolean good = listing != NULL;

  while (good && (name = g_dir_read_name(listing))) {
    char *path = g_build_filename(dir, name, NULL);
    char *inside = relative ? g_strconcat(relative, "/", name, NULL) : g_strdup(name);
    GStatBuf info;
    if (g_lstat(path, &info) != 0) {
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "cannot read %s", inside);
      good = FALSE;
    } else if (S_ISDIR(info.st_mode)) {
      good = list_files(top, inside, files, error);
    } else if (S_ISREG(info.st_mode)) {
      g_ptr_array_add(files, g_steal_pointer(&inside));
    } else {
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_REGULAR_FILE,
                  "%s is a link or a special file: plugins hold plain files only", inside);
      good = FALSE;
    }
    g_free(inside);
    g_free(path);
  }
  if (listing)
    g_dir_close(listing);
  g_free(dir);
  return good;
}

static int by_bytes(gconstpointer a, gconstpointer b)
{
  return strcmp(*(char *const *)a, *(char *const *)b);
}

char *registry_tree_hash(const char *dir, GError **error)
{
  GPtrArray *files = g_ptr_array_new_with_free_func(g_free);
  GChecksum *tree = g_checksum_new(G_CHECKSUM_SHA256);
  char *hash = NULL;
  gboolean good = list_files(dir, NULL, files, error);

  g_ptr_array_sort(files, by_bytes);
  for (guint i = 0; good && i < files->len; i++) {
    const char *inside = g_ptr_array_index(files, i);
    char *path = g_build_filename(dir, inside, NULL), *contents = NULL, *line;
    gsize length = 0;
    GStatBuf info;
    if ((good = g_file_get_contents(path, &contents, &length, error) && g_stat(path, &info) == 0)) {
      char *file_hash = g_compute_checksum_for_data(G_CHECKSUM_SHA256, (const guchar *)contents, length);
      line = g_strdup_printf("%s %c %s\n", file_hash, info.st_mode & 0111 ? 'x' : '-', inside);
      g_checksum_update(tree, (const guchar *)line, -1);
      g_free(line);
      g_free(file_hash);
    }
    g_free(contents);
    g_free(path);
  }
  if (good)
    hash = g_strdup(g_checksum_get_string(tree));
  g_checksum_free(tree);
  g_ptr_array_free(files, TRUE);
  return hash;
}

char *registry_archive_url(const RegistryEntry *entry)
{
  if (g_str_has_prefix(entry->repository, "https://github.com/"))
    return g_strdup_printf("%s/archive/%s.tar.gz", entry->repository, entry->commit);
  return g_strdup_printf("%s/%s.tar.gz", entry->repository, entry->commit);
}

/* Fetching the index ----------------------------------------------------------------------------- */

typedef struct {
  RegistryFetched done;
  gpointer data;
} Fetch;

static void fetched(Fetch *f, const char *json, const char *failure)
{
  GError *error = NULL;
  GPtrArray *entries = json ? registry_parse(json, &error) : NULL;
  char *message = NULL;

  if (!json)
    message = g_strdup(failure);
  else if (!entries)
    message = g_strdup_printf("The registry's index is broken: %s", error->message);
  f->done(entries, message, f->data);
  g_clear_error(&error);
  g_free(message);
  g_free(f);
}

static void on_index(GObject *source, GAsyncResult *result, gpointer data)
{
  char *out = NULL;

  g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result, &out, NULL, NULL);
  fetched(data, out && *out && g_subprocess_get_successful(G_SUBPROCESS(source)) ? out : NULL,
          "Could not reach the registry: offline, or GitHub is having a moment");
  g_free(out);
}

void registry_fetch(RegistryFetched done, gpointer data)
{
  const char *where = g_getenv("TCODE_REGISTRY");
  Fetch *f = g_new0(Fetch, 1);
  GSubprocess *process;
  char *url;

  f->done = done;
  f->data = data;
  if (where && *where && g_path_is_absolute(where)) { /* a file: the tests', a local copy */
    char *json = NULL;
    g_file_get_contents(where, &json, NULL, NULL);
    fetched(f, json, "Could not read the registry's index file");
    g_free(json);
    return;
  }
  url = g_strdup(where && *where ? where : REGISTRY_INDEX_URL);
  process = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, NULL, "curl", "-fsSL",
                             "--max-time", FETCH_SECONDS, url, NULL);
  g_free(url);
  if (!process) {
    fetched(f, NULL, "Could not reach the registry: curl is not installed");
    return;
  }
  g_subprocess_communicate_utf8_async(process, NULL, NULL, on_index, f);
  g_object_unref(process);
}

/* Installing ------------------------------------------------------------------------------------- */

typedef struct {
  RegistryEntry entry; /* its own copy of the strings it needs */
  char *plugins_dir, *work, *archive;
  RegistryInstalled done;
  gpointer data;
} Install;

static void remove_tree(const char *path)
{
  GStatBuf info;

  if (g_lstat(path, &info) != 0)
    return;
  if (S_ISDIR(info.st_mode)) {
    GDir *dir = g_dir_open(path, 0, NULL);
    const char *name;
    while (dir && (name = g_dir_read_name(dir))) {
      char *child = g_build_filename(path, name, NULL);
      remove_tree(child);
      g_free(child);
    }
    if (dir)
      g_dir_close(dir);
    g_rmdir(path);
  } else {
    g_unlink(path);
  }
}

/* Ends an install: done hears how it went, the cache's folder goes. */
static void finish(Install *in, const char *error)
{
  in->done(error, in->data);
  if (in->work)
    remove_tree(in->work);
  g_free(in->entry.id);
  g_free(in->entry.commit);
  g_free(in->entry.path);
  g_free(in->entry.tree);
  g_free(in->plugins_dir);
  g_free(in->work);
  g_free(in->archive);
  g_free(in);
}

/* The unpacked archive's only folder (GitHub's "<repo>-<commit>"), or NULL. */
static char *archive_top(const char *unpacked)
{
  GDir *dir = g_dir_open(unpacked, 0, NULL);
  const char *name = dir ? g_dir_read_name(dir) : NULL;
  char *top = name ? g_build_filename(unpacked, name, NULL) : NULL;

  if (dir && g_dir_read_name(dir)) /* more than one */
    g_clear_pointer(&top, g_free);
  if (top && !g_file_test(top, G_FILE_TEST_IS_DIR))
    g_clear_pointer(&top, g_free);
  if (dir)
    g_dir_close(dir);
  return top;
}

/* plugin.json's "id" (or the folder's name) in dir is id. */
static gboolean same_id(const char *dir, const char *id)
{
  char *manifest = g_build_filename(dir, "plugin.json", NULL);
  JsonParser *parser = json_parser_new();
  JsonNode *root;
  const char *named = NULL;
  gboolean same = FALSE;

  if (json_parser_load_from_file(parser, manifest, NULL) && (root = json_parser_get_root(parser)) &&
      JSON_NODE_HOLDS_OBJECT(root)) {
    named = text_of(json_node_get_object(root), "id");
    same = !named || strcmp(named, id) == 0;
  }
  g_object_unref(parser);
  g_free(manifest);
  return same;
}

/* Checked: the plugin's folder moves into the plugins folder, an older one out of the way first. */
static void put_in_place(Install *in, const char *plugin)
{
  char *target = g_build_filename(in->plugins_dir, in->entry.id, NULL);
  char *old = g_strconcat(target, ".old", NULL);
  gboolean had = g_file_test(target, G_FILE_TEST_EXISTS);
  char *error = NULL;

  remove_tree(old);
  g_mkdir_with_parents(in->plugins_dir, 0755);
  if (had && g_rename(target, old) != 0)
    error = g_strdup_printf("Could not move the installed %s out of the way", in->entry.id);
  else if (g_rename(plugin, target) != 0) {
    error = g_strdup_printf("Could not put %s into %s", in->entry.id, in->plugins_dir);
    if (had)
      g_rename(old, target); /* the old one comes back */
  }
  remove_tree(old);
  finish(in, error);
  g_free(error);
  g_free(old);
  g_free(target);
}

static void on_unpacked(GObject *source, GAsyncResult *result, gpointer data)
{
  Install *in = data;
  char *unpacked = g_build_filename(in->work, "unpacked", NULL), *top = NULL, *plugin = NULL, *hash = NULL;
  char *manifest = NULL, *error = NULL;
  GError *problem = NULL;

  if (!g_subprocess_wait_check_finish(G_SUBPROCESS(source), result, NULL))
    error = g_strdup("The download is not a tar.gz archive tar can unpack");
  else if (!(top = archive_top(unpacked)))
    error = g_strdup("The archive does not hold one folder");
  else {
    plugin = *in->entry.path ? g_build_filename(top, in->entry.path, NULL) : g_strdup(top);
    manifest = g_build_filename(plugin, "plugin.json", NULL);
    if (!g_file_test(manifest, G_FILE_TEST_IS_REGULAR))
      error = g_strdup_printf("No plugin.json at \"%s\" in that commit", in->entry.path);
    else if (!(hash = registry_tree_hash(plugin, &problem)))
      error = g_strdup_printf("The plugin's files cannot be checked: %s", problem->message);
    else if (strcmp(hash, in->entry.tree) != 0)
      error = g_strdup("The download does not match the registry's tree-sha256: not installed");
    else if (!same_id(plugin, in->entry.id))
      error = g_strdup("Its plugin.json names another id than the registry: not installed");
  }
  if (error)
    finish(in, error);
  else
    put_in_place(in, plugin);
  g_clear_error(&problem);
  g_free(error);
  g_free(manifest);
  g_free(hash);
  g_free(plugin);
  g_free(top);
  g_free(unpacked);
}

static void on_downloaded(GObject *source, GAsyncResult *result, gpointer data)
{
  Install *in = data;
  char *unpacked = g_build_filename(in->work, "unpacked", NULL);
  GSubprocess *tar;

  if (!g_subprocess_wait_check_finish(G_SUBPROCESS(source), result, NULL)) {
    finish(in, "Could not download the plugin: offline, or that commit is gone");
  } else {
    g_mkdir_with_parents(unpacked, 0700);
    /* --no-same-owner / -permissions: the user's own files; GNU tar refuses ".." and leading "/" */
    tar = g_subprocess_new(G_SUBPROCESS_FLAGS_STDERR_SILENCE, NULL, "tar", "-xzf", in->archive, "-C", unpacked,
                           "--no-same-owner", "--no-same-permissions", NULL);
    if (tar) {
      g_subprocess_wait_check_async(tar, NULL, on_unpacked, in);
      g_object_unref(tar);
    } else {
      finish(in, "Could not unpack the plugin: tar is not installed");
    }
  }
  g_free(unpacked);
}

void registry_install(const RegistryEntry *entry, const char *plugins_dir, RegistryInstalled done, gpointer data)
{
  Install *in = g_new0(Install, 1);
  char *cache = g_build_filename(g_get_user_cache_dir(), TCODE_NAME, NULL), *url;
  GSubprocess *curl;

  in->entry.id = g_strdup(entry->id);
  in->entry.commit = g_strdup(entry->commit);
  in->entry.path = g_strdup(entry->path);
  in->entry.tree = g_strdup(entry->tree);
  in->plugins_dir = g_strdup(plugins_dir);
  in->done = done;
  in->data = data;
  g_mkdir_with_parents(cache, 0700);
  in->work = g_build_filename(cache, "plugin-install-XXXXXX", NULL);
  g_free(cache);
  if (!g_mkdtemp(in->work)) {
    g_clear_pointer(&in->work, g_free);
    finish(in, "Could not make a folder in the cache to unpack it");
    return;
  }
  in->archive = g_build_filename(in->work, "plugin.tar.gz", NULL);
  url = registry_archive_url(entry);
  curl = g_subprocess_new(G_SUBPROCESS_FLAGS_STDERR_SILENCE, NULL, "curl", "-fsSL", "--max-time", DOWNLOAD_SECONDS,
                          "-o", in->archive, url, NULL);
  g_free(url);
  if (!curl) {
    finish(in, "Could not download the plugin: curl is not installed");
    return;
  }
  g_subprocess_wait_check_async(curl, NULL, on_downloaded, in);
  g_object_unref(curl);
}

/* plugins.json's "registry" ---------------------------------------------------------------------- */

static char *choices_file(void)
{
  return g_build_filename(g_get_user_config_dir(), TCODE_NAME, "plugins.json", NULL);
}

/* plugins.json's root object (an empty one when it is missing or broken), for json_node_unref. */
static JsonNode *load_choices(void)
{
  char *path = choices_file();
  JsonParser *parser = json_parser_new();
  JsonNode *root = NULL;

  if (json_parser_load_from_file(parser, path, NULL) && json_parser_get_root(parser) &&
      JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser)))
    root = json_node_copy(json_parser_get_root(parser));
  if (!root) {
    root = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(root, json_object_new());
  }
  g_object_unref(parser);
  g_free(path);
  return root;
}

static const char *const warned_names[] = { "install", "enable", "run" };

void registry_record_free(RegistryRecord *record)
{
  if (!record)
    return;
  g_free(record->repository);
  g_free(record->commit);
  g_free(record);
}

RegistryRecord *registry_record(const char *id)
{
  JsonNode *root = load_choices();
  JsonNode *registry = json_object_get_member(json_node_get_object(root), "registry");
  JsonNode *node = registry && JSON_NODE_HOLDS_OBJECT(registry) && id
                       ? json_object_get_member(json_node_get_object(registry), id)
                       : NULL;
  RegistryRecord *r = NULL;

  if (node && JSON_NODE_HOLDS_OBJECT(node)) {
    JsonObject *obj = json_node_get_object(node);
    JsonNode *reviewed = json_object_get_member(obj, "reviewed");
    char **warned = texts_of(obj, "warned");
    r = g_new0(RegistryRecord, 1);
    r->repository = g_strdup(text_of(obj, "repository") ?: "");
    r->commit = g_strdup(text_of(obj, "commit") ?: "");
    r->reviewed = reviewed && JSON_NODE_HOLDS_VALUE(reviewed) && json_node_get_value_type(reviewed) == G_TYPE_BOOLEAN &&
                  json_node_get_boolean(reviewed);
    for (guint i = 0; i < G_N_ELEMENTS(warned_names); i++)
      if (g_strv_contains((const char *const *)warned, warned_names[i]))
        r->warned |= 1 << i;
    g_strfreev(warned);
  }
  json_node_unref(root);
  return r;
}

void registry_set_record(const char *id, const RegistryRecord *record)
{
  JsonNode *root = load_choices();
  JsonObject *obj = json_node_get_object(root);
  JsonNode *registry = json_object_get_member(obj, "registry");
  char *path = choices_file(), *dir = g_path_get_dirname(path), *text;
  GError *error = NULL;

  if (!registry || !JSON_NODE_HOLDS_OBJECT(registry)) {
    json_object_set_object_member(obj, "registry", json_object_new());
    registry = json_object_get_member(obj, "registry");
  }
  if (!record) {
    json_object_remove_member(json_node_get_object(registry), id);
  } else {
    JsonObject *r = json_object_new();
    JsonArray *warned = json_array_new();
    json_object_set_string_member(r, "repository", record->repository);
    json_object_set_string_member(r, "commit", record->commit);
    json_object_set_boolean_member(r, "reviewed", record->reviewed);
    for (guint i = 0; i < G_N_ELEMENTS(warned_names); i++)
      if (record->warned & (1 << i))
        json_array_add_string_element(warned, warned_names[i]);
    json_object_set_array_member(r, "warned", warned);
    json_object_set_object_member(json_node_get_object(registry), id, r);
  }
  text = json_to_string(root, TRUE);
  g_mkdir_with_parents(dir, 0755);
  if (!g_file_set_contents(path, text, -1, &error)) {
    g_warning("t-code: cannot save %s: %s", path, error->message);
    g_clear_error(&error);
  }
  g_free(text);
  g_free(dir);
  g_free(path);
  json_node_unref(root);
}
