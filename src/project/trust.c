/*
 * The trusted folders' list (see trust.h).
 */
#define _DEFAULT_SOURCE /* realpath on musl */
#include "project/trust.h"

#include "application/identity.h"

#include <errno.h>
#include <glib/gstdio.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

static char *list_path(void)
{
  return g_build_filename(g_get_user_config_dir(), TCODE_NAME, "trusted", NULL);
}

/* folder with its links resolved (a link into a trusted folder is that folder), or only tidied when
 * it does not exist. */
static char *real_folder(const char *folder)
{
  char resolved[PATH_MAX];

  return realpath(folder, resolved) ? g_strdup(resolved) : g_canonicalize_filename(folder, NULL);
}

/* Whether dir is top or a folder inside it. */
static gboolean inside(const char *dir, const char *top)
{
  size_t n = strlen(top);

  while (n > 1 && top[n - 1] == '/')
    n--;
  return strncmp(dir, top, n) == 0 && (dir[n] == '\0' || dir[n] == '/' || (n == 1 && *top == '/'));
}

gboolean trust_has(const char *folder)
{
  char *path, *text = NULL, *real;
  gboolean found = FALSE;

  if (!folder || !*folder)
    return FALSE;
  path = list_path();
  if (g_file_get_contents(path, &text, NULL, NULL)) {
    real = real_folder(folder);
    char **lines = g_strsplit(text, "\n", -1);
    for (int i = 0; lines[i] && !found; i++) {
      char *line = g_strstrip(lines[i]);
      found = *line == '/' && inside(real, line);
    }
    g_strfreev(lines);
    g_free(real);
  }
  g_free(text);
  g_free(path);
  return found;
}

gboolean trust_add(const char *folder, GError **error)
{
  char *path, *dir, *text = NULL, *real;
  gboolean ok = TRUE;

  g_return_val_if_fail(folder && *folder, FALSE);
  if (trust_has(folder))
    return TRUE;
  path = list_path();
  dir = g_path_get_dirname(path);
  real = real_folder(folder);
  if (strchr(real, '\n')) { /* one folder per line */
    g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL, "a folder name with a line break cannot be trusted");
    ok = FALSE;
  } else if (g_mkdir_with_parents(dir, 0700) != 0) {
    int saved = errno;
    g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(saved), "%s: %s", dir, g_strerror(saved));
    ok = FALSE;
  } else {
    gsize length = 0;
    g_file_get_contents(path, &text, &length, NULL); /* none yet: a new list */
    GString *list = g_string_new_len(text ? text : "", text ? (gssize)length : 0);
    if (list->len && list->str[list->len - 1] != '\n')
      g_string_append_c(list, '\n');
    g_string_append_printf(list, "%s\n", real);
    ok = g_file_set_contents_full(path, list->str, (gssize)list->len, G_FILE_SET_CONTENTS_CONSISTENT, 0600, error);
    g_string_free(list, TRUE);
  }
  g_free(text);
  g_free(real);
  g_free(dir);
  g_free(path);
  return ok;
}
