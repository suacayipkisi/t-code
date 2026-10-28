/*
 * The preview's Markdown (see state.h): md4c parses the file (GitHub flavor: tables, task lists,
 * strikethrough, autolinks, $math$) and its blocks and spans are appended as text with tags. Code
 * blocks are colored with GtkSourceView's language definitions and the editor's style scheme; local
 * images and tables become widgets in the text, formulas (math.c) paintables; quotes that start with
 * [!NOTE], [!TIP], ... are GitHub's alerts.
 */
#include "editor/preview/state.h"

#include "editor/editorview.h"

#include <md4c.h>
#include <stdlib.h>
#include <string.h>

#define MATH_SCALE 1.1 /* serif math beside the sans text looks smaller at the same size */

/* Appending ---------------------------------------------------------------------------------- */

static int end_offset(Render *r)
{
  return gtk_text_buffer_get_char_count(r->p->buffer);
}

static void append(Render *r, const char *text, gssize len)
{
  GtkTextIter end;

  gtk_text_buffer_get_end_iter(r->p->buffer, &end);
  gtk_text_buffer_insert(r->p->buffer, &end, text, len);
}

/* Applies tag (a name, or a tag when name is NULL) from start to the end of the buffer. */
static void tag_from(Render *r, const char *name, GtkTextTag *tag, int start)
{
  GtkTextIter from, end;

  gtk_text_buffer_get_iter_at_offset(r->p->buffer, &from, start);
  gtk_text_buffer_get_end_iter(r->p->buffer, &end);
  if (name)
    gtk_text_buffer_apply_tag_by_name(r->p->buffer, name, &from, &end);
  else
    gtk_text_buffer_apply_tag(r->p->buffer, tag, &from, &end);
}

/* Applies name to the buffer's last line (paragraph spacing is a property of a whole line). */
static void tag_last_line(Render *r, const char *name)
{
  GtkTextIter from, end;

  gtk_text_buffer_get_end_iter(r->p->buffer, &end);
  from = end;
  if (gtk_text_iter_backward_char(&from)) /* the last line break */
    gtk_text_iter_set_line_offset(&from, 0);
  gtk_text_buffer_apply_tag_by_name(r->p->buffer, name, &from, &end);
}

/* A block starts on a line of its own. */
static void begin_block(Render *r)
{
  GtkTextIter end;

  gtk_text_buffer_get_end_iter(r->p->buffer, &end);
  if (gtk_text_iter_backward_char(&end) && gtk_text_iter_get_char(&end) != '\n')
    append(r, "\n", 1);
}

/* Ends the paragraph that started at start: a line break, its indent and the space after it. */
static void end_paragraph(Render *r, int start, const char *gap)
{
  char name[16];

  append(r, "\n", 1);
  g_snprintf(name, sizeof name, "indent%d", MIN(r->level, MAX_LEVEL));
  tag_from(r, name, NULL, start);
  if (gap)
    tag_last_line(r, gap);
}

/* A block drawn below the text (draw_blocks), from start to the end of the buffer. */
static void add_block(Render *r, BlockKind kind, int start, int level)
{
  Block block = { kind, start, end_offset(r), level, 0 };

  g_array_append_val(r->p->blocks, block);
}

static void push_start(Render *r)
{
  int start = end_offset(r);

  g_array_append_val(r->starts, start);
}

static int pop_start(Render *r)
{
  int start = r->starts->len ? g_array_index(r->starts, int, r->starts->len - 1) : 0;

  if (r->starts->len)
    g_array_set_size(r->starts, r->starts->len - 1);
  return start;
}

char *preview_decode_entity(const char *text, gsize size)
{
  static const char *const named[][2] = {
    { "&amp;", "&" },       { "&lt;", "<" },     { "&gt;", ">" },   { "&quot;", "\"" }, { "&apos;", "'" },
    { "&nbsp;", "\u00a0" }, { "&copy;", "©" },   { "&reg;", "®" },  { "&trade;", "™" }, { "&mdash;", "—" },
    { "&ndash;", "–" },     { "&hellip;", "…" }, { "&rarr;", "→" }, { "&larr;", "←" },  { "&times;", "×" },
    { "&middot;", "·" },    { "&bull;", "•" },
  };
  char utf8[8];

  if (size > 3 && text[1] == '#') {
    gboolean hex = text[2] == 'x' || text[2] == 'X';
    gunichar c = (gunichar)strtoul(text + (hex ? 3 : 2), NULL, hex ? 16 : 10);
    if (c && g_unichar_validate(c))
      return g_strndup(utf8, g_unichar_to_utf8(c, utf8));
  }
  for (gsize i = 0; i < G_N_ELEMENTS(named); i++)
    if (strlen(named[i][0]) == size && strncmp(text, named[i][0], size) == 0)
      return g_strdup(named[i][1]);
  return g_strndup(text, size);
}

static void append_entity(Render *r, const char *text, gsize size)
{
  char *decoded = preview_decode_entity(text, size);

  append(r, decoded, -1);
  g_free(decoded);
}

/* Images ------------------------------------------------------------------------------------- */

char *preview_local_path(const char *dir, const char *target)
{
  char *path = NULL, *plain;
  const char *cut;

  if (g_str_has_prefix(target, "file://"))
    return g_filename_from_uri(target, NULL, NULL);
  if (g_uri_peek_scheme(target) || !*target || *target == '#')
    return NULL;
  cut = strpbrk(target, "#?");
  plain = g_uri_unescape_segment(target, cut, NULL);
  if (plain && *plain)
    path = g_path_is_absolute(plain) && g_file_test(plain, G_FILE_TEST_EXISTS) ? g_strdup(plain)
                                                                               : g_build_filename(dir, plain, NULL);
  g_free(plain);
  return path;
}

/* Shows the image at src (width: wanted width in pixels, 0 for its own); FALSE when it is not a
 * local image that loads. */
static gboolean append_image(Render *r, const char *src, int width)
{
  char *path = preview_local_path(r->dir, src);
  GdkTexture *texture = path ? gdk_texture_new_from_filename(path, NULL) : NULL;

  g_free(path);
  if (!texture)
    return FALSE;

  GtkWidget *picture = gtk_picture_new_for_paintable(GDK_PAINTABLE(texture));
  GtkTextIter end;
  GtkTextChildAnchor *anchor;

  g_object_set_data(G_OBJECT(picture), "t-code-width",
                    GINT_TO_POINTER(width > 0 ? width : gdk_texture_get_width(texture)));
  gtk_picture_set_content_fit(GTK_PICTURE(picture), GTK_CONTENT_FIT_CONTAIN);
  preview_size_picture(picture, preview_content_width(r->p));
  gtk_text_buffer_get_end_iter(r->p->buffer, &end);
  anchor = gtk_text_buffer_create_child_anchor(r->p->buffer, &end);
  gtk_text_view_add_child_at_anchor(r->p->view, picture, anchor);
  g_ptr_array_add(r->p->pictures, picture);
  g_object_unref(texture);
  return TRUE;
}

/* HTML --------------------------------------------------------------------------------------- */

/* The value of attribute name in an HTML tag, or NULL. */
static char *html_attribute(const char *tag, const char *name)
{
  char *pattern = g_strdup_printf("\\s%s\\s*=\\s*(?:\"([^\"]*)\"|'([^']*)'|([^\\s>]+))", name);
  GMatchInfo *match = NULL;
  char *value = NULL;

  if (g_regex_match_simple(pattern, tag, G_REGEX_CASELESS, 0)) {
    GRegex *regex = g_regex_new(pattern, G_REGEX_CASELESS, 0, NULL);
    if (g_regex_match(regex, tag, 0, &match))
      for (int group = 1; group <= 3 && (!value || !*value); group++) {
        g_free(value);
        value = g_match_info_fetch(match, group);
      }
    g_match_info_free(match);
    g_regex_unref(regex);
  }
  g_free(pattern);
  return value;
}

/* Whether tag is <name> or <name ...> (closing: </name>), in any case. */
static gboolean is_tag(const char *tag, const char *name, gboolean closing)
{
  const char *rest = tag + 1 + closing;
  size_t len = strlen(name);

  return (!closing || tag[1] == '/') && g_ascii_strncasecmp(rest, name, len) == 0 &&
         (rest[len] == '>' || g_ascii_isspace(rest[len]));
}

/* Raw HTML: <img> shows its image, <br> breaks the line, <sub>, <sup> and <small> make their text
 * smaller, other tags are left out and the text between them is shown. */
static void append_html(Render *r, const char *text, gsize size, gboolean block)
{
  const char *p = text, *end = text + size;

  while (p < end) {
    if (*p == '<') {
      const char *close = memchr(p, '>', end - p);
      if (!close)
        break;
      char *tag = g_strndup(p, close - p + 1);
      if (g_ascii_strncasecmp(tag, "<img", 4) == 0) {
        char *src = html_attribute(tag, "src"), *width = html_attribute(tag, "width");
        if (src && !append_image(r, src, width ? atoi(width) : 0)) {
          char *alt = html_attribute(tag, "alt");
          if (alt && *alt) {
            int start = end_offset(r);
            append(r, alt, -1);
            tag_from(r, "dim", NULL, start);
          }
          g_free(alt);
        }
        g_free(width);
        g_free(src);
      } else if (g_ascii_strncasecmp(tag, "<br", 3) == 0) {
        append(r, "\u2028", -1);
      } else if (is_tag(tag, "sub", FALSE) || is_tag(tag, "sup", FALSE) || is_tag(tag, "small", FALSE)) {
        int start = end_offset(r);
        g_array_append_val(r->smalls, start);
      } else if ((is_tag(tag, "sub", TRUE) || is_tag(tag, "sup", TRUE) || is_tag(tag, "small", TRUE)) &&
                 r->smalls->len) {
        tag_from(r, "small", NULL, g_array_index(r->smalls, int, r->smalls->len - 1));
        g_array_set_size(r->smalls, r->smalls->len - 1);
      }
      g_free(tag);
      p = close + 1;
    } else if (*p == '&') {
      const char *semi = memchr(p, ';', MIN(end - p, 12));
      gsize len = semi ? (gsize)(semi - p + 1) : 1;
      append_entity(r, p, len);
      p += len;
    } else {
      const char *next = p;
      while (next < end && *next != '<' && *next != '&')
        next++;
      if (block) { /* the lines of an HTML block flow like a paragraph's */
        char *flat = g_strndup(p, next - p);
        g_strdelimit(flat, "\r\n\t", ' ');
        append(r, flat, -1);
        g_free(flat);
      } else {
        append(r, p, next - p);
      }
      p = next;
    }
  }
}

/* Code blocks -------------------------------------------------------------------------------- */

/* The GtkSourceView language for a code block's info string (```c, ```bash, ...), or NULL. */
static GtkSourceLanguage *code_language(const char *lang)
{
  static const char *const aliases[][2] = {
    { "c++", "cpp" },  { "cxx", "cpp" },     { "hpp", "cpphdr" },    { "h", "chdr" },        { "bash", "sh" },
    { "shell", "sh" }, { "zsh", "sh" },      { "console", "sh" },    { "py", "python3" },    { "rs", "rust" },
    { "yml", "yaml" }, { "md", "markdown" }, { "make", "makefile" }, { "ts", "typescript" },
  };
  GtkSourceLanguageManager *languages = gtk_source_language_manager_get_default();
  GtkSourceLanguage *language;
  char *id, *name;

  if (!lang || !*lang)
    return NULL;
  id = g_ascii_strdown(lang, -1);
  for (gsize i = 0; i < G_N_ELEMENTS(aliases); i++)
    if (strcmp(id, aliases[i][0]) == 0)
      g_set_str(&id, aliases[i][1]);
  language = gtk_source_language_manager_get_language(languages, id);
  if (!language) {
    name = g_strconcat("file.", id, NULL);
    language = gtk_source_language_manager_guess_language(languages, name, NULL);
    g_free(name);
  }
  g_free(id);
  return language;
}

/* The preview's tag with the colors of a style scheme tag, made the first time; NULL when it sets
 * none of them. */
static GtkTextTag *color_tag(Preview *p, GtkTextTag *source)
{
  gboolean fg_set, weight_set, style_set;
  GdkRGBA *fg = NULL;
  int weight = 0, style = 0;
  char *key, *color;
  GtkTextTag *tag;

  g_object_get(source, "foreground-set", &fg_set, "weight-set", &weight_set, "style-set", &style_set, NULL);
  if (!fg_set && !weight_set && !style_set)
    return NULL;
  if (fg_set)
    g_object_get(source, "foreground-rgba", &fg, NULL);
  if (weight_set)
    g_object_get(source, "weight", &weight, NULL);
  if (style_set)
    g_object_get(source, "style", &style, NULL);
  color = fg ? gdk_rgba_to_string(fg) : g_strdup("");
  key = g_strdup_printf("%s|%d|%d", color, weight, style);
  tag = g_hash_table_lookup(p->colors, key);
  if (!tag) {
    tag = gtk_text_buffer_create_tag(p->buffer, NULL, NULL);
    if (fg)
      g_object_set(tag, "foreground-rgba", fg, NULL);
    if (weight_set)
      g_object_set(tag, "weight", weight, NULL);
    if (style_set)
      g_object_set(tag, "style", style, NULL);
    g_hash_table_insert(p->colors, g_steal_pointer(&key), tag);
  }
  g_free(key);
  g_free(color);
  if (fg)
    gdk_rgba_free(fg);
  return tag;
}

/* A code block's text, colored like the editor colors it: GtkSourceView highlights it in a buffer
 * of its own, and its colors are copied over. */
static void append_code(Render *r, const char *text, const char *lang)
{
  GtkSourceLanguage *language = code_language(lang);
  GtkSourceBuffer *source;
  GtkTextIter at, end;

  if (!language) {
    append(r, text, -1);
    return;
  }
  source = gtk_source_buffer_new_with_language(language);
  gtk_source_buffer_set_style_scheme(source, editor_style_scheme(r->p->tab->app));
  gtk_text_buffer_set_text(GTK_TEXT_BUFFER(source), text, -1);
  gtk_text_buffer_get_bounds(GTK_TEXT_BUFFER(source), &at, &end);
  gtk_source_buffer_ensure_highlight(source, &at, &end);
  while (!gtk_text_iter_equal(&at, &end)) {
    GtkTextIter next = at;
    gtk_text_iter_forward_to_tag_toggle(&next, NULL);
    char *piece = gtk_text_iter_get_slice(&at, &next);
    GSList *tags = gtk_text_iter_get_tags(&at);
    int start = end_offset(r);
    append(r, piece, -1);
    for (GSList *each = tags; each; each = each->next) {
      GtkTextTag *tag = color_tag(r->p, each->data);
      if (tag)
        tag_from(r, NULL, tag, start);
    }
    g_slist_free(tags);
    g_free(piece);
    at = next;
  }
  g_object_unref(source);
}

/* Math --------------------------------------------------------------------------------------- */

/* The tag that lowers a formula depth pixels: it is drawn on the baseline, its depth goes below it. */
static GtkTextTag *rise_tag(Preview *p, int depth)
{
  GtkTextTag *tag = g_hash_table_lookup(p->rises, GINT_TO_POINTER(depth));

  if (!tag) {
    tag = gtk_text_buffer_create_tag(p->buffer, NULL, "rise", -depth * PANGO_SCALE, NULL);
    g_hash_table_insert(p->rises, GINT_TO_POINTER(depth), tag);
  }
  return tag;
}

/* A TeX formula: inline on the text's baseline, or (display) centered on a line of its own. */
static void append_math(Render *r, const char *tex, gboolean display)
{
  double size = BODY_POINTS * r->p->scale * 96.0 / 72.0 * MATH_SCALE;
  int depth = 0, start;
  GdkPaintable *math;
  GtkTextIter end;

  if (r->heading)
    size *= preview_heading_scale(r->heading);
  math = preview_math_new(tex, display, size, &depth);
  if (!math)
    return;
  if (display)
    begin_block(r);
  start = end_offset(r);
  gtk_text_buffer_get_end_iter(r->p->buffer, &end);
  gtk_text_buffer_insert_paintable(r->p->buffer, &end, math);
  g_object_unref(math);
  if (depth > 0)
    tag_from(r, NULL, rise_tag(r->p, depth), start);
  if (display) {
    tag_from(r, "math-display", NULL, start);
    r->after_display = TRUE;
  }
}

/* Text after a display formula starts a new line. */
static void after_display(Render *r)
{
  if (r->after_display) {
    r->after_display = FALSE;
    append(r, "\n", 1);
  }
}

/* Alerts ------------------------------------------------------------------------------------- */

/* A quote's first paragraph (from start) that begins with [!NOTE], [!TIP], ...: the marker becomes
 * the alert's title, in its color, and the quote is drawn in it. */
static void find_alert(Render *r, int start)
{
  static const char *const markers[] = { "[!NOTE]", "[!TIP]", "[!IMPORTANT]", "[!WARNING]", "[!CAUTION]" };
  static const char *const titles[] = { "ⓘ  Note", "✱  Tip", "❢  Important", "⚠  Warning", "⊘  Caution" };
  GtkTextBuffer *b = r->p->buffer;
  GtkTextIter from, to;
  char *text;

  gtk_text_buffer_get_iter_at_offset(b, &from, start);
  gtk_text_buffer_get_end_iter(b, &to);
  text = gtk_text_iter_get_text(&from, &to);
  for (int i = 0; i < (int)G_N_ELEMENTS(markers); i++) {
    int len = (int)strlen(markers[i]);
    if (g_ascii_strncasecmp(text, markers[i], len) != 0 || (text[len] && text[len] != ' '))
      continue;
    gboolean more = text[len] == ' ' && text[len + 1];
    char *title = g_strconcat(titles[i], more ? "\u2028" : "", NULL); /* the text goes on the next line */
    int removed = len + (text[len] == ' '), added = (int)g_utf8_strlen(title, -1), shift = added - removed;
    char name[16];

    to = from;
    gtk_text_iter_forward_chars(&to, removed);
    gtk_text_buffer_delete(b, &from, &to);
    gtk_text_buffer_insert(b, &from, title, -1);
    gtk_text_buffer_get_iter_at_offset(b, &from, start);
    gtk_text_buffer_get_iter_at_offset(b, &to, start + (int)g_utf8_strlen(titles[i], -1));
    g_snprintf(name, sizeof name, "alert%d", i + 1);
    gtk_text_buffer_apply_tag_by_name(b, "bold", &from, &to);
    gtk_text_buffer_apply_tag_by_name(b, name, &from, &to);
    for (guint l = 0; l < r->p->links->len; l++) { /* links and code after it moved */
      Link *link = &g_array_index(r->p->links, Link, l);
      if (link->start >= start) {
        link->start += shift;
        link->end += shift;
      }
    }
    for (guint c = 0; c < r->p->code_spans->len; c++) {
      CodeSpan *span = &g_array_index(r->p->code_spans, CodeSpan, c);
      if (span->start >= start) {
        span->start += shift;
        span->end += shift;
      }
    }
    g_array_index(r->alerts, int, r->alerts->len - 1) = i + 1;
    g_free(title);
    break;
  }
  g_free(text);
}

/* Headings ----------------------------------------------------------------------------------- */

/* A heading's anchor as GitHub makes it: lower case, spaces to '-', punctuation left out; the
 * second "Usage" is "usage-1". */
static void add_anchor(Render *r, int start)
{
  GtkTextIter from, end;
  GString *slug = g_string_new(NULL);
  char *text;

  gtk_text_buffer_get_iter_at_offset(r->p->buffer, &from, start);
  gtk_text_buffer_get_end_iter(r->p->buffer, &end);
  text = gtk_text_iter_get_text(&from, &end);
  for (const char *c = text; *c; c = g_utf8_next_char(c)) {
    gunichar ch = g_utf8_get_char(c);
    if (ch == ' ')
      g_string_append_c(slug, '-');
    else if (g_unichar_isalnum(ch) || ch == '-' || ch == '_')
      g_string_append_unichar(slug, g_unichar_tolower(ch));
  }
  g_free(text);
  char *key = g_strdup(slug->str);
  for (int n = 1; g_hash_table_contains(r->p->anchors, key); n++) {
    g_free(key);
    key = g_strdup_printf("%s-%d", slug->str, n);
  }
  g_hash_table_insert(r->p->anchors, key, GINT_TO_POINTER(start));
  g_string_free(slug, TRUE);
}

/* md4c's callbacks --------------------------------------------------------------------------- */

static int enter_block(MD_BLOCKTYPE type, void *detail, void *data)
{
  Render *r = data;
  gboolean first_in_quote = r->quote_start;

  r->quote_start = FALSE;
  switch (type) {
    case MD_BLOCK_UL:
    case MD_BLOCK_OL:
      if (r->nlists < MAX_LEVEL) {
        r->lists[r->nlists] = type == MD_BLOCK_OL ? (int)((MD_BLOCK_OL_DETAIL *)detail)->start : 0;
        r->tight[r->nlists] =
            type == MD_BLOCK_OL ? ((MD_BLOCK_OL_DETAIL *)detail)->is_tight : ((MD_BLOCK_UL_DETAIL *)detail)->is_tight;
      }
      r->nlists++;
      r->level++;
      break;
    case MD_BLOCK_LI: {
      static const char *const bullets[] = { "•   ", "◦   ", "▪   " };
      MD_BLOCK_LI_DETAIL *li = detail;
      int list = MIN(r->nlists, MAX_LEVEL) - 1;
      begin_block(r);
      push_start(r);
      r->in_item_start = TRUE;
      r->in_item_p = FALSE;
      if (li->is_task) {
        append(r, li->task_mark == ' ' ? "☐   " : "☑   ", -1);
      } else if (list >= 0 && r->lists[list] > 0) {
        char *number = g_strdup_printf("%d.  ", r->lists[list]++);
        append(r, number, -1);
        g_free(number);
      } else {
        append(r, bullets[MAX(list, 0) % 3], -1);
      }
      break;
    }
    case MD_BLOCK_QUOTE: {
      int none = 0;
      begin_block(r);
      push_start(r);
      r->level++;
      g_array_append_val(r->alerts, none);
      r->quote_start = TRUE;
      break;
    }
    case MD_BLOCK_CODE: {
      MD_BLOCK_CODE_DETAIL *code = detail;
      begin_block(r);
      push_start(r);
      r->code = g_string_new(NULL);
      r->lang = g_strndup(code->lang.text, code->lang.size);
      break;
    }
    case MD_BLOCK_HR: {
      begin_block(r);
      append(r, "\n", 1);
      tag_last_line(r, "spacer");
      int start = end_offset(r);
      append(r, " \n", 2); /* the line the rule is drawn on */
      tag_from(r, "hr", NULL, start);
      add_block(r, BLOCK_HR, start, MIN(r->level, MAX_LEVEL));
      append(r, "\n", 1);
      tag_last_line(r, "spacer");
      break;
    }
    case MD_BLOCK_TABLE:
      begin_block(r);
      preview_table_begin(r);
      break;
    case MD_BLOCK_TR:
      r->row++;
      r->cell = 0;
      break;
    case MD_BLOCK_TH:
    case MD_BLOCK_TD:
      if (r->table)
        preview_cell_begin(r, ((MD_BLOCK_TD_DETAIL *)detail)->align);
      break;
    case MD_BLOCK_HTML:
      r->html_block = TRUE;
      begin_block(r);
      push_start(r);
      break;
    case MD_BLOCK_H:
      begin_block(r);
      push_start(r);
      r->heading = (int)((MD_BLOCK_H_DETAIL *)detail)->level;
      break;
    case MD_BLOCK_P:
      r->alert_p = first_in_quote;
      if (r->in_item_start) {
        r->in_item_start = FALSE;
        r->in_item_p = TRUE;
      } else {
        begin_block(r);
      }
      push_start(r);
      break;
    default: /* the document, a table's head and body */ break;
  }
  return 0;
}

static int leave_block(MD_BLOCKTYPE type, void *detail, void *data)
{
  Render *r = data;

  r->after_display = FALSE; /* the block's line break ends a display formula's line */
  switch (type) {
    case MD_BLOCK_UL:
    case MD_BLOCK_OL:
      r->nlists--;
      r->level--;
      if (r->nlists == 0) /* a list inside an item keeps the item's spacing */
        tag_last_line(r, "gap");
      break;
    case MD_BLOCK_LI: {
      int start = pop_start(r);
      GtkTextIter end;
      gtk_text_buffer_get_end_iter(r->p->buffer, &end);
      if (gtk_text_iter_backward_char(&end) && gtk_text_iter_get_char(&end) != '\n')
        append(r, "\n", 1);

      GtkTextIter from, to;
      gtk_text_buffer_get_iter_at_offset(r->p->buffer, &from, start);
      to = from;
      gtk_text_iter_forward_to_line_end(&to);
      if (!gtk_text_iter_is_end(&to))
        gtk_text_iter_forward_char(&to);

      char name[16];
      g_snprintf(name, sizeof name, "li%d", MIN(MAX(r->nlists, 1), MAX_LEVEL));
      gtk_text_buffer_apply_tag_by_name(r->p->buffer, name, &from, &to);

      gboolean tight = r->nlists > 0 && r->tight[MIN(r->nlists, MAX_LEVEL) - 1];
      tag_last_line(r, tight ? "tight" : "gap");
      r->in_item_start = FALSE;
      r->in_item_p = FALSE;
      break;
    }
    case MD_BLOCK_QUOTE:
      begin_block(r);
      int start = pop_start(r), alert = 0;
      if (r->alerts->len) {
        alert = g_array_index(r->alerts, int, r->alerts->len - 1);
        g_array_set_size(r->alerts, r->alerts->len - 1);
      }
      if (!alert) /* an alert's text keeps the text's color */
        tag_from(r, "quote", NULL, start);
      add_block(r, BLOCK_QUOTE, start, MIN(r->level, MAX_LEVEL) - 1); /* the bar left of its text */
      g_array_index(r->p->blocks, Block, r->p->blocks->len - 1).alert = alert;
      r->level--;
      tag_last_line(r, "gap");
      break;
    case MD_BLOCK_CODE: {
      int start = pop_start(r);
      if (r->lang && g_ascii_strcasecmp(r->lang, "math") == 0) { /* ```math: a display formula */
        append_math(r, r->code->str, TRUE);
        g_string_free(r->code, TRUE);
        g_clear_pointer(&r->lang, g_free);
        r->code = NULL;
        r->after_display = FALSE;
        end_paragraph(r, start, "gap");
        break;
      }
      if (r->code->len && r->code->str[r->code->len - 1] == '\n')
        g_string_truncate(r->code, r->code->len - 1);
      append_code(r, r->code->str, r->lang);
      g_string_free(r->code, TRUE);
      g_clear_pointer(&r->lang, g_free);
      r->code = NULL;
      end_paragraph(r, start, "pre-bottom");
      tag_from(r, "pre", NULL, start);
      char pre[16];
      g_snprintf(pre, sizeof pre, "pre%d", MIN(r->level, MAX_LEVEL));
      tag_from(r, pre, NULL, start);
      add_block(r, BLOCK_PRE, start, MIN(r->level, MAX_LEVEL));
      GtkTextIter from, first_end;
      gtk_text_buffer_get_iter_at_offset(r->p->buffer, &from, start);
      first_end = from;
      gtk_text_iter_forward_line(&first_end);
      gtk_text_buffer_apply_tag_by_name(r->p->buffer, "pre-top", &from, &first_end);
      append(r, "\n", 1);
      tag_last_line(r, "spacer");
      break;
    }
    case MD_BLOCK_H: {
      int start = pop_start(r);
      char name[4];
      r->heading = 0;
      add_anchor(r, start);
      end_paragraph(r, start, NULL);
      unsigned level = MIN(((MD_BLOCK_H_DETAIL *)detail)->level, 6u);
      g_snprintf(name, sizeof name, "h%u", level);
      tag_from(r, name, NULL, start);
      if (level <= 2)
        add_block(r, BLOCK_RULE, start, MIN(r->level, MAX_LEVEL));
      break;
    }
    case MD_BLOCK_P: {
      int start = pop_start(r);
      if (r->alert_p) {
        r->alert_p = FALSE;
        find_alert(r, start);
      }
      if (r->in_item_p) {
        r->in_item_p = FALSE;
        append(r, "\n", 1);
      } else if (r->nlists > 0) {
        gboolean in_tight = r->tight[MIN(r->nlists, MAX_LEVEL) - 1];
        end_paragraph(r, start, in_tight ? "tight" : "gap");
      } else {
        end_paragraph(r, start, "gap");
      }
      break;
    }
    case MD_BLOCK_HTML: {
      int start = pop_start(r);
      r->html_block = FALSE;
      if (end_offset(r) > start) /* not only tags */
        end_paragraph(r, start, "gap");
      break;
    }
    case MD_BLOCK_TH:
    case MD_BLOCK_TD:
      if (r->markup)
        preview_cell_end(r, type == MD_BLOCK_TH);
      break;
    case MD_BLOCK_TABLE:
      preview_table_end(r);
      append(r, "\n", 1);
      tag_last_line(r, "gap");
      break;
    default: break;
  }
  return 0;
}

static int enter_span(MD_SPANTYPE type, void *detail, void *data)
{
  Render *r = data;

  if (r->markup) {
    preview_cell_span(r, type, detail);
    return 0;
  }
  if (type == MD_SPAN_LATEXMATH || type == MD_SPAN_LATEXMATH_DISPLAY)
    r->math = g_string_new(NULL);
  else
    after_display(r);
  push_start(r);
  if (type == MD_SPAN_CODE)
    append(r, "\u2009", -1); /* a thin space: the pill's edge, inside it, so the text keeps its spaces */
  if (type == MD_SPAN_A) {
    MD_SPAN_A_DETAIL *a = detail;
    g_ptr_array_add(r->hrefs, g_strndup(a->href.text, a->href.size));
  } else if (type == MD_SPAN_IMG) {
    MD_SPAN_IMG_DETAIL *img = detail;
    char *src = g_strndup(img->src.text, img->src.size);
    gboolean shown = !r->hidden && append_image(r, src, 0);
    g_array_append_val(r->images, shown);
    r->hidden += shown;
    g_free(src);
  }
  return 0;
}

static int leave_span(MD_SPANTYPE type, void *detail G_GNUC_UNUSED, void *data)
{
  Render *r = data;

  if (r->markup) {
    preview_cell_span_end(r);
    return 0;
  }
  int start = pop_start(r);

  switch (type) {
    case MD_SPAN_EM: tag_from(r, "italic", NULL, start); break;
    case MD_SPAN_STRONG: tag_from(r, "bold", NULL, start); break;
    case MD_SPAN_DEL: tag_from(r, "strike", NULL, start); break;
    case MD_SPAN_U: tag_from(r, "underline", NULL, start); break;
    case MD_SPAN_LATEXMATH:
    case MD_SPAN_LATEXMATH_DISPLAY:
      if (r->math && !r->hidden)
        append_math(r, r->math->str, type == MD_SPAN_LATEXMATH_DISPLAY);
      if (r->math)
        g_string_free(r->math, TRUE);
      r->math = NULL;
      break;
    case MD_SPAN_CODE: {
      append(r, "\u2009", -1);
      int end = end_offset(r);
      if (end > start) {
        CodeSpan span = { start, end };
        g_array_append_val(r->p->code_spans, span);
      }
      tag_from(r, "code", NULL, start);
      break;
    }
    case MD_SPAN_A: {
      char *url = r->hrefs->len ? g_ptr_array_steal_index(r->hrefs, r->hrefs->len - 1) : NULL;
      int end = end_offset(r);
      if (url && end > start) {
        Link link = { start, end, url };
        g_array_append_val(r->p->links, link);
        tag_from(r, "link", NULL, start);
      } else {
        g_free(url);
      }
      break;
    }
    case MD_SPAN_IMG: {
      gboolean shown = r->images->len && g_array_index(r->images, gboolean, r->images->len - 1);
      if (r->images->len)
        g_array_set_size(r->images, r->images->len - 1);
      r->hidden -= shown;
      if (!shown)
        tag_from(r, "dim", NULL, start); /* its alt text */
      break;
    }
    default: break;
  }
  return 0;
}

static int on_text(MD_TEXTTYPE type, const MD_CHAR *text, MD_SIZE size, void *data)
{
  Render *r = data;

  if (r->in_item_start)
    r->in_item_start = FALSE;

  if (r->code) {
    g_string_append_len(r->code, text, size);
    return 0;
  }
  if (r->markup) {
    preview_cell_text(r, type, text, size);
    return 0;
  }
  if (r->math) {
    g_string_append_len(r->math, type == MD_TEXT_SOFTBR || type == MD_TEXT_BR ? " " : text,
                        type == MD_TEXT_SOFTBR || type == MD_TEXT_BR ? 1 : size);
    return 0;
  }
  if (r->hidden)
    return 0;
  if (r->after_display) { /* the text after a display formula: on the next line, without its spaces */
    if (type == MD_TEXT_SOFTBR || type == MD_TEXT_BR)
      return 0;
    while (size && g_ascii_isspace(*text)) {
      text++;
      size--;
    }
    if (!size)
      return 0;
    after_display(r);
  }
  switch (type) {
    case MD_TEXT_NULLCHAR: append(r, "\uFFFD", -1); break;
    case MD_TEXT_BR: append(r, "\u2028", -1); break; /* a line break inside the paragraph */
    case MD_TEXT_SOFTBR: append(r, " ", 1); break;
    case MD_TEXT_ENTITY: append_entity(r, text, size); break;
    case MD_TEXT_HTML: append_html(r, text, size, r->html_block); break;
    default: append(r, text, size); break;
  }
  return 0;
}

void preview_render(Preview *p)
{
  MD_PARSER parser = {
    0, MD_DIALECT_GITHUB | MD_FLAG_LATEXMATHSPANS, enter_block, leave_block, enter_span, leave_span, on_text, NULL, NULL
  };
  Render r = { .p = p };
  char *text = NULL;
  gsize len = 0;
  GError *error = NULL;
  GtkTextIter start, end;

  g_object_set(gtk_text_tag_table_lookup(gtk_text_buffer_get_tag_table(p->buffer), "base"), "size-points",
               BODY_POINTS * p->scale, NULL);
  r.dir = g_path_get_dirname(p->tab->path);
  r.starts = g_array_new(FALSE, FALSE, sizeof(int));
  r.images = g_array_new(FALSE, FALSE, sizeof(gboolean));
  r.hrefs = g_ptr_array_new_with_free_func(g_free);
  r.closers = g_ptr_array_new();
  r.smalls = g_array_new(FALSE, FALSE, sizeof(int));
  r.alerts = g_array_new(FALSE, FALSE, sizeof(int));
  if (g_file_get_contents(p->tab->path, &text, &len, &error)) {
    md_parse(text, (MD_SIZE)len, &parser, &r);
  } else {
    append(&r, error->message, -1);
    tag_from(&r, "error", NULL, 0);
    g_error_free(error);
  }
  if (r.code)
    g_string_free(r.code, TRUE);
  g_free(r.lang);
  g_free(text);
  g_free(r.dir);
  g_array_free(r.starts, TRUE);
  g_array_free(r.images, TRUE);
  g_ptr_array_free(r.hrefs, TRUE);
  g_ptr_array_free(r.closers, TRUE);
  g_array_free(r.smalls, TRUE);
  g_array_free(r.alerts, TRUE);
  if (r.math)
    g_string_free(r.math, TRUE);
  if (r.markup) /* md4c stopped inside a table */
    g_string_free(r.markup, TRUE);
  if (r.table) {
    g_object_ref_sink(r.table->grid);
    g_object_unref(r.table->grid);
    preview_table_free(r.table);
  }
  gtk_text_buffer_get_bounds(p->buffer, &start, &end);
  gtk_text_buffer_apply_tag_by_name(p->buffer, "base", &start, &end);
}
