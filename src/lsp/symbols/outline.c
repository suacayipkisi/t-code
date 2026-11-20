/*
 * The Outline panel (see outline.h). A GtkListView over a flat list of rows: a file with tens of
 * thousands of symbols makes as many small objects, but widgets only for the rows in view.
 */
#include "lsp/symbols/outline.h"

#include "editor/editorview.h"
#include "editor/tabs.h"
#include "lsp/client/client.h"
#include "lsp/symbols/symbols.h"
#include "window/palette.h"

#define INDENT_PX 14 /* per level */

/* One row: a copy of a symbol (the symbols go when new ones come or the editor closes). */
struct _TcodeOutlineRow {
  GObject parent;
  char *name, *detail;
  int kind, depth, line, column;
};

#define TCODE_TYPE_OUTLINE_ROW (tcode_outline_row_get_type())
G_DECLARE_FINAL_TYPE(TcodeOutlineRow, tcode_outline_row, TCODE, OUTLINE_ROW, GObject)
G_DEFINE_FINAL_TYPE(TcodeOutlineRow, tcode_outline_row, G_TYPE_OBJECT)

static void tcode_outline_row_finalize(GObject *object)
{
  TcodeOutlineRow *self = TCODE_OUTLINE_ROW(object);

  g_free(self->name);
  g_free(self->detail);
  G_OBJECT_CLASS(tcode_outline_row_parent_class)->finalize(object);
}

static void tcode_outline_row_class_init(TcodeOutlineRowClass *klass)
{
  G_OBJECT_CLASS(klass)->finalize = tcode_outline_row_finalize;
}

static void tcode_outline_row_init(TcodeOutlineRow *self G_GNUC_UNUSED)
{
}

static struct {
  GtkListView *list; /* both NULL once the window is gone: tabs switch while it closes */
  GtkLabel *note;    /* "No symbols" and the like, instead of the list */
  GListStore *rows;  /* TcodeOutlineRow, in the list's order */
  GtkSingleSelection *selection;
  char *path; /* the editor whose symbols are listed */
} outline;

static const char *const outline_css =
    ".tcode-outline { background: var(--tc-panel); }"
    ".tcode-outline listview { background: transparent; }"
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

static void add_rows(GPtrArray *out, GPtrArray *level, int depth)
{
  for (guint i = 0; level && i < level->len; i++) {
    Sym *sym = level->pdata[i];
    TcodeOutlineRow *row = g_object_new(TCODE_TYPE_OUTLINE_ROW, NULL);

    row->name = g_strdup(sym->name);
    row->detail = sym->detail && *sym->detail ? g_strdup(sym->detail) : NULL;
    row->kind = sym->kind;
    row->depth = depth;
    row->line = sym->name_line;
    row->column = sym->name_column;
    g_ptr_array_add(out, row);
    add_rows(out, sym->children, depth + 1);
  }
}

static App *outline_app;

void outline_fill(App *t, Tab *tab, GPtrArray *roots)
{
  GPtrArray *rows = g_ptr_array_new_with_free_func(g_object_unref);

  outline_app = t;
  if (!outline.list || !outline.note) {
    g_ptr_array_unref(rows);
    return;
  }
  g_set_str(&outline.path, tab ? tab->path : NULL);
  if (!tab)
    set_note("Open a file to see its symbols.");
  else if (!roots)
    set_note(lsp_has(tab) ? "Waiting for the language server…" : "No language server for this file.");
  else
    set_note(roots->len == 0 ? "No symbols in this file." : NULL);
  if (tab)
    add_rows(rows, roots, 0);
  /* One change for the whole list. */
  g_list_store_splice(outline.rows, 0, g_list_model_get_n_items(G_LIST_MODEL(outline.rows)), rows->pdata, rows->len);
  g_ptr_array_unref(rows);
  if (tab && roots)
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
  guint found = GTK_INVALID_LIST_POSITION, n;

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
  n = g_list_model_get_n_items(G_LIST_MODEL(outline.rows));
  for (guint i = 0; line >= 0 && i < n && found == GTK_INVALID_LIST_POSITION; i++) {
    TcodeOutlineRow *row = g_list_model_get_item(G_LIST_MODEL(outline.rows), i);
    if (row->line == line && row->column == column)
      found = i;
    g_object_unref(row);
  }
  gtk_single_selection_set_selected(outline.selection, found);
}

static void setup_row(GtkSignalListItemFactory *factory G_GNUC_UNUSED, GtkListItem *item, gpointer data G_GNUC_UNUSED)
{
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  GtkWidget *kind = gtk_label_new(NULL), *name = gtk_label_new(NULL), *detail = gtk_label_new(NULL);

  gtk_widget_add_css_class(kind, "kind");
  gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class(detail, "detail");
  gtk_label_set_ellipsize(GTK_LABEL(detail), PANGO_ELLIPSIZE_END);
  gtk_label_set_xalign(GTK_LABEL(detail), 0);
  gtk_widget_set_hexpand(detail, TRUE);
  gtk_box_append(GTK_BOX(box), kind);
  gtk_box_append(GTK_BOX(box), name);
  gtk_box_append(GTK_BOX(box), detail);
  gtk_list_item_set_child(item, box);
}

static void bind_row(GtkSignalListItemFactory *factory G_GNUC_UNUSED, GtkListItem *item, gpointer data G_GNUC_UNUSED)
{
  TcodeOutlineRow *row = gtk_list_item_get_item(item);
  GtkWidget *box = gtk_list_item_get_child(item), *kind = gtk_widget_get_first_child(box);
  GtkWidget *name = gtk_widget_get_next_sibling(kind), *detail = gtk_widget_get_next_sibling(name);

  gtk_widget_set_margin_start(box, row->depth * INDENT_PX);
  gtk_label_set_text(GTK_LABEL(kind), symbols_kind_mark(row->kind));
  gtk_label_set_text(GTK_LABEL(name), row->name);
  gtk_label_set_text(GTK_LABEL(detail), row->detail ? row->detail : "");
  gtk_widget_set_visible(detail, row->detail != NULL);
}

/* A row clicked (or Enter on it): the editor goes to the symbol's name. */
static void on_outline_row(GtkListView *list G_GNUC_UNUSED, guint position, gpointer data G_GNUC_UNUSED)
{
  Tab *tab = outline.path && outline_app ? find_tab(outline_app, TAB_EDITOR, outline.path) : NULL;
  TcodeOutlineRow *row = tab ? g_list_model_get_item(G_LIST_MODEL(outline.rows), position) : NULL;
  GtkTextIter iter;

  if (!row)
    return;
  gtk_text_buffer_get_iter_at_line_offset(GTK_TEXT_BUFFER(tab->buffer), &iter, row->line, row->column);
  g_object_unref(row);
  gtk_text_buffer_place_cursor(GTK_TEXT_BUFFER(tab->buffer), &iter);
  editor_view_reveal_cursor(tab);
  gtk_widget_grab_focus(GTK_WIDGET(tab->source));
}

char *outline_text(App *t G_GNUC_UNUSED)
{
  GString *out = g_string_new(NULL);
  guint n = outline.list ? g_list_model_get_n_items(G_LIST_MODEL(outline.rows)) : 0;

  for (guint i = 0; i < n; i++) {
    TcodeOutlineRow *row = g_list_model_get_item(G_LIST_MODEL(outline.rows), i);
    g_string_append_printf(out, "%*s%s %s%s\n", row->depth * 2, "", symbols_kind_mark(row->kind), row->name,
                           gtk_single_selection_get_selected(outline.selection) == i ? " *" : "");
    g_object_unref(row);
  }
  if (outline.note && gtk_widget_get_visible(GTK_WIDGET(outline.note)))
    g_string_append_printf(out, "(%s)\n", gtk_label_get_text(outline.note));
  return g_string_free(out, FALSE);
}

/* The window is going: the list's model goes with it. */
static void on_panel_destroyed(GtkWidget *panel G_GNUC_UNUSED, gpointer data G_GNUC_UNUSED)
{
  g_clear_object(&outline.rows);
  outline.selection = NULL;
  g_clear_pointer(&outline.path, g_free);
}

GtkWidget *outline_panel(App *t)
{
  GtkWidget *panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0), *scroll = gtk_scrolled_window_new();
  GtkListItemFactory *factory = gtk_signal_list_item_factory_new();

  palette_add_css(outline_css);
  outline_app = t;
  gtk_widget_add_css_class(panel, "tcode-outline");
  outline.note = GTK_LABEL(gtk_label_new(NULL));
  gtk_widget_add_css_class(GTK_WIDGET(outline.note), "note");
  gtk_label_set_wrap(outline.note, TRUE);
  gtk_label_set_xalign(outline.note, 0);
  gtk_box_append(GTK_BOX(panel), GTK_WIDGET(outline.note));
  outline.rows = g_list_store_new(TCODE_TYPE_OUTLINE_ROW);
  outline.selection = gtk_single_selection_new(G_LIST_MODEL(g_object_ref(outline.rows)));
  gtk_single_selection_set_autoselect(outline.selection, FALSE);
  gtk_single_selection_set_can_unselect(outline.selection, TRUE);
  g_signal_connect(factory, "setup", G_CALLBACK(setup_row), NULL);
  g_signal_connect(factory, "bind", G_CALLBACK(bind_row), NULL);
  outline.list = GTK_LIST_VIEW(gtk_list_view_new(GTK_SELECTION_MODEL(outline.selection), factory));
  g_object_add_weak_pointer(G_OBJECT(outline.note), (gpointer *)&outline.note);
  g_object_add_weak_pointer(G_OBJECT(outline.list), (gpointer *)&outline.list);
  gtk_list_view_set_single_click_activate(outline.list, TRUE);
  g_signal_connect(outline.list, "activate", G_CALLBACK(on_outline_row), NULL);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(outline.list));
  gtk_widget_set_vexpand(scroll, TRUE);
  gtk_box_append(GTK_BOX(panel), scroll);
  g_signal_connect(panel, "destroy", G_CALLBACK(on_panel_destroyed), NULL);
  set_note("Open a file to see its symbols.");
  return panel;
}
