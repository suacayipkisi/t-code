/*
 * Finding t-code's data next to its executable (see paths.h).
 *   build tree   build/t-code, styles/, snippets/
 *   installed    bin/t-code, share/t-code/styles, share/t-code/snippets
 */
#include "window/paths.h"

#include <unistd.h>

/* Looked up next to the executable: the installed layout (bin/t-code, share/t-code/styles) or the
 * build tree (build/t-code, styles). candidates are paths relative to the executable's folder,
 * tried in order; the first one that exists wins. */
static char *find_beside_exe(const char *const *candidates)
{
  char *exe = g_file_read_link("/proc/self/exe", NULL);
  char *dir = exe ? g_path_get_dirname(exe) : g_get_current_dir();
  char *found = NULL;

  for (int i = 0; candidates[i] && !found; i++) {
    char *path = g_build_filename(dir, candidates[i], NULL);
    char *clean = g_canonicalize_filename(path, NULL);
    if (g_file_test(clean, G_FILE_TEST_EXISTS))
      found = g_strdup(clean);
    g_free(clean);
    g_free(path);
  }
  if (!found) /* loading from it fails and says so */
    found = g_build_filename(dir, candidates[0], NULL);
  g_free(dir);
  g_free(exe);
  return found;
}

void find_files(App *t)
{
  static const char *const styles[] = { "../share/t-code/styles", "../styles", NULL };
  static const char *const snippets[] = { "../share/t-code/snippets", "../snippets", NULL };
  /* Installed, the icon is in ~/.local/share/icons; from the build tree it is in desktop/. */
  static const char *const icons[] = { "../desktop", NULL };
  /* Old file versions for comparisons; the pid keeps two t-code windows apart. */
  char *versions = g_strdup_printf("t-code-%d-versions", getpid());

  t->paths.styles = find_beside_exe(styles);
  t->paths.snippets = find_beside_exe(snippets);
  t->paths.icons = find_beside_exe(icons);
  t->paths.temp = g_build_filename(g_get_user_runtime_dir(), versions, NULL);
  g_free(versions);
}
