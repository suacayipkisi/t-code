/*
 * The { } blocks of a text (see blocks.h).
 */
#include "editor/text/blocks.h"

#include <string.h>

/* Whether the line from start (up to its line break) is blank. */
static gboolean blank_line(const char *start)
{
  for (; *start && *start != '\n'; start++)
    if (!g_ascii_isspace(*start))
      return FALSE;
  return TRUE;
}

/* The line before the one starting at line_start names the block a '{' alone on its line opens:
 * unless it is blank or ends a statement or a block ("x;" "}" "{"). */
static gboolean names_block(const char *text, const char *line_start)
{
  const char *end = line_start - 1, *p; /* its line break */

  if (line_start == text)
    return FALSE;
  for (p = end; p > text && p[-1] != '\n'; p--)
    ;
  if (blank_line(p))
    return FALSE;
  while (end > p && g_ascii_isspace(end[-1]))
    end--;
  return end > p && !strchr(";{}", end[-1]);
}

const char *code_next(CodeScan *scan)
{
  const char *p;

  for (p = scan->p; *p; p++) {
    if (*p == '\n') {
      scan->line++;
      scan->line_start = p + 1;
    } else if (scan->comment) {
      if (p[0] == '*' && p[1] == '/') {
        scan->comment = FALSE;
        p++;
      }
    } else if (p[0] == '/' && p[1] == '/') {
      while (p[1] && p[1] != '\n')
        p++;
    } else if (p[0] == '/' && p[1] == '*') {
      scan->comment = TRUE;
      p++;
    } else if (*p == '"' || *p == '\'') {
      char quote = *p;
      for (p++; *p && *p != quote && *p != '\n'; p++)
        if (*p == '\\' && p[1])
          p++;
      if (!*p)
        break;
      if (*p == '\n') {
        scan->line++;
        scan->line_start = p + 1;
      }
    } else {
      scan->p = p + 1;
      return p;
    }
  }
  scan->p = p;
  return NULL;
}

GArray *blocks_of(const char *text)
{
  GArray *blocks = g_array_new(FALSE, FALSE, sizeof(Block)), *closed;
  GArray *open = g_array_new(FALSE, FALSE, sizeof(guint)); /* indexes of the blocks still open */
  CodeScan scan = { text, text, 0, FALSE };

  for (const char *p; (p = code_next(&scan));) {
    if (*p == '{') {
      gboolean starts_line = TRUE;
      for (const char *q = scan.line_start; q < p; q++)
        starts_line = starts_line && g_ascii_isspace(*q);
      Block b = { starts_line && names_block(text, scan.line_start) ? scan.line - 1 : scan.line, scan.line, -1 };
      guint index = blocks->len;
      g_array_append_val(blocks, b);
      g_array_append_val(open, index);
    } else if (*p == '}' && open->len > 0) {
      g_array_index(blocks, Block, g_array_index(open, guint, open->len - 1)).close_line = scan.line;
      g_array_set_size(open, open->len - 1);
    }
  }
  closed = g_array_sized_new(FALSE, FALSE, sizeof(Block), blocks->len);
  for (guint i = 0; i < blocks->len; i++)
    if (g_array_index(blocks, Block, i).close_line >= 0)
      g_array_append_val(closed, g_array_index(blocks, Block, i));
  g_array_free(open, TRUE);
  g_array_free(blocks, TRUE);
  return closed;
}
