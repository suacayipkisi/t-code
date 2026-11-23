/*
 * The window's title bar (see docks/docks.h): the menu bar (Files, T-Code, Settings, Panels, About,
 * Help) and the buttons that show or hide the sides. The Panels menu turns t-code's panels and parts of
 * the window on and off; with the menu bar off, t-code's icon opens the same menus. In the middle, the
 * search: Go to File's picker, where '#' first finds commands.
 */
#include "window/docks/docks.h"

#include "editor/tabs.h"
#include "popups/picker.h"
#include "project/session.h"
#include "window/commands.h"
#include "window/window.h"

#include <stddef.h>
#include <string.h>

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
  toggle_pane(t, side_has_pages(t, 1) ? t->layout.bottom : t->shell.panel);
}

/* win.panel-<name> (the Panels menu): turns that panel on or off. */
static void on_panel_action(GSimpleAction *action, GVariant *param G_GNUC_UNUSED, gpointer t)
{
  const char *page = g_action_get_name(G_ACTION(action)) + strlen("panel-");

  set_panel_enabled(t, page, !panel_enabled(t, page));
}

/* The parts of the window the Panels menu turns on and off: each one's action and setting. */
static const struct {
  const char *action, *label;
  size_t field; /* offsetof(App, settings.<name>), a gboolean */
} parts[] = {
  { "ui-menu-bar", "Menu Bar", offsetof(App, settings.menu_bar) },
  { "ui-status-bar", "Status Bar", offsetof(App, settings.status_bar) },
  { "ui-build-buttons", "Build and Run Buttons", offsetof(App, settings.build_buttons) },
  { "ui-side-buttons", "Sidebar Buttons", offsetof(App, settings.side_buttons) },
  { "ui-search-bar", "Search Bar", offsetof(App, settings.search_bar) },
};

static gboolean *part_setting(App *t, size_t i)
{
  return (gboolean *)((char *)t + parts[i].field);
}

/* win.ui-<part>: turns that part of the window on or off. */
static void on_part_action(GSimpleAction *action, GVariant *param G_GNUC_UNUSED, gpointer data)
{
  App *t = data;

  for (size_t i = 0; i < G_N_ELEMENTS(parts); i++)
    if (strcmp(g_action_get_name(G_ACTION(action)), parts[i].action) == 0)
      *part_setting(t, i) = !*part_setting(t, i);
  apply_interface(t);
  session_save_settings(t);
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

/* The menu bar's actions: a checked one per panel and part of the window, win.command and win.quit. */
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
  for (size_t i = 0; i < G_N_ELEMENTS(parts); i++) {
    GSimpleAction *action = g_simple_action_new_stateful(parts[i].action, NULL, g_variant_new_boolean(TRUE));
    g_signal_connect(action, "activate", G_CALLBACK(on_part_action), t);
    g_action_map_add_action(G_ACTION_MAP(t->window), G_ACTION(action));
    g_object_unref(action);
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

GMenu *panels_menu(void)
{
  static const char *const labels[PANELS] = { "Files",     "Source Control", "Search",   "Terminal",
                                              "Debug",     "Run and Debug",  "Problems", "Outline",
                                              "Hierarchy", "Testing",        "Plugins" };
  GMenu *menu = g_menu_new(), *section = g_menu_new();

  for (int i = 0; i < PANELS; i++) {
    char *action = g_strconcat("win.panel-", panel_names[i], NULL);

    add_item(section, labels[i], action, NULL, NULL);
    g_free(action);
  }
  g_menu_append_section(menu, "Panels", G_MENU_MODEL(section));
  g_object_unref(section);
  section = g_menu_new();
  for (size_t i = 0; i < G_N_ELEMENTS(parts); i++) {
    char *action = g_strconcat("win.", parts[i].action, NULL);

    add_item(section, parts[i].label, action, NULL, NULL);
    g_free(action);
  }
  g_menu_append_section(menu, "Window", G_MENU_MODEL(section));
  g_object_unref(section);
  return menu;
}

/* Files, T-Code, Settings, Plugins, Panels (checked while turned on), About and Help. */
static GMenu *menu_bar_model(App *t)
{
  GMenu *bar = g_menu_new(), *menu = g_menu_new(), *section = g_menu_new();

  add_command(menu, "Open Folder…", "Open Folder…", "<Control><Shift>o");
  g_menu_append_submenu(menu, "Recent Folders", G_MENU_MODEL(t->recent_menu));
  add_submenu(bar, "Files", menu);

  menu = g_menu_new();
  add_item(menu, "New Window", "app.new-window", NULL, NULL);
  add_command(menu, "Command Palette…", "Command Palette…", "<Control><Shift>p");
  add_command(menu, "Process Monitor…", "Help: Process Monitor", NULL);
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
  section = g_menu_new();
  add_command(menu, "Manage Plugins…", "Plugins: Manage Plugins", NULL);
  add_command(menu, "Browse the Registry…", "Plugins: Browse the Registry", NULL);
  add_command(menu, "Plugins Panel", "View: Toggle Plugins", NULL);
  add_command(section, "Reload Installed Plugins", "Plugins: Reload Installed Plugins", NULL);
  add_command(section, "Open Plugins Folder", "Plugins: Open Plugins Folder", NULL);
  add_command(section, "Process Monitor…", "Help: Process Monitor", NULL);
  g_menu_append_section(menu, NULL, G_MENU_MODEL(section));
  g_object_unref(section);
  add_submenu(bar, "Plugins", menu);

  add_submenu(bar, "Panels", panels_menu());

  menu = g_menu_new();
  add_item(menu, "About t-code", "win.about", NULL, NULL);
  add_submenu(bar, "About", menu);

  menu = g_menu_new();
  add_item(menu, "Keyboard Shortcuts", "win.keybinds", NULL, NULL);
  add_item(menu, "Reset Panel Layout", "win.reset-panels", NULL, NULL);
  add_submenu(bar, "Help", menu);
  return bar;
}

/* The window controls: t-code's own icons instead of the theme's. GtkWindowControls makes its
 * buttons again when the layout or the window's state changes, so this runs after each time. */
static void own_control_icons(GtkWidget *widget, gboolean maximized)
{
  for (GtkWidget *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child)) {
    const char *name = NULL;
    if (GTK_IS_IMAGE(child) && gtk_widget_get_parent(child) && GTK_IS_BUTTON(gtk_widget_get_parent(child))) {
      GtkWidget *button = gtk_widget_get_parent(child);
      if (gtk_widget_has_css_class(button, "minimize"))
        name = "tcode-window-minimize-symbolic";
      else if (gtk_widget_has_css_class(button, "maximize"))
        name = maximized ? "tcode-window-restore-symbolic" : "tcode-window-maximize-symbolic";
      else if (gtk_widget_has_css_class(button, "close"))
        name = "tcode-window-close-symbolic";
    }
    if (name && g_strcmp0(gtk_image_get_icon_name(GTK_IMAGE(child)), name) != 0)
      gtk_image_set_from_icon_name(GTK_IMAGE(child), name);
    own_control_icons(child, maximized);
  }
}

static gboolean controls_idle(gpointer data)
{
  GtkRoot *root = gtk_widget_get_root(data);

  own_control_icons(data, GTK_IS_WINDOW(root) && gtk_window_is_maximized(GTK_WINDOW(root)));
  return G_SOURCE_REMOVE;
}

static void controls_changed(GtkWidget *header)
{
  g_idle_add_full(G_PRIORITY_DEFAULT, controls_idle, g_object_ref(header), g_object_unref);
}

/* Watches every GtkWindowControls under widget for new buttons. */
static void watch_controls(GtkWidget *widget, GtkWidget *header)
{
  for (GtkWidget *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child)) {
    if (GTK_IS_WINDOW_CONTROLS(child)) {
      GListModel *buttons = gtk_widget_observe_children(child);
      g_signal_connect_object(buttons, "items-changed", G_CALLBACK(controls_changed), header, G_CONNECT_SWAPPED);
      g_object_set_data_full(G_OBJECT(child), "t-code-buttons", buttons, g_object_unref);
    }
    watch_controls(child, header);
  }
}

/* The desktop's button layout without "icon": the title bar has its own t-code logo, and the window
 * manager's icon beside it showed the logo twice (Debian, desktops whose layout starts with "icon"). */
char *title_bar_layout_without_icon(const char *layout)
{
  GString *out = g_string_new(NULL);
  char **sides = g_strsplit(layout ? layout : "", ":", 2);

  for (int s = 0; sides[s]; s++) {
    char **items = g_strsplit(sides[s], ",", -1);
    gboolean first = TRUE;
    if (s > 0)
      g_string_append_c(out, ':');
    for (int i = 0; items[i]; i++) {
      char *item = g_strstrip(items[i]);
      if (!*item || strcmp(item, "icon") == 0)
        continue;
      g_string_append_printf(out, "%s%s", first ? "" : ",", item);
      first = FALSE;
    }
    g_strfreev(items);
  }
  g_strfreev(sides);
  return g_string_free(out, FALSE);
}

static void sync_decoration_layout(GtkSettings *settings, GParamSpec *pspec G_GNUC_UNUSED, GtkWidget *header)
{
  char *layout = NULL, *own;

  g_object_get(settings, "gtk-decoration-layout", &layout, NULL);
  own = title_bar_layout_without_icon(layout);
  if (g_strcmp0(gtk_header_bar_get_decoration_layout(GTK_HEADER_BAR(header)), own) != 0)
    gtk_header_bar_set_decoration_layout(GTK_HEADER_BAR(header), own);
  g_free(own);
  g_free(layout);
}

/* How wide a side of the title bar wants to be: the widgets beside the search bar on that side. */
static int side_width(GtkWidget *side)
{
  int natural = 0;

  if (side && gtk_widget_get_visible(side))
    gtk_widget_measure(side, GTK_ORIENTATION_HORIZONTAL, -1, NULL, &natural, NULL, NULL);
  return natural;
}

/* The search bar is a third of the window wide, but no wider than leaves it in the middle beside the
 * menus and buttons (at least 160 pixels): after the window's allocation, which the surface's size does
 * not tell (it has the shadow around the window too). */
static gboolean size_search_bar_idle(gpointer bar)
{
  GtkRoot *root = gtk_widget_get_root(bar);
  int width = root ? gtk_widget_get_width(GTK_WIDGET(root)) : 0, now;
  int sides = MAX(side_width(gtk_widget_get_prev_sibling(bar)), side_width(gtk_widget_get_next_sibling(bar)));
  int want = MAX(MIN(width / 3, width - 2 * sides - 24), 160);

  gtk_widget_get_size_request(bar, &now, NULL);
  if (now != want)
    gtk_widget_set_size_request(bar, want, -1);
  return G_SOURCE_REMOVE;
}

/* Each time the window's size changes. */
static void size_search_bar(GdkSurface *surface G_GNUC_UNUSED, int width G_GNUC_UNUSED, int height G_GNUC_UNUSED,
                            GtkWidget *bar)
{
  g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, size_search_bar_idle, g_object_ref(bar), g_object_unref);
}

static void on_header_realize(GtkWidget *header)
{
  App *t = g_object_get_data(G_OBJECT(header), "t-code-app");
  GdkSurface *surface = gtk_native_get_surface(gtk_widget_get_native(header));

  GtkSettings *settings = gtk_widget_get_settings(header);

  g_signal_connect_object(settings, "notify::gtk-decoration-layout", G_CALLBACK(sync_decoration_layout), header, 0);
  sync_decoration_layout(settings, NULL, header);
  watch_controls(header, header);
  g_signal_connect_object(gtk_widget_get_root(header), "notify::maximized", G_CALLBACK(controls_changed), header,
                          G_CONNECT_SWAPPED);
  controls_changed(header);
  if (t && surface) {
    g_signal_connect_object(surface, "layout", G_CALLBACK(size_search_bar), t->layout.search_bar, 0);
    size_search_bar(surface, 0, 0, t->layout.search_bar);
  }
}

/* GtkHeaderBar leaves the children it did not add: the right-click menu goes with it. */
static void on_header_destroy(GtkWidget *header)
{
  GtkWidget *pop = g_object_steal_data(G_OBJECT(header), "t-code-panels-pop");

  if (pop)
    gtk_widget_unparent(pop);
}

/* Right-click on the title bar's empty part: the Panels menu (panels and the window's parts on and off)
 * and the layout's reset, instead of the window manager's menu. On its menus and buttons nothing
 * changes. */
static void on_header_right_click(GtkGestureClick *gesture, int n_press G_GNUC_UNUSED, double x, double y,
                                  gpointer data G_GNUC_UNUSED)
{
  GtkWidget *header = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
  GtkWidget *picked = gtk_widget_pick(header, x, y, GTK_PICK_DEFAULT);
  GType types[] = { GTK_TYPE_SEARCH_ENTRY,     GTK_TYPE_BUTTON,          GTK_TYPE_MENU_BUTTON,
                    GTK_TYPE_POPOVER_MENU_BAR, GTK_TYPE_WINDOW_CONTROLS, GTK_TYPE_POPOVER };
  GtkPopover *pop;

  for (size_t i = 0; picked && i < G_N_ELEMENTS(types); i++)
    if (G_TYPE_CHECK_INSTANCE_TYPE(picked, types[i]) || gtk_widget_get_ancestor(picked, types[i]))
      return;
  gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
  pop = g_object_get_data(G_OBJECT(header), "t-code-panels-pop");
  if (!pop) {
    GMenu *menu = panels_menu(), *section = g_menu_new();
    g_menu_append(section, "Reset Panel Layout", "win.reset-panels");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(section));
    g_object_unref(section);
    pop = GTK_POPOVER(gtk_popover_menu_new_from_model(G_MENU_MODEL(menu)));
    g_object_unref(menu);
    gtk_popover_set_has_arrow(pop, FALSE);
    gtk_widget_set_halign(GTK_WIDGET(pop), GTK_ALIGN_START);
    gtk_widget_set_parent(GTK_WIDGET(pop), header);
    g_object_set_data(G_OBJECT(header), "t-code-panels-pop", pop);
  }
  gtk_popover_set_pointing_to(pop, &(const GdkRectangle){ (int)x, (int)y, 1, 1 });
  gtk_popover_popup(pop);
}

/* The search in the middle of the title bar: popups/picker.c puts its entry here (picker_place), so
 * typing goes into the bar and the results drop down from it. A click opens Go to File; '#' first finds
 * commands, as Ctrl+Shift+P opens it. */
static GtkWidget *search_bar(void)
{
  GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

  gtk_widget_add_css_class(bar, "tcode-search-bar");
  gtk_widget_set_size_request(bar, 160, -1); /* a third of the window once it has a size */
  gtk_widget_set_hexpand(bar, FALSE);        /* the entry's hexpand would stretch it over the title bar */
  gtk_widget_set_valign(bar, GTK_ALIGN_CENTER);
  return bar;
}

GtkWidget *title_bar(App *t)
{
  GtkWidget *header = gtk_header_bar_new();
  GMenu *model = menu_bar_model(t);
  GtkWidget *menu_bar = gtk_popover_menu_bar_new_from_model(G_MENU_MODEL(model));
  GtkWidget *left_btn = gtk_button_new_from_icon_name("tcode-sidebar-left-symbolic");
  GtkWidget *right_btn = gtk_button_new_from_icon_name("tcode-sidebar-right-symbolic");
  GtkWidget *bottom_btn = gtk_button_new_from_icon_name("tcode-panel-bottom-symbolic");
  GtkWidget *logo = gtk_image_new_from_icon_name("tcode-logo"); /* from the resources: also when not installed */
  GtkWidget *logo_menu = gtk_menu_button_new(), *logo_image = gtk_image_new_from_icon_name("tcode-logo");

  add_menu_actions(t);
  g_object_set_data(G_OBJECT(header), "t-code-app", t);
  g_signal_connect(header, "realize", G_CALLBACK(on_header_realize), NULL);
  /* in the capture phase, before the window handle's own right-click (the window manager's menu) */
  GtkGesture *right = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(right), GDK_BUTTON_SECONDARY);
  gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(right), GTK_PHASE_CAPTURE);
  g_signal_connect(right, "pressed", G_CALLBACK(on_header_right_click), NULL);
  gtk_widget_add_controller(header, GTK_EVENT_CONTROLLER(right));
  g_signal_connect(header, "destroy", G_CALLBACK(on_header_destroy), NULL);
  /* t-code's icon, left of the menus. */
  gtk_image_set_pixel_size(GTK_IMAGE(logo), 18);
  gtk_widget_set_tooltip_text(logo, "t-code");
  gtk_widget_set_margin_start(logo, 4);
  gtk_header_bar_pack_start(GTK_HEADER_BAR(header), logo);
  /* The menu bar off: the same menus from the icon. */
  gtk_image_set_pixel_size(GTK_IMAGE(logo_image), 18);
  gtk_menu_button_set_child(GTK_MENU_BUTTON(logo_menu), logo_image);
  /* nested, as the menu bar's: each submenu its own popover as wide as its items (sliding ones share
   * the widest one's width) */
  gtk_menu_button_set_popover(GTK_MENU_BUTTON(logo_menu),
                              gtk_popover_menu_new_from_model_full(G_MENU_MODEL(model), GTK_POPOVER_MENU_NESTED));
  gtk_menu_button_set_has_frame(GTK_MENU_BUTTON(logo_menu), FALSE);
  gtk_widget_set_tooltip_text(logo_menu, "Menu");
  gtk_widget_set_visible(logo_menu, FALSE);
  gtk_header_bar_pack_start(GTK_HEADER_BAR(header), logo_menu);
  gtk_header_bar_pack_start(GTK_HEADER_BAR(header), menu_bar);
  g_object_unref(model);
  t->layout.logo = logo;
  t->layout.logo_menu = logo_menu;
  t->layout.menu_bar = menu_bar;
  t->layout.side_buttons[0] = left_btn;
  t->layout.side_buttons[1] = bottom_btn;
  t->layout.side_buttons[2] = right_btn;
  t->layout.search_bar = search_bar();
  gtk_header_bar_set_title_widget(GTK_HEADER_BAR(header), t->layout.search_bar);

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

void title_bar_apply(App *t)
{
  gboolean bar = t->settings.menu_bar;

  if (!t->layout.menu_bar)
    return;
  gtk_widget_set_visible(t->layout.menu_bar, bar);
  gtk_widget_set_visible(t->layout.logo, bar);
  gtk_widget_set_visible(t->layout.logo_menu, !bar);
  for (int side = 0; side < 3; side++)
    gtk_widget_set_visible(t->layout.side_buttons[side], t->settings.side_buttons);
  picker_place(t);
  gtk_widget_set_visible(t->layout.search_bar, t->settings.search_bar);
  for (size_t i = 0; i < G_N_ELEMENTS(parts); i++) {
    GAction *action = g_action_map_lookup_action(G_ACTION_MAP(t->window), parts[i].action);
    if (action)
      g_simple_action_set_state(G_SIMPLE_ACTION(action), g_variant_new_boolean(*part_setting(t, i)));
  }
}
