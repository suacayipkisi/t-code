/*
 * The Run and Debug panel's Breakpoints (see state.h): every file's breakpoints by place, a click
 * opens one, its button or menu removes it.
 */
#include "debug/view/state.h"

#include "debug/view/debugview.h"
#include "editor/editor.h"
#include "editor/gutter/breakpoints.h"
#include "popups/contextmenu.h"

#include <string.h>

/* Breakpoints ---------------------------------------------------------------------------------- */

typedef struct {
  const char *path;
  Breakpoint *b;
} Listed;

static int by_place(gconstpointer x, gconstpointer y)
{
  const Listed *a = x, *b = y;
  int paths = strcmp(a->path, b->path);

  return paths ? paths : a->b->line - b->b->line;
}

static void on_remove_breakpoint(GtkButton *button, gpointer data G_GNUC_UNUSED)
{
  char *path = g_strdup(g_object_get_data(G_OBJECT(button), "path"));

  breakpoint_remove(view.app, path, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "line")));
  g_free(path);
}

/* A click on a breakpoint: its line in the editor. */
void bplist_on_activated(GtkListBox *box G_GNUC_UNUSED, GtkListBoxRow *row, gpointer data G_GNUC_UNUSED)
{
  const char *path = g_object_get_data(G_OBJECT(row), "path");

  if (path)
    open_file_at(view.app, path, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "line")), 1);
}

/* A breakpoint's right-click menu: its place, copied. */
typedef struct {
  char *path;
  int line;
} Place;

static void place_free(Place *p)
{
  g_free(p->path);
  g_free(p);
}

static void open_place(gpointer data)
{
  open_file_at(view.app, ((Place *)data)->path, ((Place *)data)->line, 1);
}

static void remove_place(gpointer data)
{
  breakpoint_remove(view.app, ((Place *)data)->path, ((Place *)data)->line);
}

static void remove_every(gpointer data G_GNUC_UNUSED)
{
  breakpoints_remove_all(view.app);
}

void bplist_on_menu(GtkWidget *list, double x, double y, gpointer data G_GNUC_UNUSED)
{
  static const MenuItem items[] = {
    { "Open", open_place },
    { NULL, NULL },
    { "Remove Breakpoint", remove_place },
    { "Remove All Breakpoints", remove_every },
  };
  GtkListBoxRow *row = gtk_list_box_get_row_at_y(GTK_LIST_BOX(list), (int)y);
  const char *path = row ? g_object_get_data(G_OBJECT(row), "path") : NULL;

  if (!path)
    return;
  Place *p = g_new(Place, 1);
  p->path = g_strdup(path);
  p->line = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "line"));
  context_menu(list, x, y, items, G_N_ELEMENTS(items), p, (GDestroyNotify)place_free);
}

void debug_view_breakpoints_changed(App *t)
{
  GArray *all = g_array_new(FALSE, FALSE, sizeof(Listed));
  GHashTableIter iter;
  gpointer path, list;

  if (!view.breakpoints) {
    g_array_free(all, TRUE);
    return;
  }
  context_menu_clear_list(view.breakpoints);
  if (t->debug.breakpoints) {
    g_hash_table_iter_init(&iter, t->debug.breakpoints);
    while (g_hash_table_iter_next(&iter, &path, &list))
      for (guint i = 0; i < ((GArray *)list)->len; i++) {
        Listed l = { path, &g_array_index((GArray *)list, Breakpoint, i) };
        g_array_append_val(all, l);
      }
  }
  g_array_sort(all, by_place);
  for (guint i = 0; i < all->len; i++) {
    Listed *l = &g_array_index(all, Listed, i);
    char *name = g_path_get_basename(l->path), *where = g_strdup_printf("%s:%d", name, l->b->line);
    GtkWidget *row = gtk_list_box_row_new(), *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *place = gtk_label_new(where), *condition = gtk_label_new(NULL);
    GtkWidget *remove = gtk_button_new_from_icon_name("window-close-symbolic");

    if (l->b->condition) {
      char *text = g_strdup_printf("when %s", l->b->condition);
      gtk_label_set_text(GTK_LABEL(condition), text);
      g_free(text);
    }
    gtk_widget_add_css_class(condition, "condition");
    gtk_label_set_xalign(GTK_LABEL(condition), 0);
    gtk_label_set_ellipsize(GTK_LABEL(condition), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(condition, TRUE);
    gtk_widget_add_css_class(remove, "flat");
    gtk_widget_add_css_class(remove, "remove");
    gtk_widget_set_tooltip_text(remove, "Remove this breakpoint");
    g_object_set_data_full(G_OBJECT(remove), "path", g_strdup(l->path), g_free);
    g_object_set_data(G_OBJECT(remove), "line", GINT_TO_POINTER(l->b->line));
    g_signal_connect(remove, "clicked", G_CALLBACK(on_remove_breakpoint), NULL);
    gtk_box_append(GTK_BOX(box), place);
    gtk_box_append(GTK_BOX(box), condition);
    gtk_box_append(GTK_BOX(box), remove);
    gtk_widget_set_margin_start(box, 10);
    gtk_widget_set_margin_end(box, 4);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
    gtk_widget_set_tooltip_text(row, l->path);
    g_object_set_data_full(G_OBJECT(row), "path", g_strdup(l->path), g_free);
    g_object_set_data(G_OBJECT(row), "line", GINT_TO_POINTER(l->b->line));
    gtk_list_box_append(view.breakpoints, row);
    g_free(where);
    g_free(name);
  }
  if (all->len == 0) {
    GtkWidget *row = gtk_list_box_row_new(), *label = gtk_label_new("None: click left of a line number to add one.");
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_label_set_wrap(GTK_LABEL(label), TRUE);
    gtk_widget_add_css_class(label, "condition");
    gtk_widget_set_margin_start(label, 10);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), label);
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
    gtk_list_box_append(view.breakpoints, row);
  }
  g_array_free(all, TRUE);
}

void bplist_on_remove_all(GtkButton *button G_GNUC_UNUSED, gpointer data G_GNUC_UNUSED)
{
  breakpoints_remove_all(view.app);
}
