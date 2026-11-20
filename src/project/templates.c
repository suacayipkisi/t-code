/*
 * File templates (see templates.h): a folder per template, named after it (lower case, "-" between
 * words), holding the file and template.ini.
 */
#include "project/templates.h"

#include "application/identity.h"

#include <errno.h>
#include <glib/gstdio.h>
#include <string.h>

#define INFO_NAME "template.ini"
#define GROUP "template"

void template_free(Template *tpl)
{
  if (!tpl)
    return;
  g_free(tpl->dir);
  g_free(tpl->name);
  g_free(tpl->description);
  g_free(tpl->file);
  g_free(tpl);
}

char *templates_dir(void)
{
  return g_build_filename(g_get_user_config_dir(), TCODE_NAME, "templates", NULL);
}

/* The template in dir, or NULL when its template.ini is missing or has no name or file. */
static Template *template_read(const char *dir)
{
  char *info = g_build_filename(dir, INFO_NAME, NULL);
  GKeyFile *kf = g_key_file_new();
  Template *tpl = NULL;

  if (g_key_file_load_from_file(kf, info, G_KEY_FILE_NONE, NULL)) {
    char *name = g_key_file_get_string(kf, GROUP, "name", NULL);
    char *file = g_key_file_get_string(kf, GROUP, "file", NULL);
    char *description = g_key_file_get_string(kf, GROUP, "description", NULL);
    if (name && *name && file && *file && !strchr(file, '/')) {
      tpl = g_new0(Template, 1);
      tpl->dir = g_strdup(dir);
      tpl->name = g_steal_pointer(&name);
      tpl->file = g_steal_pointer(&file);
      tpl->description = description ? g_steal_pointer(&description) : g_strdup("");
    }
    g_free(name);
    g_free(file);
    g_free(description);
  }
  g_key_file_free(kf);
  g_free(info);
  return tpl;
}

static int by_name(gconstpointer a, gconstpointer b)
{
  const Template *x = *(Template *const *)a, *y = *(Template *const *)b;
  char *fx = g_utf8_casefold(x->name, -1), *fy = g_utf8_casefold(y->name, -1);
  int order = g_utf8_collate(fx, fy);

  g_free(fx);
  g_free(fy);
  return order;
}

GPtrArray *templates_list(const char *root)
{
  GPtrArray *list = g_ptr_array_new_with_free_func((GDestroyNotify)template_free);
  GDir *dir = g_dir_open(root, 0, NULL);
  const char *entry;

  while (dir && (entry = g_dir_read_name(dir))) {
    char *path = g_build_filename(root, entry, NULL);
    Template *tpl = g_file_test(path, G_FILE_TEST_IS_DIR) ? template_read(path) : NULL;
    if (tpl)
      g_ptr_array_add(list, tpl);
    g_free(path);
  }
  if (dir)
    g_dir_close(dir);
  g_ptr_array_sort(list, by_name);
  return list;
}

/* name's extension without the dot ("" when it has none; a leading dot is not one). */
static const char *extension_of(const char *name)
{
  const char *dot = strrchr(name, '.');

  return dot && dot != name ? dot + 1 : "";
}

gboolean template_matches(const Template *tpl, const char *file_name)
{
  char *name = g_path_get_basename(file_name);
  const char *mine = extension_of(tpl->file), *theirs = extension_of(name);
  gboolean same = *mine || *theirs ? g_ascii_strcasecmp(mine, theirs) == 0 : g_strcmp0(tpl->file, name) == 0;

  g_free(name);
  return same;
}

/* A folder name for name: lower case letters and digits, "-" for the rest ("C main" → "c-main"). */
static char *folder_name(const char *name)
{
  GString *slug = g_string_new(NULL);

  for (const char *p = name; *p; p++) {
    if (g_ascii_isalnum(*p))
      g_string_append_c(slug, g_ascii_tolower(*p));
    else if (slug->len && slug->str[slug->len - 1] != '-')
      g_string_append_c(slug, '-');
  }
  while (slug->len && slug->str[slug->len - 1] == '-')
    g_string_truncate(slug, slug->len - 1);
  if (!slug->len)
    g_string_append(slug, "template");
  return g_string_free(slug, FALSE);
}

/* The template already called name (any case) in root, or NULL. */
static Template *find_named(const char *root, const char *name)
{
  GPtrArray *list = templates_list(root);
  char *wanted = g_utf8_casefold(name, -1);
  Template *found = NULL;

  for (guint i = 0; i < list->len && !found; i++) {
    Template *tpl = list->pdata[i];
    char *have = g_utf8_casefold(tpl->name, -1);
    if (strcmp(have, wanted) == 0)
      found = g_ptr_array_steal_index(list, i);
    g_free(have);
  }
  g_free(wanted);
  g_ptr_array_unref(list);
  return found;
}

/* A new folder in root for name: its folder name, then "-2", "-3", ... when taken. */
static char *new_folder(const char *root, const char *name)
{
  char *slug = folder_name(name), *path = g_build_filename(root, slug, NULL);

  for (int n = 2; g_file_test(path, G_FILE_TEST_EXISTS); n++) {
    char *numbered = g_strdup_printf("%s-%d", slug, n);
    g_free(path);
    path = g_build_filename(root, numbered, NULL);
    g_free(numbered);
  }
  g_free(slug);
  return path;
}

gboolean templates_save(const char *root, const char *name, const char *description, const char *file_name,
                        const char *text, gssize length, GError **error)
{
  char *title = g_strstrip(g_strdup(name ? name : "")), *file = g_path_get_basename(file_name);
  Template *old = NULL;
  char *dir = NULL, *path = NULL, *info = NULL, *data = NULL;
  GKeyFile *kf = NULL;
  gboolean ok = FALSE;

  if (!*title) {
    g_set_error_literal(error, G_FILE_ERROR, G_FILE_ERROR_INVAL, "The template needs a name");
    goto out;
  }
  if (strcmp(file, INFO_NAME) == 0 || strcmp(file, ".") == 0 || strcmp(file, G_DIR_SEPARATOR_S) == 0) {
    g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL, "A file named \"%s\" cannot be a template", file);
    goto out;
  }
  old = find_named(root, title);
  dir = old ? g_strdup(old->dir) : new_folder(root, title);
  if (g_mkdir_with_parents(dir, 0755) != 0) {
    int code = errno;
    g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(code), "Could not make %s: %s", dir, g_strerror(code));
    goto out;
  }
  path = g_build_filename(dir, file, NULL);
  if (!g_file_set_contents(path, text ? text : "", text ? length : 0, error))
    goto out;
  if (old && strcmp(old->file, file) != 0) { /* the file it replaces had another name */
    char *previous = g_build_filename(dir, old->file, NULL);
    g_unlink(previous);
    g_free(previous);
  }
  kf = g_key_file_new();
  g_key_file_set_string(kf, GROUP, "name", title);
  g_key_file_set_string(kf, GROUP, "description", description ? description : "");
  g_key_file_set_string(kf, GROUP, "file", file);
  data = g_key_file_to_data(kf, NULL, NULL);
  info = g_build_filename(dir, INFO_NAME, NULL);
  ok = g_file_set_contents(info, data, -1, error);
out:
  if (kf)
    g_key_file_free(kf);
  template_free(old);
  g_free(data);
  g_free(info);
  g_free(path);
  g_free(dir);
  g_free(file);
  g_free(title);
  return ok;
}

gboolean template_delete(const Template *tpl, GError **error)
{
  GDir *dir = g_dir_open(tpl->dir, 0, error);
  const char *entry;

  if (!dir)
    return FALSE;
  while ((entry = g_dir_read_name(dir))) {
    char *path = g_build_filename(tpl->dir, entry, NULL);
    g_unlink(path);
    g_free(path);
  }
  g_dir_close(dir);
  if (g_rmdir(tpl->dir) != 0) {
    int code = errno;
    g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(code), "Could not remove %s: %s", tpl->dir,
                g_strerror(code));
    return FALSE;
  }
  return TRUE;
}

char *template_contents(const Template *tpl, gsize *length, GError **error)
{
  char *path = g_build_filename(tpl->dir, tpl->file, NULL), *text = NULL;

  if (!g_file_get_contents(path, &text, length, error))
    text = NULL;
  g_free(path);
  return text;
}
