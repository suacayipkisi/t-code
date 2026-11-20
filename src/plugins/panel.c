/*
 * The Plugins panel (see manager.h): a header with the count, Reload and a ⋯ menu; each plugin as a
 * row with a dot in its state's color, its name and its switch; Browse the Registry at the bottom. A
 * row's click opens the plugin in the Plugins window. While the panel shows, it looks at the plugins
 * every REFRESH_MS and rebuilds its rows when one was added, removed, switched or changed its state.
 */
#include "plugins/manager.h"

#include "dialogs/processes.h"
#include "window/palette.h"
#include "window/theme.h"

#define REFRESH_MS 1000

static const char *const css =
    ".tcode-plugins-panel { background: var(--tc-panel); }"
    ".tcode-plugins-panel list { background: transparent; }"
    ".tcode-plugins-panel .header { margin: 4px 6px 2px 10px; }"
    ".tcode-plugins-panel .header button { min-height: 22px; min-width: 22px; padding: 1px 2px; }"
    ".tcode-plugins-panel .summary { color: var(--tc-fg-dim); }"
    ".tcode-plugins-panel .note { color: var(--tc-fg-dim); margin: 8px 10px; }"
    ".tcode-plugins-panel row { padding: 2px 6px; margin: 0 4px; min-height: 26px; border-radius: 6px;"
    "  color: var(--tc-fg); }"
    ".tcode-plugins-panel row:hover { background: var(--tc-hover); }"
    ".tcode-plugins-panel row .off { color: var(--tc-fg-dim); font-style: italic; }"
    ".tcode-plugins-panel switch { min-height: 16px; min-width: 30px; }"
    ".tcode-plugins-panel switch slider { min-height: 14px; min-width: 14px; }"
    ".tcode-plugins-panel .browse { margin: 6px 8px 8px 8px; }";

static struct {
  App *app;
  GtkWidget *list, *summary, *empty;
  char *shown; /* what the rows show: each plugin's id, on / off and state, to see that it changed */
  guint refresh_id;
} panel;

/* The plugins as the rows would show them. */
static char *signature(void)
{
  GString *text = g_string_new(NULL);

  for (guint i = 0; panel.app->plugins && i < panel.app->plugins->len; i++) {
    Plugin *p = g_ptr_array_index(panel.app->plugins, i);
    g_string_append_printf(text, "%s %d %d %s\n", p->id, p->enabled, p->state, p->name);
  }
  return g_string_free(text, FALSE);
}

static gboolean refresh(gpointer data);

/* After the switch's handler: its row goes in the rebuild. */
static void refresh_later(gpointer data G_GNUC_UNUSED)
{
  if (panel.list)
    refresh(NULL);
}

static void on_switched(GtkSwitch *toggle, GParamSpec *spec G_GNUC_UNUSED, gpointer data G_GNUC_UNUSED)
{
  Plugin *p = plugin_find(panel.app, g_object_get_data(G_OBJECT(toggle), "plugin-id"));
  GtkWindow *parent = GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(toggle)));

  if (p && !plugins_switch(panel.app, p, gtk_switch_get_active(toggle), parent)) {
    g_signal_handlers_block_by_func(toggle, on_switched, NULL);
    gtk_switch_set_active(toggle, FALSE); /* until the answer: a yes shows at the next look */
    g_signal_handlers_unblock_by_func(toggle, on_switched, NULL);
    return;
  }
  g_idle_add_once(refresh_later, NULL);
}

static void on_row_activated(GtkListBox *list G_GNUC_UNUSED, GtkListBoxRow *row, gpointer data G_GNUC_UNUSED)
{
  show_plugins_window_at(panel.app, g_object_get_data(G_OBJECT(row), "plugin-id"), FALSE);
}

static GtkWidget *row_of(Plugin *p)
{
  GtkWidget *row = gtk_list_box_row_new(), *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *name = gtk_label_new(p->name), *toggle = gtk_switch_new();
  const char *state = plugin_state_text(p);
  char *tip = *p->description ? g_strdup_printf("%s: %s\n%s", p->name, state, p->description)
                              : g_strdup_printf("%s: %s", p->name, state);

  gtk_box_append(GTK_BOX(box), plugin_state_dot(p));

  gtk_label_set_xalign(GTK_LABEL(name), 0);
  gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
  gtk_widget_set_hexpand(name, TRUE);
  if (!p->enabled)
    gtk_widget_add_css_class(name, "off");
  gtk_box_append(GTK_BOX(box), name);

  gtk_switch_set_active(GTK_SWITCH(toggle), p->enabled);
  gtk_widget_set_valign(toggle, GTK_ALIGN_CENTER);
  gtk_widget_set_tooltip_text(toggle, p->enabled ? "On: click to turn it off" : "Off: click to turn it on");
  g_object_set_data_full(G_OBJECT(toggle), "plugin-id", g_strdup(p->id), g_free);
  g_signal_connect(toggle, "notify::active", G_CALLBACK(on_switched), NULL);
  gtk_box_append(GTK_BOX(box), toggle);

  gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
  gtk_widget_set_tooltip_text(row, tip);
  g_object_set_data_full(G_OBJECT(row), "plugin-id", g_strdup(p->id), g_free);
  g_free(tip);
  return row;
}

static void rebuild(void)
{
  GPtrArray *plugins = panel.app->plugins;
  guint on = 0, n = plugins ? plugins->len : 0;
  GtkWidget *child;
  char *summary;

  while ((child = gtk_widget_get_first_child(panel.list)))
    gtk_list_box_remove(GTK_LIST_BOX(panel.list), child);
  for (guint i = 0; i < n; i++) {
    Plugin *p = g_ptr_array_index(plugins, i);
    gtk_list_box_append(GTK_LIST_BOX(panel.list), row_of(p));
    on += p->enabled;
  }
  summary = n ? g_strdup_printf("%u plugin%s · %u on", n, n == 1 ? "" : "s", on) : g_strdup("No plugins");
  gtk_label_set_text(GTK_LABEL(panel.summary), summary);
  gtk_widget_set_visible(panel.empty, n == 0);
  g_free(summary);
  g_free(panel.shown);
  panel.shown = signature();
}

static gboolean refresh(gpointer data G_GNUC_UNUSED)
{
  char *now = signature();

  if (g_strcmp0(now, panel.shown))
    rebuild();
  g_free(now);
  return G_SOURCE_CONTINUE;
}

static void on_map(GtkWidget *widget G_GNUC_UNUSED, gpointer data G_GNUC_UNUSED)
{
  refresh(NULL);
  if (!panel.refresh_id)
    panel.refresh_id = g_timeout_add(REFRESH_MS, refresh, NULL);
}

static void on_unmap(GtkWidget *widget G_GNUC_UNUSED, gpointer data G_GNUC_UNUSED)
{
  g_clear_handle_id(&panel.refresh_id, g_source_remove);
}

static void on_destroy(GtkWidget *widget G_GNUC_UNUSED, gpointer data G_GNUC_UNUSED)
{
  g_clear_handle_id(&panel.refresh_id, g_source_remove);
  g_clear_pointer(&panel.shown, g_free);
  panel.list = panel.summary = panel.empty = NULL;
}

static void on_reload(GtkButton *b G_GNUC_UNUSED, gpointer data G_GNUC_UNUSED)
{
  plugins_reload(panel.app);
  rebuild();
}

static void on_browse(GtkButton *b G_GNUC_UNUSED, gpointer data G_GNUC_UNUSED)
{
  show_plugins_window_at(panel.app, NULL, TRUE);
}

static void on_manage(GtkButton *b G_GNUC_UNUSED, gpointer data G_GNUC_UNUSED)
{
  show_plugins_window(panel.app);
}

static void on_folder(GtkButton *b G_GNUC_UNUSED, gpointer data G_GNUC_UNUSED)
{
  open_plugins_folder(panel.app);
}

static void on_processes(GtkButton *b G_GNUC_UNUSED, gpointer data G_GNUC_UNUSED)
{
  show_processes(panel.app);
}

static void on_menu_item(GtkButton *b, gpointer data G_GNUC_UNUSED)
{
  gtk_popover_popdown(GTK_POPOVER(gtk_widget_get_ancestor(GTK_WIDGET(b), GTK_TYPE_POPOVER)));
}

static void add_menu_item(GtkWidget *menu, const char *text, GCallback clicked)
{
  GtkWidget *b = gtk_button_new_with_label(text);

  gtk_widget_add_css_class(b, "flat");
  gtk_widget_set_halign(gtk_button_get_child(GTK_BUTTON(b)), GTK_ALIGN_START);
  g_signal_connect(b, "clicked", G_CALLBACK(on_menu_item), NULL);
  g_signal_connect(b, "clicked", clicked, NULL);
  gtk_box_append(GTK_BOX(menu), b);
}

static GtkWidget *header_button(const char *icon, const char *tooltip, GCallback clicked)
{
  GtkWidget *b = gtk_button_new_from_icon_name(icon);

  gtk_widget_add_css_class(b, "flat");
  gtk_widget_set_tooltip_text(b, tooltip);
  g_signal_connect(b, "clicked", clicked, NULL);
  return b;
}

GtkWidget *plugins_panel(App *t)
{
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0), *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
  GtkWidget *scroll = gtk_scrolled_window_new(), *more = gtk_menu_button_new(), *popover = gtk_popover_new();
  GtkWidget *menu = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0), *browse;

  palette_add_css(css);
  panel.app = t;
  gtk_widget_add_css_class(box, "tcode-plugins-panel");

  gtk_widget_add_css_class(header, "header");
  panel.summary = gtk_label_new(NULL);
  gtk_widget_add_css_class(panel.summary, "summary");
  gtk_label_set_xalign(GTK_LABEL(panel.summary), 0);
  gtk_label_set_ellipsize(GTK_LABEL(panel.summary), PANGO_ELLIPSIZE_END);
  gtk_widget_set_hexpand(panel.summary, TRUE);
  gtk_box_append(GTK_BOX(header), panel.summary);
  gtk_box_append(GTK_BOX(header),
                 header_button("tcode-refresh-symbolic", "Reload Installed Plugins", G_CALLBACK(on_reload)));
  add_menu_item(menu, "Manage Plugins…", G_CALLBACK(on_manage));
  add_menu_item(menu, "Open Plugins Folder", G_CALLBACK(on_folder));
  add_menu_item(menu, "Process Monitor", G_CALLBACK(on_processes));
  gtk_popover_set_child(GTK_POPOVER(popover), menu);
  gtk_menu_button_set_popover(GTK_MENU_BUTTON(more), popover);
  gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(more), "tcode-more-symbolic");
  gtk_menu_button_set_has_frame(GTK_MENU_BUTTON(more), FALSE);
  gtk_widget_set_tooltip_text(more, "More");
  gtk_box_append(GTK_BOX(header), more);
  gtk_box_append(GTK_BOX(box), header);

  panel.empty = gtk_label_new("No plugins yet. Browse the registry, or put one in your plugins folder.");
  gtk_label_set_wrap(GTK_LABEL(panel.empty), TRUE);
  gtk_label_set_xalign(GTK_LABEL(panel.empty), 0);
  gtk_widget_add_css_class(panel.empty, "note");
  gtk_box_append(GTK_BOX(box), panel.empty);

  panel.list = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(panel.list), GTK_SELECTION_NONE);
  gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(panel.list), TRUE);
  g_signal_connect(panel.list, "row-activated", G_CALLBACK(on_row_activated), NULL);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), panel.list);
  gtk_widget_set_vexpand(scroll, TRUE);
  gtk_box_append(GTK_BOX(box), scroll);

  browse = gtk_button_new_with_label("Browse the Registry…");
  gtk_widget_add_css_class(browse, "browse");
  label_shrinks(browse);
  g_signal_connect(browse, "clicked", G_CALLBACK(on_browse), NULL);
  gtk_box_append(GTK_BOX(box), browse);

  g_signal_connect(box, "map", G_CALLBACK(on_map), NULL);
  g_signal_connect(box, "unmap", G_CALLBACK(on_unmap), NULL);
  g_signal_connect(box, "destroy", G_CALLBACK(on_destroy), NULL);
  rebuild();
  return box;
}
