/*
 * Problems panel: lists diagnostics (errors, warnings, notes) across open files.
 */
#include "lsp/problems.h"

#include "editor/editor.h"
#include "editor/tabs.h"
#include "files/files.h"
#include "lsp/diagnostics.h"
#include "window/window.h"


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

void show_problems(App *t)
{
  show_sidebar_page(t, "problems");
  problems_focus(t);
}

/* Rebuilds the list from every open editor's diagnostics. */
static void rebuild(App *t)
{
  /* Clear existing rows */
  GtkWidget *child = gtk_widget_get_first_child(pv.list);
  while (child) {
    GtkWidget *next = gtk_widget_get_next_sibling(child);
    gtk_list_box_remove(GTK_LIST_BOX(pv.list), child);
    child = next;
  }

  int total_err = 0, total_warn = 0, total_info = 0;
  int n_tabs = tab_count(t);

  for (int i = 0; i < n_tabs; i++) {
    Tab *tab = tab_nth(t, i);
    if (!tab || tab->kind != TAB_EDITOR || !tab->path)
      continue;

    GPtrArray *diags = diagnostics_of(tab);
    if (!diags)
      continue;

    char *basename = g_path_get_basename(tab->path);

    for (guint j = 0; j < diags->len; j++) {
      Diagnostic *d = g_ptr_array_index(diags, j);
      if (!d)
        continue;

      int severity = diagnostic_severity(d);
      const char *msg = diagnostic_message(d);
      int line = 1, col = 1;
      diagnostic_position(tab, d, &line, &col);

      const char *icon_name = "dialog-information-symbolic";
      const char *css_cls = "tcode-problem-info";
      if (severity == 1) {
        total_err++;
        icon_name = "dialog-error-symbolic";
        css_cls = "tcode-problem-error";
      } else if (severity == 2) {
        total_warn++;
        icon_name = "dialog-warning-symbolic";
        css_cls = "tcode-problem-warning";
      } else {
        total_info++;
      }

      GtkWidget *row = gtk_list_box_row_new();
      gtk_widget_add_css_class(row, "tcode-problem-row");
      g_object_set_data_full(G_OBJECT(row), "problem-path", g_strdup(tab->path), g_free);
      g_object_set_data(G_OBJECT(row), "problem-line", GINT_TO_POINTER(line));
      g_object_set_data(G_OBJECT(row), "problem-col", GINT_TO_POINTER(col));

      GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
      GtkWidget *icon = gtk_image_new_from_icon_name(icon_name);
      gtk_widget_add_css_class(icon, css_cls);
      gtk_box_append(GTK_BOX(hbox), icon);

      GtkWidget *msg_label = gtk_label_new(msg);
      gtk_label_set_xalign(GTK_LABEL(msg_label), 0.0f);
      gtk_label_set_ellipsize(GTK_LABEL(msg_label), PANGO_ELLIPSIZE_END);
      gtk_widget_set_hexpand(msg_label, TRUE);
      gtk_box_append(GTK_BOX(hbox), msg_label);

      char *loc_text = g_strdup_printf("%s:%d:%d", basename, line, col);
      GtkWidget *loc_label = gtk_label_new(loc_text);
      gtk_widget_add_css_class(loc_label, "tcode-problem-location");
      gtk_box_append(GTK_BOX(hbox), loc_label);
      g_free(loc_text);

      char *tip = g_strdup_printf("%s (%s:%d:%d)", msg, tab->path, line, col);
      gtk_widget_set_tooltip_text(row, tip);
      g_free(tip);

      gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), hbox);
      gtk_list_box_append(GTK_LIST_BOX(pv.list), row);
    }
    g_free(basename);
  }

  char err_buf[32], warn_buf[32], info_buf[32];
  g_snprintf(err_buf, sizeof(err_buf), "%d Errors", total_err);
  g_snprintf(warn_buf, sizeof(warn_buf), "%d Warnings", total_warn);
  g_snprintf(info_buf, sizeof(info_buf), "%d Notes", total_info);

  if (pv.err_label)
    gtk_label_set_text(pv.err_label, err_buf);
  if (pv.warn_label)
    gtk_label_set_text(pv.warn_label, warn_buf);
  if (pv.info_label)
    gtk_label_set_text(pv.info_label, info_buf);

  int total = total_err + total_warn + total_info;
  if (pv.empty_label)
    gtk_widget_set_visible(pv.empty_label, total == 0);
  if (pv.list)
    gtk_widget_set_visible(pv.list, total > 0);
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

  GtkCssProvider *provider = gtk_css_provider_new();
  gtk_css_provider_load_from_string(provider, problems_css);
  gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(provider),
                                             GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref(provider);

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
