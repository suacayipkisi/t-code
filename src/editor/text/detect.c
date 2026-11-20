/*
 * A file's indentation and its language, from its text (see detect.h).
 */
#include "editor/text/detect.h"

#include <stdlib.h>
#include <string.h>

#define MAX_LINES 10000 /* enough to know; big files are not read to the end */

gboolean detect_indent(const char *text, gssize len, gboolean *tabs, int *width)
{
  const char *p = text, *end = text + (len < 0 ? (gssize)strlen(text) : len);
  int tab_lines = 0, space_lines = 0, steps[9] = { 0 }, previous = -1, best = 0;

  for (int n = 0; p < end && n < MAX_LINES; n++) {
    const char *eol = memchr(p, '\n', end - p), *q = p;
    if (!eol)
      eol = end;
    while (q < eol && (*q == ' ' || *q == '\t'))
      q++;
    if (q < eol && *q != '\r') { /* not blank */
      if (memchr(p, '\t', q - p)) {
        tab_lines++;
        previous = -1;
      } else {
        int lead = (int)(q - p);
        if (lead > 1)
          space_lines++;
        if (previous >= 0 && abs(lead - previous) >= 2 && abs(lead - previous) <= 8)
          steps[abs(lead - previous)]++;
        previous = lead;
      }
    }
    p = eol + 1;
  }
  if (tab_lines > space_lines) {
    *tabs = TRUE;
    return TRUE;
  }
  if (space_lines == 0)
    return FALSE;
  for (int step = 2; step <= 8; step++)
    if (steps[step] > steps[best])
      best = step;
  *tabs = FALSE;
  if (best)
    *width = best;
  return TRUE;
}

gboolean is_cxx_text(const char *text, gssize len)
{
  static GRegex *cxx;

  if (!cxx)
    cxx = g_regex_new("^\\s*(class|namespace|template\\s*<|public:|private:|protected:|using\\s)|std::"
                      "|\\bnoexcept\\b|\\boperator\\s*[^\\w\\s]",
                      G_REGEX_MULTILINE | G_REGEX_OPTIMIZE | G_REGEX_RAW, 0, NULL);
  /* Only the start: a header says what it is early on. (C23 has constexpr too.) */
  return g_regex_match_full(cxx, text, MIN(len < 0 ? (gssize)strlen(text) : len, 65536), 0, 0, NULL, NULL);
}

/* A .mm file in folder, or stem.h: Objective-C lives there. */
static gboolean objc_folder(const char *folder, const char *stem)
{
  char *header = g_strconcat(stem, ".h", NULL);
  GDir *dir = g_dir_open(folder, 0, NULL);
  const char *name;
  gboolean found = FALSE;

  while (dir && !found && (name = g_dir_read_name(dir)))
    found = g_str_has_suffix(name, ".mm") || strcmp(name, header) == 0;
  if (dir)
    g_dir_close(dir);
  g_free(header);
  return found;
}

gboolean is_objc_m(const char *path, const char *text)
{
  static GRegex *objc, *matlab;

  if (!g_str_has_suffix(path, ".m"))
    return FALSE;
  if (!objc) {
    objc = g_regex_new("^\\s*(#import|#include|@interface|@implementation|@protocol)\\b",
                       G_REGEX_MULTILINE | G_REGEX_OPTIMIZE | G_REGEX_RAW, 0, NULL);
    matlab = g_regex_new("^\\s*(%|function\\b)", G_REGEX_MULTILINE | G_REGEX_OPTIMIZE | G_REGEX_RAW, 0, NULL);
  }
  if (text) {
    gssize len = MIN((gssize)strlen(text), 65536);
    if (g_regex_match_full(objc, text, len, 0, 0, NULL, NULL))
      return TRUE;
    if (g_regex_match_full(matlab, text, len, 0, 0, NULL, NULL))
      return FALSE;
  }
  char *folder = g_path_get_dirname(path), *base = g_path_get_basename(path);
  base[strlen(base) - 2] = '\0';
  gboolean found = objc_folder(folder, base);
  g_free(base);
  g_free(folder);
  return found;
}
