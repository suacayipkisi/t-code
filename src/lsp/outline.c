/*
 * The Outline panel (see outline.h).
 */
#include "lsp/outline.h"

#include "editor/editorview.h"
#include "editor/tabs.h"
#include "lsp/client.h"
#include "lsp/symbols.h"
#include "window/palette.h"

#define INDENT_PX 14 /* per level */

static struct {
  GtkListBox *list;
  GtkLabel *note; /* "No symbols" and the like, instead of the list */
  char *path;     /* the editor whose symbols are listed */
} outline;

static const char *const outline_css =
    ".tcode-outline { background: var(--tc-panel); }"
    ".tcode-outline list { background: transparent; }"
    ".tcode-outline row { padding: 0 6px; margin: 0 4px; min-height: 22px; border-radius: 6px; color: var(--tc-fg); }"
    ".tcode-outline row:hover { background: var(--tc-hover); }"
    ".tcode-outline row:selected { background: var(--tc-selection); }"
    ".tcode-outline .kind { color: var(--tc-accent); min-width: 16px; }"
    ".tcode-outline .detail { color: var(--tc-fg-faint); font-size: 0.9em; }"
    ".tcode-outline .note { color: var(--tc-fg-dim); margin: 10px; }";

static void set_note(const char *text)
{
  gtk_label_set_text(outline.note, text ? text : "");
  gtk_widget_set_visible(GTK_WIDGET(outline.note), text != NULL);
}

static void clear_rows(void)
{
  GtkListBoxRow *row;

  while ((row = gtk_list_box_get_row_at_index(outline.list, 0)))
    gtk_list_box_remove(outline.list, GTK_WIDGET(row));
}

static void add_rows(GPtrArray *level, int depth)
{
  for (guint i = 0; level && i < level->len; i++) {
    Sym *sym = level->pdata[i];
    GtkWidget *row = gtk_list_box_row_new(), *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget *kind = gtk_label_new(symbols_kind_mark(sym->kind)), *name = gtk_label_new(sym->name);

    gtk_widget_set_margin_start(box, depth * INDENT_PX);
    gtk_widget_add_css_class(kind, "kind");
    gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(box), kind);
    gtk_box_append(GTK_BOX(box), name);
    if (sym->detail && *sym->detail) {
      GtkWidget *detail = gtk_label_new(sym->detail);
      gtk_widget_add_css_class(detail, "detail");
      gtk_label_set_ellipsize(GTK_LABEL(detail), PANGO_ELLIPSIZE_END);
      gtk_label_set_xalign(GTK_LABEL(detail), 0);
      gtk_widget_set_hexpand(detail, TRUE);
      gtk_box_append(GTK_BOX(box), detail);
    }
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
    /* Copies: the symbols go when new ones come or the editor closes. */
    g_object_set_data_full(G_OBJECT(row), "name", g_strdup(sym->name), g_free);
    g_object_set_data(G_OBJECT(row), "kind", GINT_TO_POINTER(sym->kind));
    g_object_set_data(G_OBJECT(row), "depth", GINT_TO_POINTER(depth));
    g_object_set_data(G_OBJECT(row), "line", GINT_TO_POINTER(sym->name_line));
    g_object_set_data(G_OBJECT(row), "column", GINT_TO_POINTER(sym->name_column));
    gtk_list_box_append(outline.list, row);
    add_rows(sym->children, depth + 1);
  }
}

static App *outline_app;

void outline_fill(App *t, Tab *tab, GPtrArray *roots)
{
  outline_app = t;
  if (!outline.list)
    return;
  clear_rows();
  g_set_str(&outline.path, tab ? tab->path : NULL);
  if (!tab) {
    set_note("Open a file to see its symbols.");
    return;
  }
  if (!roots) {
    set_note(lsp_has(tab) ? "Waiting for the language server…" : "No language server for this file.");
    return;
  }
  set_note(roots->len == 0 ? "No symbols in this file." : NULL);
  add_rows(roots, 0);
  outline_cursor_moved(tab);
}

void outline_show_tab(App *t)
{
  Tab *tab = current_tab(t);

  if (tab && tab->kind != TAB_EDITOR)
    tab = NULL;
  outline_fill(t, tab, tab ? symbols_roots(tab) : NULL);
}

void outline_symbols_changed(Tab *tab)
{
  if (outline.list && tab == current_tab(tab->app))
    outline_fill(tab->app, tab, symbols_roots(tab));
}

void outline_cursor_moved(Tab *tab)
{
  GtkTextIter cursor;
  GPtrArray *chain;
  int line = -1, column = -1;

  if (!outline.list || tab != current_tab(tab->app) || g_strcmp0(tab->path, outline.path) != 0)
    return;
  gtk_text_buffer_get_iter_at_mark(GTK_TEXT_BUFFER(tab->buffer), &cursor,
                                   gtk_text_buffer_get_insert(GTK_TEXT_BUFFER(tab->buffer)));
  chain = symbols_at(tab, &cursor);
  if (chain->len) { /* the innermost, by where its name is */
    line = ((Sym *)chain->pdata[chain->len - 1])->name_line;
    column = ((Sym *)chain->pdata[chain->len - 1])->name_column;
  }
  g_ptr_array_unref(chain);
  gtk_list_box_unselect_all(outline.list);
  for (GtkWidget *row = gtk_widget_get_first_child(GTK_WIDGET(outline.list)); line >= 0 && row;
       row = gtk_widget_get_next_sibling(row))
    if (GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "line")) == line &&
        GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "column")) == column) {
      gtk_list_box_select_row(outline.list, GTK_LIST_BOX_ROW(row));
      break;
    }
}

/* A row clicked (or Enter on it): the editor goes to the symbol's name. */
static void on_outline_row(GtkListBox *list G_GNUC_UNUSED, GtkListBoxRow *row, gpointer data G_GNUC_UNUSED)
{
  Tab *tab = outline.path && outline_app ? find_tab(outline_app, TAB_EDITOR, outline.path) : NULL;
  GtkTextIter iter;

  if (!tab)
    return;
  gtk_text_buffer_get_iter_at_line_offset(GTK_TEXT_BUFFER(tab->buffer), &iter,
                                          GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "line")),
                                          GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "column")));
  gtk_text_buffer_place_cursor(GTK_TEXT_BUFFER(tab->buffer), &iter);
  editor_view_reveal_cursor(tab);
  gtk_widget_grab_focus(GTK_WIDGET(tab->source));
}

char *outline_text(App *t G_GNUC_UNUSED)
{
  GString *out = g_string_new(NULL);

  for (GtkWidget *row = outline.list ? gtk_widget_get_first_child(GTK_WIDGET(outline.list)) : NULL; row;
       row = gtk_widget_get_next_sibling(row)) {
    int depth = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "depth"));
    g_string_append_printf(out, "%*s%s %s%s\n", depth * 2, "",
                           symbols_kind_mark(GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "kind"))),
                           (const char *)g_object_get_data(G_OBJECT(row), "name"),
                           gtk_list_box_row_is_selected(GTK_LIST_BOX_ROW(row)) ? " *" : "");
  }
  if (gtk_widget_get_visible(GTK_WIDGET(outline.note)))
    g_string_append_printf(out, "(%s)\n", gtk_label_get_text(outline.note));
  return g_string_free(out, FALSE);
}

GtkWidget *outline_panel(App *t)
{
  GtkWidget *panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0), *scroll = gtk_scrolled_window_new();

  palette_add_css(outline_css);
  outline_app = t;
  gtk_widget_add_css_class(panel, "tcode-outline");
  outline.note = GTK_LABEL(gtk_label_new(NULL));
  gtk_widget_add_css_class(GTK_WIDGET(outline.note), "note");
  gtk_label_set_wrap(outline.note, TRUE);
  gtk_label_set_xalign(outline.note, 0);
  gtk_box_append(GTK_BOX(panel), GTK_WIDGET(outline.note));
  outline.list = GTK_LIST_BOX(gtk_list_box_new());
  gtk_list_box_set_selection_mode(outline.list, GTK_SELECTION_SINGLE);
  gtk_list_box_set_activate_on_single_click(outline.list, TRUE);
  g_signal_connect(outline.list, "row-activated", G_CALLBACK(on_outline_row), NULL);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(outline.list));
  gtk_widget_set_vexpand(scroll, TRUE);
  gtk_box_append(GTK_BOX(panel), scroll);
  set_note("Open a file to see its symbols.");
  return panel;
}
