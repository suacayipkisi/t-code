/*
 * What the Markdown preview's files share (preview.c, markdown.c, table.c, view.c). Only they include it.
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
  double scale;        /* the previews' text size (t->settings.preview_scale) */
  gboolean over_link;
  int hovered; /* the link under the pointer (its index in links, underlined), or -1 */
  GFileMonitor *monitor;
  guint reload_id;
};

/* markdown.c: parses the file and appends it to the (empty) buffer: its text, tags, links,
 * anchors, blocks, tables and images. */
void preview_render(Preview *p);

/* markdown.c: an HTML entity (&amp;, &#169;, &#xA9;), as the text it stands for; the caller frees it. */
char *preview_decode_entity(const char *text, gsize size);

/* markdown.c: the local file a link or image points to (relative to dir, the Markdown file's
 * folder), or NULL. */
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
