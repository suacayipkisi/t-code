/*
 * Markdown preview (Ctrl+Shift+V, or "Open Preview" in the file manager).
 * md4c parses the file (GitHub flavor: tables, task lists, strikethrough, autolinks) and its blocks
 * and spans become text with tags in one read-only GtkTextView: no browser engine, so a preview
 * costs about what its text does. Code blocks are colored with GtkSourceView's language definitions
 * and the editor's style scheme, local images are shown, links open (anchors scroll to their
 * heading, Markdown files open as previews, other files in an editor, the rest in the browser).
 * The preview follows the file as it is saved, keeping its scroll position.
 */
#include "editor/preview.h"

#include "editor/editor.h"
#include "editor/editorview.h"
#include "editor/tabs.h"
#include "window/terminal.h"
#include "window/palette.h"

#include <md4c.h>
#include <stdlib.h>
#include <string.h>

#define BODY_POINTS 10.5      /* the text at text size 1: 14 px */
#define MARGIN 32             /* minimum pixels left and right of the text */
#define MAX_CONTENT_WIDTH 840 /* maximum comfortable reading width */
#define INDENT 24             /* pixels per list or quote level */
#define MAX_LEVEL 8
#define CELL_PAD 10 /* pixels left and right of a table cell's text (its CSS padding) */
#define PRE_PAD 14  /* pixels between a code block's box and its text */
#define BAR_WIDTH 4 /* a quote's bar left of it */

typedef struct {
  int start, end;
  char *url;
} Link;

typedef struct {
  int start, end;
} CodeSpan;

/* What snapshot_layer draws below a block's text, within the text's column. */
typedef enum { BLOCK_QUOTE, BLOCK_PRE, BLOCK_RULE, BLOCK_HR } BlockKind;

typedef struct {
  BlockKind kind;
  int start, end; /* the block's text: a heading's for the rule under it */
  int level;      /* lists and quotes around it */
} Block;

/* A table: a grid of wrapping labels in the text, its columns sized to the view's width. */
typedef struct {
  GtkWidget *grid;
  GArray *natural; /* int per column: its widest cell on one line, padding included */
} PreviewTable;

struct Preview {
  Tab *tab;
  GtkTextView *view;
  GtkTextBuffer *buffer;
  GArray *links;       /* Link, in the order of the text */
  GArray *code_spans;  /* CodeSpan: inline code spans for pill background */
  GArray *blocks;      /* Block: quotes, code blocks, the rules under h1 / h2, horizontal rules */
  GHashTable *anchors; /* heading slug -> its offset */
  GHashTable *colors;  /* a code coloring ("fg|weight|style") -> its tag, made once per preview */
  GPtrArray *tables;   /* PreviewTable: the tables shown, sized to the view's width */
  GPtrArray *pictures; /* the images shown, sized to the view's width */
  int width;           /* the view's width the images were sized for */
  double scale;        /* the previews' text size (t->preview_scale) */
  gboolean over_link;
  int hovered; /* the link under the pointer (its index in links, underlined), or -1 */
  GFileMonitor *monitor;
  guint reload_id;
};

/* One rendering: md4c's callbacks append to the end of the buffer. Tags are applied when a block or
 * span ends, from where it started (starts) to the end. */
typedef struct {
  Preview *p;
  char *dir;                /* the file's folder: relative links and images are in it */
  GArray *starts;           /* int: where each open block and span started */
  GArray *images;           /* gboolean: per open image, whether it is shown (then its alt text is not) */
  GPtrArray *hrefs;         /* per open link, its target */
  int level;                /* lists and quotes around the text */
  int lists[MAX_LEVEL + 1]; /* per open list: the next number, or 0 for bullets */
  gboolean tight[MAX_LEVEL + 1];
  int nlists;
  gboolean in_item_start; /* at the start of a list item before its first block/text */
  gboolean in_item_p;     /* inside the first paragraph of a list item */
  PreviewTable *table;    /* inside a table */
  GString *markup;        /* inside a table cell: its text as Pango markup */
  GPtrArray *closers;     /* per span open in a cell, the markup that ends it */
  int row, cell;          /* the cell in the table */
  MD_ALIGN align;         /* its column's alignment */
  GString *code;          /* inside a code block: its text */
  char *lang;
  int hidden;          /* inside a shown image: its alt text is not added */
  GArray *smalls;      /* int: where each open <sub>, <sup> or <small> started */
  gboolean html_block; /* inside an HTML block: its lines flow like a paragraph's */
} Render;

static const char *const css =
    ".tcode-preview, .tcode-preview text { background: var(--tc-panel); color: var(--tc-fg); }"
    ".tcode-preview-table { border: 1px solid var(--tc-border-strong); }"
    ".tcode-preview-table label { padding: 6px 10px; color: var(--tc-fg); }"
    ".tcode-preview-table label.head { background: var(--tc-border); color: var(--tc-fg-strong); }"
    ".tcode-preview-table label.odd { background: var(--tc-hover); }"
    ".tcode-preview-table label link { color: var(--tc-link); }";

gboolean is_markdown(const char *path)
{
  static const char *const extensions[] = { ".md", ".markdown", ".mdown", ".mkd", NULL };
  const char *ext = strrchr(path, '.');

  for (int i = 0; ext && extensions[i]; i++)
    if (g_ascii_strcasecmp(ext, extensions[i]) == 0)
      return TRUE;
  return FALSE;
}

/* Tags -------------------------------------------------------------------------------------- */

/* The tags' colors, from the color theme (again when it changes). */
static void color_tags(GtkTextBuffer *b)
{
  static const struct {
    const char *tag, *property;
    PalColor color;
  } colors[] = {
    { "base", "foreground", PAL_FG },        { "quote", "foreground", PAL_FG_DIM },
    { "h1", "foreground", PAL_FG_STRONG },   { "h2", "foreground", PAL_FG_STRONG },
    { "h3", "foreground", PAL_FG_BRIGHT },   { "h4", "foreground", PAL_FG_BRIGHT },
    { "h5", "foreground", PAL_FG_BRIGHT },   { "h6", "foreground", PAL_FG_DIM },
    { "bold", "foreground", PAL_FG_STRONG }, { "code", "foreground", PAL_FG_STRONG },
    { "dim", "foreground", PAL_FG_DIM },     { "error", "foreground", PAL_ERROR },
    { "link", "foreground", PAL_LINK },
  };
  GtkTextTagTable *table = gtk_text_buffer_get_tag_table(b);

  for (size_t i = 0; i < G_N_ELEMENTS(colors); i++) {
    GtkTextTag *tag = gtk_text_tag_table_lookup(table, colors[i].tag);
    if (tag)
      g_object_set(tag, colors[i].property, palette_hex(colors[i].color), NULL);
  }
}

/* The tags every rendering uses, made with the buffer. Later tags win where they set the same
 * thing, so the order matters: the base first, deeper indents after shallower ones. */
static void create_tags(GtkTextBuffer *b, const char *mono)
{
  static const double heading_scale[] = { 1.85, 1.45, 1.25, 1.1, 1.0, 0.9 };
  char name[16];

  gtk_text_buffer_create_tag(b, "base", NULL);
  gtk_text_buffer_create_tag(b, "indent0", "indent", 0, NULL);
  for (int i = 1; i <= MAX_LEVEL; i++) {
    g_snprintf(name, sizeof name, "indent%d", i);
    gtk_text_buffer_create_tag(b, name, "left-margin", MARGIN + i * INDENT, "indent", 0, NULL);
  }
  for (int i = 1; i <= MAX_LEVEL; i++) {
    g_snprintf(name, sizeof name, "li%d", i);
    gtk_text_buffer_create_tag(b, name, "left-margin", MARGIN + i * INDENT, "indent", -INDENT, NULL);
  }
  gtk_text_buffer_create_tag(b, "tight", "pixels-below-lines", 3, NULL);
  gtk_text_buffer_create_tag(b, "gap", "pixels-below-lines", 14, NULL);
  gtk_text_buffer_create_tag(b, "spacer", "size-points", 4.0, NULL);
  gtk_text_buffer_create_tag(b, "quote", NULL);
  for (int i = 0; i < 6; i++) {
    g_snprintf(name, sizeof name, "h%d", i + 1);
    gtk_text_buffer_create_tag(b, name, "weight", PANGO_WEIGHT_SEMIBOLD, "scale", heading_scale[i],
                               "pixels-above-lines", i < 2 ? 24 : 16, "pixels-below-lines", i < 2 ? 16 : 8, NULL);
  }
  gtk_text_buffer_create_tag(b, "bold", "weight", PANGO_WEIGHT_BOLD, NULL);
  gtk_text_buffer_create_tag(b, "italic", "style", PANGO_STYLE_ITALIC, NULL);
  gtk_text_buffer_create_tag(b, "strike", "strikethrough", TRUE, NULL);
  gtk_text_buffer_create_tag(b, "underline", "underline", PANGO_UNDERLINE_SINGLE, NULL);
  gtk_text_buffer_create_tag(b, "code", "family", mono, "scale", 0.90, NULL);
  /* Wrapped: a line wider than the view would scroll the whole preview sideways. */
  gtk_text_buffer_create_tag(b, "pre", "family", mono, "scale", 0.92, "wrap-mode", GTK_WRAP_WORD_CHAR, NULL);
  for (int i = 0; i <= MAX_LEVEL; i++) { /* a code block's text, inside its box */
    g_snprintf(name, sizeof name, "pre%d", i);
    gtk_text_buffer_create_tag(b, name, "left-margin", MARGIN + i * INDENT + PRE_PAD, "right-margin", MARGIN + PRE_PAD,
                               NULL);
  }
  gtk_text_buffer_create_tag(b, "pre-top", "pixels-above-lines", 12, NULL);
  gtk_text_buffer_create_tag(b, "pre-bottom", "pixels-below-lines", 12, NULL);
  gtk_text_buffer_create_tag(b, "hr", "size-points", 1.0, NULL);
  gtk_text_buffer_create_tag(b, "dim", "style", PANGO_STYLE_ITALIC, NULL);
  gtk_text_buffer_create_tag(b, "error", NULL);
  gtk_text_buffer_create_tag(b, "small", "scale", 0.85, NULL);
  gtk_text_buffer_create_tag(b, "link", NULL);
  gtk_text_buffer_create_tag(b, "link-hover", "underline", PANGO_UNDERLINE_SINGLE, NULL);
  color_tags(b);
}

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
  Block block = { kind, start, end_offset(r), level };

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

/* An HTML entity (&amp;, &#169;, &#xA9;), as the text it stands for; the caller frees it. */
static char *decode_entity(const char *text, gsize size)
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
  char *decoded = decode_entity(text, size);

  append(r, decoded, -1);
  g_free(decoded);
}

/* Images ------------------------------------------------------------------------------------- */

/* The local file a link or image points to (relative to the Markdown file), or NULL. */
static char *local_path(Render *r, const char *target)
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
                                                                               : g_build_filename(r->dir, plain, NULL);
  g_free(plain);
  return path;
}

/* The width an image may have in a view this wide. */
static int image_width(GtkWidget *picture, int max_width)
{
  int natural = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(picture), "t-code-width"));

  return max_width > 0 ? MAX(MIN(natural, max_width), 16) : 16;
}

static void size_picture(GtkWidget *picture, int max_width)
{
  GdkPaintable *paintable = gtk_picture_get_paintable(GTK_PICTURE(picture));
  int w = image_width(picture, max_width);
  int pw = gdk_paintable_get_intrinsic_width(paintable), ph = gdk_paintable_get_intrinsic_height(paintable);

  gtk_widget_set_size_request(picture, w, pw > 0 ? w * ph / pw : w);
}

/* The margin left and right of the text in a view this wide: the text is a centered column. */
static int side_margin(Preview *p, int width)
{
  int max_w = (int)(MAX_CONTENT_WIDTH * p->scale);

  return width > max_w + 2 * MARGIN ? (width - max_w) / 2 : MARGIN;
}

/* The width the text has: images and tables are sized to it. */
static int content_width(Preview *p)
{
  int width = p->width > 0 ? p->width : gtk_widget_get_width(GTK_WIDGET(p->view));

  return width > 0 ? width - 2 * side_margin(p, width) : (int)(MAX_CONTENT_WIDTH * p->scale);
}

static void size_table(PreviewTable *table, int width);

/* Adjusts left and right margins to center content on wide displays and sizes images and tables. */
static void update_layout(Preview *p, int width)
{
  if (width <= 0)
    width = gtk_widget_get_width(GTK_WIDGET(p->view));
  if (width <= 0)
    return;
  p->width = width;

  int margin = side_margin(p, width);

  gtk_text_view_set_left_margin(p->view, margin);
  gtk_text_view_set_right_margin(p->view, margin);

  GtkTextTagTable *table = gtk_text_buffer_get_tag_table(p->buffer);
  for (int i = 1; i <= MAX_LEVEL; i++) {
    char name[16];
    g_snprintf(name, sizeof name, "indent%d", i);
    GtkTextTag *tag = gtk_text_tag_table_lookup(table, name);
    if (tag)
      g_object_set(tag, "left-margin", margin + i * INDENT, NULL);

    g_snprintf(name, sizeof name, "li%d", i);
    tag = gtk_text_tag_table_lookup(table, name);
    if (tag)
      g_object_set(tag, "left-margin", margin + i * INDENT, NULL);
  }
  for (int i = 0; i <= MAX_LEVEL; i++) {
    char name[16];
    g_snprintf(name, sizeof name, "pre%d", i);
    GtkTextTag *tag = gtk_text_tag_table_lookup(table, name);
    if (tag)
      g_object_set(tag, "left-margin", margin + i * INDENT + PRE_PAD, "right-margin", margin + PRE_PAD, NULL);
  }

  int content_w = width - 2 * margin;
  for (guint i = 0; i < p->pictures->len; i++)
    size_picture(g_ptr_array_index(p->pictures, i), content_w);
  for (guint i = 0; i < p->tables->len; i++)
    size_table(g_ptr_array_index(p->tables, i), content_w);
}

/* Shows the image at src (width: wanted width in pixels, 0 for its own); FALSE when it is not a
 * local image that loads. */
static gboolean append_image(Render *r, const char *src, int width)
{
  char *path = local_path(r, src);
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
  size_picture(picture, content_width(r->p));
  gtk_text_buffer_get_end_iter(r->p->buffer, &end);
  anchor = gtk_text_buffer_create_child_anchor(r->p->buffer, &end);
  gtk_text_view_add_child_at_anchor(r->p->view, picture, anchor);
  g_ptr_array_add(r->p->pictures, picture);
  g_object_unref(texture);
  return TRUE;
}

/* The view subclass that centers content and adapts on any size allocation */
#define TCODE_TYPE_PREVIEW_VIEW (tcode_preview_view_get_type())
G_DECLARE_FINAL_TYPE(TcodePreviewView, tcode_preview_view, TCODE, PREVIEW_VIEW, GtkTextView)

struct _TcodePreviewView {
  GtkTextView parent;
  Preview *preview;
};

G_DEFINE_FINAL_TYPE(TcodePreviewView, tcode_preview_view, GTK_TYPE_TEXT_VIEW)

static void tcode_preview_view_size_allocate(GtkWidget *widget, int width, int height, int baseline)
{
  TcodePreviewView *self = TCODE_PREVIEW_VIEW(widget);

  GTK_WIDGET_CLASS(tcode_preview_view_parent_class)->size_allocate(widget, width, height, baseline);

  if (self->preview && width > 0 && width != self->preview->width) {
    update_layout(self->preview, width);
  }
}

/* The buffer lines from first to last (the line after the end of a block that ends with its line
 * break is not its). */
static void block_lines(GtkTextBuffer *buffer, int start, int end, int *first, int *last)
{
  GtkTextIter from, to;

  gtk_text_buffer_get_iter_at_offset(buffer, &from, start);
  gtk_text_buffer_get_iter_at_offset(buffer, &to, end);
  if (end > start && gtk_text_iter_starts_line(&to))
    gtk_text_iter_backward_char(&to);
  *first = gtk_text_iter_get_line(&from);
  *last = gtk_text_iter_get_line(&to);
}

static void fill(GtkSnapshot *snapshot, const GdkRGBA *color, float x, float y, float w, float h, float radius)
{
  graphene_rect_t rect = GRAPHENE_RECT_INIT(x, y, w, h);
  GskRoundedRect rounded;

  if (w <= 0 || h <= 0)
    return;
  gsk_rounded_rect_init_from_rect(&rounded, &rect, radius);
  gtk_snapshot_push_rounded_clip(snapshot, &rounded);
  gtk_snapshot_append_color(snapshot, color, &rect);
  gtk_snapshot_pop(snapshot);
}

/* Quotes (a bar and a light background), code blocks (a box), the line under h1 and h2 and horizontal
 * rules: within the text's column, not to the view's edge. */
static void draw_blocks(GtkTextView *view, Preview *p, GtkSnapshot *snapshot, int first_line, int last_line)
{
  int margin = side_margin(p, p->width), right = p->width - margin;
  GdkRGBA bar = *palette_rgba(PAL_ACCENT), quote = *palette_rgba(PAL_FG);
  GdkRGBA box = *palette_rgba(PAL_FRAME), rule = *palette_rgba(PAL_BORDER), hr = *palette_rgba(PAL_BORDER_STRONG);

  bar.alpha = 0.5f;
  quote.alpha = 0.05f;
  for (guint i = 0; i < p->blocks->len; i++) {
    const Block *block = &g_array_index(p->blocks, Block, i);
    GtkTextIter iter;
    int first, last, top, bottom, height, left = margin + block->level * INDENT;

    block_lines(p->buffer, block->start, block->end, &first, &last);
    if (last < first_line || first > last_line)
      continue;
    gtk_text_buffer_get_iter_at_line(p->buffer, &iter, first);
    gtk_text_view_get_line_yrange(view, &iter, &top, &height);
    gtk_text_buffer_get_iter_at_line(p->buffer, &iter, last);
    gtk_text_view_get_line_yrange(view, &iter, &bottom, &height);
    bottom += height;
    switch (block->kind) {
      case BLOCK_QUOTE: /* its last line has a paragraph's gap below it: most of it is outside */
        fill(snapshot, &quote, left, top - 6, right - left, bottom - top - 2, 0);
        fill(snapshot, &bar, left, top - 6, BAR_WIDTH, bottom - top - 2, 0);
        break;
      case BLOCK_PRE: fill(snapshot, &box, left, top, right - left, bottom - top, 6); break;
      case BLOCK_RULE: fill(snapshot, &rule, left, bottom - 8, right - left, 1, 0); break;
      case BLOCK_HR: fill(snapshot, &hr, left, (top + bottom) / 2 - 1, right - left, 2, 0); break;
    }
  }
}

/* Inline code's background, a rectangle per row it takes on screen (it may wrap). */
static void draw_code_spans(GtkTextView *view, Preview *p, GtkSnapshot *snapshot, int first_line, int last_line)
{
  GdkRGBA bg = *palette_rgba(PAL_BORDER_STRONG);

  bg.alpha = 0.75f;
  for (guint i = 0; i < p->code_spans->len; i++) {
    const CodeSpan *span = &g_array_index(p->code_spans, CodeSpan, i);
    GtkTextIter iter, end;
    int first, last;
    GdkRectangle row = { 0 }, at;

    block_lines(p->buffer, span->start, span->end, &first, &last);
    if (last < first_line || first > last_line)
      continue;
    gtk_text_buffer_get_iter_at_offset(p->buffer, &iter, span->start);
    gtk_text_buffer_get_iter_at_offset(p->buffer, &end, span->end);
    for (; gtk_text_iter_compare(&iter, &end) < 0; gtk_text_iter_forward_char(&iter)) {
      gtk_text_view_get_iter_location(view, &iter, &at);
      if (row.width > 0 && at.y == row.y) {
        row.width = at.x + at.width - row.x;
        continue;
      }
      if (row.width > 0) /* a little above and below the letters */
        fill(snapshot, &bg, row.x, row.y - 2, row.width, row.height + 4, 4);
      row = at;
    }
    if (row.width > 0)
      fill(snapshot, &bg, row.x, row.y - 2, row.width, row.height + 4, 4);
  }
}

static void tcode_preview_view_snapshot_layer(GtkTextView *view, GtkTextViewLayer layer, GtkSnapshot *snapshot)
{
  TcodePreviewView *self = TCODE_PREVIEW_VIEW(view);

  if (layer == GTK_TEXT_VIEW_LAYER_BELOW_TEXT && self->preview && self->preview->code_spans) {
    GdkRectangle visible;
    GtkTextIter iter;

    gtk_text_view_get_visible_rect(view, &visible);
    gtk_text_view_get_line_at_y(view, &iter, visible.y, NULL);
    int first_line = gtk_text_iter_get_line(&iter);
    gtk_text_view_get_line_at_y(view, &iter, visible.y + visible.height, NULL);
    int last_line = gtk_text_iter_get_line(&iter);
    draw_blocks(view, self->preview, snapshot, first_line, last_line);
    draw_code_spans(view, self->preview, snapshot, first_line, last_line);
  }

  if (GTK_TEXT_VIEW_CLASS(tcode_preview_view_parent_class)->snapshot_layer)
    GTK_TEXT_VIEW_CLASS(tcode_preview_view_parent_class)->snapshot_layer(view, layer, snapshot);
}

static void tcode_preview_view_class_init(TcodePreviewViewClass *klass)
{
  GTK_WIDGET_CLASS(klass)->size_allocate = tcode_preview_view_size_allocate;
  GTK_TEXT_VIEW_CLASS(klass)->snapshot_layer = tcode_preview_view_snapshot_layer;
}

static void tcode_preview_view_init(TcodePreviewView *self G_GNUC_UNUSED)
{
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

/* Tables ------------------------------------------------------------------------------------- */

static void follow(Preview *p, const char *url);

static void table_free(gpointer data)
{
  PreviewTable *table = data;

  g_array_free(table->natural, TRUE);
  g_free(table);
}

/* Shares width between the columns like a browser: when they all fit, each gets its widest cell;
 * else the columns narrower than an even share keep theirs and the others split the rest, their
 * cells wrapping. */
static void size_table(PreviewTable *table, int width)
{
  guint n = table->natural->len;
  int *widths = g_new0(int, n), remaining = MAX(width - 2, 1), left = (int)n; /* inside its 1px border */
  gboolean *done = g_new0(gboolean, n);

  while (left > 0) {
    int share = remaining / left;
    gboolean fixed = FALSE;
    for (guint c = 0; c < n; c++)
      if (!done[c] && g_array_index(table->natural, int, c) <= share) {
        widths[c] = g_array_index(table->natural, int, c);
        remaining -= widths[c];
        done[c] = TRUE;
        left--;
        fixed = TRUE;
      }
    if (!fixed) {
      for (guint c = 0; c < n; c++)
        if (!done[c])
          widths[c] = share;
      break;
    }
  }
  for (GtkWidget *cell = gtk_widget_get_first_child(table->grid); cell; cell = gtk_widget_get_next_sibling(cell)) {
    int column;
    gtk_grid_query_child(GTK_GRID(table->grid), cell, &column, NULL, NULL, NULL);
    gtk_widget_set_size_request(cell, MAX(widths[column], 1), -1); /* its CSS padding included */
  }
  g_free(done);
  g_free(widths);
}

/* A link in a table cell: followed like the others. */
static gboolean on_cell_link(GtkLabel *label G_GNUC_UNUSED, const char *uri, Preview *p)
{
  char *url = g_strdup(uri); /* following it may show the file again, and free the label */

  follow(p, url);
  g_free(url);
  return TRUE;
}

static gboolean link_to_span(const GMatchInfo *match, GString *result, gpointer data G_GNUC_UNUSED)
{
  char *tag = g_match_info_fetch(match, 0);

  g_string_append(result, tag[1] == '/' ? "</span>" : "<span>");
  g_free(tag);
  return FALSE;
}

/* A table cell ended: its markup becomes a wrapping label in the grid. */
static void add_cell(Render *r, gboolean head)
{
  static const float xalign[] = { 0.0f, 0.0f, 0.5f, 1.0f }; /* MD_ALIGN_DEFAULT .. RIGHT */
  GtkWidget *label = gtk_label_new(NULL);
  GArray *natural = r->table->natural;
  char size[G_ASCII_DTOSTR_BUF_SIZE];
  char *markup = g_strdup_printf("<span size=\"%spt\" insert_hyphens=\"false\">%s%s%s</span>",
                                 g_ascii_dtostr(size, sizeof size, BODY_POINTS * r->p->scale), head ? "<b>" : "",
                                 r->markup->str, head ? "</b>" : "");
  PangoLayout *layout = gtk_widget_create_pango_layout(GTK_WIDGET(r->p->view), NULL); /* in the window's font */
  int width = 0;

  static GRegex *links;
  if (!links) /* <a> is GtkLabel's, not Pango's: measured as a <span> */
    links = g_regex_new("<a href=\"[^\"]*\">|</a>", G_REGEX_OPTIMIZE, 0, NULL);
  char *measured = g_regex_replace_eval(links, markup, -1, 0, 0, link_to_span, NULL, NULL);
  pango_layout_set_markup(layout, measured, -1);
  g_free(measured);
  pango_layout_get_pixel_size(layout, &width, NULL); /* on one line */
  g_object_unref(layout);
  gtk_label_set_markup(GTK_LABEL(label), markup);
  gtk_label_set_wrap(GTK_LABEL(label), TRUE);
  gtk_label_set_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
  gtk_label_set_max_width_chars(GTK_LABEL(label), 1); /* its width is the one size_table() gives it */
  gtk_label_set_xalign(GTK_LABEL(label), xalign[r->align]);
  gtk_label_set_yalign(GTK_LABEL(label), 0.0f);
  gtk_widget_add_css_class(label, head ? "head" : r->row % 2 ? "odd" : "even");
  g_signal_connect(label, "activate-link", G_CALLBACK(on_cell_link), r->p);
  if ((guint)r->cell >= natural->len)
    g_array_set_size(natural, r->cell + 1);
  g_array_index(natural, int, r->cell) = MAX(g_array_index(natural, int, r->cell), width + 2 * CELL_PAD + 2);
  gtk_grid_attach(GTK_GRID(r->table->grid), label, r->cell, r->row, 1, 1);
  g_free(markup);
  g_string_free(g_steal_pointer(&r->markup), TRUE);
}

/* Text in a table cell, as markup. */
static void cell_text(Render *r, MD_TEXTTYPE type, const char *text, gsize size)
{
  char *plain = NULL, *escaped;

  switch (type) {
    case MD_TEXT_NULLCHAR: g_string_append(r->markup, "\uFFFD"); return;
    case MD_TEXT_BR: g_string_append_c(r->markup, '\n'); return;
    case MD_TEXT_SOFTBR: g_string_append_c(r->markup, ' '); return;
    case MD_TEXT_HTML: /* tags are left out, but <br> */
      if (g_ascii_strncasecmp(text, "<br", MIN(size, 3)) == 0 && size >= 3)
        g_string_append_c(r->markup, '\n');
      return;
    case MD_TEXT_ENTITY: plain = decode_entity(text, size); break;
    default: plain = g_strndup(text, size); break;
  }
  escaped = g_markup_escape_text(plain, -1);
  g_string_append(r->markup, escaped);
  g_free(escaped);
  g_free(plain);
}

/* Whether a link is open in the table cell. */
static gboolean in_cell_link(Render *r)
{
  for (guint i = 0; i < r->closers->len; i++)
    if (strcmp(g_ptr_array_index(r->closers, i), "</a>") == 0)
      return TRUE;
  return FALSE;
}

/* A span opens in a table cell: its markup, and what closes it. */
static void cell_span(Render *r, MD_SPANTYPE type, void *detail)
{
  const char *open = "", *close = "";
  char *link = NULL;

  switch (type) {
    case MD_SPAN_EM: open = "<i>", close = "</i>"; break;
    case MD_SPAN_STRONG: open = "<b>", close = "</b>"; break;
    case MD_SPAN_DEL: open = "<s>", close = "</s>"; break;
    case MD_SPAN_U: open = "<u>", close = "</u>"; break;
    case MD_SPAN_CODE:
    case MD_SPAN_LATEXMATH:
    case MD_SPAN_LATEXMATH_DISPLAY:
      if (in_cell_link(r)) /* a foreground would win over the link's color */
        link = g_strdup_printf("<span font_family=\"monospace\" background=\"%s\">", palette_hex(PAL_BORDER_STRONG));
      else
        link = g_strdup_printf("<span font_family=\"monospace\" background=\"%s\" foreground=\"%s\">",
                               palette_hex(PAL_BORDER_STRONG), palette_hex(PAL_FG_STRONG));
      open = link;
      close = "</span>";
      break;
    case MD_SPAN_A: {
      MD_SPAN_A_DETAIL *a = detail;
      char *href = g_strndup(a->href.text, a->href.size);
      link = g_markup_printf_escaped("<a href=\"%s\">", href);
      g_free(href);
      open = link, close = "</a>";
      break;
    }
    case MD_SPAN_IMG: /* its alt text */
      link = g_strdup_printf("<span foreground=\"%s\">", palette_hex(PAL_FG_DIM));
      open = link, close = "</span>";
      break;
    default: break;
  }
  g_string_append(r->markup, open);
  g_ptr_array_add(r->closers, (gpointer)close);
  g_free(link);
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
    case MD_BLOCK_QUOTE:
      begin_block(r);
      push_start(r);
      r->level++;
      break;
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
      r->table = g_new0(PreviewTable, 1);
      r->table->natural = g_array_new(FALSE, TRUE, sizeof(int));
      r->table->grid = gtk_grid_new();
      gtk_widget_add_css_class(r->table->grid, "tcode-preview-table");
      r->row = -1;
      break;
    case MD_BLOCK_TR:
      r->row++;
      r->cell = 0;
      break;
    case MD_BLOCK_TH:
    case MD_BLOCK_TD:
      if (r->table) {
        r->markup = g_string_new(NULL);
        r->align = ((MD_BLOCK_TD_DETAIL *)detail)->align;
      }
      break;
    case MD_BLOCK_HTML:
      r->html_block = TRUE;
      begin_block(r);
      push_start(r);
      break;
    case MD_BLOCK_H:
      begin_block(r);
      push_start(r);
      break;
    case MD_BLOCK_P:
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
      int start = pop_start(r);
      tag_from(r, "quote", NULL, start);
      add_block(r, BLOCK_QUOTE, start, MIN(r->level, MAX_LEVEL) - 1); /* the bar left of its text */
      r->level--;
      tag_last_line(r, "gap");
      break;
    case MD_BLOCK_CODE: {
      int start = pop_start(r);
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
      if (r->markup) {
        add_cell(r, type == MD_BLOCK_TH);
        r->cell++;
      }
      break;
    case MD_BLOCK_TABLE: {
      GtkTextIter end;
      gtk_text_buffer_get_end_iter(r->p->buffer, &end);
      gtk_text_view_add_child_at_anchor(r->p->view, r->table->grid,
                                        gtk_text_buffer_create_child_anchor(r->p->buffer, &end));
      g_ptr_array_add(r->p->tables, r->table);
      size_table(g_steal_pointer(&r->table), content_width(r->p));
      append(r, "\n", 1);
      tag_last_line(r, "gap");
      break;
    }
    default: break;
  }
  return 0;
}

static int enter_span(MD_SPANTYPE type, void *detail, void *data)
{
  Render *r = data;

  if (r->markup) {
    cell_span(r, type, detail);
    return 0;
  }
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
    if (r->closers->len)
      g_string_append(r->markup, g_ptr_array_steal_index(r->closers, r->closers->len - 1));
    return 0;
  }
  int start = pop_start(r);

  switch (type) {
    case MD_SPAN_EM: tag_from(r, "italic", NULL, start); break;
    case MD_SPAN_STRONG: tag_from(r, "bold", NULL, start); break;
    case MD_SPAN_DEL: tag_from(r, "strike", NULL, start); break;
    case MD_SPAN_U: tag_from(r, "underline", NULL, start); break;
    case MD_SPAN_CODE:
    case MD_SPAN_LATEXMATH:
    case MD_SPAN_LATEXMATH_DISPLAY: {
      if (type == MD_SPAN_CODE)
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
    cell_text(r, type, text, size);
    return 0;
  }
  if (r->hidden)
    return 0;
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

/* Rendering ---------------------------------------------------------------------------------- */

static void clear_links(Preview *p)
{
  for (guint i = 0; i < p->links->len; i++)
    g_free(g_array_index(p->links, Link, i).url);
  g_array_set_size(p->links, 0);
}

/* Shows the file again from the top line it showed before. */
static void reload(Preview *p)
{
  GtkTextTagTable *table = gtk_text_buffer_get_tag_table(p->buffer);
  MD_PARSER parser = { 0, MD_DIALECT_GITHUB, enter_block, leave_block, enter_span, leave_span, on_text, NULL, NULL };
  Render r = { .p = p };
  GdkRectangle visible;
  GtkTextIter iter, end;
  int top_line = -1;
  char *text = NULL;
  gsize len = 0;
  GError *error = NULL;

  if (gtk_text_buffer_get_char_count(p->buffer) > 0) {
    gtk_text_view_get_visible_rect(p->view, &visible);
    gtk_text_view_get_line_at_y(p->view, &iter, visible.y, NULL);
    top_line = gtk_text_iter_get_line(&iter);
  }
  g_ptr_array_set_size(p->tables, 0);
  g_ptr_array_set_size(p->pictures, 0);
  g_array_set_size(p->code_spans, 0);
  g_array_set_size(p->blocks, 0);
  clear_links(p);
  p->hovered = -1;
  g_hash_table_remove_all(p->anchors);
  gtk_text_buffer_set_text(p->buffer, "", 0);
  g_object_set(gtk_text_tag_table_lookup(table, "base"), "size-points", BODY_POINTS * p->scale, NULL);

  r.dir = g_path_get_dirname(p->tab->path);
  r.starts = g_array_new(FALSE, FALSE, sizeof(int));
  r.images = g_array_new(FALSE, FALSE, sizeof(gboolean));
  r.hrefs = g_ptr_array_new_with_free_func(g_free);
  r.closers = g_ptr_array_new();
  r.smalls = g_array_new(FALSE, FALSE, sizeof(int));
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
  if (r.markup) /* md4c stopped inside a table */
    g_string_free(r.markup, TRUE);
  if (r.table) {
    g_object_ref_sink(r.table->grid);
    g_object_unref(r.table->grid);
    table_free(r.table);
  }

  gtk_text_buffer_get_bounds(p->buffer, &iter, &end);
  gtk_text_buffer_apply_tag_by_name(p->buffer, "base", &iter, &end);
  gtk_text_buffer_place_cursor(p->buffer, &iter);
  if (p->width <= 0)
    p->width = gtk_widget_get_width(GTK_WIDGET(p->view));
  if (p->width > 0)
    update_layout(p, p->width);
  if (top_line > 0) {
    gtk_text_buffer_get_iter_at_line(p->buffer, &iter, top_line);
    GtkTextMark *mark = gtk_text_buffer_get_mark(p->buffer, "t-code-top");
    if (mark)
      gtk_text_buffer_move_mark(p->buffer, mark, &iter);
    else
      mark = gtk_text_buffer_create_mark(p->buffer, "t-code-top", &iter, TRUE);
    gtk_text_view_scroll_to_mark(p->view, mark, 0, TRUE, 0, 0);
  }
}

/* on_file_changed()'s timeout. */
static gboolean reload_now(gpointer data)
{
  Preview *p = data;

  p->reload_id = 0;
  reload(p);
  return G_SOURCE_REMOVE;
}

/* The file was saved: show it again shortly, once for a burst of changes. */
static void on_file_changed(GFileMonitor *monitor G_GNUC_UNUSED, GFile *file G_GNUC_UNUSED, GFile *other G_GNUC_UNUSED,
                            GFileMonitorEvent event, Preview *p)
{
  if (event == G_FILE_MONITOR_EVENT_DELETED || event == G_FILE_MONITOR_EVENT_ATTRIBUTE_CHANGED)
    return;
  if (p->reload_id)
    g_source_remove(p->reload_id);
  p->reload_id = g_timeout_add(150, reload_now, p);
}

/* Links -------------------------------------------------------------------------------------- */

/* The link at a point of the view, or NULL. */
static const Link *link_at(Preview *p, double x, double y)
{
  GtkTextIter iter;
  int bx, by;

  gtk_text_view_window_to_buffer_coords(p->view, GTK_TEXT_WINDOW_WIDGET, (int)x, (int)y, &bx, &by);
  if (!gtk_text_view_get_iter_at_location(p->view, &iter, bx, by) ||
      !gtk_text_iter_has_tag(&iter, gtk_text_tag_table_lookup(gtk_text_buffer_get_tag_table(p->buffer), "link")))
    return NULL;
  int offset = gtk_text_iter_get_offset(&iter);
  for (guint i = 0; i < p->links->len; i++) {
    const Link *link = &g_array_index(p->links, Link, i);
    if (offset >= link->start && offset < link->end)
      return link;
  }
  return NULL;
}

/* Anchors scroll to their heading, Markdown files open as previews, other local files in an
 * editor, and everything else in the default app (the browser, for web links). */
static void follow(Preview *p, const char *url)
{
  Render where = { .p = p, .dir = g_path_get_dirname(p->tab->path) };
  char *path = local_path(&where, url);

  if (url[0] == '#') {
    char *slug = g_uri_unescape_string(url + 1, NULL);
    gpointer offset;
    if (slug && g_hash_table_lookup_extended(p->anchors, slug, NULL, &offset)) {
      GtkTextIter iter;
      gtk_text_buffer_get_iter_at_offset(p->buffer, &iter, GPOINTER_TO_INT(offset));
      gtk_text_view_scroll_to_iter(p->view, &iter, 0, TRUE, 0, 0);
    }
    g_free(slug);
  } else if (path && is_markdown(path) && g_file_test(path, G_FILE_TEST_IS_REGULAR)) {
    open_preview(p->tab->app, path);
  } else if (path && g_file_test(path, G_FILE_TEST_IS_REGULAR)) {
    open_file(p->tab->app, path);
  } else if (!path) {
    GtkUriLauncher *launcher = gtk_uri_launcher_new(url);
    gtk_uri_launcher_launch(launcher, p->tab->app->window, NULL, NULL, NULL);
    g_object_unref(launcher);
  }
  g_free(path);
  g_free(where.dir);
}

static void on_click(GtkGestureClick *gesture G_GNUC_UNUSED, int n_press, double x, double y, Preview *p)
{
  const Link *link = n_press == 1 && !gtk_text_buffer_get_has_selection(p->buffer) ? link_at(p, x, y) : NULL;

  if (link) {
    char *url = g_strdup(link->url); /* following it may show the file again */
    follow(p, url);
    g_free(url);
  }
}

/* Underlines the link at index (-1: none) and only it: the one under the pointer. */
static void hover_link(Preview *p, int index)
{
  GtkTextIter from, to;

  if (index == p->hovered)
    return;
  if (p->hovered >= 0 && (guint)p->hovered < p->links->len) {
    const Link *old = &g_array_index(p->links, Link, p->hovered);
    gtk_text_buffer_get_iter_at_offset(p->buffer, &from, old->start);
    gtk_text_buffer_get_iter_at_offset(p->buffer, &to, old->end);
    gtk_text_buffer_remove_tag_by_name(p->buffer, "link-hover", &from, &to);
  }
  p->hovered = index;
  if (index >= 0) {
    const Link *link = &g_array_index(p->links, Link, index);
    gtk_text_buffer_get_iter_at_offset(p->buffer, &from, link->start);
    gtk_text_buffer_get_iter_at_offset(p->buffer, &to, link->end);
    gtk_text_buffer_apply_tag_by_name(p->buffer, "link-hover", &from, &to);
  }
}

/* The hand over links, and the link underlined. */
static void on_motion(GtkEventControllerMotion *motion G_GNUC_UNUSED, double x, double y, Preview *p)
{
  const Link *link = link_at(p, x, y);
  gboolean over = link != NULL;

  hover_link(p, link ? (int)(link - &g_array_index(p->links, Link, 0)) : -1);
  if (over != p->over_link) {
    p->over_link = over;
    gtk_widget_set_cursor_from_name(GTK_WIDGET(p->view), over ? "pointer" : "text");
  }
}

static void on_leave(GtkEventControllerMotion *motion G_GNUC_UNUSED, Preview *p)
{
  hover_link(p, -1);
}

/* Tabs --------------------------------------------------------------------------------------- */

void preview_free(Preview *p)
{
  if (p->reload_id)
    g_source_remove(p->reload_id);
  if (p->monitor) {
    g_file_monitor_cancel(p->monitor);
    g_object_unref(p->monitor);
  }
  clear_links(p);
  g_array_free(p->links, TRUE);
  g_array_free(p->code_spans, TRUE);
  g_array_free(p->blocks, TRUE);
  g_hash_table_destroy(p->anchors);
  g_hash_table_destroy(p->colors);
  g_ptr_array_free(p->tables, TRUE);
  g_ptr_array_free(p->pictures, TRUE);
  g_free(p);
}

void preview_restyle(Tab *tab)
{
  if (!tab->preview)
    return;
  color_tags(tab->preview->buffer);
  reload(tab->preview); /* code blocks colored with the new scheme */
}

void preview_set_zoom(Tab *tab, double scale)
{
  if (tab->preview) {
    tab->preview->scale = scale;
    reload(tab->preview); /* the tables' cells and columns are sized at the text size */
  }
}

void open_preview(App *t, const char *path)
{
  static gboolean styled;
  Tab *open = find_tab(t, TAB_PREVIEW, path);

  if (open) {
    select_tab(open);
    return;
  }
  if (!styled) {
    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(provider, css);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(provider),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
    styled = TRUE;
  }

  Preview *p = g_new0(Preview, 1);
  GtkWidget *scrolled = gtk_scrolled_window_new();
  GtkGesture *click = gtk_gesture_click_new();
  GtkEventController *motion = gtk_event_controller_motion_new();
  GFile *file = g_file_new_for_path(path);

  p->scale = t->preview_scale;
  p->links = g_array_new(FALSE, FALSE, sizeof(Link));
  p->code_spans = g_array_new(FALSE, FALSE, sizeof(CodeSpan));
  p->blocks = g_array_new(FALSE, FALSE, sizeof(Block));
  p->hovered = -1;
  p->anchors = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  p->colors = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  p->tables = g_ptr_array_new_with_free_func(table_free);
  p->pictures = g_ptr_array_new();
  p->buffer = gtk_text_buffer_new(NULL);
  PangoFontDescription *font = pango_font_description_from_string(t->settings.font ? t->settings.font : DEFAULT_FONT);
  const char *family = pango_font_description_get_family(font);
  char *families = g_strconcat(family ? family : "monospace", ",monospace", NULL); /* when it is not installed */
  create_tags(p->buffer, families);
  g_free(families);
  pango_font_description_free(font);
  TcodePreviewView *view = g_object_new(TCODE_TYPE_PREVIEW_VIEW, "buffer", p->buffer, NULL);
  view->preview = p;
  p->view = GTK_TEXT_VIEW(view);
  g_object_unref(p->buffer); /* the view holds it */
  gtk_text_view_set_editable(p->view, FALSE);
  gtk_text_view_set_cursor_visible(p->view, FALSE);
  gtk_text_view_set_wrap_mode(p->view, GTK_WRAP_WORD_CHAR);
  gtk_text_view_set_pixels_inside_wrap(p->view, 5);
  gtk_text_view_set_left_margin(p->view, MARGIN);
  gtk_text_view_set_right_margin(p->view, MARGIN);
  gtk_text_view_set_top_margin(p->view, 24);
  gtk_text_view_set_bottom_margin(p->view, 48);
  gtk_widget_add_css_class(GTK_WIDGET(p->view), "tcode-preview");
  /* Nothing is wider than the view (code blocks wrap, images and tables are sized to it), but
   * GtkTextView measures lines with a hanging indent (list items) a few pixels too wide: no
   * horizontal scrollbar for those. EXTERNAL, not NEVER, which would make the view's width the
   * scrolled window's minimum. */
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_EXTERNAL, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), GTK_WIDGET(p->view));
  gtk_widget_set_hexpand(scrolled, TRUE);
  gtk_widget_set_vexpand(scrolled, TRUE);
  g_signal_connect(click, "released", G_CALLBACK(on_click), p);
  gtk_widget_add_controller(GTK_WIDGET(p->view), GTK_EVENT_CONTROLLER(click));
  g_signal_connect(motion, "motion", G_CALLBACK(on_motion), p);
  g_signal_connect(motion, "leave", G_CALLBACK(on_leave), p);
  gtk_widget_add_controller(GTK_WIDGET(p->view), motion);

  p->tab = add_tab(t, TAB_PREVIEW, scrolled, path);
  p->tab->preview = p;
  p->monitor = g_file_monitor_file(file, G_FILE_MONITOR_WATCH_MOVES, NULL, NULL);
  if (p->monitor)
    g_signal_connect(p->monitor, "changed", G_CALLBACK(on_file_changed), p);
  g_object_unref(file);

  reload(p);
  focus_current_tab(t);
}
