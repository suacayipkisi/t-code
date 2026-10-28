/*
 * What the Markdown preview's files share (preview.c, markdown.c, html.c, images.c, footnotes.c, math.c,
 * table.c, view.c). Only they include it.
 */
#pragma once

#include "editor/preview/preview.h"

#include <md4c.h>

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
  char *title; /* shown over it, or NULL */
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
  int alert;      /* BLOCK_QUOTE: a GitHub alert's kind (1 note ... 5 caution), or 0 */
} Block;

/* <details>: its content shows when it is open; a click on its summary opens or closes it. */
typedef struct {
  int summary, content, end; /* offsets: the summary's line, the content, its end (-1: none) */
  gboolean open;
} Details;

/* An open HTML element (html.c): what its end tag applies, from start. */
typedef struct {
  int element; /* in html.c's table */
  int start;
  int extra;  /* <details>: its index in p->details */
  char align; /* 'c'enter, 'r'ight, or 0 */
  char *href, *title;
} HtmlOpen;

/* A name and the text it stands for (entities.h, emoji.h). */
typedef struct {
  const char *name, *text;
} PreviewName;

/* A table: a grid of wrapping labels in the text, its columns sized to the view's width. */
typedef struct {
  GtkWidget *grid;
  GArray *natural; /* int per column: its widest cell on one line, padding included */
} PreviewTable;

struct Preview {
  Tab *tab;
  GtkTextView *view;
  GtkTextBuffer *buffer;
  GArray *links;        /* Link, in the order of the text */
  GArray *code_spans;   /* CodeSpan: inline code spans for pill background */
  GArray *blocks;       /* Block: quotes, code blocks, the rules under h1 / h2, horizontal rules */
  GHashTable *anchors;  /* heading slug -> its offset */
  GHashTable *colors;   /* a code coloring ("fg|weight|style") -> its tag, made once per preview */
  GPtrArray *tables;    /* PreviewTable: the tables shown, sized to the view's width */
  GPtrArray *pictures;  /* the images shown, sized to the view's width */
  GHashTable *rises;    /* a formula's depth below the baseline -> the tag that lowers it that far */
  GHashTable *remote;   /* a web image's address, a diagram's key -> its paintable (or pending, failed) */
  GCancellable *cancel; /* the background fetches, cancelled when the preview goes */
  GArray *details;      /* Details, in the order of the text */
  int width;            /* the view's width the images were sized for */
  double scale;         /* the previews' text size (t->settings.preview_scale) */
  gboolean over_link;
  int hovered; /* the link under the pointer (its index in links, underlined), or -1 */
  GFileMonitor *monitor;
  guint reload_id;
};

/* markdown.c: parses the file and appends it to the (empty) buffer: its text, tags, links,
 * anchors, blocks, tables and images. */
void preview_render(Preview *p);

/* markdown.c: an HTML entity (&amp;, &#169;, &#xA9;: any of HTML5's), as the text it stands for; the
 * caller frees it. */
char *preview_decode_entity(const char *text, gsize size);

/* markdown.c: text with GitHub's :emoji: shortcodes replaced, or NULL when it has none. */
char *preview_emojify(const char *text, gsize size);

/* markdown.c: a link's or image's target as a string, "" when it is empty: md4c gives no text for
 * ![alt]() (a half typed image), and NULL would crash. */
char *preview_target(const MD_ATTRIBUTE *target);

/* markdown.c: the local file a link or image points to (relative to dir, the Markdown file's
 * folder; one starting with / from the root of its git repository), or NULL. */
char *preview_local_path(const char *dir, const char *target);

/* view.c: the view (TcodePreviewView, drawing the blocks and inline code below the text), the
 * width the text has, and the margins, images and tables fitted to a view this wide. */
GtkTextView *preview_view_new(Preview *p);
int preview_content_width(Preview *p);
void preview_update_layout(Preview *p, int width);
/* view.c: an image's size in a text this wide. */
void preview_size_picture(GtkWidget *picture, int max_width);
/* view.c: the tags every rendering uses, made with the buffer (mono: the code's font families),
 * and their colors from the color theme (again when it changes). */
void preview_create_tags(GtkTextBuffer *b, const char *mono);
void preview_color_tags(GtkTextBuffer *b);
/* view.c: a heading's text size (level 1 to 6) relative to the body's. */
double preview_heading_scale(int level);
/* view.c: the color of a GitHub alert (1 note ... 5 caution). */
const GdkRGBA *preview_alert_color(int alert);

/* math.c: a TeX formula as Pango markup on one line (for table cells): x<sup>2</sup>, a⁄b, √x. */
char *preview_math_markup(const char *tex);

/* footnotes.c: the Markdown text with GitHub's footnotes turned into links, anchors and a numbered
 * list at the end (md4c does not know them), or NULL when it has none. */
char *preview_footnotes(const char *text);

/* images.c: the paintable for an image (src relative to dir), or NULL when it cannot be shown now: a
 * web image is fetched in the background, and the preview shown again when it arrives. */
GdkPaintable *preview_image(Preview *p, const char *dir, const char *src);
/* images.c: a mermaid diagram drawn by mmdc (in the background, cached), or NULL: not (yet) drawn. */
GdkPaintable *preview_mermaid(Preview *p, const char *source);
/* images.c: an image's picture: width its own when 0, clicking it follows href, title shown over it. */
GtkWidget *preview_picture_new(Preview *p, GdkPaintable *paintable, int width, const char *href, const char *title);
/* images.c: frees a value of p->remote. */
void preview_remote_free(gpointer data);

/* preview.c: hides a closed <details>' content, shows an open one's, its summary's ▶ or ▼ to match. */
void preview_show_details(Preview *p, Details *details);

/* images.c: whether mermaid diagrams can be drawn (mermaid-cli's mmdc is installed). */
gboolean preview_can_draw_mermaid(void);

/* preview.c: shows the file again shortly (once for a burst of calls), keeping the scroll position. */
void preview_reload_later(Preview *p);

/* math.c: a TeX formula as a paintable in the text's color, size: the text's size in pixels; display: set
 * apart (larger operators, limits above and below). It is drawn on the baseline; *depth: how far it
 * reaches below it, in pixels (the caller lowers it that far). NULL when it shows nothing. */
GdkPaintable *preview_math_new(const char *tex, gboolean display, double size, int *depth);

/* One rendering: md4c's callbacks append to the end of the buffer. Tags are applied when a block or
 * span ends, from where it started (starts) to the end. */
typedef struct {
  Preview *p;
  char *dir;                /* the file's folder: relative links and images are in it */
  GArray *starts;           /* int: where each open block and span started */
  GArray *images;           /* gboolean: per open image, whether it is shown (then its alt text is not) */
  GPtrArray *hrefs;         /* per open link, its target */
  GPtrArray *titles;        /* per open link, its title (or NULL) */
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
  int hidden;              /* inside a shown image: its alt text is not added */
  gboolean html_block;     /* inside an HTML block: its lines flow like a paragraph's */
  GString *html_carry;     /* raw HTML cut inside a tag: the rest comes with the next piece */
  gboolean in_comment;     /* inside an HTML comment */
  GArray *html_open;       /* the HTML elements open (html.c's HtmlOpen) */
  char *picture_src;       /* inside <picture>: the image for the theme */
  GtkWidget *cell_box;     /* a table cell with images: its labels and pictures, in a row */
  int cell_width;          /* their width so far */
  GPtrArray *cell_html;    /* the HTML elements open in a table cell */
  GPtrArray *cell_openers; /* per span open in a cell, the markup that opened it */
  GString *math;           /* inside a math span: its TeX */
  gboolean after_display;  /* a display formula ended the line: what follows starts a new one */
  int heading;             /* inside a heading: its level (formulas are as large as its text) */
  GArray *alerts;          /* int: per open quote, its GitHub alert (0: none) */
  gboolean quote_start;    /* the next block is a quote's first */
  gboolean alert_p;        /* inside a quote's first paragraph: it may start with [!NOTE] */
} Render;

/* markdown.c, for html.c and table.c: appending to the buffer, tags from start to its end, blocks,
 * anchors, links, code, images. */
int preview_end_offset(Render *r);
void preview_append(Render *r, const char *text, gssize len);
void preview_append_text(Render *r, const char *text, gsize size); /* emoji shortcodes replaced */
gboolean preview_at_line_start(Render *r);
void preview_tag(Render *r, const char *name, GtkTextTag *tag, int start);
void preview_begin_block(Render *r);
void preview_end_paragraph(Render *r, int start, const char *gap);
void preview_add_block(Render *r, BlockKind kind, int start, int level);
void preview_add_rule(Render *r);
void preview_add_anchor(Render *r, int start); /* a heading's, from its text */
void preview_add_named_anchor(Render *r, const char *name, int offset);
void preview_add_link(Render *r, int start, char *url, char *title); /* takes url and title */
void preview_add_code_span(Render *r, int start);
/* An image at the end (width, height: wanted size, 0 for its own; href: a link around it); FALSE when it
 * cannot be shown (its alt text is, then). */
gboolean preview_append_image(Render *r, const char *src, int width, int height, const char *href, const char *title);

/* html.c: raw HTML (block: an HTML block's lines, which flow like a paragraph's); the elements left open
 * at the end; an attribute's value (entities decoded) or NULL; frees an open element. */
void preview_html(Render *r, const char *text, gsize size, gboolean block);
void preview_html_finish(Render *r);
char *preview_html_attribute(const char *tag, const char *name);
void preview_html_open_clear(gpointer data);
/* html.c: a tag in a table cell as Pango markup (open: the elements open in the cell), or NULL. */
char *preview_html_markup(const char *tag, GPtrArray *open);

/* table.c: a table starts (r->table) and ends (its grid goes into the text, sized to it); a cell
 * starts (r->markup), gets text and spans as Pango markup, and ends as a label in the grid. */
void preview_table_begin(Render *r);
void preview_table_end(Render *r);
void preview_cell_begin(Render *r, MD_ALIGN align);
void preview_cell_text(Render *r, MD_TEXTTYPE type, const char *text, gsize size);
void preview_cell_span(Render *r, MD_SPANTYPE type, void *detail);
void preview_cell_span_end(Render *r);
void preview_cell_end(Render *r, gboolean head);
/* table.c: shares width out between a table's columns; frees one (Preview's tables). */
void preview_size_table(PreviewTable *table, int width);
void preview_table_free(gpointer data);

/* preview.c: follows a link (an anchor, a file, a web address). */
void preview_follow(Preview *p, const char *url);
