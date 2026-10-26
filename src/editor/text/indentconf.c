/*
 * What a project's config files say about a file's indentation (see indentconf.h).
 */
#include "editor/text/indentconf.h"

#include <stdlib.h>
#include <string.h>

/* .editorconfig ------------------------------------------------------------------------------ */

/* An EditorConfig glob as a regular expression over a path relative to the file's folder: "*" stays
 * in a folder, "**" does not, "{a,b}" is either, "[!a]" is not a; a glob without "/" matches the
 * name in any folder. */
static char *glob_regex(const char *glob)
{
  GString *re = g_string_new("^");
  int braces = 0;

  if (*glob == '/')
    glob++;
  else if (!strchr(glob, '/'))
    g_string_append(re, "(?:.*/)?");
  for (const char *p = glob; *p; p++) {
    if (p[0] == '*' && p[1] == '*') {
      g_string_append(re, ".*");
      p++;
    } else if (*p == '*') {
      g_string_append(re, "[^/]*");
    } else if (*p == '?') {
      g_string_append(re, "[^/]");
    } else if (*p == '[' && strchr(p, ']')) {
      const char *end = strchr(p + 1, ']');
      g_string_append_c(re, '[');
      p++;
      if (*p == '!') {
        g_string_append_c(re, '^');
        p++;
      }
      for (; p < end; p++) {
        if (*p == '\\' || *p == '[')
          g_string_append_c(re, '\\');
        g_string_append_c(re, *p);
      }
      g_string_append_c(re, ']');
    } else if (*p == '{') {
      g_string_append(re, "(?:");
      braces++;
    } else if (*p == '}' && braces) {
      g_string_append_c(re, ')');
      braces--;
    } else if (*p == ',' && braces) {
      g_string_append_c(re, '|');
    } else if (*p == '\\' && p[1]) {
      char *quoted = g_regex_escape_string(++p, 1);
      g_string_append(re, quoted);
      g_free(quoted);
    } else {
      char *quoted = g_regex_escape_string(p, 1);
      g_string_append(re, quoted);
      g_free(quoted);
    }
  }
  while (braces--)
    g_string_append_c(re, ')');
  g_string_append_c(re, '$');
  return g_string_free(re, FALSE);
}

static gboolean glob_matches(const char *glob, const char *relative)
{
  char *pattern = glob_regex(glob);
  gboolean matches = g_regex_match_simple(pattern, relative, G_REGEX_CASELESS, 0);

  g_free(pattern);
  return matches;
}

/* A positive number, or 0. */
static int number(const char *text)
{
  char *end;
  long value = strtol(text, &end, 10);

  return *text && *end == '\0' && value > 0 && value <= 16 ? (int)value : 0;
}

/* Reads one .editorconfig (in folder) into style for relative (the file's path from there);
 * indent_size "tab" is left for tab_width, through *size_is_tab. */
static void read_editorconfig(const char *folder, const char *relative, IndentStyle *style, int *tab_width,
                              gboolean *size_is_tab)
{
  char *path = g_build_filename(folder, ".editorconfig", NULL), *text = NULL;
  gboolean in_match = FALSE;

  if (!g_file_get_contents(path, &text, NULL, NULL)) {
    g_free(path);
    return;
  }
  char **lines = g_strsplit(text, "\n", -1);
  for (int i = 0; lines[i]; i++) {
    char *line = g_strstrip(lines[i]), *equals;
    if (!*line || *line == '#' || *line == ';')
      continue;
    if (*line == '[' && line[strlen(line) - 1] == ']') {
      line[strlen(line) - 1] = '\0';
      in_match = glob_matches(line + 1, relative);
      continue;
    }
    if (!(equals = strchr(line, '=')))
      continue;
    *equals = '\0';
    char *key = g_ascii_strdown(g_strstrip(line), -1), *value = g_ascii_strdown(g_strstrip(equals + 1), -1);
    if (in_match && strcmp(key, "indent_style") == 0)
      style->tabs = strcmp(value, "tab") == 0 ? 1 : strcmp(value, "space") == 0 ? 0 : -1;
    else if (in_match && strcmp(key, "indent_size") == 0) {
      *size_is_tab = strcmp(value, "tab") == 0;
      style->width = number(value);
    } else if (in_match && strcmp(key, "tab_width") == 0)
      *tab_width = number(value);
    g_free(key);
    g_free(value);
  }
  g_strfreev(lines);
  g_free(text);
  g_free(path);
}

/* What .editorconfig files say about file: the sections that match it, the nearer file and the
 * later section winning, up to the one with "root = true". */
static void indent_from_editorconfig(const char *file, IndentStyle *style)
{
  GPtrArray *folders = g_ptr_array_new_with_free_func(g_free);
  char *folder = g_path_get_dirname(file);
  int tab_width = 0;
  gboolean size_is_tab = FALSE;

  /* The folders with one, nearest first, up to the one that says "root = true" before its first
   * section; read farthest first so the nearer win. */
  for (;;) {
    char *path = g_build_filename(folder, ".editorconfig", NULL), *text = NULL;
    gboolean found = g_file_get_contents(path, &text, NULL, NULL);
    gboolean root = found && g_regex_match_simple("\\A(?:\\s*(?:[#;].*)?\\n)*\\s*root\\s*=\\s*true\\s*$", text,
                                                  G_REGEX_MULTILINE | G_REGEX_CASELESS, 0);
    g_free(text);
    g_free(path);
    if (found)
      g_ptr_array_add(folders, g_strdup(folder));
    char *parent = g_path_get_dirname(folder);
    gboolean top = root || strcmp(parent, folder) == 0;
    g_free(folder);
    folder = parent;
    if (top)
      break;
  }
  g_free(folder);

  for (guint i = folders->len; i-- > 0;) {
    const char *dir = g_ptr_array_index(folders, i);
    const char *relative = file + strlen(dir);
    while (*relative == '/')
      relative++;
    read_editorconfig(dir, relative, style, &tab_width, &size_is_tab);
  }
  if (!style->width && (size_is_tab || style->tabs == 1))
    style->width = tab_width;
  g_ptr_array_free(folders, TRUE);
}

/* .clang-format ------------------------------------------------------------------------------ */

static gboolean is_c_family(const char *file)
{
  static const char *const extensions[] = { ".c",   ".h",   ".cc",  ".cpp", ".cxx", ".c++", ".hh", ".hpp",
                                            ".hxx", ".h++", ".ipp", ".inl", ".tpp", ".m",   ".mm", NULL };
  const char *dot = strrchr(file, '.');

  for (int i = 0; dot && extensions[i]; i++)
    if (g_ascii_strcasecmp(dot, extensions[i]) == 0)
      return TRUE;
  return FALSE;
}

/* The nearest .clang-format's text, or NULL. */
static char *nearest_clang_format(const char *file)
{
  char *folder = g_path_get_dirname(file), *text = NULL;

  while (!text) {
    for (int i = 0; !text && i < 2; i++) {
      char *path = g_build_filename(folder, i ? "_clang-format" : ".clang-format", NULL);
      if (!g_file_get_contents(path, &text, NULL, NULL))
        text = NULL;
      g_free(path);
    }
    char *parent = g_path_get_dirname(folder);
    gboolean top = strcmp(parent, folder) == 0;
    g_free(folder);
    folder = parent;
    if (top)
      break;
  }
  g_free(folder);
  return text;
}

/* A top-level "Key: value" of one YAML document, or NULL. */
static char *yaml_value(const char *document, const char *key)
{
  char *pattern = g_strdup_printf("^%s:\\s*([^#\\s]+)", key), *value = NULL;
  GMatchInfo *match;
  GRegex *regex = g_regex_new(pattern, G_REGEX_MULTILINE, 0, NULL);

  if (regex && g_regex_match(regex, document, 0, &match))
    value = g_match_info_fetch(match, 1);
  if (regex) {
    g_match_info_free(match);
    g_regex_unref(regex);
  }
  g_free(pattern);
  return value;
}

/* What the nearest .clang-format (or _clang-format) says, for a C or C++ file: its Cpp section,
 * with clang-format's defaults for what it leaves out. */
static void indent_from_clang_format(const char *file, IndentStyle *style)
{
  char *text = is_c_family(file) ? nearest_clang_format(file) : NULL;
  char **documents;

  if (!text)
    return;
  documents = g_regex_split_simple("^---.*$", text, G_REGEX_MULTILINE, 0);
  for (int i = 0; documents[i]; i++) {
    char *language = yaml_value(documents[i], "Language");
    gboolean ours = !language || strcmp(language, "Cpp") == 0 || strcmp(language, "ObjC") == 0;
    g_free(language);
    if (!ours || !*g_strstrip(documents[i]))
      continue;

    char *use_tab = yaml_value(documents[i], "UseTab"), *width = yaml_value(documents[i], "IndentWidth");
    char *based_on = yaml_value(documents[i], "BasedOnStyle");
    if (style->tabs < 0) /* Never, false: spaces; Always, ForIndentation, ...: tabs */
      style->tabs = use_tab && strcmp(use_tab, "Never") != 0 && g_ascii_strcasecmp(use_tab, "false") != 0;
    if (!style->width && width)
      style->width = number(width);
    if (!style->width) /* the styles' own: 4 in WebKit and Microsoft, 2 in the others */
      style->width =
          based_on && (g_ascii_strcasecmp(based_on, "WebKit") == 0 || g_ascii_strcasecmp(based_on, "Microsoft") == 0)
              ? 4
              : 2;
    g_free(use_tab);
    g_free(width);
    g_free(based_on);
    break;
  }
  g_strfreev(documents);
  g_free(text);
}

/* Both ------------------------------------------------------------------------------------- */

const char *indent_from_config(const char *file, IndentStyle *style)
{
  gboolean editorconfig;

  style->tabs = -1;
  style->width = 0;
  indent_from_editorconfig(file, style);
  editorconfig = style->tabs >= 0 || style->width > 0;
  indent_from_clang_format(file, style);
  if (editorconfig)
    return ".editorconfig";
  return style->tabs >= 0 || style->width > 0 ? ".clang-format" : NULL;
}
