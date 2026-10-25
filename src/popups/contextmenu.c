/*
 * Right-click menus (see contextmenu.h): a GtkPopoverMenu over a GMenu whose items are actions of
 * a group ("ctx") put on the widget while the menu shows.
 */
#include "popups/contextmenu.h"

typedef struct {
  GtkWidget *widget, *popover;
  MenuItem *items;
  int n, chosen; /* the item picked, or -1 */
  gpointer data;
  GDestroyNotify free_data;
  guint finish_id;
} Menu;

static Menu *active_menu;

static void on_item(GSimpleAction *action G_GNUC_UNUSED, GVariant *param G_GNUC_UNUSED, gpointer data)
{
  Menu *menu = g_object_get_data(G_OBJECT(action), "t-code-menu");

  menu->chosen = GPOINTER_TO_INT(data);
}

static void on_closed(GtkPopover *popover G_GNUC_UNUSED, Menu *menu);

static void menu_free(Menu *menu, gboolean run_chosen)
{
  if (!menu)
    return;
  if (menu == active_menu)
    active_menu = NULL;
  if (menu->finish_id) {
    g_source_remove(menu->finish_id);
    menu->finish_id = 0;
  }
  gtk_widget_insert_action_group(menu->widget, "ctx", NULL);
  if (menu->popover) {
    g_signal_handlers_disconnect_by_func(menu->popover, on_closed, menu);
    if (gtk_widget_get_parent(menu->popover))
      gtk_widget_unparent(menu->popover);
    menu->popover = NULL;
  }
  if (run_chosen && menu->chosen >= 0 && menu->items[menu->chosen].run)
    menu->items[menu->chosen].run(menu->data);
  if (menu->free_data)
    menu->free_data(menu->data);
  g_object_unref(menu->widget);
  g_free(menu->items);
  g_free(menu);
}

/* The menu has closed: its item runs (the widget may go then), and the menu goes. */
static gboolean finish(gpointer data)
{
  Menu *menu = data;

  menu->finish_id = 0;
  menu_free(menu, TRUE);
  return G_SOURCE_REMOVE;
}

static void on_closed(GtkPopover *popover G_GNUC_UNUSED, Menu *menu)
{
  if (!menu->finish_id)
    menu->finish_id = g_idle_add(finish, menu); /* an item's action comes in after closed */
}

void context_menu(GtkWidget *widget, double x, double y, const MenuItem *items, int n, gpointer data,
                  GDestroyNotify free_data)
{
  if (active_menu)
    menu_free(active_menu, FALSE);

  Menu *menu = g_new0(Menu, 1);
  GSimpleActionGroup *group = g_simple_action_group_new();
  GMenu *model = g_menu_new(), *section = g_menu_new();

  menu->widget = g_object_ref(widget);
  menu->items = g_memdup2(items, sizeof *items * n);
  menu->n = n;
  menu->chosen = -1;
  menu->data = data;
  menu->free_data = free_data;
  active_menu = menu;
  for (int i = 0; i < n; i++) {
    if (!items[i].label) { /* a line: the items so far are a section */
      g_menu_append_section(model, NULL, G_MENU_MODEL(section));
      g_object_unref(section);
      section = g_menu_new();
      continue;
    }
    char name[16], detailed[24];
    g_snprintf(name, sizeof name, "i%d", i);
    g_snprintf(detailed, sizeof detailed, "ctx.%s", name);
    GSimpleAction *action = g_simple_action_new(name, NULL);
    g_object_set_data(G_OBJECT(action), "t-code-menu", menu);
    g_signal_connect(action, "activate", G_CALLBACK(on_item), GINT_TO_POINTER(i));
    g_action_map_add_action(G_ACTION_MAP(group), G_ACTION(action));
    g_object_unref(action);
    g_menu_append(section, items[i].label, detailed);
  }
  g_menu_append_section(model, NULL, G_MENU_MODEL(section));
  g_object_unref(section);

  gtk_widget_insert_action_group(widget, "ctx", G_ACTION_GROUP(group));
  menu->popover = gtk_popover_menu_new_from_model(G_MENU_MODEL(model));
  gtk_popover_set_has_arrow(GTK_POPOVER(menu->popover), FALSE);
  gtk_widget_set_halign(menu->popover, GTK_ALIGN_START);
  gtk_widget_set_parent(menu->popover, widget);
  gtk_popover_set_pointing_to(GTK_POPOVER(menu->popover), &(GdkRectangle){ (int)x, (int)y, 1, 1 });
  g_signal_connect(menu->popover, "closed", G_CALLBACK(on_closed), menu);
  gtk_popover_popup(GTK_POPOVER(menu->popover));
  g_object_unref(model);
  g_object_unref(group);
}

typedef struct {
  ContextMenuShow show;
  gpointer data;
} Attached;

static void on_right_click(GtkGestureClick *click, int n G_GNUC_UNUSED, double x, double y, Attached *attached)
{
  GtkWidget *widget = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(click));

  gtk_gesture_set_state(GTK_GESTURE(click), GTK_EVENT_SEQUENCE_CLAIMED); /* not the widget's own menu too */
  attached->show(widget, x, y, attached->data);
}

void context_menu_attach(GtkWidget *widget, ContextMenuShow show, gpointer data)
{
  GtkGesture *click = gtk_gesture_click_new();
  Attached *attached = g_new(Attached, 1);

  attached->show = show;
  attached->data = data;
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_SECONDARY);
  g_signal_connect_data(click, "pressed", G_CALLBACK(on_right_click), attached, (GClosureNotify)(void (*)(void))g_free,
                        0);
  gtk_widget_add_controller(widget, GTK_EVENT_CONTROLLER(click));
}

void copy_to_clipboard(const char *text)
{
  gdk_clipboard_set_text(gdk_display_get_clipboard(gdk_display_get_default()), text ? text : "");
}
