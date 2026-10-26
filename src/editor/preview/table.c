/*
 * The preview's tables (see state.h): a grid of wrapping labels in the text, one per cell, its
 * columns sized to the view's width like a browser does.
 */
#include "editor/preview/state.h"

#include "window/palette.h"

#include <string.h>

void preview_table_begin(Render *r)
{
  r->table = g_new0(PreviewTable, 1);
  r->table->natural = g_array_new(FALSE, TRUE, sizeof(int));
  r->table->grid = gtk_grid_new();
  gtk_widget_add_css_class(r->table->grid, "tcode-preview-table");
  r->row = -1;
}

void preview_table_end(Render *r)
{
  GtkTextIter end;

  gtk_text_buffer_get_end_iter(r->p->buffer, &end);
  gtk_text_view_add_child_at_anchor(r->p->view, r->table->grid,
                                    gtk_text_buffer_create_child_anchor(r->p->buffer, &end));
  g_ptr_array_add(r->p->tables, r->table);
  preview_size_table(g_steal_pointer(&r->table), preview_content_width(r->p));
}

void preview_cell_begin(Render *r, MD_ALIGN align)
{
  r->markup = g_string_new(NULL);
  r->align = align;
}

/* A link in a table cell: followed like the others. */
static gboolean on_cell_link(GtkLabel *label G_GNUC_UNUSED, const char *uri, Preview *p)
{
  char *url = g_strdup(uri); /* following it may show the file again, and free the label */

  preview_follow(p, url);
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
void preview_cell_end(Render *r, gboolean head)
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
  gtk_label_set_max_width_chars(GTK_LABEL(label), 1); /* its width is the one preview_size_table() gives it */
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
  r->cell++;
}

/* Text in a table cell, as markup. */
void preview_cell_text(Render *r, MD_TEXTTYPE type, const char *text, gsize size)
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
    case MD_TEXT_ENTITY: plain = preview_decode_entity(text, size); break;
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
void preview_cell_span(Render *r, MD_SPANTYPE type, void *detail)
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

void preview_cell_span_end(Render *r)
{
  if (r->closers->len)
    g_string_append(r->markup, g_ptr_array_steal_index(r->closers, r->closers->len - 1));
}

/* Shares width between the columns like a browser: when they all fit, each gets its widest cell;
 * else the columns narrower than an even share keep theirs and the others split the rest, their
 * cells wrapping. */
void preview_size_table(PreviewTable *table, int width)
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

void preview_table_free(gpointer data)
{
  PreviewTable *table = data;

  g_array_free(table->natural, TRUE);
  g_free(table);
}
