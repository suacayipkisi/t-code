/*
 * Plugins' views (see views.h): a GtkTextView in a scrolled window, the panel of a dock (window.h's
 * add_panel) or the page of a TAB_VIEW tab. Each is kept by its name, "view:<plugin>/<id>", until its
 * widget goes.
 */
#include "plugins/views.h"

#include "editor/tabs.h"
#include "ipc/socket.h"
#include "window/window.h"

#include <string.h>

typedef struct {
  App *t;
  char *owner, *id, *name, *title;
  GtkWidget *scrolled; /* the panel or the tab's page */
  GtkTextView *text;
  gboolean in_dock;
} View;

static GHashTable *views; /* name → View */

static char *view_name(const char *owner, const char *id)
{
  return g_strdup_printf("view:%s/%s", owner, id);
}

static View *find_view(const char *owner, const char *id)
{
  char *name = view_name(owner, id);
  View *view = views ? g_hash_table_lookup(views, name) : NULL;

  g_free(name);
  return view;
}

gboolean view_place(const char *word, ViewPlace *place)
{
  static const char *const words[] = { "left", "bottom", "right", "editor", "side" };

  for (size_t i = 0; i < G_N_ELEMENTS(words); i++)
    if (g_strcmp0(word, words[i]) == 0) {
      *place = (ViewPlace)i;
      return TRUE;
    }
  return FALSE;
}

/* The view's widget went (its panel taken away, its tab closed, the window gone). */
static void on_view_destroy(GtkWidget *widget G_GNUC_UNUSED, View *view)
{
  if (!view->in_dock && !view->t->closing && g_hash_table_lookup(views, view->name) == view)
    socket_view_closed(view->t, view->owner, view->id);
  g_hash_table_remove(views, view->name);
}

static void view_free(View *view)
{
  g_free(view->owner);
  g_free(view->id);
  g_free(view->name);
  g_free(view->title);
  g_free(view);
}

/* A line was activated: the plugin hears which (from 1) and its text. */
static void activate_line(View *view, GtkTextIter *at)
{
  GtkTextBuffer *buffer = gtk_text_view_get_buffer(view->text);
  GtkTextIter start = *at, end;
  char *text;

  gtk_text_iter_set_line_offset(&start, 0);
  end = start;
  if (!gtk_text_iter_ends_line(&end))
    gtk_text_iter_forward_to_line_end(&end);
  text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
  socket_view_activated(view->t, view->owner, view->id, gtk_text_iter_get_line(&start) + 1, text);
  g_free(text);
}

static void on_double_click(GtkGestureClick *gesture, int n_press, double x, double y, View *view)
{
  GtkTextIter at;
  int bx, by;

  if (n_press != 2)
    return;
  gtk_text_view_window_to_buffer_coords(view->text, GTK_TEXT_WINDOW_WIDGET, (int)x, (int)y, &bx, &by);
  gtk_text_view_get_iter_at_location(view->text, &at, bx, by);
  activate_line(view, &at);
  gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
}

static gboolean on_key(GtkEventControllerKey *keys G_GNUC_UNUSED, guint keyval, guint code G_GNUC_UNUSED,
                       GdkModifierType state G_GNUC_UNUSED, View *view)
{
  GtkTextBuffer *buffer = gtk_text_view_get_buffer(view->text);
  GtkTextIter at;

  if (keyval != GDK_KEY_Return && keyval != GDK_KEY_KP_Enter)
    return FALSE;
  gtk_text_buffer_get_iter_at_mark(buffer, &at, gtk_text_buffer_get_insert(buffer));
  activate_line(view, &at);
  return TRUE;
}

static View *new_view(App *t, const char *owner, const char *id, const char *title)
{
  View *view = g_new0(View, 1);
  GtkGesture *click = gtk_gesture_click_new();
  GtkEventController *keys = gtk_event_controller_key_new();

  view->t = t;
  view->owner = g_strdup(owner);
  view->id = g_strdup(id);
  view->name = view_name(owner, id);
  view->title = g_strdup(title);
  view->text = GTK_TEXT_VIEW(gtk_text_view_new());
  gtk_text_view_set_editable(view->text, FALSE);
  gtk_text_view_set_monospace(view->text, TRUE);
  gtk_text_view_set_left_margin(view->text, 8);
  gtk_text_view_set_top_margin(view->text, 6);
  gtk_widget_add_css_class(GTK_WIDGET(view->text), "tcode-view");
  g_signal_connect(click, "pressed", G_CALLBACK(on_double_click), view);
  gtk_widget_add_controller(GTK_WIDGET(view->text), GTK_EVENT_CONTROLLER(click));
  g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key), view);
  gtk_widget_add_controller(GTK_WIDGET(view->text), keys);
  view->scrolled = gtk_scrolled_window_new();
  gtk_widget_set_hexpand(view->scrolled, TRUE);
  gtk_widget_set_vexpand(view->scrolled, TRUE);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(view->scrolled), GTK_WIDGET(view->text));
  g_object_set_data_full(G_OBJECT(view->scrolled), "t-code-view-title", g_strdup(title), g_free);
  g_signal_connect(view->scrolled, "destroy", G_CALLBACK(on_view_destroy), view);
  if (!views)
    views = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, (GDestroyNotify)view_free);
  g_hash_table_insert(views, view->name, view);
  return view;
}

/* The tab showing a view, or NULL (it is in a dock). */
static Tab *view_tab(View *view)
{
  return find_tab(view->t, TAB_VIEW, view->name);
}

void view_open(App *t, const char *owner, const char *id, const char *title, ViewPlace place, const char *icon,
               const char *text)
{
  View *view = find_view(owner, id);

  if (!title || !*title)
    title = id;
  if (!view) {
    view = new_view(t, owner, id, title);
    if (place <= VIEW_RIGHT) {
      view->in_dock = TRUE;
      add_panel(t, view->name, title, icon && *icon ? icon : "tcode-plugins-symbolic", view->scrolled, (int)place);
    } else {
      if (place == VIEW_SIDE)
        side_group(t);
      Tab *tab = add_tab(t, TAB_VIEW, view->scrolled, view->name);
      keep_tab(tab);
    }
  } else if (g_strcmp0(view->title, title) != 0) {
    g_set_str(&view->title, title);
    g_object_set_data_full(G_OBJECT(view->scrolled), "t-code-view-title", g_strdup(title), g_free);
    if (view->in_dock) {
      GtkStack *stack = panel_stack(t, view->name);
      if (stack)
        gtk_stack_page_set_title(gtk_stack_get_page(stack, view->scrolled), title);
    } else if (view_tab(view)) {
      update_tab_label(view_tab(view));
    }
  }
  if (text)
    view_set_text(t, owner, id, text, FALSE);
  view_show(t, owner, id);
}

gboolean view_set_text(App *t G_GNUC_UNUSED, const char *owner, const char *id, const char *text, gboolean append)
{
  View *view = find_view(owner, id);
  GtkTextBuffer *buffer;
  GtkTextIter end;

  if (!view)
    return FALSE;
  buffer = gtk_text_view_get_buffer(view->text);
  if (!append) {
    gtk_text_buffer_set_text(buffer, text ? text : "", -1);
    return TRUE;
  }
  gtk_text_buffer_get_end_iter(buffer, &end);
  if (gtk_text_buffer_get_char_count(buffer) > 0)
    gtk_text_buffer_insert(buffer, &end, "\n", 1);
  gtk_text_buffer_insert(buffer, &end, text ? text : "", -1);
  gtk_text_buffer_get_end_iter(buffer, &end); /* a log: its end in view */
  gtk_text_view_scroll_to_iter(view->text, &end, 0, FALSE, 0, 1);
  return TRUE;
}

gboolean view_show(App *t, const char *owner, const char *id)
{
  View *view = find_view(owner, id);

  if (!view)
    return FALSE;
  if (view->in_dock) {
    show_sidebar_page(t, view->name);
  } else if (view_tab(view)) {
    Tab *tab = view_tab(view);
    gtk_notebook_set_current_page(tab_group(tab), tab_position(tab));
  }
  return TRUE;
}

gboolean view_close(App *t, const char *owner, const char *id)
{
  View *view = find_view(owner, id);

  if (!view)
    return FALSE;
  g_signal_handlers_disconnect_by_func(view->scrolled, on_view_destroy, view); /* no view_closed: the plugin did */
  if (view->in_dock)
    remove_panel(t, view->name);
  else if (view_tab(view))
    remove_tab(view_tab(view));
  g_hash_table_remove(views, view->name);
  return TRUE;
}

const char *view_title(Tab *tab)
{
  return tab->kind == TAB_VIEW ? g_object_get_data(G_OBJECT(tab->page), "t-code-view-title") : NULL;
}
