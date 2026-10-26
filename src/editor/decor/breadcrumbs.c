/*
 * The breadcrumbs over an editor (see breadcrumbs.h).
 */
#include "editor/decor/breadcrumbs.h"

#include "editor/editorview.h"
#include "lsp/symbols/symbols.h"
#include "window/palette.h"

#include <string.h>

static const char *const breadcrumbs_css =
    ".tcode-breadcrumbs { padding: 1px 10px; font-size: 0.85em; background: var(--tc-panel); }"
    ".tcode-breadcrumbs label { color: var(--tc-fg-dim); }"
    ".tcode-breadcrumbs label.separator { color: var(--tc-fg-faint); margin: 0 4px; }"
    ".tcode-breadcrumbs label.file { color: var(--tc-fg); }"
    ".tcode-breadcrumbs button { min-height: 0; min-width: 0; padding: 0 3px; border: none; box-shadow: none;"
    "  background: none; color: var(--tc-fg); }"
    ".tcode-breadcrumbs button:hover { background: color-mix(in srgb, var(--tc-fg) 12%, transparent); }"
    ".tcode-breadcrumbs .kind { color: var(--tc-accent); margin-right: 3px; }";

/* The bar's scrolled window: it asks for no width of its own, so a long path and symbol never widen
 * the editor (which then rewraps its lines and jumps); what does not fit is cut off at the start. */
static GtkWidget *bar_frame(GtkWidget *bar)
{
  return gtk_widget_get_ancestor(bar, GTK_TYPE_SCROLLED_WINDOW);
}

/* The bar got wider or narrower: its end (the file and the symbols) stays in view. */
static void keep_end(GtkAdjustment *adjustment)
{
  gtk_adjustment_set_value(adjustment, gtk_adjustment_get_upper(adjustment) - gtk_adjustment_get_page_size(adjustment));
}

/* A symbol crumb was clicked: the cursor goes to its name. */
static void crumb_clicked(GtkButton *button, Tab *tab)
{
  int line = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "line"));
  int column = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "column"));
  GtkTextIter iter;

  gtk_text_buffer_get_iter_at_line_offset(GTK_TEXT_BUFFER(tab->buffer), &iter, line, column);
  gtk_text_buffer_place_cursor(GTK_TEXT_BUFFER(tab->buffer), &iter);
  editor_view_reveal_cursor(tab);
  gtk_widget_grab_focus(GTK_WIDGET(tab->source));
}

static void crumb_label(GtkWidget *bar, const char *text, const char *css_class)
{
  GtkWidget *label = gtk_label_new(text);

  if (css_class)
    gtk_widget_add_css_class(label, css_class);
  gtk_box_append(GTK_BOX(bar), label);
}

static void crumb_symbol(GtkWidget *bar, Tab *tab, const Sym *sym)
{
  GtkWidget *button = gtk_button_new(), *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *kind = gtk_label_new(symbols_kind_mark(sym->kind)), *name = gtk_label_new(sym->name);

  gtk_widget_add_css_class(kind, "kind");
  gtk_box_append(GTK_BOX(box), kind);
  gtk_box_append(GTK_BOX(box), name);
  gtk_button_set_child(GTK_BUTTON(button), box);
  gtk_widget_set_focusable(button, FALSE);
  if (sym->detail && *sym->detail)
    gtk_widget_set_tooltip_text(button, sym->detail);
  g_object_set_data(G_OBJECT(button), "line", GINT_TO_POINTER(sym->name_line));
  g_object_set_data(G_OBJECT(button), "column", GINT_TO_POINTER(sym->name_column));
  g_signal_connect(button, "clicked", G_CALLBACK(crumb_clicked), tab);
  gtk_box_append(GTK_BOX(bar), button);
}

void breadcrumbs_update(Tab *tab)
{
  GtkWidget *bar = tab && tab->view ? tab->view->breadcrumbs : NULL;
  const char *root = tab ? tab->app->project.root : NULL;
  GtkWidget *child;
  GtkTextIter cursor;

  if (!bar)
    return;
  gtk_widget_set_visible(bar, tab->app->settings.breadcrumbs && tab->path && !tab->view->readonly);
  gtk_widget_set_visible(bar_frame(bar), gtk_widget_get_visible(bar));
  if (!gtk_widget_get_visible(bar))
    return;
  while ((child = gtk_widget_get_first_child(bar)))
    gtk_box_remove(GTK_BOX(bar), child);

  /* The folders under the project, then the file. */
  const char *relative = tab->path;
  if (root && g_str_has_prefix(tab->path, root) && tab->path[strlen(root)] == '/')
    relative = tab->path + strlen(root) + 1;
  char **parts = g_strsplit(relative, "/", -1);
  for (int i = 0; parts[i]; i++) {
    if (!*parts[i])
      continue;
    if (gtk_widget_get_first_child(bar))
      crumb_label(bar, "›", "separator");
    crumb_label(bar, parts[i], parts[i + 1] ? NULL : "file");
  }
  g_strfreev(parts);

  gtk_text_buffer_get_iter_at_mark(GTK_TEXT_BUFFER(tab->buffer), &cursor,
                                   gtk_text_buffer_get_insert(GTK_TEXT_BUFFER(tab->buffer)));
  GPtrArray *chain = symbols_at(tab, &cursor);
  for (guint i = 0; i < chain->len; i++) {
    crumb_label(bar, "›", "separator");
    crumb_symbol(bar, tab, chain->pdata[i]);
  }
  g_ptr_array_unref(chain);
}

/* A widget's labels' text, in order. */
static void crumb_texts(GtkWidget *widget, GString *out)
{
  if (GTK_IS_LABEL(widget) && !gtk_widget_has_css_class(widget, "kind")) {
    const char *text = gtk_label_get_text(GTK_LABEL(widget));
    if (strcmp(text, "›") != 0)
      g_string_append_printf(out, "%s%s", out->len ? " › " : "", text);
  }
  for (GtkWidget *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child))
    crumb_texts(child, out);
}

char *breadcrumbs_text(Tab *tab)
{
  GString *out = g_string_new(NULL);

  if (tab->view->breadcrumbs && gtk_widget_get_visible(tab->view->breadcrumbs))
    crumb_texts(tab->view->breadcrumbs, out);
  return g_string_free(out, FALSE);
}

void breadcrumbs_attach(Tab *tab)
{
  GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0), *frame = gtk_scrolled_window_new();

  palette_add_css(breadcrumbs_css);
  gtk_widget_add_css_class(bar, "tcode-breadcrumbs");
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(frame), GTK_POLICY_EXTERNAL, GTK_POLICY_NEVER);
  gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(frame), TRUE);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(frame), bar);
  g_signal_connect(gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(frame)), "changed", G_CALLBACK(keep_end),
                   NULL);
  gtk_box_prepend(GTK_BOX(tab->view->column), frame);
  tab->view->breadcrumbs = bar;
  gtk_widget_set_visible(bar, FALSE); /* filled once the file is there */
  gtk_widget_set_visible(frame, FALSE);
}
