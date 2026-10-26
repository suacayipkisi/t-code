/*
 * Problems panel: lists diagnostics (errors, warnings, notes) across open files.
 */
#include "lsp/problems.h"

#include "editor/editor.h"
#include "editor/tabs.h"
#include "files/files.h"
#include "lsp/inline/diagnostics.h"
#include "window/window.h"
#include "window/palette.h"

static const char *const problems_css =
    ".tcode-problems { background: var(--tc-panel); }"
    ".tcode-problems-header { padding: 4px 8px; border-bottom: 1px solid var(--tc-border); font-size: 0.85em; }"
    ".tcode-problems-badge { font-weight: bold; margin-right: 12px; }"
    ".tcode-problem-row { padding: 4px 8px; }"
    ".tcode-problem-error { color: var(--tc-error); }"
    ".tcode-problem-warning { color: var(--tc-warning); }"
    ".tcode-problem-info { color: var(--tc-info); }"
    ".tcode-problem-location { color: var(--tc-fg-dim); font-size: 0.85em; margin-left: 8px; }";

typedef struct {
  App *app;
  GtkWidget *panel;
  GtkWidget *list;
  GtkWidget *empty_label;
  GtkLabel *err_label;
  GtkLabel *warn_label;
  GtkLabel *info_label;
  guint update_id; /* a rebuild waiting for the main loop to be idle */
} ProblemsView;

static ProblemsView pv = { 0 };

static void on_row_activated(GtkListBox *box G_GNUC_UNUSED, GtkListBoxRow *row, gpointer user_data)
{
  App *t = user_data;
  char *path = g_object_get_data(G_OBJECT(row), "problem-path");
  int line = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "problem-line"));
  int col = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "problem-col"));

  if (t && path)
    open_file_at(t, path, line, col);
}

void problems_focus(App *t)
{
  (void)t;
  if (pv.list)
    gtk_widget_grab_focus(pv.list);
}

/* A diagnostic's row: its icon, message and place; activating it opens the place. */
static GtkWidget *problem_row(Tab *tab, const char *name, Diagnostic *d, int kind)
{
  static const char *const icons[] = { "dialog-error-symbolic", "dialog-warning-symbolic",
                                       "dialog-information-symbolic" };
  static const char *const classes[] = { "tcode-problem-error", "tcode-problem-warning", "tcode-problem-info" };
  const char *message = diagnostic_message(d);
  GtkWidget *row = gtk_list_box_row_new(), *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *icon = gtk_image_new_from_icon_name(icons[kind]), *label = gtk_label_new(message);
  int line = 1, col = 1;

  diagnostic_position(tab, d, &line, &col);
  char *place = g_strdup_printf("%s:%d:%d", name, line, col);
  char *tip = g_strdup_printf("%s (%s:%d:%d)", message, tab->path, line, col);
  GtkWidget *where = gtk_label_new(place);

  gtk_widget_add_css_class(row, "tcode-problem-row");
  g_object_set_data_full(G_OBJECT(row), "problem-path", g_strdup(tab->path), g_free);
  g_object_set_data(G_OBJECT(row), "problem-line", GINT_TO_POINTER(line));
  g_object_set_data(G_OBJECT(row), "problem-col", GINT_TO_POINTER(col));
  gtk_widget_set_tooltip_text(row, tip);
  gtk_widget_add_css_class(icon, classes[kind]);
  gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
  gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
  gtk_widget_set_hexpand(label, TRUE);
  gtk_widget_add_css_class(where, "tcode-problem-location");
  gtk_box_append(GTK_BOX(box), icon);
  gtk_box_append(GTK_BOX(box), label);
  gtk_box_append(GTK_BOX(box), where);
  gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
  g_free(tip);
  g_free(place);
  return row;
}

/* Rebuilds the list from every open editor's diagnostics. */
static void rebuild(App *t)
{
  int counts[3] = { 0 }; /* errors, warnings, the rest */
  char *text;

  gtk_list_box_remove_all(GTK_LIST_BOX(pv.list));
  for (int i = 0; i < tab_count(t); i++) {
    Tab *tab = tab_nth(t, i);
    GPtrArray *diags = tab->kind == TAB_EDITOR && tab->path ? diagnostics_of(tab) : NULL;
    char *name = diags ? g_path_get_basename(tab->path) : NULL;
    for (guint j = 0; diags && j < diags->len; j++) {
      Diagnostic *d = g_ptr_array_index(diags, j);
      int severity = diagnostic_severity(d), kind = severity == 1 ? 0 : severity == 2 ? 1 : 2;
      counts[kind]++;
      gtk_list_box_append(GTK_LIST_BOX(pv.list), problem_row(tab, name, d, kind));
    }
    g_free(name);
  }
  text = g_strdup_printf("%d Errors", counts[0]);
  gtk_label_set_text(pv.err_label, text);
  g_free(text);
  text = g_strdup_printf("%d Warnings", counts[1]);
  gtk_label_set_text(pv.warn_label, text);
  g_free(text);
  text = g_strdup_printf("%d Notes", counts[2]);
  gtk_label_set_text(pv.info_label, text);
  g_free(text);
  gtk_widget_set_visible(pv.empty_label, counts[0] + counts[1] + counts[2] == 0);
  gtk_widget_set_visible(pv.list, counts[0] + counts[1] + counts[2] > 0);
}

static gboolean rebuild_now(gpointer data)
{
  pv.update_id = 0;
  rebuild(data);
  return G_SOURCE_REMOVE;
}

/* The server publishes diagnostics file by file, often while typing: one rebuild for a burst. */
void problems_update(App *t)
{
  if (t)
    files_problems_changed(t);
  if (!pv.list || !t || t->closing || pv.update_id)
    return;
  pv.update_id = g_idle_add(rebuild_now, t);
}

/* The panel goes with its window: a rebuild still waiting must not reach its widgets. */
static void on_panel_destroy(GtkWidget *panel G_GNUC_UNUSED, gpointer data G_GNUC_UNUSED)
{
  g_clear_handle_id(&pv.update_id, g_source_remove);
  pv = (ProblemsView){ 0 };
}

GtkWidget *problems_panel(App *t)
{
  pv.app = t;

  palette_add_css(problems_css);

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class(box, "tcode-problems");

  /* Header bar with badges */
  GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_add_css_class(header, "tcode-problems-header");

  pv.err_label = GTK_LABEL(gtk_label_new("0 Errors"));
  gtk_widget_add_css_class(GTK_WIDGET(pv.err_label), "tcode-problem-error");
  gtk_widget_add_css_class(GTK_WIDGET(pv.err_label), "tcode-problems-badge");
  gtk_box_append(GTK_BOX(header), GTK_WIDGET(pv.err_label));

  pv.warn_label = GTK_LABEL(gtk_label_new("0 Warnings"));
  gtk_widget_add_css_class(GTK_WIDGET(pv.warn_label), "tcode-problem-warning");
  gtk_widget_add_css_class(GTK_WIDGET(pv.warn_label), "tcode-problems-badge");
  gtk_box_append(GTK_BOX(header), GTK_WIDGET(pv.warn_label));

  pv.info_label = GTK_LABEL(gtk_label_new("0 Notes"));
  gtk_widget_add_css_class(GTK_WIDGET(pv.info_label), "tcode-problem-info");
  gtk_widget_add_css_class(GTK_WIDGET(pv.info_label), "tcode-problems-badge");
  gtk_box_append(GTK_BOX(header), GTK_WIDGET(pv.info_label));

  gtk_box_append(GTK_BOX(box), header);

  /* Scrolled list of problems */
  GtkWidget *scroll = gtk_scrolled_window_new();
  gtk_widget_set_vexpand(scroll, TRUE);
  gtk_widget_set_hexpand(scroll, TRUE);

  pv.list = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(pv.list), GTK_SELECTION_SINGLE);
  g_signal_connect(pv.list, "row-activated", G_CALLBACK(on_row_activated), t);

  pv.empty_label = gtk_label_new("No problems have been detected in the workspace.");
  gtk_widget_set_margin_top(pv.empty_label, 20);
  gtk_widget_set_margin_bottom(pv.empty_label, 20);
  gtk_widget_add_css_class(pv.empty_label, "tcode-problem-location");

  GtkWidget *inner = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append(GTK_BOX(inner), pv.empty_label);
  gtk_box_append(GTK_BOX(inner), pv.list);

  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), inner);
  gtk_box_append(GTK_BOX(box), scroll);

  pv.panel = box;
  g_signal_connect(box, "destroy", G_CALLBACK(on_panel_destroy), NULL);
  rebuild(t);

  return box;
}
