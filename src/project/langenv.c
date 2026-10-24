/*
 * A project's language environment (see langenv.h): Python venvs, Cargo features, and the
 * options and flags the chosen ones give.
 */
#include "project/langenv.h"

#include <json-glib/json-glib.h>
#include <string.h>

/* .venv first, then venv, then the rest by name. */
static int venv_order(gconstpointer a, gconstpointer b)
{
  const char *x = *(const char *const *)a, *y = *(const char *const *)b;
  int rank_x = strcmp(x, ".venv") == 0 ? 0 : strcmp(x, "venv") == 0 ? 1 : 2;
  int rank_y = strcmp(y, ".venv") == 0 ? 0 : strcmp(y, "venv") == 0 ? 1 : 2;

  return rank_x != rank_y ? rank_x - rank_y : strcmp(x, y);
}

/* dir's bin/python when dir is a venv, else NULL. */
static char *venv_in(const char *dir)
{
  char *config = g_build_filename(dir, "pyvenv.cfg", NULL), *python = g_build_filename(dir, "bin", "python", NULL);
  gboolean venv = g_file_test(config, G_FILE_TEST_IS_REGULAR) && g_file_test(python, G_FILE_TEST_IS_EXECUTABLE);

  g_free(config);
  if (!venv)
    g_clear_pointer(&python, g_free);
  return python;
}

/* Adds path to list unless it is there already (took it). */
static void add_once(GPtrArray *list, char *path)
{
  for (guint i = 0; path && i < list->len; i++)
    if (strcmp(g_ptr_array_index(list, i), path) == 0) {
      g_free(path);
      return;
    }
  if (path)
    g_ptr_array_add(list, path);
}

/* Adds the venvs right in root to list. */
static void add_venvs(GPtrArray *list, const char *root)
{
  GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
  GDir *dir = root ? g_dir_open(root, 0, NULL) : NULL;
  const char *name;

  while (dir && (name = g_dir_read_name(dir)))
    g_ptr_array_add(names, g_strdup(name));
  if (dir)
    g_dir_close(dir);
  g_ptr_array_sort(names, venv_order);
  for (guint i = 0; i < names->len; i++) {
    char *path = g_build_filename(root, g_ptr_array_index(names, i), NULL);
    add_once(list, venv_in(path));
    g_free(path);
  }
  g_ptr_array_unref(names);
}

GPtrArray *langenv_interpreters(const char *root)
{
  GPtrArray *list = g_ptr_array_new_with_free_func(g_free);
  const char *active = g_getenv("VIRTUAL_ENV");

  add_venvs(list, root);
  if (active && *active)
    add_once(list, venv_in(active));
  add_once(list, g_find_program_in_path("python3"));
  return list;
}

char *langenv_interpreter_name(const char *python)
{
  char *bin = g_path_get_dirname(python), *env = g_path_get_dirname(bin);
  char *config = g_build_filename(env, "pyvenv.cfg", NULL), *name;

  if (g_file_test(config, G_FILE_TEST_IS_REGULAR)) {
    char *base = g_path_get_basename(env);
    name = g_strdup_printf("python (%s)", base);
    g_free(base);
  } else {
    name = g_path_get_basename(python);
  }
  g_free(config);
  g_free(env);
  g_free(bin);
  return name;
}

char *langenv_venv_python(const char *root)
{
  GPtrArray *list = g_ptr_array_new_with_free_func(g_free);
  char *python = NULL;

  add_venvs(list, root);
  if (list->len)
    python = g_strdup(g_ptr_array_index(list, 0));
  g_ptr_array_unref(list);
  return python;
}

GStrv langenv_cargo_features(const char *cargo_toml)
{
  char *text = NULL;
  GStrvBuilder *features;
  gboolean inside = FALSE;

  if (!g_file_get_contents(cargo_toml, &text, NULL, NULL))
    return NULL;
  features = g_strv_builder_new();
  char **lines = g_strsplit(text, "\n", -1);
  for (int i = 0; lines[i]; i++) {
    char *line = g_strstrip(lines[i]), *equals;
    if (*line == '[') {
      inside = g_str_has_prefix(line, "[features]");
      continue;
    }
    if (!inside || !*line || *line == '#' || !(equals = strchr(line, '=')))
      continue;
    *equals = '\0';
    char *key = g_strstrip(line);
    size_t len = strlen(key);
    if (len >= 2 && (*key == '"' || *key == '\'') && key[len - 1] == *key) { /* a quoted key */
      key[len - 1] = '\0';
      key++;
    }
    if (*key && strcmp(key, "default") != 0)
      g_strv_builder_add(features, key);
  }
  g_strfreev(lines);
  g_free(text);
  GStrv result = g_strv_builder_end(features);
  g_strv_builder_unref(features);
  return result;
}

char *langenv_rust_options(const char *features, const char *target)
{
  gboolean some = features && *features, all = g_strcmp0(features, "all") == 0, triple = target && *target;
  JsonBuilder *builder;
  JsonGenerator *generator;
  JsonNode *root;
  char *json;

  if (!some && !triple)
    return NULL;
  builder = json_builder_new();
  json_builder_begin_object(builder);
  json_builder_set_member_name(builder, "cargo");
  json_builder_begin_object(builder);
  if (some) {
    json_builder_set_member_name(builder, "features");
    if (all) {
      json_builder_add_string_value(builder, "all");
    } else {
      char **names = g_strsplit(features, ",", -1);
      json_builder_begin_array(builder);
      for (int i = 0; names[i]; i++)
        if (*g_strstrip(names[i]))
          json_builder_add_string_value(builder, names[i]);
      json_builder_end_array(builder);
      g_strfreev(names);
    }
  }
  if (triple) {
    json_builder_set_member_name(builder, "target");
    json_builder_add_string_value(builder, target);
  }
  json_builder_end_object(builder);
  json_builder_end_object(builder);
  root = json_builder_get_root(builder);
  generator = json_generator_new();
  json_generator_set_root(generator, root);
  json = json_generator_to_data(generator, NULL);
  g_object_unref(generator);
  json_node_unref(root);
  g_object_unref(builder);
  return json;
}

char *langenv_cargo_flags(const char *features, const char *target)
{
  GString *flags = g_string_new(NULL);

  if (g_strcmp0(features, "all") == 0) {
    g_string_append(flags, " --all-features");
  } else if (features && *features) {
    char *quoted = g_shell_quote(features);
    g_string_append_printf(flags, " --features %s", quoted);
    g_free(quoted);
  }
  if (target && *target) {
    char *quoted = g_shell_quote(target);
    g_string_append_printf(flags, " --target %s", quoted);
    g_free(quoted);
  }
  return g_string_free(flags, FALSE);
}
