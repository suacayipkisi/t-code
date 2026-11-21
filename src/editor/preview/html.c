/*
 * Raw HTML in the preview (see state.h), shown as GitHub shows it: formatting tags (<b>, <i>, <ins>,
 * <del>, <sub>, <sup>, <kbd>, <mark>, ...), <a href> links and <a name> / id anchors, <img> and
 * <picture> (its source for the theme), <br>, <hr>, <h1> .. <h6>, align on <p>, <div> and headings,
 * <details> that open and close on their <summary>. Comments are left out, the tags GFM filters
 * (<script>, <style>, ...) show as text, other tags are left out and the text between them shown. A
 * tag or comment cut by md4c (it gives raw HTML line by line) is completed by the next piece.
 */
#include "editor/preview/state.h"

#include "window/palette.h"

#include <stdlib.h>
#include <string.h>

typedef enum {
  HTML_STYLE,
  HTML_CODE,
  HTML_LINK,
  HTML_HEADING,
  HTML_BLOCK,
  HTML_DETAILS,
  HTML_SUMMARY,
  HTML_PICTURE,
  HTML_QUOTE,
  HTML_ITEM
} HtmlKind;

/* clang-format off */
static const struct { const char *name; HtmlKind kind; const char *tag; } elements[] = {
  { "b", HTML_STYLE, "bold" }, { "strong", HTML_STYLE, "bold" }, { "i", HTML_STYLE, "italic" },
  { "em", HTML_STYLE, "italic" }, { "cite", HTML_STYLE, "italic" }, { "var", HTML_STYLE, "italic" },
  { "dfn", HTML_STYLE, "italic" }, { "u", HTML_STYLE, "underline" }, { "ins", HTML_STYLE, "underline" },
  { "s", HTML_STYLE, "strike" }, { "del", HTML_STYLE, "strike" }, { "strike", HTML_STYLE, "strike" },
  { "sub", HTML_STYLE, "sub" }, { "sup", HTML_STYLE, "sup" }, { "small", HTML_STYLE, "small" },
  { "mark", HTML_STYLE, "mark" }, { "code", HTML_CODE, NULL }, { "kbd", HTML_CODE, NULL },
  { "samp", HTML_CODE, NULL }, { "tt", HTML_CODE, NULL }, { "a", HTML_LINK, NULL }, { "h1", HTML_HEADING, NULL },
  { "h2", HTML_HEADING, NULL }, { "h3", HTML_HEADING, NULL }, { "h4", HTML_HEADING, NULL },
  { "h5", HTML_HEADING, NULL }, { "h6", HTML_HEADING, NULL }, { "p", HTML_BLOCK, NULL }, { "div", HTML_BLOCK, NULL },
  { "center", HTML_BLOCK, NULL }, { "blockquote", HTML_BLOCK, NULL }, { "pre", HTML_BLOCK, NULL },
  { "details", HTML_DETAILS, NULL }, { "summary", HTML_SUMMARY, NULL }, { "picture", HTML_PICTURE, NULL },
  { "q", HTML_QUOTE, NULL }, { "li", HTML_ITEM, NULL }, { "dt", HTML_ITEM, NULL }, { "dd", HTML_BLOCK, NULL },
  { "tr", HTML_BLOCK, NULL },
};
/* clang-format on */

/* GFM's tag filter: these change how a browser reads what follows, so they show as text. */
static const char *const filtered[] = { "title",   "textarea", "style",  "xmp",      "iframe",
                                        "noembed", "noframes", "script", "plaintext" };

void preview_html_open_clear(gpointer data)
{
  HtmlOpen *open = data;

  g_free(open->href);
  g_free(open->title);
}

char *preview_html_attribute(const char *tag, const char *name)
{
  char *pattern = g_strdup_printf("\\s%s\\s*=\\s*(?:\"([^\"]*)\"|'([^']*)'|([^\\s>]+))", name);
  GMatchInfo *match = NULL;
  char *value = NULL;
  GRegex *regex = g_regex_new(pattern, G_REGEX_CASELESS, 0, NULL);

  if (regex && g_regex_match(regex, tag, 0, &match))
    for (int group = 1; group <= 3 && (!value || !*value); group++) {
      g_free(value);
      value = g_match_info_fetch(match, group);
    }
  g_match_info_free(match);
  if (regex)
    g_regex_unref(regex);
  g_free(pattern);
  if (value && !*value)
    g_clear_pointer(&value, g_free);
  if (value) { /* entities in attribute values: &amp; in a URL */
    GString *decoded = g_string_new(NULL);
    for (const char *c = value; *c;) {
      const char *semi = *c == '&' ? strchr(c, ';') : NULL;
      if (semi && semi - c < 40) {
        char *text = preview_decode_entity(c, semi - c + 1);
        g_string_append(decoded, text);
        g_free(text);
        c = semi + 1;
      } else {
        g_string_append_c(decoded, *c++);
      }
    }
    g_free(value);
    value = g_string_free(decoded, FALSE);
  }
  return value;
}

/* Whether a boolean attribute (<details open>) is there. */
static gboolean has_attribute(const char *tag, const char *name)
{
  char *pattern = g_strdup_printf("\\s%s(\\s|=|/?>)", name);
  gboolean has = g_regex_match_simple(pattern, tag, G_REGEX_CASELESS, 0);

  g_free(pattern);
  return has;
}

/* A tag's name in lower case, and whether it is an end tag. */
static char *tag_name(const char *tag, gboolean *closing)
{
  const char *start = tag + 1 + (tag[1] == '/'), *end = start;

  *closing = tag[1] == '/';
  while (g_ascii_isalnum(*end) || *end == '-')
    end++;
  return g_ascii_strdown(start, end - start);
}

static int find_element(const char *name)
{
  for (int i = 0; i < (int)G_N_ELEMENTS(elements); i++)
    if (strcmp(elements[i].name, name) == 0)
      return i;
  return -1;
}

static char align_of(const char *tag)
{
  char *align = preview_html_attribute(tag, "align");
  char a = align && g_ascii_strcasecmp(align, "center") == 0  ? 'c'
           : align && g_ascii_strcasecmp(align, "right") == 0 ? 'r'
                                                              : 0;

  g_free(align);
  return a;
}

/* <img>: its image (or <picture>'s source for the theme), else its alt text. */
static void html_image(Render *r, const char *tag)
{
  char *src = r->picture_src ? g_strdup(r->picture_src) : preview_html_attribute(tag, "src");
  char *width = preview_html_attribute(tag, "width"), *height = preview_html_attribute(tag, "height");
  char *alt = preview_html_attribute(tag, "alt"), *title = preview_html_attribute(tag, "title");
  const char *href = NULL;

  for (guint i = r->html_open->len; i-- > 0 && !href;) /* inside <a href>: clicking it follows the link */
    href = g_array_index(r->html_open, HtmlOpen, i).href;
  if (!href && r->hrefs->len)
    href = g_ptr_array_index(r->hrefs, r->hrefs->len - 1);
  if (!src || !preview_append_image(r, src, width ? atoi(width) : 0, height ? atoi(height) : 0, href, title)) {
    if (alt) {
      int start = preview_end_offset(r);
      preview_append(r, alt, -1);
      preview_tag(r, "dim", NULL, start);
    }
  }
  g_free(title);
  g_free(alt);
  g_free(height);
  g_free(width);
  g_free(src);
}

/* <source> in <picture>: the first whose media query is the theme's (or has none) gives the image. */
static void html_source(Render *r, const char *tag)
{
  char *media = preview_html_attribute(tag, "media"), *srcset = preview_html_attribute(tag, "srcset");
  gboolean dark = !palette_light();

  if (!r->picture_src && srcset && (!media || (strstr(media, "dark") && dark) || (strstr(media, "light") && !dark))) {
    char *end = strpbrk(srcset, " \t,"); /* its first address */
    r->picture_src = g_strndup(srcset, end ? (gsize)(end - srcset) : strlen(srcset));
  }
  g_free(srcset);
  g_free(media);
}

static void open_element(Render *r, int element, const char *tag)
{
  HtmlOpen open = { element, 0, -1, align_of(tag), r->depth, NULL, NULL };
  char *id = preview_html_attribute(tag, "id"), *name = preview_html_attribute(tag, "name");

  switch (elements[element].kind) {
    case HTML_HEADING:
    case HTML_BLOCK:
    case HTML_ITEM:
    case HTML_DETAILS: preview_begin_block(r); break;
    case HTML_SUMMARY: {
      preview_begin_block(r);
      for (guint i = r->html_open->len; i-- > 0;) {
        HtmlOpen *details = &g_array_index(r->html_open, HtmlOpen, i);
        if (elements[details->element].kind == HTML_DETAILS) {
          g_array_index(r->p->details, Details, details->extra).summary = preview_end_offset(r);
          break;
        }
      }
      preview_append(r, "▶  ", -1); /* ▼ when it is open (after rendering) */
      break;
    }
    case HTML_LINK:
      open.href = preview_html_attribute(tag, "href");
      open.title = preview_html_attribute(tag, "title");
      break;
    case HTML_PICTURE: g_clear_pointer(&r->picture_src, g_free); break;
    default: break;
  }
  open.start = preview_end_offset(r);
  if (strcmp(elements[element].name, "pre") == 0)
    r->html_pre++;
  if (elements[element].kind == HTML_DETAILS) {
    Details details = { -1, -1, -1, has_attribute(tag, "open") };
    g_array_append_val(r->p->details, details);
    open.extra = (int)r->p->details->len - 1;
  } else if (elements[element].kind == HTML_QUOTE) {
    preview_append(r, "“", -1);
  } else if (elements[element].kind == HTML_ITEM && strcmp(elements[element].name, "li") == 0) {
    preview_append(r, "•   ", -1);
  }
  if (id) /* <a name="x"></a>, id="x": #x links to it */
    preview_add_named_anchor(r, id, open.start);
  if (name)
    preview_add_named_anchor(r, name, open.start);
  g_free(name);
  g_free(id);
  g_array_append_val(r->html_open, open);
}

/* Applies an element's end to the text from where it started. */
static void close_element(Render *r, HtmlOpen *open)
{
  int end = preview_end_offset(r);
  const char *name = elements[open->element].name;

  switch (elements[open->element].kind) {
    case HTML_STYLE: preview_tag(r, elements[open->element].tag, NULL, open->start); break;
    case HTML_CODE: preview_add_code_span(r, open->start); break;
    case HTML_LINK:
      if (open->href && end > open->start)
        preview_add_link(r, open->start, g_steal_pointer(&open->href), g_steal_pointer(&open->title));
      break;
    case HTML_HEADING: {
      int level = name[1] - '0';
      char h[4];
      preview_add_anchor(r, open->start);
      preview_end_paragraph(r, open->start, NULL);
      g_snprintf(h, sizeof h, "h%d", level);
      preview_tag(r, h, NULL, open->start);
      if (level <= 2)
        preview_add_block(r, BLOCK_RULE, open->start, MIN(r->level, MAX_LEVEL));
      break;
    }
    case HTML_BLOCK:
      if (strcmp(name, "pre") == 0) { /* its text in a code block's box */
        r->html_pre--;
        GtkTextIter last, buffer_end;
        gtk_text_buffer_get_end_iter(r->p->buffer, &buffer_end);
        last = buffer_end;
        if (end > open->start && gtk_text_iter_backward_char(&last) && gtk_text_iter_get_char(&last) == '\n')
          gtk_text_buffer_delete(r->p->buffer, &last, &buffer_end);
        if (preview_end_offset(r) > open->start)
          preview_end_code_box(r, open->start);
      } else if (end > open->start && !preview_at_line_start(r)) {
        preview_end_paragraph(r, open->start, "gap");
      }
      break;
    case HTML_ITEM:
      if (!preview_at_line_start(r))
        preview_append(r, "\n", 1);
      break;
    case HTML_SUMMARY: { /* a paragraph of its own, its ▶ included: indented in a list item, apart from what follows */
      GtkTextIter line;
      gtk_text_buffer_get_iter_at_offset(r->p->buffer, &line, open->start);
      gtk_text_iter_set_line_offset(&line, 0);
      preview_end_paragraph(r, gtk_text_iter_get_offset(&line), "gap");
      for (guint i = r->html_open->len; i-- > 0;) {
        HtmlOpen *details = &g_array_index(r->html_open, HtmlOpen, i);
        if (elements[details->element].kind == HTML_DETAILS) {
          g_array_index(r->p->details, Details, details->extra).content = preview_end_offset(r);
          break;
        }
      }
      break;
    }
    case HTML_DETAILS: g_array_index(r->p->details, Details, open->extra).end = end; break;
    case HTML_QUOTE: preview_append(r, "”", -1); break;
    case HTML_PICTURE: g_clear_pointer(&r->picture_src, g_free); break;
  }
  if (open->align && preview_end_offset(r) > open->start)
    preview_tag(r, open->align == 'c' ? "center" : "right", NULL, open->start);
}

/* An end tag closes its element and the ones opened inside it and left open. */
static void end_tag(Render *r, int element)
{
  guint i = r->html_open->len;

  while (i > 0 && g_array_index(r->html_open, HtmlOpen, i - 1).element != element)
    i--;
  if (!i)
    return; /* not open */
  while (r->html_open->len >= i) {
    HtmlOpen open = g_array_index(r->html_open, HtmlOpen, r->html_open->len - 1);
    g_array_remove_index(r->html_open, r->html_open->len - 1); /* (the copy keeps href, title) */
    close_element(r, &open);
    preview_html_open_clear(&open);
  }
}

/* Inline elements (and headings) that a Markdown block opened and left open end with it, as unclosed <b> or <h1>
 * written in a raw HTML block would otherwise style the rest of the document. Blocks (<div>, <details>, <p align>)
 * stay open: a README wraps Markdown in them, closing them several blocks later. */
void preview_html_close_inline(Render *r)
{
  /* the first inline element this block opened: it ends, and with it everything opened after it (a <p> or <li>
   * written after a stray <b> would otherwise keep the <b> open) */
  for (guint i = 0; i < r->html_open->len; i++) {
    const HtmlOpen *open = &g_array_index(r->html_open, HtmlOpen, i);
    HtmlKind kind = elements[open->element].kind;
    if (open->depth >= r->depth &&
        (kind == HTML_STYLE || kind == HTML_CODE || kind == HTML_LINK || kind == HTML_QUOTE || kind == HTML_HEADING)) {
      while (r->html_open->len > i) {
        HtmlOpen last = g_array_index(r->html_open, HtmlOpen, r->html_open->len - 1);
        g_array_remove_index(r->html_open, r->html_open->len - 1);
        close_element(r, &last);
        preview_html_open_clear(&last);
      }
      return;
    }
  }
}

void preview_html_finish(Render *r)
{
  while (r->html_open->len)
    end_tag(r, g_array_index(r->html_open, HtmlOpen, r->html_open->len - 1).element);
  g_clear_pointer(&r->picture_src, g_free);
}

/* Raw HTML's <table>: laid out like a Markdown table (table.c), its cells ended by </td>, the next <td> or <tr>,
 * </table>. Tags inside a cell go into it. TRUE when the tag was the table's. */
static gboolean table_tag(Render *r, const char *name, gboolean closing, const char *tag)
{
  gboolean cell = strcmp(name, "td") == 0 || strcmp(name, "th") == 0, row = strcmp(name, "tr") == 0;

  if (strcmp(name, "table") == 0) {
    if (!closing && !r->table) {
      preview_begin_block(r);
      preview_table_begin(r);
      r->html_table = TRUE;
    } else if (closing && r->html_table) {
      if (r->in_cell)
        preview_cell_end(r, r->html_th);
      preview_table_end(r);
      preview_tag_last_line(r, "table-gap");
      r->html_table = FALSE;
    }
    return TRUE;
  }
  if (!r->html_table)
    return FALSE;
  if (cell || row) {
    if (r->in_cell)
      preview_cell_end(r, r->html_th);
    if (row && !closing) {
      r->row++;
      r->cell = 0;
    } else if (cell && !closing) {
      char *align = preview_html_attribute(tag, "align");
      if (r->row < 0)
        r->row = 0;
      r->html_th = name[1] == 'h';
      preview_cell_begin(r, !align                                     ? MD_ALIGN_DEFAULT
                            : g_ascii_strcasecmp(align, "center") == 0 ? MD_ALIGN_CENTER
                            : g_ascii_strcasecmp(align, "right") == 0  ? MD_ALIGN_RIGHT
                                                                       : MD_ALIGN_LEFT);
      g_free(align);
    }
    return TRUE;
  }
  if (r->in_cell)
    preview_cell_text(r, MD_TEXT_HTML, tag, strlen(tag));
  return TRUE; /* <thead>, <tbody>, <caption>... between the cells: left out */
}

static void handle_tag(Render *r, const char *tag)
{
  gboolean closing;
  char *name = tag_name(tag, &closing);
  int element = find_element(name);

  for (gsize i = 0; i < G_N_ELEMENTS(filtered); i++)
    if (strcmp(name, filtered[i]) == 0) {
      preview_append(r, tag, -1);
      g_free(name);
      return;
    }
  if (table_tag(r, name, closing, tag)) {
    g_free(name);
    return;
  }
  if (closing) {
    if (element >= 0)
      end_tag(r, element);
  } else if (strcmp(name, "br") == 0) {
    preview_append(r, "\u2028", -1);
    r->after_break = TRUE;
  } else if (strcmp(name, "hr") == 0) {
    preview_add_rule(r);
  } else if (strcmp(name, "img") == 0) {
    html_image(r, tag);
  } else if (strcmp(name, "source") == 0) {
    html_source(r, tag);
  } else if (element >= 0) {
    open_element(r, element, tag);
    if (g_str_has_suffix(tag, "/>"))
      end_tag(r, element);
  } else { /* an unknown element can still be an anchor */
    char *id = preview_html_attribute(tag, "id");
    if (id)
      preview_add_named_anchor(r, id, preview_end_offset(r));
    g_free(id);
  }
  g_free(name);
}

/* Where a tag that starts at p ends (its '>', quotes respected), or NULL when it is cut. */
static const char *tag_end(const char *p, const char *end)
{
  char quote = 0;

  for (; p < end; p++) {
    if (quote) {
      if (*p == quote)
        quote = 0;
    } else if (*p == '"' || *p == '\'') {
      quote = *p;
    } else if (*p == '>') {
      return p;
    }
  }
  return NULL;
}

void preview_html(Render *r, const char *text, gsize size, gboolean block)
{
  GString *joined = NULL;
  const char *p, *end;

  if (r->html_carry) { /* the rest of a cut tag */
    joined = g_steal_pointer(&r->html_carry);
    g_string_append_len(joined, text, size);
    text = joined->str;
    size = joined->len;
  }
  p = text;
  end = text + size;
  while (p < end) {
    if (r->in_comment) {
      const char *close = g_strstr_len(p, end - p, "-->");
      if (!close)
        break;
      p = close + 3;
      r->in_comment = FALSE;
    } else if (*p == '<' && end - p >= 4 && strncmp(p, "<!--", 4) == 0) {
      r->in_comment = TRUE;
      p += 4;
    } else if (*p == '<' && end - p >= 2 && (p[1] == '!' || p[1] == '?')) { /* <!DOCTYPE>, <?php ?>, CDATA */
      const char *close = memchr(p, '>', end - p);
      if (!close) {
        r->html_carry = g_string_new_len(p, end - p);
        break;
      }
      p = close + 1;
    } else if (*p == '<' && end - p >= 2 &&
               (g_ascii_isalpha(p[1]) || (p[1] == '/' && end - p >= 3 && g_ascii_isalpha(p[2])))) {
      const char *close = tag_end(p, end);
      if (!close) {
        r->html_carry = g_string_new_len(p, end - p);
        break;
      }
      char *tag = g_strndup(p, close - p + 1);
      g_strdelimit(tag, "\r\n\t", ' ');
      handle_tag(r, tag);
      g_free(tag);
      p = close + 1;
    } else if (*p == '&') {
      const char *semi = memchr(p, ';', MIN(end - p, 40));
      gsize len = semi ? (gsize)(semi - p + 1) : 1;
      char *decoded = preview_decode_entity(p, len);
      if (r->in_cell)
        preview_cell_text(r, MD_TEXT_NORMAL, decoded, strlen(decoded));
      else if (!r->html_table)
        preview_append(r, decoded, -1);
      g_free(decoded);
      p += len;
    } else {
      const char *next = p + 1;
      while (next < end && *next != '<' && *next != '&')
        next++;
      char *run = g_strndup(p, next - p);
      if (r->html_pre) { /* as it is, but the line break right after <pre> */
        const char *from = run;
        if (*from == '\n' && preview_at_line_start(r))
          from++;
        preview_append(r, from, -1);
      } else {
        if (block) /* the lines of an HTML block flow like a paragraph's */
          g_strdelimit(run, "\r\n\t", ' ');
        const char *from = run;
        if (r->after_break) /* the spaces that start the line after a <br> */
          from += strspn(from, " ");
        if (*from)
          r->after_break = FALSE;
        if (r->html_table) {
          if (r->in_cell && strspn(from, " ") < strlen(from))
            preview_cell_text(r, MD_TEXT_NORMAL, from, strlen(from));
        } else if (*from && (!block || !preview_at_line_start(r) || strspn(from, " ") < strlen(from))) {
          preview_append_text(r, from, strlen(from));
        }
      }
      g_free(run);
      p = next;
    }
  }
  if (joined)
    g_string_free(joined, TRUE);
}

/* Table cells ----------------------------------------------------------------------------------- */

char *preview_html_name(const char *tag, gboolean *closing)
{
  return tag_name(tag, closing);
}
