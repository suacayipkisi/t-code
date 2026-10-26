/*
 * The Keybinds window (status bar button, or "Keyboard Shortcuts" in the command palette):
 * every shortcut in one place. The window's own come from the command table in window/commands.c.
 */
#include "dialogs/keybinds.h"

#include "window/commands.h"
#include "window/palette.h"

static const KeyHelp tab_keys[] = {
  { "Switch to a tab", "Click" },
  { "Close a tab (a comparison closes with both sides)", "Middle click, ×" },
  { "Keep a temporary (italic) tab", "Double click it, or edit the file" },
  { "Move a tab to the other group", "Drag it" },
  { "Open a compiler message's file and line", "Ctrl+Click in the shell" },
};

static const KeyHelp files_keys[] = {
  { "Open file (in a temporary tab: the next click replaces it) / open or close folder", "Click, Enter, l" },
  { "Open file for good / to the side", "Double click / middle click" },
  { "Open folder or go into it / close it or go to its parent", "→ / ←, h, Backspace" },
  { "Move", "↑ ↓, j k, Home End" },
  { "More (open to the side, preview, git, …)", "Right click" },
  { "New file (a/b.c makes folders, end with / for a folder)", "a" },
  { "Rename / delete", "r, F2 / d, Delete" },
  { "Cut / copy / paste files", "Ctrl+X / Ctrl+C / Ctrl+V" },
  { "Move into a folder / open in an editor", "Drag a file onto it / onto the editors" },
  { "Hide / show dotfiles / read the tree again", ". / R" },
};

/* The editor's own keys (GtkSourceView's); its commands (save, find, comment, ...) are in the
 * window's table above. */
static const KeyHelp editor_keys[] = {
  { "Undo / redo", "Ctrl+Z / Ctrl+Shift+Z" },
  { "Copy / cut / paste", "Ctrl+C / Ctrl+X / Ctrl+V" },
  { "Select all", "Ctrl+A" },
  { "Indent / outdent the selected lines", "Tab / Shift+Tab" },
  { "Word left / right, delete a word", "Ctrl+← / Ctrl+→, Ctrl+Backspace" },
  { "Start of the text, then of the line", "Home" },
  { "Start / end of the file", "Ctrl+Home / Ctrl+End" },
  { "Brackets and quotes close by themselves", "( [ { \" '" },
};

static const char *const css =
    ".tcode-keybinds { background: var(--tc-panel); }"
    ".tcode-keybinds .section { font-weight: bold; color: var(--tc-fg-bright); margin: 18px 0 6px; }"
    ".tcode-keybinds .action { color: var(--tc-fg); }"
    ".tcode-keybinds .keys { font-family: 'Cascadia Code', monospace; font-size: smaller; color: var(--tc-fg);"
    "  background: var(--tc-input); border: 1px solid var(--tc-border-strong); border-radius: 4px; padding: 1px 6px; }";

/* A heading, then one row per shortcut: what it does on the left, the keys on the right. */
static void add_section(GtkGrid *grid, int *row, const char *title, const KeyHelp *keys, int n)
{
  GtkWidget *heading = gtk_label_new(title);

  gtk_widget_add_css_class(heading, "section");
  gtk_label_set_xalign(GTK_LABEL(heading), 0);
  gtk_grid_attach(grid, heading, 0, (*row)++, 2, 1);
  for (int i = 0; i < n; i++) {
    GtkWidget *action = gtk_label_new(keys[i].name);
    GtkWidget *shortcut = gtk_label_new(keys[i].keys ? keys[i].keys : "—");

    gtk_widget_add_css_class(action, "action");
    gtk_label_set_xalign(GTK_LABEL(action), 0);
    gtk_widget_set_hexpand(action, TRUE);
    gtk_widget_add_css_class(shortcut, "keys");
    gtk_widget_set_halign(shortcut, GTK_ALIGN_END);
    gtk_grid_attach(grid, action, 0, *row, 1, 1);
    gtk_grid_attach(grid, shortcut, 1, (*row)++, 1, 1);
  }
}

void show_keybinds(App *t)
{
  static GtkWidget *window;
  GtkWidget *scroll, *grid;
  GtkEventController *keys;
  int n, row = 0;
  const KeyHelp *window_list = window_keys(&n);

  if (window) {
    gtk_window_present(GTK_WINDOW(window));
    return;
  }
  palette_add_css(css);

  window = gtk_window_new();
  gtk_window_set_title(GTK_WINDOW(window), "Keyboard Shortcuts");
  gtk_window_set_transient_for(GTK_WINDOW(window), t->window);
  gtk_window_set_default_size(GTK_WINDOW(window), 620, 720);
  gtk_widget_add_css_class(window, "tcode-keybinds");
  g_object_add_weak_pointer(G_OBJECT(window), (gpointer *)&window);

  grid = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
  gtk_grid_set_column_spacing(GTK_GRID(grid), 24);
  gtk_widget_set_margin_start(grid, 24);
  gtk_widget_set_margin_end(grid, 24);
  gtk_widget_set_margin_bottom(grid, 24);
  add_section(GTK_GRID(grid), &row, "Window", window_list, n);
  add_section(GTK_GRID(grid), &row, "Tabs and mouse", tab_keys, G_N_ELEMENTS(tab_keys));
  add_section(GTK_GRID(grid), &row, "Files", files_keys, G_N_ELEMENTS(files_keys));
  add_section(GTK_GRID(grid), &row, "Editor", editor_keys, G_N_ELEMENTS(editor_keys));

  scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), grid);
  gtk_window_set_child(GTK_WINDOW(window), scroll);

  keys = gtk_shortcut_controller_new(); /* Escape closes it */
  gtk_shortcut_controller_add_shortcut(
      GTK_SHORTCUT_CONTROLLER(keys),
      gtk_shortcut_new(gtk_keyval_trigger_new(GDK_KEY_Escape, 0), gtk_named_action_new("window.close")));
  gtk_widget_add_controller(window, keys);
  gtk_window_present(GTK_WINDOW(window));
}
