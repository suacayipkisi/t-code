/*
 * Icons and full height for every popover menu (see menus.h).
 */
#include "popups/menus.h"

#include <string.h>

/* clang-format off */
/* A label, or with a trailing '*' every label starting so; the first match wins. */
static const struct { const char *label, *icon; } icons[] = {
  { "Open to the Side", "split" },          { "Split Right", "split" },        { "Split Down", "split-down" },
  { "Open Changes", "source-control" },     { "Open Folder…", "files" },       { "Open File", "files" },
  { "Open", "files" },                      { "Reveal in Files", "files" },
  { "Copy*", "copy" },                      { "Cut", "cut" },                  { "Paste", "paste" },
  { "Delete", "delete" },                   { "Delete…", "delete" },
  { "Rename*", "rename" },                  { "New File…", "new-file" },       { "New File from Template…", "new-file" },
  { "Close", "close" },                     { "Close*", "close" },
  { "Stage*", "add" },                      { "Unstage*", "remove" },          { "Discard*", "discard" },
  { "Remove*", "remove" },                  { "Add to Watch", "add" },
  { "Find All References", "search" },      { "Go to Symbol…", "search" },     { "Show Call Hierarchy", "hierarchy" },
  { "Settings…", "settings" },              { "Color Theme…", "palette" },     { "Keyboard Shortcuts", "keyboard" },
  { "Command Palette…", "command" },        { "About t-code", "info" },        { "Quit", "quit" },
  { "New Window", "add" },                  { "Manage Plugins…", "plugins" },  { "Plugins Panel", "plugins" },
  { "Check for Updates Now", "refresh" },   { "Reset Panel Layout", "refresh" },
};
/* clang-format on */

const char *menu_icon_for(const char *label)
{
  static char name[64];

  if (!label || !*label)
    return NULL;
  for (size_t i = 0; i < G_N_ELEMENTS(icons); i++) {
    size_t n = strlen(icons[i].label);
    gboolean prefix = icons[i].label[n - 1] == '*';
    if (prefix ? strncmp(label, icons[i].label, n - 1) == 0 : strcmp(label, icons[i].label) == 0) {
      g_snprintf(name, sizeof name, "tcode-%s-symbolic", icons[i].icon);
      return name;
    }
  }
  return NULL;
}

static gboolean is_model_button(GtkWidget *widget)
{
  return strcmp(G_OBJECT_TYPE_NAME(widget), "GtkModelButton") == 0; /* GTK's own, not public */
}

static GtkWidget *child_of_type(GtkWidget *widget, GType type)
{
  for (GtkWidget *c = gtk_widget_get_first_child(widget); c; c = gtk_widget_get_next_sibling(c))
    if (G_TYPE_CHECK_INSTANCE_TYPE(c, type))
      return c;
  return NULL;
}

/* The menu's item buttons, in every page (submenus too). */
static void collect_buttons(GtkWidget *widget, GPtrArray *out)
{
  for (GtkWidget *c = gtk_widget_get_first_child(widget); c; c = gtk_widget_get_next_sibling(c)) {
    if (GTK_IS_POPOVER(c)) /* a nested submenu: shown (and done) on its own */
      continue;
    if (is_model_button(c))
      g_ptr_array_add(out, c);
    else
      collect_buttons(c, out);
  }
}

/* GTK shows a menu item's icon only without a label: the items found in the table get theirs beside the
 * label, and the others in a menu with icons the same room before theirs, so the labels line up. */
static void add_icons(GtkWidget *popover)
{
  GPtrArray *buttons = g_ptr_array_new();
  gboolean any = FALSE;

  collect_buttons(popover, buttons);
  for (guint i = 0; i < buttons->len; i++) {
    GtkWidget *button = g_ptr_array_index(buttons, i);
    char *text = NULL;
    GIcon *had = NULL;
    g_object_get(button, "text", &text, "icon", &had, NULL);
    const char *name = menu_icon_for(text);
    if (name && !had) {
      GIcon *icon = g_themed_icon_new(name);
      g_object_set(button, "icon", icon, NULL);
      g_object_unref(icon);
    }
    GtkWidget *image = child_of_type(button, GTK_TYPE_IMAGE);
    gboolean shown = name && image;
    if (shown)
      gtk_widget_set_visible(image, TRUE);
    g_object_set_data(G_OBJECT(button), "t-code-icon", GINT_TO_POINTER(shown));
    any |= shown;
    g_clear_object(&had);
    g_free(text);
  }
  for (guint i = 0; i < buttons->len; i++) {
    GtkWidget *button = g_ptr_array_index(buttons, i);
    if (any && !g_object_get_data(G_OBJECT(button), "t-code-icon"))
      gtk_widget_add_css_class(button, "tcode-no-icon");
    else
      gtk_widget_remove_css_class(button, "tcode-no-icon");
  }
  g_ptr_array_unref(buttons);
}

/* The menu's scrolled list asks for all its items (at most the screen's height less a margin): GTK then
 * flips or slides the menu to fit instead of shrinking it to the room under the pointer. */
static void full_height(GtkWidget *popover)
{
  GtkWidget *contents = gtk_widget_get_first_child(popover), *scrolled = NULL;
  GtkNative *native = gtk_widget_get_native(gtk_widget_get_parent(popover));
  GdkSurface *surface = native ? gtk_native_get_surface(native) : NULL;
  GdkMonitor *monitor = surface ? gdk_display_get_monitor_at_surface(gdk_surface_get_display(surface), surface) : NULL;
  GdkRectangle area = { 0, 0, 0, 800 };
  int natural = 0;

  if (contents)
    scrolled = GTK_IS_SCROLLED_WINDOW(contents) ? contents : child_of_type(contents, GTK_TYPE_SCROLLED_WINDOW);
  if (!scrolled || !gtk_scrolled_window_get_child(GTK_SCROLLED_WINDOW(scrolled)))
    return;
  if (monitor)
    gdk_monitor_get_geometry(monitor, &area);
  gtk_widget_measure(gtk_scrolled_window_get_child(GTK_SCROLLED_WINDOW(scrolled)), GTK_ORIENTATION_VERTICAL, -1, NULL,
                     &natural, NULL, NULL);
  gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scrolled), MIN(natural, MAX(area.height - 96, 200)));
}

static gboolean on_show(GSignalInvocationHint *hint G_GNUC_UNUSED, guint n G_GNUC_UNUSED, const GValue *values,
                        gpointer data G_GNUC_UNUSED)
{
  GObject *widget = g_value_get_object(&values[0]);

  if (GTK_IS_POPOVER_MENU(widget)) {
    add_icons(GTK_WIDGET(widget));
    full_height(GTK_WIDGET(widget));
  }
  return TRUE; /* stays */
}

void menus_init(void)
{
  static gboolean done;

  if (done)
    return;
  done = TRUE;
  g_type_ensure(GTK_TYPE_POPOVER_MENU);
  g_signal_add_emission_hook(g_signal_lookup("show", GTK_TYPE_WIDGET), 0, on_show, NULL, NULL);
}
