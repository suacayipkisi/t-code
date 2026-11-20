/*
 * Finding the git repositories inside a project (see repos.h).
 */
#include "git/repos.h"

#include "git/vcs.h"

#include <string.h>

static int compare_paths(gconstpointer a, gconstpointer b)
{
  return strcmp(*(const char *const *)a, *(const char *const *)b);
}

gboolean git_is_nested_record(const char *record, gsize len)
{
  return len > 3 && record[0] == '?' && record[1] == ' ' && record[len - 1] == '/';
}

/* Where an untracked file's path (relative) is inside an hg or svn repository's mark: that
 * repository's top ("sub" for "sub/.hg/store/data"), else NULL. */
static char *mark_top(const char *path, gsize len)
{
  static const char *const marks[] = { "/.hg/", "/.svn/" };

  for (int i = 0; i < 2; i++) {
    const char *at = g_strstr_len(path, len, marks[i]);
    if (at)
      return g_strndup(path, at - path);
  }
  return NULL;
}

GPtrArray *git_nested_repos(const char *top, const char *out, gsize size)
{
  GPtrArray *repos = g_ptr_array_new_with_free_func(g_free);
  GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);

  for (const char *p = out, *end = out ? out + size : NULL; out && p < end;) {
    const char *nul = memchr(p, '\0', end - p);
    gsize len = (nul ? nul : end) - p;
    char *inner = len > 2 && p[0] == '?' && p[1] == ' ' ? mark_top(p + 2, len - 2) : NULL;
    if (inner) { /* git lists an hg or svn repository's files one by one, its mark's too */
      char *path = g_build_filename(top, inner, NULL);
      if (!g_hash_table_contains(seen, path)) {
        g_hash_table_add(seen, path);
        g_ptr_array_add(repos, path);
      } else {
        g_free(path);
      }
      g_free(inner);
    } else if (git_is_nested_record(p, len) || (len > 3 && p[0] == '!' && p[1] == ' ' && p[len - 1] == '/')) {
      char *relative = g_strndup(p + 2, len - 3);
      char *path = g_build_filename(top, relative, NULL), *git = g_build_filename(path, ".git", NULL);
      if ((p[0] == '?' || g_file_test(git, G_FILE_TEST_EXISTS) || vcs_mark_in(path)) &&
          !g_hash_table_contains(seen, path)) {
        g_hash_table_add(seen, path);
        g_ptr_array_add(repos, g_steal_pointer(&path));
      }
      g_free(git);
      g_free(path);
      g_free(relative);
    }
    /* A rename's record is followed by its old path: not a record of its own. */
    if (len && *p == '2' && nul && nul + 1 < end) {
      const char *old = memchr(nul + 1, '\0', end - nul - 1);
      p = old ? old + 1 : end;
      continue;
    }
    p += len + 1;
  }
  g_hash_table_unref(seen);
  g_ptr_array_sort(repos, compare_paths);
  return repos;
}

GBytes *git_status_without(GBytes *out, const char *top, const GPtrArray *inner)
{
  gsize size = 0, top_len = strlen(top);
  const char *data = out ? g_bytes_get_data(out, &size) : NULL;
  GPtrArray *prefixes = g_ptr_array_new_with_free_func(g_free);

  for (guint i = 0; inner && i < inner->len; i++) {
    const char *path = inner->pdata[i];
    if (strncmp(path, top, top_len) == 0 && path[top_len] == '/')
      g_ptr_array_add(prefixes, g_strconcat(path + top_len + 1, "/", NULL));
  }
  if (!data || prefixes->len == 0) {
    g_ptr_array_unref(prefixes);
    return out ? g_bytes_ref(out) : NULL;
  }
  GByteArray *kept = g_byte_array_sized_new(size);
  for (const char *p = data, *end = data + size; p < end;) {
    const char *nul = memchr(p, '\0', end - p);
    const char *next = nul ? nul + 1 : end;
    gsize len = (nul ? nul : end) - p;
    /* v2 "? path" / "! path", v1 "?? path" / "!! path": an untracked or ignored file */
    gboolean loose = len > 2 && (p[0] == '?' || p[0] == '!');
    const char *path = !loose ? NULL : p[1] == ' ' ? p + 2 : len > 3 && p[2] == ' ' ? p + 3 : NULL;
    gboolean drop = FALSE;
    for (guint i = 0; path && i < prefixes->len && !drop; i++)
      drop = g_str_has_prefix(path, prefixes->pdata[i]);
    /* A rename's or copy's old path follows its record (v2 "2 ...", v1 "R  " / " R " / "C  "...). */
    gboolean v1_moved = len > 3 && p[2] == ' ' && (strchr("RC", p[0]) || strchr("RC", p[1]));
    if (len && (p[0] == '2' || v1_moved) && next < end) {
      const char *old = memchr(next, '\0', end - next);
      next = old ? old + 1 : end;
    }
    if (!drop)
      g_byte_array_append(kept, (const guint8 *)p, next - p);
    p = next;
  }
  g_ptr_array_unref(prefixes);
  return g_byte_array_free_to_bytes(kept);
}

static void find_in(const char *dir, int depth, guint max, GPtrArray *repos)
{
  GDir *handle = g_dir_open(dir, 0, NULL);
  const char *name;

  if (!handle)
    return;
  while (repos->len < max && (name = g_dir_read_name(handle))) {
    if (name[0] == '.' || strcmp(name, "node_modules") == 0)
      continue;
    char *path = g_build_filename(dir, name, NULL);
    if (g_file_test(path, G_FILE_TEST_IS_DIR) && !g_file_test(path, G_FILE_TEST_IS_SYMLINK)) {
      char *git = g_build_filename(path, ".git", NULL);
      if (g_file_test(git, G_FILE_TEST_EXISTS) || vcs_mark_in(path))
        g_ptr_array_add(repos, g_steal_pointer(&path));
      else if (depth > 1)
        find_in(path, depth - 1, max, repos);
      g_free(git);
    }
    g_free(path);
  }
  g_dir_close(handle);
}

GPtrArray *git_find_repos(const char *root, int depth, guint max)
{
  GPtrArray *repos = g_ptr_array_new_with_free_func(g_free);

  if (root && depth > 0)
    find_in(root, depth, max, repos);
  g_ptr_array_sort(repos, compare_paths);
  return repos;
}
