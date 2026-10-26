/*
 * The window's title bar (see docks/docks.h): the menu bar (Files, T-Code, Settings, Panels, About,
 * Help) and the buttons that show or hide the sides and split the editor.
 */
#include "window/docks/docks.h"

#include "editor/tabs.h"
#include "window/commands.h"
#include "window/window.h"

#include <string.h>

/* The Split Editor button in the title bar. */
static void on_split_toggled(GtkToggleButton *button, App *t)
{
  set_split(t, gtk_toggle_button_get_active(button));
  focus_current_tab(t);
}

static void on_toggle_left(App *t)
{
  toggle_pane(t, t->layout.left);
}

static void on_toggle_right(App *t)
{
  if (side_has_pages(t, 2))
    toggle_pane(t, t->layout.right);
}

static void on_toggle_bottom(App *t)
{
  toggle_pane(t, side_has_pages(t, 1) ? t->layout.bottom : GTK_WIDGET(t->shell.term));
}

/* win.panel-<name> (the Panels menu): shows or hides that panel. */
static void on_panel_action(GSimpleAction *action, GVariant *param G_GNUC_UNUSED, gpointer t)
{
  toggle_panel(t, g_action_get_name(G_ACTION(action)) + strlen("panel-"));
}

/* win.command: runs commands[]'s command of that name (window/commands.c), for the menu bar. */
static void on_command_action(GSimpleAction *action G_GNUC_UNUSED, GVariant *param, gpointer t)
{
  run_command(t, g_variant_get_string(param, NULL));
}

/* win.quit: closes the window, asking about unsaved files as its close button does. */
static void on_quit_action(GSimpleAction *action G_GNUC_UNUSED, GVariant *param G_GNUC_UNUSED, gpointer t)
{
  gtk_window_close(((App *)t)->window);
}

/* The menu bar's actions: a checked one per panel, win.command and win.quit. */
static void add_menu_actions(App *t)
{
  static const GActionEntry actions[] = {
    { .name = "command", .activate = on_command_action, .parameter_type = "s" },
    { .name = "quit", .activate = on_quit_action },
  };

  g_action_map_add_action_entries(G_ACTION_MAP(t->window), actions, G_N_ELEMENTS(actions), t);
  for (int i = 0; i < PANELS; i++) {
    char *name = g_strconcat("panel-", panel_names[i], NULL);
    GSimpleAction *action = g_simple_action_new_stateful(name, NULL, g_variant_new_boolean(FALSE));

    g_signal_connect(action, "activate", G_CALLBACK(on_panel_action), t);
    g_action_map_add_action(G_ACTION_MAP(t->window), G_ACTION(action));
    g_object_unref(action);
    g_free(name);
  }
}

/* An item running action (with target, a string, when not NULL), its shortcut accel shown. */
static void add_item(GMenu *menu, const char *label, const char *action, const char *target, const char *accel)
{
  GMenuItem *item = g_menu_item_new(label, NULL);

  g_menu_item_set_action_and_target_value(item, action, target ? g_variant_new_string(target) : NULL);
  if (accel)
    g_menu_item_set_attribute(item, "accel", "s", accel);
  g_menu_append_item(menu, item);
  g_object_unref(item);
}

/* An item running commands[]'s command of that name. */
static void add_command(GMenu *menu, const char *label, const char *command, const char *accel)
{
  add_item(menu, label, "win.command", command, accel);
}

static void add_submenu(GMenu *bar, const char *label, GMenu *menu)
{
  g_menu_append_submenu(bar, label, G_MENU_MODEL(menu));
  g_object_unref(menu);
}

/* Files, T-Code, Settings, Panels (checked while they show), About and Help. */
static GMenu *menu_bar_model(App *t)
{
  static const char *const labels[PANELS] = { "Files",         "Source Control", "Search",  "Terminal", "Debug",
                                              "Run and Debug", "Problems",       "Outline", "Hierarchy" };
  static const char *const accels[PANELS] = { "<Alt>1", "<Control><Shift>g", "<Control><Shift>f", "<Control>j",
                                              NULL,     "<Control><Shift>d", "<Control><Shift>m", NULL,
                                              NULL };
  GMenu *bar = g_menu_new(), *menu = g_menu_new(), *section = g_menu_new();

  add_command(menu, "Open Folder…", "Open Folder…", "<Control><Shift>o");
  g_menu_append_submenu(menu, "Recent Folders", G_MENU_MODEL(t->recent_menu));
  add_submenu(bar, "Files", menu);

  menu = g_menu_new();
  add_item(menu, "New Window", "app.new-window", NULL, NULL);
  add_command(menu, "Command Palette…", "Command Palette…", "<Control><Shift>p");
  add_command(menu, "Check for Updates Now", "Check for Updates Now", NULL);
  add_item(section, "Quit", "win.quit", NULL, NULL);
  g_menu_append_section(menu, NULL, G_MENU_MODEL(section));
  g_object_unref(section);
  add_submenu(bar, "T-Code", menu);

  menu = g_menu_new();
  section = g_menu_new();
  add_command(menu, "Settings…", "Settings…", "<Control>comma");
  add_command(menu, "Color Theme…", "Preferences: Color Theme…", NULL);
  add_command(section, "Open Settings File", "Preferences: Open Settings File", NULL);
  add_command(section, "Open Theme CSS", "Preferences: Open Theme CSS", NULL);
  add_command(section, "Open Editor Colors", "Preferences: Open Editor Colors", NULL);
  g_menu_append_section(menu, NULL, G_MENU_MODEL(section));
  g_object_unref(section);
  add_submenu(bar, "Settings", menu);

  menu = g_menu_new();
  for (int i = 0; i < PANELS; i++) {
    char *action = g_strconcat("win.panel-", panel_names[i], NULL);

    add_item(menu, labels[i], action, NULL, accels[i]);
    g_free(action);
  }
  add_submenu(bar, "Panels", menu);

  menu = g_menu_new();
  add_item(menu, "About t-code", "win.about", NULL, NULL);
  add_submenu(bar, "About", menu);

  menu = g_menu_new();
  add_item(menu, "Keyboard Shortcuts", "win.keybinds", NULL, NULL);
  add_item(menu, "Reset Panel Layout", "win.reset-panels", NULL, NULL);
  add_submenu(bar, "Help", menu);
  return bar;
}

GtkWidget *title_bar(App *t)
{
  GtkWidget *header = gtk_header_bar_new();
  GMenu *model = menu_bar_model(t);
  GtkWidget *menu_bar = gtk_popover_menu_bar_new_from_model(G_MENU_MODEL(model));
  GtkWidget *left_btn = gtk_button_new_from_icon_name("sidebar-show-symbolic");
  GtkWidget *right_btn = gtk_button_new_from_icon_name("sidebar-show-right-symbolic");
  GtkWidget *bottom_btn = gtk_button_new_from_icon_name("go-bottom-symbolic");
  GtkWidget *split = gtk_toggle_button_new();

  add_menu_actions(t);
  gtk_header_bar_pack_start(GTK_HEADER_BAR(header), menu_bar);
  g_object_unref(model);

  gtk_button_set_icon_name(GTK_BUTTON(split), "view-dual-symbolic");
  gtk_widget_set_tooltip_text(split, "Split Editor (Ctrl+\\)");
  t->editors.split_button = GTK_TOGGLE_BUTTON(split);
  g_signal_connect(split, "toggled", G_CALLBACK(on_split_toggled), t);
  gtk_header_bar_pack_end(GTK_HEADER_BAR(header), split);

  gtk_widget_set_tooltip_text(right_btn, "Toggle Right Sidebar");
  g_signal_connect_swapped(right_btn, "clicked", G_CALLBACK(on_toggle_right), t);
  gtk_header_bar_pack_end(GTK_HEADER_BAR(header), right_btn);

  gtk_widget_set_tooltip_text(bottom_btn, "Toggle Bottom Panel (Ctrl+J)");
  g_signal_connect_swapped(bottom_btn, "clicked", G_CALLBACK(on_toggle_bottom), t);
  gtk_header_bar_pack_end(GTK_HEADER_BAR(header), bottom_btn);

  gtk_widget_set_tooltip_text(left_btn, "Toggle Left Sidebar (Ctrl+B)");
  g_signal_connect_swapped(left_btn, "clicked", G_CALLBACK(on_toggle_left), t);
  gtk_header_bar_pack_end(GTK_HEADER_BAR(header), left_btn);
  return header;
}
