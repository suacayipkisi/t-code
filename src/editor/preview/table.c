/*
 * The preview's tables (see state.h): text in the buffer like the rest, so they are selected and
 * copied with it. A cell's content is kept as runs (text with its formatting, a picture, a formula)
 * while md4c reads the table; at its end the columns are sized to the view's width like a browser does,
 * each cell is broken into lines that fit its column (Pango measures it), and the rows go into the
 * text one line at a time: the cells' lines separated by tabs, at the table's tab stops. Its grid,
 * header and stripes are drawn below the text (view.c's draw_tables).
 */
#include "editor/preview/state.h"

#include <stdlib.h>
#include <string.h>

enum {
  RUN_BOLD = 1 << 0,
  RUN_ITALIC = 1 << 1,
  RUN_STRIKE = 1 << 2,
  RUN_UNDERLINE = 1 << 3,
  RUN_CODE = 1 << 4,
  RUN_SUB = 1 << 5,
  RUN_SUP = 1 << 6,
  RUN_SMALL = 1 << 7,
  RUN_MARK = 1 << 8,
  RUN_DIM = 1 << 9,
};

/* Text with one formatting, or (paintable, tex) one object shown as U+FFFC. */
typedef struct {
  GString *text;
  guint flags;
  char *href;
  GdkPaintable *paintable; /* an image */
  int width;               /* its width in the text */
  char *tex;               /* a formula */
} TableRun;

/* A span or HTML element open in a cell. */
typedef enum { STYLE_PLAIN, STYLE_CODE, STYLE_MATH, STYLE_IMAGE } StyleKind;

typedef struct {
  guint flags;
  char *href;
  const char *element; /* the HTML element that opened it (a static name), or NULL for Markdown */
  StyleKind kind;
} CellStyle;

/* clang-format off */
static const struct { const char *name; guint flags; } html_styles[] = {
  { "b", RUN_BOLD }, { "strong", RUN_BOLD }, { "i", RUN_ITALIC }, { "em", RUN_ITALIC }, { "cite", RUN_ITALIC },
  { "var", RUN_ITALIC }, { "u", RUN_UNDERLINE }, { "ins", RUN_UNDERLINE }, { "s", RUN_STRIKE }, { "del", RUN_STRIKE },
  { "strike", RUN_STRIKE }, { "sub", RUN_SUB }, { "sup", RUN_SUP }, { "small", RUN_SMALL }, { "mark", RUN_MARK },
  { "code", RUN_CODE }, { "kbd", RUN_CODE }, { "tt", RUN_CODE }, { "samp", RUN_CODE }, { "a", 0 },
};
/* clang-format on */

static void run_clear(gpointer data)
{
  TableRun *run = data;

  if (run->text)
    g_string_free(run->text, TRUE);
  g_free(run->href);
  g_clear_object(&run->paintable);
  g_free(run->tex);
}

static void style_clear(gpointer data)
{
  g_free(((CellStyle *)data)->href);
}

static void cell_clear(gpointer data)
{
  g_array_free(((TableCell *)data)->runs, TRUE);
}

void preview_table_begin(Render *r)
{
  r->table = g_new0(PreviewTable, 1);
  r->table->cells = g_array_new(FALSE, TRUE, sizeof(TableCell));
  g_array_set_clear_func(r->table->cells, cell_clear);
  r->table->level = preview_content_level(r);
  r->row = -1;
  if (!r->cell_styles) {
    r->cell_styles = g_array_new(FALSE, FALSE, sizeof(CellStyle));
    g_array_set_clear_func(r->cell_styles, style_clear);
  }
}

void preview_table_free(gpointer data)
{
  PreviewTable *table = data;

  if (table->cells)
    g_array_free(table->cells, TRUE);
  if (table->rows)
    g_array_free(table->rows, TRUE);
  g_free(table->edges);
  g_free(table);
}

static TableCell *current_cell(Render *r)
{
  return &g_array_index(r->table->cells, TableCell, r->table->cells->len - 1);
}

static const CellStyle *current_style(Render *r)
{
  static const CellStyle plain = { 0 };

  return r->cell_styles->len ? &g_array_index(r->cell_styles, CellStyle, r->cell_styles->len - 1) : &plain;
}

static void push_style(Render *r, guint flags, const char *href, const char *element, StyleKind kind)
{
  const CellStyle *top = current_style(r);
  CellStyle style = { top->flags | flags, g_strdup(href ? href : top->href), element, kind };

  g_array_append_val(r->cell_styles, style);
}

void preview_cell_begin(Render *r, MD_ALIGN align)
{
  TableCell cell = { .row = r->row, .column = r->cell, .align = align };

  cell.runs = g_array_new(FALSE, TRUE, sizeof(TableRun));
  g_array_set_clear_func(cell.runs, run_clear);
  g_array_append_val(r->table->cells, cell);
  g_array_set_size(r->cell_styles, 0);
  r->in_cell = TRUE;
}

/* Text in the current formatting, joined to the last run when that has the same. */
static void add_text(Render *r, const char *text, gssize size)
{
  GArray *runs = current_cell(r)->runs;
  const CellStyle *style = current_style(r);

  if (size < 0)
    size = (gssize)strlen(text);
  if (!size)
    return;
  if (runs->len) {
    TableRun *last = &g_array_index(runs, TableRun, runs->len - 1);
    if (last->text && !last->paintable && !last->tex && last->flags == style->flags &&
        g_strcmp0(last->href, style->href) == 0) {
      g_string_append_len(last->text, text, size);
      return;
    }
  }
  TableRun run = { g_string_new_len(text, size), style->flags, g_strdup(style->href), NULL, 0, NULL };
  g_array_append_val(runs, run);
}

static void add_object(Render *r, GdkPaintable *paintable, int width, const char *tex)
{
  const CellStyle *style = current_style(r);
  TableRun run = { NULL,  style->flags, g_strdup(style->href), paintable ? g_object_ref(paintable) : NULL,
                   width, g_strdup(tex) };

  g_array_append_val(current_cell(r)->runs, run);
}

/* An image in a cell, FALSE when it cannot be shown (its alt text is). */
static gboolean cell_image(Render *r, const char *src, int width)
{
  GdkPaintable *paintable = src ? preview_image(r->p, r->dir, src) : NULL;

  if (!paintable)
    return FALSE;
  if (width <= 0)
    width = gdk_paintable_get_intrinsic_width(paintable);
  add_object(r, paintable, MAX(width, 1), NULL);
  g_object_unref(paintable);
  return TRUE;
}

/* Raw HTML in a cell: formatting tags and links style what follows, <br> breaks the line, <img> is a
 * picture; other tags are left out. */
static void cell_html(Render *r, const char *text, gsize size)
{
  char *tag = g_strndup(text, size);
  gboolean closing;
  char *name;

  g_strdelimit(tag, "\r\n\t", ' ');
  if (tag[0] != '<' || tag[1] == '!') { /* a comment */
    g_free(tag);
    return;
  }
  name = preview_html_name(tag, &closing);
  if (strcmp(name, "br") == 0) {
    add_text(r, "\n", 1);
  } else if (strcmp(name, "img") == 0) {
    char *src = preview_html_attribute(tag, "src"), *width = preview_html_attribute(tag, "width");
    if (!cell_image(r, src, width ? atoi(width) : 0)) {
      char *alt = preview_html_attribute(tag, "alt");
      if (alt)
        add_text(r, alt, -1);
      g_free(alt);
    }
    g_free(width);
    g_free(src);
  } else {
    for (gsize i = 0; i < G_N_ELEMENTS(html_styles); i++) {
      if (strcmp(name, html_styles[i].name) != 0)
        continue;
      if (!closing) {
        char *href = i == G_N_ELEMENTS(html_styles) - 1 ? preview_html_attribute(tag, "href") : NULL;
        push_style(r, html_styles[i].flags, href, html_styles[i].name, STYLE_PLAIN);
        g_free(href);
      } else { /* closes only what is open, and the ones opened inside it */
        guint at = r->cell_styles->len;
        while (at > 0 && g_strcmp0(g_array_index(r->cell_styles, CellStyle, at - 1).element, html_styles[i].name))
          at--;
        if (at)
          g_array_set_size(r->cell_styles, at - 1);
      }
      break;
    }
  }
  g_free(name);
  g_free(tag);
}

void preview_cell_text(Render *r, MD_TEXTTYPE type, const char *text, gsize size)
{
  char *plain = NULL;

  if (r->math) {
    g_string_append_len(r->math, type == MD_TEXT_SOFTBR || type == MD_TEXT_BR ? " " : text,
                        type == MD_TEXT_SOFTBR || type == MD_TEXT_BR ? 1 : size);
    return;
  }
  if (r->hidden) /* a shown image's alt text */
    return;
  switch (type) {
    case MD_TEXT_NULLCHAR: add_text(r, "�", -1); return;
    case MD_TEXT_BR: add_text(r, "\n", 1); return;
    case MD_TEXT_SOFTBR: add_text(r, " ", 1); return;
    case MD_TEXT_HTML: cell_html(r, text, size); return;
    case MD_TEXT_ENTITY: plain = preview_decode_entity(text, size); break;
    case MD_TEXT_NORMAL: plain = preview_emojify(text, size); break;
    default: break;
  }
  if (plain) {
    add_text(r, plain, -1);
    g_free(plain);
  } else {
    add_text(r, text, (gssize)size);
  }
}

void preview_cell_span(Render *r, MD_SPANTYPE type, void *detail)
{
  switch (type) {
    case MD_SPAN_EM: push_style(r, RUN_ITALIC, NULL, NULL, STYLE_PLAIN); break;
    case MD_SPAN_STRONG: push_style(r, RUN_BOLD, NULL, NULL, STYLE_PLAIN); break;
    case MD_SPAN_DEL: push_style(r, RUN_STRIKE, NULL, NULL, STYLE_PLAIN); break;
    case MD_SPAN_U: push_style(r, RUN_UNDERLINE, NULL, NULL, STYLE_PLAIN); break;
    case MD_SPAN_LATEXMATH:
    case MD_SPAN_LATEXMATH_DISPLAY: /* its TeX is collected, and becomes a formula at its end */
      r->math = g_string_new(NULL);
      push_style(r, 0, NULL, NULL, STYLE_MATH);
      break;
    case MD_SPAN_CODE:
      push_style(r, RUN_CODE, NULL, NULL, STYLE_CODE);
      add_text(r, " ", -1); /* a thin space: the box's edge, inside it (as in paragraphs) */
      break;
    case MD_SPAN_A: {
      char *href = preview_target(&((MD_SPAN_A_DETAIL *)detail)->href);
      push_style(r, 0, href, NULL, STYLE_PLAIN);
      g_free(href);
      break;
    }
    case MD_SPAN_IMG: {
      char *src = preview_target(&((MD_SPAN_IMG_DETAIL *)detail)->src);
      if (!r->hidden && cell_image(r, src, 0)) {
        r->hidden++;
        push_style(r, 0, NULL, NULL, STYLE_IMAGE);
      } else { /* its alt text */
        push_style(r, RUN_DIM, NULL, NULL, STYLE_PLAIN);
      }
      g_free(src);
      break;
    }
    default: push_style(r, 0, NULL, NULL, STYLE_PLAIN); break;
  }
}

void preview_cell_span_end(Render *r)
{
  if (!r->cell_styles->len)
    return;
  CellStyle *style = &g_array_index(r->cell_styles, CellStyle, r->cell_styles->len - 1);
  switch (style->kind) {
    case STYLE_CODE: add_text(r, " ", -1); break;
    case STYLE_MATH:
      if (r->math) {
        g_array_set_size(r->cell_styles, r->cell_styles->len - 1);
        add_object(r, NULL, 0, r->math->str);
        g_string_free(g_steal_pointer(&r->math), TRUE);
        return;
      }
      break;
    case STYLE_IMAGE: r->hidden--; break;
    case STYLE_PLAIN: break;
  }
  g_array_set_size(r->cell_styles, r->cell_styles->len - 1);
}

void preview_cell_end(Render *r, gboolean head)
{
  current_cell(r)->head = head;
  g_array_set_size(r->cell_styles, 0);
  r->in_cell = FALSE;
  r->cell++;
}

/* Laying out --------------------------------------------------------------------------------- */

/* A formula's size in a cell: the text's, as in paragraphs. */
static double math_size(Render *r)
{
  return BODY_POINTS * r->p->scale * 96.0 / 72.0 * MATH_SCALE;
}

/* A cell's text as Pango lays it out (objects as U+FFFC with their size), in the preview's fonts. */
static PangoLayout *cell_layout(Render *r, const TableCell *cell)
{
  PangoLayout *layout = gtk_widget_create_pango_layout(GTK_WIDGET(r->p->view), NULL);
  PangoFontDescription *font =
      pango_font_description_copy(pango_context_get_font_description(pango_layout_get_context(layout)));
  PangoAttrList *attrs = pango_attr_list_new();
  GString *text = g_string_new(NULL);
  char *mono = NULL;

  g_object_get(gtk_text_tag_table_lookup(gtk_text_buffer_get_tag_table(r->p->buffer), "code"), "family", &mono, NULL);
  pango_font_description_set_size(font, (int)(BODY_POINTS * r->p->scale * PANGO_SCALE));
  pango_layout_set_font_description(layout, font);
  pango_font_description_free(font);
  for (guint i = 0; i < cell->runs->len; i++) {
    const TableRun *run = &g_array_index(cell->runs, TableRun, i);
    guint start = (guint)text->len, flags = run->flags | (cell->head ? RUN_BOLD : 0);
    if (run->text) {
      g_string_append_len(text, run->text->str, (gssize)run->text->len);
    } else {
      int width = run->width, height = (int)(math_size(r));
      if (run->tex) {
        int depth = 0;
        GdkPaintable *math = preview_math_new(run->tex, FALSE, math_size(r), &depth);
        width = math ? gdk_paintable_get_intrinsic_width(math) : 0;
        height = math ? gdk_paintable_get_intrinsic_height(math) : 0;
        g_clear_object(&math);
      } else if (run->paintable && gdk_paintable_get_intrinsic_width(run->paintable) > 0) {
        height = width * gdk_paintable_get_intrinsic_height(run->paintable) /
                 gdk_paintable_get_intrinsic_width(run->paintable);
      }
      g_string_append(text, "￼");
      PangoRectangle rect = { 0, -height * PANGO_SCALE, width * PANGO_SCALE, height * PANGO_SCALE };
      PangoAttribute *shape = pango_attr_shape_new(&rect, &rect);
      shape->start_index = start, shape->end_index = (guint)text->len;
      pango_attr_list_insert(attrs, shape);
    }
    PangoAttribute *added[4] = { NULL };
    int n = 0;
    if (flags & RUN_BOLD)
      added[n++] = pango_attr_weight_new(PANGO_WEIGHT_BOLD);
    if (flags & RUN_ITALIC)
      added[n++] = pango_attr_style_new(PANGO_STYLE_ITALIC);
    if (flags & RUN_CODE) {
      added[n++] = pango_attr_family_new(mono ? mono : "monospace");
      added[n++] = pango_attr_scale_new(0.90);
    } else if (flags & (RUN_SUB | RUN_SUP)) {
      added[n++] = pango_attr_scale_new(0.75);
    } else if (flags & RUN_SMALL) {
      added[n++] = pango_attr_scale_new(0.85);
    }
    for (int k = 0; k < n; k++) {
      added[k]->start_index = start, added[k]->end_index = (guint)text->len;
      pango_attr_list_insert(attrs, added[k]);
    }
  }
  pango_layout_set_text(layout, text->str, (int)text->len);
  pango_layout_set_attributes(layout, attrs);
  pango_attr_list_unref(attrs);
  g_string_free(text, TRUE);
  g_free(mono);
  return layout;
}

/* The width of a layout's widest piece that cannot be broken (a word), laid out on one line. */
static int longest_word(PangoLayout *layout)
{
  const char *text = pango_layout_get_text(layout);
  int n = 0, longest = 0, from = 0;
  const PangoLogAttr *attrs = pango_layout_get_log_attrs_readonly(layout, &n);

  for (int i = 1; i < n; i++) { /* attrs[i]: before the i-th character */
    if (!attrs[i].is_line_break && i < n - 1)
      continue;
    const char *a = g_utf8_offset_to_pointer(text, from), *b = g_utf8_offset_to_pointer(text, i);
    while (b > a && g_ascii_isspace(b[-1])) /* the space before the break hangs */
      b--;
    PangoRectangle start, end;
    pango_layout_index_to_pos(layout, (int)(a - text), &start);
    pango_layout_index_to_pos(layout, (int)(b - text), &end);
    if (start.y == end.y)
      longest = MAX(longest, (end.x - start.x) / PANGO_SCALE);
    from = i;
  }
  return longest;
}

/* Shares width between the columns like a browser: when they all fit, each gets its widest cell on one line
 * (natural); else each gets its longest word (least) and the room left is shared in proportion to what each
 * still wants, its cells wrapping. A table whose words do not fit is wider than the text. */
static void share_widths(const int *natural, const int *least, int *widths, guint n, int width)
{
  int sum_natural = 0, sum_least = 0;

  for (guint c = 0; c < n; c++) {
    sum_natural += natural[c];
    sum_least += least[c];
  }
  for (guint c = 0; c < n; c++) {
    if (sum_natural <= width)
      widths[c] = natural[c];
    else if (sum_least >= width || sum_natural == sum_least)
      widths[c] = least[c];
    else
      widths[c] = least[c] + (int)((gint64)(natural[c] - least[c]) * (width - sum_least) / (sum_natural - sum_least));
  }
}

/* The tag of a table's lines: its tab stops, its left edge, no space between wrapped rows (the cells are broken
 * already: a line wraps only when a word is wider than the text, and never widens the view). */
static GtkTextTag *table_tag(Render *r, PreviewTable *table, const MD_ALIGN *aligns)
{
  PangoTabArray *tabs = pango_tab_array_new(table->columns, TRUE);
  int n = 0;

  for (int c = 0; c < table->columns; c++) {
    int left = table->edges[c] + CELL_PAD, right = table->edges[c + 1] - CELL_PAD;
    if (c == 0 && aligns[0] != MD_ALIGN_CENTER && aligns[0] != MD_ALIGN_RIGHT)
      continue; /* the line starts there */
    switch (aligns[c]) {
      case MD_ALIGN_CENTER: pango_tab_array_set_tab(tabs, n++, PANGO_TAB_CENTER, (left + right) / 2 - CELL_PAD); break;
      case MD_ALIGN_RIGHT: pango_tab_array_set_tab(tabs, n++, PANGO_TAB_RIGHT, right - CELL_PAD); break;
      default: pango_tab_array_set_tab(tabs, n++, PANGO_TAB_LEFT, left - CELL_PAD); break;
    }
  }
  pango_tab_array_resize(tabs, MAX(n, 1));
  if (!n)
    pango_tab_array_set_tab(tabs, 0, PANGO_TAB_LEFT, table->edges[table->columns]);
  GtkTextTag *tag = gtk_text_buffer_create_tag(r->p->buffer, NULL, "tabs", tabs, "wrap-mode", GTK_WRAP_WORD_CHAR,
                                               "pixels-inside-wrap", 0, NULL);
  pango_tab_array_free(tabs);
  return tag;
}

/* A line of a cell (bytes from..to of its layout's text) at the end of the buffer, formatted. */
static void emit_piece(Render *r, const TableCell *cell, int from, int to)
{
  static const struct {
    guint flag;
    const char *tag;
  } tags[] = {
    { RUN_BOLD, "bold" },           { RUN_ITALIC, "italic" }, { RUN_STRIKE, "strike" },
    { RUN_UNDERLINE, "underline" }, { RUN_SUB, "sub" },       { RUN_SUP, "sup" },
    { RUN_SMALL, "small" },         { RUN_MARK, "mark" },     { RUN_DIM, "dim" },
  };
  int at = 0;

  for (guint i = 0; i < cell->runs->len && at < to; i++) {
    const TableRun *run = &g_array_index(cell->runs, TableRun, i);
    int size = run->text ? (int)run->text->len : 3; /* U+FFFC */
    int a = MAX(from, at), b = MIN(to, at + size);
    if (a < b) {
      int start = preview_end_offset(r);
      guint flags = run->flags | (cell->head ? RUN_BOLD : 0);
      if (run->text) {
        GString *piece = g_string_new_len(run->text->str + (a - at), b - a);
        for (char *nl; (nl = strchr(piece->str, '\n'));) /* a <br>: the line ends there */
          g_string_erase(piece, nl - piece->str, 1);
        preview_append(r, piece->str, (gssize)piece->len);
        g_string_free(piece, TRUE);
      } else if (run->tex) {
        preview_append_math(r, run->tex, FALSE);
      } else if (run->paintable) {
        preview_add_picture(r, preview_picture_new(r->p, run->paintable, run->width, run->href, NULL));
      }
      if (preview_end_offset(r) > start) {
        for (gsize t = 0; t < G_N_ELEMENTS(tags); t++)
          if (flags & tags[t].flag)
            preview_tag(r, tags[t].tag, NULL, start);
        if (flags & RUN_CODE)
          preview_add_code_span(r, start);
        if (run->href && !run->paintable)
          preview_add_link(r, start, g_strdup(run->href), NULL);
      }
    }
    at += size;
  }
}

/* The line breaks of a cell laid out in its column: pairs of byte offsets, one per line. */
static GArray *cell_lines(Render *r, const TableCell *cell, int width)
{
  PangoLayout *layout = cell_layout(r, cell);
  GArray *lines = g_array_new(FALSE, FALSE, sizeof(int));

  pango_layout_set_width(layout, MAX(width, 1) * PANGO_SCALE);
  pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
  for (GSList *l = pango_layout_get_lines_readonly(layout); l; l = l->next) {
    PangoLayoutLine *line = l->data;
    int from = pango_layout_line_get_start_index(line);
    int to = from + pango_layout_line_get_length(line);
    g_array_append_val(lines, from);
    g_array_append_val(lines, to);
  }
  g_object_unref(layout);
  return lines;
}

void preview_table_end(Render *r)
{
  PreviewTable *table = g_steal_pointer(&r->table);
  GArray *cells = table->cells;
  int columns = 0, rows = 0;

  for (guint i = 0; i < cells->len; i++) {
    columns = MAX(columns, g_array_index(cells, TableCell, i).column + 1);
    rows = MAX(rows, g_array_index(cells, TableCell, i).row + 1);
  }
  if (!columns || !rows) {
    preview_table_free(table);
    return;
  }
  table->columns = columns;
  int *natural = g_new0(int, columns), *least = g_new0(int, columns), *widths = g_new0(int, columns);
  MD_ALIGN *aligns = g_new0(MD_ALIGN, columns);
  for (guint i = 0; i < cells->len; i++) {
    const TableCell *cell = &g_array_index(cells, TableCell, i);
    PangoLayout *layout = cell_layout(r, cell);
    int width;
    pango_layout_get_pixel_size(layout, &width, NULL);
    natural[cell->column] = MAX(natural[cell->column], width + 2 * CELL_PAD + 1);
    least[cell->column] = MAX(least[cell->column], longest_word(layout) + 2 * CELL_PAD + 1);
    g_object_unref(layout);
    if (cell->row == 0)
      aligns[cell->column] = cell->align;
  }
  table->content = preview_content_width(r->p);
  int room = table->content - table->level * INDENT - 1;
  for (int c = 0; c < columns; c++)
    table->natural += natural[c];
  for (int c = 0; c < columns; c++) /* a word longer than half the text breaks */
    least[c] = MIN(least[c], MAX(room / 2, 2 * CELL_PAD + 1));
  share_widths(natural, least, widths, (guint)columns, room);
  table->edges = g_new0(int, columns + 1);
  for (int c = 0; c < columns; c++)
    table->edges[c + 1] = table->edges[c] + widths[c];

  GtkTextTag *tag = table_tag(r, table, aligns);
  gboolean lead = aligns[0] == MD_ALIGN_CENTER || aligns[0] == MD_ALIGN_RIGHT;
  int start = preview_end_offset(r);
  table->rows = g_array_new(FALSE, FALSE, sizeof(int));
  for (int row = 0, i = 0; row < rows; row++) {
    GArray **lines = g_new0(GArray *, columns);
    const TableCell **in_row = g_new0(const TableCell *, columns);
    guint height = 1;
    for (; i < (int)cells->len && g_array_index(cells, TableCell, i).row == row; i++) {
      const TableCell *cell = &g_array_index(cells, TableCell, i);
      in_row[cell->column] = cell;
      lines[cell->column] = cell_lines(r, cell, widths[cell->column] - 2 * CELL_PAD);
      height = MAX(height, lines[cell->column]->len / 2);
    }
    int row_start = preview_end_offset(r);
    g_array_append_val(table->rows, row_start);
    for (guint k = 0; k < height; k++) {
      int line_start = preview_end_offset(r);
      if (lead)
        preview_append(r, "\t", 1);
      for (int c = 0; c < columns; c++) {
        if (c > 0)
          preview_append(r, "\t", 1);
        if (in_row[c] && k < lines[c]->len / 2)
          emit_piece(r, in_row[c], g_array_index(lines[c], int, 2 * k), g_array_index(lines[c], int, 2 * k + 1));
      }
      preview_append(r, "\n", 1);
      if (k == 0)
        preview_tag(r, "tr-first", NULL, line_start);
      if (k == height - 1)
        preview_tag(r, "tr-last", NULL, line_start);
    }
    preview_tag(r, NULL, tag, row_start);
    for (int c = 0; c < columns; c++)
      if (lines[c])
        g_array_free(lines[c], TRUE);
    g_free(lines);
    g_free(in_row);
  }
  int end = preview_end_offset(r);
  g_array_append_val(table->rows, end);
  table->tag = tag;
  table->start = start;
  g_array_free(g_steal_pointer(&table->cells), TRUE);
  g_ptr_array_add(r->p->tables, table);
  g_free(aligns);
  g_free(widths);
  g_free(least);
  g_free(natural);
}

gboolean preview_tables_fit(Preview *p, int content)
{
  for (guint i = 0; i < p->tables->len; i++) {
    const PreviewTable *table = g_ptr_array_index(p->tables, i);
    int room = table->level * INDENT + 1;
    /* laid out for another width, and it wraps (or wrapped) in one of them */
    if (table->content != content && (table->natural + room > content || table->natural + room > table->content))
      return FALSE;
  }
  return TRUE;
}
