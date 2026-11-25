/*
 * The plugin registry (see registry.h). Downloads go through curl in a GSubprocess (like the update
 * check: no HTTP library), archives are unpacked by tar into a fresh folder in the cache, and nothing
 * lands in the plugins folder before its tree hash matches the pin.
 */
#define _DEFAULT_SOURCE /* lstat() under -std=c23 */
#include "plugins/registry.h"

#include "application/identity.h"
#include "plugins/ed25519.h"

#include <errno.h>
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
  if (entry->needs)
    g_ptr_array_unref(entry->needs);
  g_free(entry);
}

/* What a plugin needs ----------------------------------------------------------------------------- */

void plugin_need_free(PluginNeed *need)
{
  if (!need)
    return;
  g_strfreev(need->programs);
  g_free(need->why);
  g_strfreev(need->install);
  g_free(need);
}

/* A program's name: a word, without "/" or spaces. */
static gboolean good_program(const char *name)
{
  return name && *name && !strpbrk(name, "/ \t\n");
}

/* The string a node holds, or NULL. */
static const char *node_text(JsonNode *node)
{
  return node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING
             ? json_node_get_string(node)
             : NULL;
}

/* "program": one name or a list of them, as a NULL-terminated array; NULL when it is neither. */
static char **need_programs(JsonNode *program)
{
  GPtrArray *programs = g_ptr_array_new_with_free_func(g_free);
  gboolean good = TRUE;

  if (program && JSON_NODE_HOLDS_ARRAY(program)) {
    JsonArray *list = json_node_get_array(program);
    for (guint i = 0; good && i < json_array_get_length(list); i++) {
      const char *name = node_text(json_array_get_element(list, i));
      good = good_program(name);
      if (good)
        g_ptr_array_add(programs, g_strdup(name));
    }
  } else if (good_program(node_text(program))) {
    g_ptr_array_add(programs, g_strdup(node_text(program)));
  }
  if (!good || !programs->len) {
    g_ptr_array_unref(programs);
    return NULL;
  }
  g_ptr_array_add(programs, NULL);
  g_ptr_array_set_free_func(programs, NULL);
  return (char **)g_ptr_array_free(programs, FALSE);
}

/* One entry of "requires", or NULL with what is wrong in *problem. */
static PluginNeed *need_from(JsonNode *node, const char **problem)
{
  JsonObject *obj = JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
  JsonNode *install = obj ? json_object_get_member(obj, "install") : NULL;
  JsonNode *optional = obj ? json_object_get_member(obj, "optional") : NULL;
  char **programs = obj ? need_programs(json_object_get_member(obj, "program")) : NULL;
  GPtrArray *commands;
  PluginNeed *need;

  if (!programs) {
    *problem = "its \"program\" is not a program's name or a list of them";
    return NULL;
  }
  if (install && !JSON_NODE_HOLDS_OBJECT(install)) {
    *problem = "its \"install\" is not an object of commands by system";
    g_strfreev(programs);
    return NULL;
  }
  commands = g_ptr_array_new();
  if (install) {
    GList *members = json_object_get_members(json_node_get_object(install));
    for (GList *m = members; m; m = m->next) {
      const char *command = node_text(json_object_get_member(json_node_get_object(install), m->data));
      if (command && *command) {
        g_ptr_array_add(commands, g_strdup(m->data));
        g_ptr_array_add(commands, g_strdup(command));
      }
    }
    g_list_free(members);
  }
  g_ptr_array_add(commands, NULL);
  need = g_new0(PluginNeed, 1);
  need->programs = programs;
  need->install = (char **)g_ptr_array_free(commands, FALSE);
  need->why = g_strdup(node_text(json_object_get_member(obj, "for")) ?: "");
  need->optional = optional && JSON_NODE_HOLDS_VALUE(optional) &&
                   json_node_get_value_type(optional) == G_TYPE_BOOLEAN && json_node_get_boolean(optional);
  return need;
}

GPtrArray *plugin_needs_of(JsonObject *obj, const char *who)
{
  JsonNode *node = json_object_get_member(obj, "requires");
  GPtrArray *needs = g_ptr_array_new_with_free_func((GDestroyNotify)plugin_need_free);

  if (!node)
    return needs;
  if (!JSON_NODE_HOLDS_ARRAY(node)) {
    g_warning("Plugin %s: \"requires\" is not a list", who);
    return needs;
  }
  for (guint i = 0; i < json_array_get_length(json_node_get_array(node)); i++) {
    const char *problem = NULL;
    PluginNeed *need = need_from(json_array_get_element(json_node_get_array(node), i), &problem);
    if (need)
      g_ptr_array_add(needs, need);
    else
      g_warning("Plugin %s: \"requires\" entry %u left out: %s", who, i + 1, problem);
  }
  return needs;
}

/* The value of KEY=value in an os-release text, unquoted; NULL without one. */
static char *os_release_value(const char *text, const char *key)
{
  char **lines = g_strsplit(text ? text : "", "\n", -1);
  char *value = NULL;
  size_t length = strlen(key);

  for (int i = 0; lines[i] && !value; i++)
    if (strncmp(lines[i], key, length) == 0 && lines[i][length] == '=') {
      char *raw = g_strstrip(g_strdup(lines[i] + length + 1));
      size_t n = strlen(raw);
      if (n >= 2 && (raw[0] == '"' || raw[0] == '\'') && raw[n - 1] == raw[0]) {
        value = g_strndup(raw + 1, n - 2);
        g_free(raw);
      } else {
        value = raw;
      }
    }
  g_strfreev(lines);
  return value;
}

char **plugin_systems_from(const char *os_release)
{
  GPtrArray *systems = g_ptr_array_new();
  char *id = os_release_value(os_release, "ID"), *like = os_release_value(os_release, "ID_LIKE");
  char **words = g_strsplit(like ? like : "", " ", -1);

  if (id && *id)
    g_ptr_array_add(systems, g_ascii_strdown(id, -1));
  for (int i = 0; words[i]; i++)
    if (*words[i])
      g_ptr_array_add(systems, g_ascii_strdown(words[i], -1));
  g_ptr_array_add(systems, g_strdup("any"));
  g_ptr_array_add(systems, NULL);
  g_strfreev(words);
  g_free(like);
  g_free(id);
  return (char **)g_ptr_array_free(systems, FALSE);
}

char *const *plugin_systems(void)
{
  static char **systems;

  if (!systems) {
    char *text = NULL;
    if (!g_file_get_contents("/run/host/os-release", &text, NULL, NULL))
      g_file_get_contents("/etc/os-release", &text, NULL, NULL);
    systems = plugin_systems_from(text);
    g_free(text);
  }
  return systems;
}

const char *plugin_need_command(const PluginNeed *need, char *const *systems)
{
  for (int s = 0; systems && systems[s]; s++)
    for (int i = 0; need->install[i] && need->install[i + 1]; i += 2)
      if (strcmp(need->install[i], systems[s]) == 0)
        return need->install[i + 1];
  return NULL;
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
  e->needs = plugin_needs_of(obj, e->id ? e->id : "an entry");
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
    if ((good = g_file_get_contents(path, &contents, &length, error)) && g_stat(path, &info) != 0) {
      g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno), "cannot read %s", inside);
      good = FALSE;
    }
    if (good) {
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

gboolean registry_signed(const char *index, gsize length, const char *signature, const char *key)
{
  char *signature_text = g_strstrip(g_strdup(signature ? signature : "")), *key_text = g_strstrip(g_strdup(key));
  gsize signature_length = 0, key_length = 0;
  guint8 *signature_bytes = *signature_text ? g_base64_decode(signature_text, &signature_length) : NULL;
  guint8 *key_bytes = *key_text ? g_base64_decode(key_text, &key_length) : NULL;
  gboolean good = signature_length == 64 && key_length == 32 &&
                  ed25519_verify(signature_bytes, (const guint8 *)index, length, key_bytes);

  g_free(signature_bytes);
  g_free(key_bytes);
  g_free(signature_text);
  g_free(key_text);
  return good;
}

typedef struct {
  RegistryFetched done;
  gpointer data;
  char *where; /* the index's URL or file */
  char *fetch; /* curl's folder for the two files */
  char *kept;  /* the cache's copy of a checked index from where; its signature beside it */
} Fetch;

/* The entries of index when signature is good, else NULL and why. */
static GPtrArray *checked(const char *index, gsize length, const char *signature, char **why)
{
  const char *key = g_getenv("TCODE_REGISTRY_KEY");
  GError *error = NULL;
  GPtrArray *entries;

  if (!signature) {
    *why = g_strdup("the index came without its signature (index.json.sig)");
    return NULL;
  }
  if (!registry_signed(index, length, signature, key && *key ? key : REGISTRY_PUBLIC_KEY)) {
    *why = g_strdup("the index's signature does not match: it was changed on the way, or not signed again yet");
    return NULL;
  }
  entries = registry_parse(index, &error);
  if (!entries) {
    *why = g_strdup_printf("the index is broken: %s", error->message);
    g_error_free(error);
  }
  return entries;
}

static char *read_text(const char *path, gsize *length)
{
  char *text = NULL;

  return path && g_file_get_contents(path, &text, length, NULL) ? text : NULL;
}

/* index (NULL: could not be had, failure says why) and its signature are in: done gets them, or the kept
 * copy with a note, or what went wrong. */
static void fetched(Fetch *f, const char *index, gsize length, const char *signature, const char *failure)
{
  char *why = NULL, *message = NULL, *kept_signature_path = g_strconcat(f->kept, ".sig", NULL);
  GPtrArray *entries = index ? checked(index, length, signature, &why) : NULL;

  if (entries) {
    char *dir = g_path_get_dirname(f->kept);
    g_mkdir_with_parents(dir, 0700);
    g_file_set_contents(f->kept, index, length, NULL);
    g_file_set_contents(kept_signature_path, signature, -1, NULL);
    g_free(dir);
  } else {
    gsize kept_length = 0;
    char *kept = read_text(f->kept, &kept_length), *kept_signature = read_text(kept_signature_path, NULL),
         *ignored = NULL;
    GStatBuf st;
    entries = kept ? checked(kept, kept_length, kept_signature, &ignored) : NULL;
    if (entries && g_stat(f->kept, &st) == 0) {
      GDateTime *when = g_date_time_new_from_unix_local(st.st_mtime);
      char *date = g_date_time_format(when, "%Y-%m-%d %H:%M");
      message = g_strdup_printf("Showing the registry as of %s: %s", date, why ? why : failure);
      g_free(date);
      g_date_time_unref(when);
    } else if (why) {
      message = g_strdup_printf("Not trusting the registry: %s", why);
    } else {
      message = g_strdup(failure);
    }
    g_free(ignored);
    g_free(kept_signature);
    g_free(kept);
  }
  f->done(entries, message, f->data);
  g_free(message);
  g_free(why);
  g_free(kept_signature_path);
  g_free(f->where);
  g_free(f->kept);
  g_free(f->fetch);
  g_free(f);
}

static void on_index(GObject *source, GAsyncResult *result, gpointer data)
{
  Fetch *f = data;
  char *index_path = g_build_filename(f->fetch, "index.json", NULL),
       *signature_path = g_strconcat(index_path, ".sig", NULL);
  gsize length = 0;
  char *index, *signature;

  g_subprocess_wait_finish(G_SUBPROCESS(source), result, NULL); /* one of the two may have failed: see below */
  index = read_text(index_path, &length);
  signature = read_text(signature_path, NULL);
  g_unlink(index_path);
  g_unlink(signature_path);
  g_rmdir(f->fetch);
  fetched(f, index && length ? index : NULL, length, signature,
          "Could not reach the registry: offline, or GitHub is having a moment");
  g_free(signature);
  g_free(index);
  g_free(signature_path);
  g_free(index_path);
}

void registry_fetch(RegistryFetched done, gpointer data)
{
  const char *where = g_getenv("TCODE_REGISTRY");
  Fetch *f = g_new0(Fetch, 1);
  GSubprocess *process;
  char *hash, *name, *index_path, *signature_path, *signature_url;

  f->done = done;
  f->data = data;
  f->where = g_strdup(where && *where ? where : REGISTRY_INDEX_URL);
  hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, f->where, -1);
  name = g_strdup_printf("index-%.16s.json", hash); /* one per registry */
  f->kept = g_build_filename(g_get_user_cache_dir(), TCODE_NAME, "registry", name, NULL);
  g_free(name);
  g_free(hash);
  if (g_path_is_absolute(f->where)) { /* a file: the tests', a local copy */
    char *signature_file = g_strconcat(f->where, ".sig", NULL);
    gsize length = 0;
    char *index = read_text(f->where, &length), *signature = read_text(signature_file, NULL);
    fetched(f, index, length, signature, "Could not read the registry's index file");
    g_free(signature);
    g_free(index);
    g_free(signature_file);
    return;
  }
  name = g_build_filename(g_get_user_cache_dir(), TCODE_NAME, NULL);
  g_mkdir_with_parents(name, 0700);
  f->fetch = g_build_filename(name, "registry-fetch-XXXXXX", NULL);
  g_free(name);
  if (!g_mkdtemp(f->fetch)) {
    fetched(f, NULL, 0, NULL, "Could not reach the registry: no room in the cache folder");
    return;
  }
  index_path = g_build_filename(f->fetch, "index.json", NULL);
  signature_path = g_strconcat(index_path, ".sig", NULL);
  signature_url = g_strconcat(f->where, ".sig", NULL);
  process = g_subprocess_new(G_SUBPROCESS_FLAGS_STDERR_SILENCE, NULL, "curl", "-fsSL", "--max-time", FETCH_SECONDS,
                             f->where, "-o", index_path, signature_url, "-o", signature_path, NULL);
  g_free(signature_url);
  g_free(signature_path);
  g_free(index_path);
  if (!process) {
    g_rmdir(f->fetch);
    fetched(f, NULL, 0, NULL, "Could not reach the registry: curl is not installed");
    return;
  }
  g_subprocess_wait_async(process, NULL, on_index, f);
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
    /* --no-same-owner / -permissions: the user's own files; -m: the files dated now, not at their commit
     * (a build that goes by times, like cargo's, sees an update as new); GNU tar refuses ".." and leading "/" */
    tar = g_subprocess_new(G_SUBPROCESS_FLAGS_STDERR_SILENCE, NULL, "tar", "-xzmf", in->archive, "-C", unpacked,
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
