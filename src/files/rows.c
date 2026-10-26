/*
 * The Files panel's rows (see panel.h): set up once, bound to the tree rows that scroll by, clicked,
 * dragged and dropped on.
 */
#include "files/panel.h"

#include "editor/editor.h"
#include "files/fileops.h"

#include <string.h>

/* Rows: set up, bound to a tree row, clicked, dragged, dropped on ---------------------------- */

static void on_row_expanded(GtkTreeListRow *row, GParamSpec *pspec G_GNUC_UNUSED, FileRow *r)
{
  if (gtk_tree_list_row_get_expanded(row)) {
    g_hash_table_add(files.expanded, g_strdup(r->path));
    files_load(row);
  } else {
    g_hash_table_remove(files.expanded, r->path);
  }
  files_paint(r);
}

/* A right click on a row selects it and shows its menu (claimed: not the empty space's menu too). */
static void on_row_menu(GtkGestureClick *gesture, int n_press G_GNUC_UNUSED, double x, double y, FileRow *r)
{
  graphene_point_t at;

  if (!r->row)
    return;
  gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
  files_select(gtk_tree_list_row_get_position(r->row));
  if (gtk_widget_compute_point(r->expander, files.panel, &GRAPHENE_POINT_INIT((float)x, (float)y), &at))
    files_show_menu(r->path, at.x, at.y);
}

/* A second left click on the same file within the double-click time (GTK's own count starts again
 * once the first click has moved the focus to the editor). time is the click's own (ms): opening the
 * first click's file keeps the main loop busy, so the second is handled later than it happened. */
static gboolean is_double_click(GtkWidget *widget, const char *path, guint32 time)
{
  static char *last_path;
  static guint32 last_time;
  int interval = 400; /* ms, GTK's default */
  gboolean twice;

  g_object_get(gtk_widget_get_settings(widget), "gtk-double-click-time", &interval, NULL);
  twice = g_strcmp0(path, last_path) == 0 && time - last_time <= (guint32)interval;
  g_set_str(&last_path, twice ? NULL : path); /* a third click is a first one again */
  last_time = time;
  return twice;
}

/* A left click opens a file (source code in a temporary tab; a double click keeps it) or opens /
 * closes a folder, on release: dragging the row cancels it. A middle click opens a file for good. */
static void on_row_click(GtkGestureClick *gesture, int n_press G_GNUC_UNUSED, double x G_GNUC_UNUSED,
                         double y G_GNUC_UNUSED, FileRow *r)
{
  guint button = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture));

  if (!r->row || button == GDK_BUTTON_SECONDARY) /* the menu's */
    return;
  if (r->dir)
    files_activate_row(r->row, FALSE);
  else if (button == GDK_BUTTON_MIDDLE ||
           is_double_click(r->expander, r->path,
                           gtk_event_controller_get_current_event_time(GTK_EVENT_CONTROLLER(gesture))))
    open_file(files.app, r->path);
  else
    files_activate_row(r->row, FALSE);
}

static GdkContentProvider *on_drag_prepare(GtkDragSource *source G_GNUC_UNUSED, double x G_GNUC_UNUSED,
                                           double y G_GNUC_UNUSED, FileRow *r)
{
  GFile *file = g_file_new_for_path(r->path);
  GdkContentProvider *content = gdk_content_provider_new_typed(G_TYPE_FILE, file);

  g_object_unref(file);
  return content;
}

static void on_drag_begin(GtkDragSource *source, GdkDrag *drag G_GNUC_UNUSED, FileRow *r)
{
  GdkPaintable *icon = gtk_widget_paintable_new(r->name);

  gtk_drag_source_set_icon(source, icon, 0, 0);
  g_object_unref(icon);
}

/* A file dropped on a row (its folder) or on the empty space (the root): from inside the project
 * it moves there, from elsewhere (a file manager) it is copied. */
gboolean files_on_drop(GtkDropTarget *target G_GNUC_UNUSED, const GValue *value, double x G_GNUC_UNUSED,
                       double y G_GNUC_UNUSED, FileRow *r)
{
  char *src = g_file_get_path(g_value_get_object(value));
  char *dir = r ? files_folder_of(r->path) : g_strdup(files.app->project.root);
  gboolean dropped = src && (!r || strcmp(src, r->path) != 0); /* a row let go on itself: nothing */

  if (dropped)
    files_move_or_copy(src, dir, files_inside(src, files.app->project.root));
  g_free(src);
  g_free(dir);
  return dropped;
}

static void free_row(FileRow *r)
{
  g_free(r->path);
  g_free(r);
}

void files_setup_row(GtkSignalListItemFactory *factory G_GNUC_UNUSED, GtkListItem *item, gpointer data G_GNUC_UNUSED)
{
  FileRow *r = g_new0(FileRow, 1);
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  GtkGesture *click = gtk_gesture_click_new(), *menu = gtk_gesture_click_new();
  GtkDragSource *drag = gtk_drag_source_new();
  GtkDropTarget *drop = gtk_drop_target_new(G_TYPE_FILE, GDK_ACTION_COPY | GDK_ACTION_MOVE);

  r->expander = gtk_tree_expander_new();
  r->icon = gtk_label_new(NULL);
  r->name = gtk_label_new(NULL);
  r->badge = gtk_label_new(NULL);
  gtk_widget_add_css_class(r->icon, "icon");
  gtk_widget_add_css_class(r->badge, "badge");
  gtk_label_set_xalign(GTK_LABEL(r->name), 0);
  gtk_label_set_ellipsize(GTK_LABEL(r->name), PANGO_ELLIPSIZE_END);
  gtk_widget_set_hexpand(r->name, TRUE);
  gtk_box_append(GTK_BOX(box), r->icon);
  gtk_box_append(GTK_BOX(box), r->name);
  gtk_box_append(GTK_BOX(box), r->badge);
  gtk_tree_expander_set_child(GTK_TREE_EXPANDER(r->expander), box);
  gtk_tree_expander_set_indent_for_icon(GTK_TREE_EXPANDER(r->expander), TRUE);

  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 0); /* left and middle; the right one: menu */
  g_signal_connect(click, "released", G_CALLBACK(on_row_click), r);
  gtk_widget_add_controller(box, GTK_EVENT_CONTROLLER(click));
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(menu), GDK_BUTTON_SECONDARY);
  g_signal_connect(menu, "pressed", G_CALLBACK(on_row_menu), r);
  gtk_widget_add_controller(box, GTK_EVENT_CONTROLLER(menu));
  gtk_drag_source_set_actions(drag, GDK_ACTION_MOVE | GDK_ACTION_COPY);
  g_signal_connect(drag, "prepare", G_CALLBACK(on_drag_prepare), r);
  g_signal_connect(drag, "drag-begin", G_CALLBACK(on_drag_begin), r);
  gtk_widget_add_controller(box, GTK_EVENT_CONTROLLER(drag));
  g_signal_connect(drop, "drop", G_CALLBACK(files_on_drop), r);
  gtk_widget_add_controller(r->expander, GTK_EVENT_CONTROLLER(drop));

  /* Not activatable: the list's own double click would claim the second click's events from the
   * row's gesture (on_row_click), which then never sees it. Enter is on_key's. */
  gtk_list_item_set_activatable(item, FALSE);
  g_object_set_data_full(G_OBJECT(item), "row", r, (GDestroyNotify)free_row);
  gtk_list_item_set_child(item, r->expander);
}

void files_bind_row(GtkSignalListItemFactory *factory G_GNUC_UNUSED, GtkListItem *item, gpointer data G_GNUC_UNUSED)
{
  FileRow *r = g_object_get_data(G_OBJECT(item), "row");
  GFileInfo *info;

  r->row = gtk_list_item_get_item(item);
  info = info_of(r->row);
  r->path = info_path(info);
  r->dir = info_is_dir(info);
  g_object_unref(info);
  gtk_tree_expander_set_list_row(GTK_TREE_EXPANDER(r->expander), r->row);
  r->expanded_handler = g_signal_connect(r->row, "notify::expanded", G_CALLBACK(on_row_expanded), r);
  g_ptr_array_add(files.rows, r);
  files_paint(r);
}

void files_unbind_row(GtkSignalListItemFactory *factory G_GNUC_UNUSED, GtkListItem *item, gpointer data G_GNUC_UNUSED)
{
  FileRow *r = g_object_get_data(G_OBJECT(item), "row");

  g_ptr_array_remove_fast(files.rows, r);
  g_clear_signal_handler(&r->expanded_handler, r->row);
  gtk_tree_expander_set_list_row(GTK_TREE_EXPANDER(r->expander), NULL);
  g_clear_pointer(&r->path, g_free);
  r->row = NULL;
}
