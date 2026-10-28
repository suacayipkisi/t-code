/*
 * GitHub's footnotes in the preview (see state.h), which md4c does not know: before parsing, the
 * definitions ([^label]: text, the lines indented under it included) are taken out of the text, each
 * reference ([^label]) becomes a superscript number linking to its note, and the notes are added at the
 * end, after a rule, numbered in the order they are first referred to, each linking back. Code blocks
 * and code spans are left alone. GLib only.
 */
#include "editor/preview/state.h"

#include <string.h>

typedef struct {
  char *label;
  GString *text;
  int number; /* 0: not referred to (not shown) */
} Note;

static void note_free(gpointer data)
{
  Note *note = data;

  g_free(note->label);
  g_string_free(note->text, TRUE);
  g_free(note);
}

/* A fence (``` or ~~~, up to 3 spaces in) that opens or closes a code block: its character and length. */
static gboolean is_fence(const char *line, char *mark, int *len)
{
  int indent = 0, n = 0;

  while (line[indent] == ' ' && indent < 4)
    indent++;
  if (indent > 3 || (line[indent] != '`' && line[indent] != '~'))
    return FALSE;
  while (line[indent + n] == line[indent])
    n++;
  if (n < 3)
    return FALSE;
  *mark = line[indent];
  *len = n;
  return TRUE;
}

/* [^label] at text: its label's length (the label starts at text + 2), or 0. */
static int reference_at(const char *text)
{
  int n = 0;

  if (text[0] != '[' || text[1] != '^')
    return 0;
  while (text[2 + n] && text[2 + n] != ']' && !g_ascii_isspace(text[2 + n]) && text[2 + n] != '[')
    n++;
  return text[2 + n] == ']' && n > 0 ? n : 0;
}

static Note *find_note(GPtrArray *notes, const char *label, gsize len)
{
  for (guint i = 0; i < notes->len; i++) {
    Note *note = g_ptr_array_index(notes, i);
    if (strlen(note->label) == len && g_ascii_strncasecmp(note->label, label, len) == 0)
      return note;
  }
  return NULL;
}

/* An anchor id from a label: what links and anchors can carry as they are. */
static char *anchor_id(const char *label)
{
  char *id = g_strdup(label);

  for (char *c = id; *c; c++)
    if (!g_ascii_isalnum(*c) && *c != '-' && *c != '_')
      *c = '-';
  return id;
}

/* A line outside code: its references replaced (code spans left alone). */
static void replace_references(GString *out, const char *line, GPtrArray *notes, int *count)
{
  for (const char *c = line; *c;) {
    if (*c == '`') { /* a code span: up to the same number of backticks */
      int ticks = 0;
      while (c[ticks] == '`')
        ticks++;
      char *run = g_strnfill(ticks, '`');
      const char *close = strstr(c + ticks, run);
      g_free(run);
      const char *end = close ? close + ticks : c + ticks;
      g_string_append_len(out, c, end - c);
      c = end;
      continue;
    }
    int len = c > line && c[-1] == '\\' ? 0 : reference_at(c);
    Note *note = len ? find_note(notes, c + 2, len) : NULL;
    if (!note) {
      g_string_append_c(out, *c++);
      continue;
    }
    char *id = anchor_id(note->label);
    if (!note->number)
      note->number = ++*count;
    g_string_append_printf(out, "<sup><a id=\"fnref-%s\" href=\"#fn-%s\">%d</a></sup>", id, id, note->number);
    g_free(id);
    c += len + 3;
  }
}

static int by_number(gconstpointer a, gconstpointer b)
{
  const Note *x = *(Note *const *)a, *y = *(Note *const *)b;

  return x->number - y->number;
}

char *preview_footnotes(const char *text)
{
  GPtrArray *notes = g_ptr_array_new_with_free_func(note_free);
  char **lines = g_strsplit(text, "\n", -1);
  GPtrArray *kept = g_ptr_array_new(); /* the lines that are not definitions, NULL: inside code */
  GString *out;
  char fence = 0, mark;
  int fence_len = 0, len, count = 0;
  Note *current = NULL;

  /* Definitions out, code blocks marked. */
  for (char **line = lines; *line; line++) {
    if (fence) {
      if (is_fence(*line, &mark, &len) && mark == fence && len >= fence_len)
        fence = 0;
      g_ptr_array_add(kept, *line);
      g_ptr_array_add(kept, NULL);
      continue;
    }
    const char *start = *line;
    while (*start == ' ' && start - *line < 3)
      start++;
    int label = reference_at(start);
    if (label && start[label + 3] == ':') {
      const char *body = start + label + 4;
      while (*body == ' ' || *body == '\t')
        body++;
      if (!find_note(notes, start + 2, label)) {
        current = g_new0(Note, 1);
        current->label = g_strndup(start + 2, label);
        current->text = g_string_new(body);
        g_ptr_array_add(notes, current);
      } else {
        current = NULL; /* a second definition of a label: the first counts */
      }
      continue;
    }
    if (current && (g_str_has_prefix(*line, "    ") || **line == '\t')) { /* the note goes on */
      g_string_append_c(current->text, '\n');
      g_string_append(current->text, *line + (**line == '\t' ? 1 : 4));
      continue;
    }
    if (current && !**line && line[1] && (g_str_has_prefix(line[1], "    ") || line[1][0] == '\t')) {
      g_string_append_c(current->text, '\n'); /* a blank line between paragraphs of one note */
      continue;
    }
    current = NULL;
    if (is_fence(*line, &mark, &len)) {
      fence = mark;
      fence_len = len;
      g_ptr_array_add(kept, *line);
      g_ptr_array_add(kept, NULL);
      continue;
    }
    g_ptr_array_add(kept, *line);
    g_ptr_array_add(kept, *line);
  }

  if (!notes->len) {
    g_ptr_array_free(kept, TRUE);
    g_strfreev(lines);
    g_ptr_array_free(notes, TRUE);
    return NULL;
  }
  out = g_string_new(NULL);
  for (guint i = 0; i < kept->len; i += 2) {
    const char *line = g_ptr_array_index(kept, i);
    if (g_ptr_array_index(kept, i + 1))
      replace_references(out, line, notes, &count);
    else
      g_string_append(out, line);
    g_string_append_c(out, '\n');
  }
  /* References inside notes are numbered too, after those in the text. */
  for (guint i = 0; i < notes->len; i++) {
    Note *note = g_ptr_array_index(notes, i);
    GString *text = g_string_new(NULL);
    replace_references(text, note->text->str, notes, &count);
    g_string_free(note->text, TRUE);
    note->text = text;
  }
  g_ptr_array_sort(notes, by_number);
  if (count) {
    g_string_append(out, "\n\n---\n\n");
    for (guint i = 0; i < notes->len; i++) {
      Note *note = g_ptr_array_index(notes, i);
      if (!note->number)
        continue;
      char *id = anchor_id(note->label), **parts = g_strsplit(note->text->str, "\n", -1);
      g_string_append_printf(out, "%d. <a id=\"fn-%s\"></a>", note->number, id);
      for (char **part = parts; *part; part++) {
        if (part != parts)
          g_string_append(out, "\n    ");
        g_string_append(out, *part);
      }
      g_string_append_printf(out, " [↩](#fnref-%s)\n", id);
      g_strfreev(parts);
      g_free(id);
    }
  }
  g_ptr_array_free(kept, TRUE);
  g_strfreev(lines);
  g_ptr_array_free(notes, TRUE);
  return g_string_free(out, FALSE);
}
