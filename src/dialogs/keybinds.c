/*
 * The Keybinds window (status bar button, or "Keyboard Shortcuts" in the command palette):
 * every shortcut in one place. The window's own come from the command table in window/commands.c.
 * Also the Keyboard Shortcuts page of the Settings window, where they are changed.
 */
#include "dialogs/keybinds.h"

#include "window/commands.h"
#include "window/palette.h"

#include <string.h>

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
  gtk_widget_add_css_class(window, "tcode-dialog"); /* window/theme.c: its edge */
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

/* Settings > Keyboard Shortcuts ----------------------------------------------------------------- */

typedef struct {
  App *t;
  GtkWidget *note;     /* what the last change did */
  GtkWidget **rows;    /* one per command */
  GtkWidget **buttons; /* its keys; a click waits for new ones */
  GtkWidget **resets;  /* back to the default, when they are the user's */
  int waiting;         /* the command waiting for keys, or -1 */
} Editor;

static void editor_free(gpointer data)
{
  Editor *e = data;

  g_free(e->rows);
  g_free(e->buttons);
  g_free(e->resets);
  g_free(e);
}

/* Every button shows the keys in effect (a change may take a key from another command). */
static void editor_refresh(Editor *e)
{
  for (int i = 0; i < command_count(); i++) {
    const char *keys = command_shortcut(i);
    gtk_button_set_label(GTK_BUTTON(e->buttons[i]), i == e->waiting ? "Press keys…" : keys ? keys : "—");
    gtk_widget_set_visible(e->resets[i], command_shortcut_changed(i));
  }
}

static void on_keys_clicked(GtkButton *button, Editor *e)
{
  e->waiting = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "t-code-index"));
  gtk_label_set_text(GTK_LABEL(e->note), "Press the new keys: Escape keeps the old ones, Backspace removes them");
  editor_refresh(e);
  gtk_widget_grab_focus(GTK_WIDGET(button));
}

/* The keys pressed while a button waits: they become its command's shortcut. */
static gboolean on_key(GtkEventControllerKey *controller, guint key, guint code G_GNUC_UNUSED, GdkModifierType state,
                       Editor *e)
{
  GdkEvent *event = gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(controller));
  GdkModifierType consumed = event ? gdk_key_event_get_consumed_modifiers(event) : 0;
  int i = e->waiting, taken;
  char *text = NULL;

  if (i < 0)
    return FALSE;
  switch (key) {
    case GDK_KEY_Shift_L:
    case GDK_KEY_Shift_R:
    case GDK_KEY_Control_L:
    case GDK_KEY_Control_R:
    case GDK_KEY_Alt_L:
    case GDK_KEY_Alt_R:
    case GDK_KEY_Super_L:
    case GDK_KEY_Super_R:
    case GDK_KEY_Meta_L:
    case GDK_KEY_Meta_R:
    case GDK_KEY_ISO_Level3_Shift: return TRUE; /* wait for the key itself */
  }
  /* Shift that makes a capital letter stays (Shift+A); one that makes another sign (Shift+7 for '/')
   * is in the sign already. */
  if (gdk_keyval_to_lower(key) != key)
    consumed &= ~GDK_SHIFT_MASK;
  state &= ~consumed & gtk_accelerator_get_default_mod_mask();
  e->waiting = -1;
  if (key == GDK_KEY_Escape && !state) {
    gtk_label_set_text(GTK_LABEL(e->note), "");
  } else if (key == GDK_KEY_BackSpace && !state) {
    command_set_shortcut(e->t, i, 0, 0);
    text = g_strdup_printf("%s has no shortcut now", command_name(i));
  } else {
    taken = command_set_shortcut(e->t, i, key, state);
    if (taken >= 0)
      text = g_strdup_printf("%s: %s (%s has lost it)", command_name(i), command_shortcut(i), command_name(taken));
    else
      text = g_strdup_printf("%s: %s", command_name(i), command_shortcut(i));
  }
  if (text)
    gtk_label_set_text(GTK_LABEL(e->note), text);
  g_free(text);
  editor_refresh(e);
  return TRUE;
}

/* The keyboard left a waiting button: it keeps its keys. */
static void on_focus_left(GtkEventControllerFocus *focus G_GNUC_UNUSED, Editor *e)
{
  if (e->waiting >= 0) {
    e->waiting = -1;
    gtk_label_set_text(GTK_LABEL(e->note), "");
    editor_refresh(e);
  }
}

static void on_reset(GtkButton *button, Editor *e)
{
  int i = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "t-code-index"));
  char *text;

  e->waiting = -1;
  command_reset_shortcut(e->t, i);
  text = g_strdup_printf("%s: %s again", command_name(i), command_shortcut(i) ? command_shortcut(i) : "no shortcut");
  gtk_label_set_text(GTK_LABEL(e->note), text);
  g_free(text);
  editor_refresh(e);
}

/* Only the commands whose name or keys have the typed text. */
static void on_filter(GtkSearchEntry *entry, Editor *e)
{
  char *wanted = g_utf8_casefold(gtk_editable_get_text(GTK_EDITABLE(entry)), -1);

  for (int i = 0; i < command_count(); i++) {
    char *name = g_utf8_casefold(command_name(i), -1);
    char *keys = g_utf8_casefold(command_shortcut(i) ? command_shortcut(i) : "", -1);
    gtk_widget_set_visible(e->rows[i], strstr(name, wanted) || strstr(keys, wanted));
    g_free(keys);
    g_free(name);
  }
  g_free(wanted);
}

GtkWidget *keybinds_editor(App *t)
{
  Editor *e = g_new0(Editor, 1);
  int n = command_count();
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6), *search = gtk_search_entry_new();
  GtkWidget *list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);

  palette_add_css(css);
  e->t = t;
  e->waiting = -1;
  e->rows = g_new0(GtkWidget *, n);
  e->buttons = g_new0(GtkWidget *, n);
  e->resets = g_new0(GtkWidget *, n);
  g_object_set_data_full(G_OBJECT(box), "t-code-keybinds-editor", e, editor_free);
  gtk_widget_add_css_class(box, "tcode-keybinds");

  gtk_widget_set_hexpand(box, TRUE);
  gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(search), "Filter by command or keys");
  g_signal_connect(search, "search-changed", G_CALLBACK(on_filter), e);
  gtk_box_append(GTK_BOX(box), search);
  e->note = gtk_label_new("Click a command's keys, then press the new ones");
  gtk_widget_add_css_class(e->note, "note");
  gtk_label_set_xalign(GTK_LABEL(e->note), 0);
  gtk_label_set_wrap(GTK_LABEL(e->note), TRUE);
  gtk_box_append(GTK_BOX(box), e->note);
  gtk_widget_set_margin_top(list, 6);
  gtk_box_append(GTK_BOX(box), list);

  for (int i = 0; i < n; i++) {
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6), *name = gtk_label_new(command_name(i));
    GtkWidget *button = gtk_button_new(), *reset = gtk_button_new_from_icon_name("edit-undo-symbolic");
    GtkEventController *keys = gtk_event_controller_key_new(), *focus = gtk_event_controller_focus_new();

    gtk_widget_add_css_class(name, "action");
    gtk_label_set_xalign(GTK_LABEL(name), 0);
    gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(name, TRUE);
    gtk_widget_add_css_class(button, "keys");
    gtk_widget_set_tooltip_text(button, "Click, then press the new keys (Backspace: none)");
    g_object_set_data(G_OBJECT(button), "t-code-index", GINT_TO_POINTER(i));
    g_signal_connect(button, "clicked", G_CALLBACK(on_keys_clicked), e);
    /* Capture: the keys go here before the button (Enter, Space) or the window (Escape closes it). */
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key), e);
    gtk_widget_add_controller(button, keys);
    g_signal_connect(focus, "leave", G_CALLBACK(on_focus_left), e);
    gtk_widget_add_controller(button, focus);
    gtk_widget_add_css_class(reset, "flat");
    gtk_widget_set_tooltip_text(reset, "Back to the default keys");
    g_object_set_data(G_OBJECT(reset), "t-code-index", GINT_TO_POINTER(i));
    g_signal_connect(reset, "clicked", G_CALLBACK(on_reset), e);
    gtk_box_append(GTK_BOX(row), name);
    gtk_box_append(GTK_BOX(row), reset);
    gtk_box_append(GTK_BOX(row), button);
    gtk_box_append(GTK_BOX(list), row);
    e->rows[i] = row;
    e->buttons[i] = button;
    e->resets[i] = reset;
  }
  editor_refresh(e);
  return box;
}
