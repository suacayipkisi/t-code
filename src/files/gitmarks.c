/* The file tree's git colors, GLib-only: see gitmarks.h. */
#define _DEFAULT_SOURCE /* strnlen under -std=c23 */
#include "files/gitmarks.h"

#include <string.h>

static char state_of(char x, char y)
{
  if (x == '?')
    return 'U';
  if (x == '!')
    return 'I';
  if (x == 'U' || y == 'U' || (x == 'A' && y == 'A') || (x == 'D' && y == 'D'))
    return 'C';
  if (x == 'D' || y == 'D')
    return 'D';
  if (x == 'A' || y == 'A') /* y: git add -N, or an hg / svn add, which has no index */
    return 'A';
  return 'M';
}

static int rank(char state)
{
  switch (state) {
    case 'C': return 5;
    case 'M':
    case 'D': return 4;
    case 'A':
    case 'U': return 3;
    case 'I': return 1;
    default: return 0;
  }
}

/* Raises path's state to state when that is stronger. */
static void raise_state(GHashTable *marks, const char *path, char state)
{
  char old = (char)GPOINTER_TO_INT(g_hash_table_lookup(marks, path));

  if (rank(state) > rank(old))
    g_hash_table_insert(marks, g_strdup(path), GINT_TO_POINTER(state));
}

GHashTable *git_marks_parse(const char *top, const char *out, gsize len)
{
  GHashTable *marks = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  size_t top_len = strlen(top);

  for (gsize i = 0; out && i + 3 < len;) {
    const char *entry = out + i;
    char x = entry[0], y = entry[1];
    i += strnlen(entry, len - i) + 1;
    if (x == 'R' || x == 'C') /* the original name of a rename or copy follows */
      i += strnlen(out + i, len > i ? len - i : 0) + 1;

    char *path = g_canonicalize_filename(entry + 3, top); /* without an ignored folder's "/" */
    char state = state_of(x, y);
    raise_state(marks, path, state);
    if (state != 'I') { /* the folders up to the top show it too */
      char folder_state = state == 'D' ? 'M' : state;
      for (char *slash = strrchr(path, '/'); slash && (size_t)(slash - path) > top_len; slash = strrchr(path, '/')) {
        *slash = '\0';
        raise_state(marks, path, folder_state);
      }
    }
    g_free(path);
  }
  return marks;
}

char git_marks_state(GHashTable *marks, const char *path)
{
  char state, *up;

  if (!marks)
    return 0;
  state = (char)GPOINTER_TO_INT(g_hash_table_lookup(marks, path));
  if (state)
    return state;
  up = g_strdup(path);
  for (char *slash = strrchr(up, '/'); slash && slash != up; slash = strrchr(up, '/')) {
    *slash = '\0';
    if ((char)GPOINTER_TO_INT(g_hash_table_lookup(marks, up)) == 'I') {
      state = 'I';
      break;
    }
  }
  g_free(up);
  return state;
}

const char *git_marks_color(char state)
{
  switch (state) {
    case 'M': return "#e2c08d";
    case 'A': return "#81b88b";
    case 'U': return "#73c991";
    case 'D': return "#c74e39";
    case 'C': return "#e4676b";
    case 'I': return "#8c8c8c";
    default: return "#cccccc";
  }
}

const char *git_marks_badge(char state)
{
  switch (state) {
    case 'M': return "M";
    case 'A': return "A";
    case 'U': return "U";
    case 'D': return "D";
    case 'C': return "!";
    default: return NULL;
  }
}
