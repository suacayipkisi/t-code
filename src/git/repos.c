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

GPtrArray *git_nested_repos(const char *top, const char *out, gsize size)
{
  GPtrArray *repos = g_ptr_array_new_with_free_func(g_free);

  for (const char *p = out, *end = out ? out + size : NULL; out && p < end;) {
    const char *nul = memchr(p, '\0', end - p);
    gsize len = (nul ? nul : end) - p;
    if (git_is_nested_record(p, len) || (len > 3 && p[0] == '!' && p[1] == ' ' && p[len - 1] == '/')) {
      char *relative = g_strndup(p + 2, len - 3);
      char *path = g_build_filename(top, relative, NULL), *git = g_build_filename(path, ".git", NULL);
      if (p[0] == '?' || g_file_test(git, G_FILE_TEST_EXISTS))
        g_ptr_array_add(repos, g_steal_pointer(&path));
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
  g_ptr_array_sort(repos, compare_paths);
  return repos;
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
