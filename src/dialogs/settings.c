/*
 * The Settings window (Ctrl+, or the gear in the title bar): pages listed on the left (General,
 * Editor, Indentation, Appearance, C / C++, Language Servers, Keyboard Shortcuts, Edit by Hand). Every change applies at once, to the open
 * editors too, and is saved in settings.ini, which can also be edited by hand.
 */
#include "dialogs/settings.h"

#include "dialogs/keybinds.h"
#include "editor/editor.h"
#include "editor/editorview.h"
#include "editor/tabs.h"
#include "lsp/client/client.h"
#include "lsp/servers.h"
#include "project/session.h"
#include "project/trust.h"
#include "project/userfiles.h"
#include "window/terminal.h"
#include "window/theme.h"
#include "window/window.h"
#include "window/palette.h"
#include "plugins/plugins.h"

static const char *const css =
    ".tcode-settings { background: var(--tc-panel); }"
    ".tcode-settings .section { font-weight: bold; font-size: larger; color: var(--tc-fg-bright); margin: 18px 0 6px; }"
    ".tcode-settings stacksidebar { min-width: 150px; border-right: 1px solid var(--tc-border); }"
    ".tcode-settings stacksidebar list { background: var(--tc-frame); }"
    ".tcode-settings stacksidebar row { padding: 6px 12px; }"
    ".tcode-settings .name { color: var(--tc-fg); }"
    ".tcode-settings .note { color: var(--tc-fg-dim); font-size: smaller; }";

static GtkWidget *window; /* the Settings window while it is open */
static char *last_page;   /* the page shown when it closed: it opens there again */

/* Applies the settings again to every open editor. */
static void each_editor(App *t)
{
  int n = tab_count(t);

  for (int i = 0; i < n; i++)
    if (tab_nth(t, i)->kind == TAB_EDITOR)
      editor_view_apply_settings(tab_nth(t, i));
}

/* The App, from any widget in the Settings window (show_settings hangs it on the window). */
static App *app_of(GtkWidget *widget)
{
  return g_object_get_data(G_OBJECT(gtk_widget_get_root(widget)), "t-code-app");
}

/* A switch or number field for a setting changed: the setting takes its value, apply (when not
 * NULL) puts it into effect, and the settings are saved. */
static void setting_changed(GtkWidget *control, App *t)
{
  void (*apply)(App *t) = g_object_get_data(G_OBJECT(control), "t-code-apply");

  if (apply)
    apply(t);
  session_save_settings(t);
}

static void on_setting_switch(GtkSwitch *toggle, GParamSpec *spec G_GNUC_UNUSED, gboolean *setting)
{
  *setting = gtk_switch_get_active(toggle);
  setting_changed(GTK_WIDGET(toggle), app_of(GTK_WIDGET(toggle)));
}

static void on_setting_spin(GtkSpinButton *spin, int *setting)
{
  *setting = gtk_spin_button_get_value_as_int(spin);
  setting_changed(GTK_WIDGET(spin), app_of(GTK_WIDGET(spin)));
}

/* A switch showing *setting, which sets it when flipped (then apply; see setting_changed). */
static GtkWidget *setting_switch(gboolean *setting, void (*apply)(App *t))
{
  GtkWidget *toggle = gtk_switch_new();

  gtk_switch_set_active(GTK_SWITCH(toggle), *setting);
  g_object_set_data(G_OBJECT(toggle), "t-code-apply", apply);
  g_signal_connect(toggle, "notify::active", G_CALLBACK(on_setting_switch), setting);
  return toggle;
}

/* A number field from low to high showing *setting, which sets it when it changes (then apply). */
static GtkWidget *setting_spin(int *setting, int low, int high, void (*apply)(App *t))
{
  GtkWidget *spin = gtk_spin_button_new_with_range(low, high, 1);

  gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin), *setting);
  g_object_set_data(G_OBJECT(spin), "t-code-apply", apply);
  g_signal_connect(spin, "value-changed", G_CALLBACK(on_setting_spin), setting);
  return spin;
}

static void on_coloring(GtkSwitch *toggle, GParamSpec *spec G_GNUC_UNUSED, gpointer data G_GNUC_UNUSED)
{
  App *t = app_of(GTK_WIDGET(toggle));

  if (gtk_switch_get_active(toggle) == t->settings.plain)
    toggle_syntax(t); /* saves the settings */
}

static void on_theme(GtkDropDown *choice, GParamSpec *spec G_GNUC_UNUSED, gpointer data G_GNUC_UNUSED)
{
  App *t = app_of(GTK_WIDGET(choice));

  t->settings.square = gtk_drop_down_get_selected(choice) == 1;
  apply_theme(t);
  session_save_settings(t);
}

static void on_color_theme(GtkDropDown *choice, GParamSpec *spec G_GNUC_UNUSED, gpointer data G_GNUC_UNUSED)
{
  App *t = app_of(GTK_WIDGET(choice));

  g_set_str(&t->settings.color_theme, palette_nth_id((int)gtk_drop_down_get_selected(choice)));
  color_theme_apply(t, t->settings.color_theme);
  session_save_settings(t);
}

/* A drop-down of the color themes, the current one chosen. */
static GtkWidget *color_themes(void)
{
  const char **names = g_new0(const char *, palette_count() + 1);
  GtkWidget *choice;
  int current = 0;

  for (int i = 0; i < palette_count(); i++) {
    names[i] = palette_nth_name(i);
    if (g_strcmp0(palette_nth_id(i), palette_id()) == 0)
      current = i;
  }
  choice = gtk_drop_down_new_from_strings(names);
  gtk_drop_down_set_selected(GTK_DROP_DOWN(choice), current);
  g_signal_connect(choice, "notify::selected", G_CALLBACK(on_color_theme), NULL);
  g_free(names);
  return choice;
}

/* The "Open" buttons: data opens a file to edit by hand, in an editor tab over this window. */
static void on_open_file(GtkButton *button, gpointer data)
{
  App *t = app_of(GTK_WIDGET(button));
  void (*open)(App *t) = data;

  gtk_window_close(GTK_WINDOW(window));
  open(t);
}

static GtkWidget *open_button(void (*open)(App *t))
{
  GtkWidget *button = gtk_button_new_with_label("Open");

  g_signal_connect(button, "clicked", G_CALLBACK(on_open_file), open);
  return button;
}

static void on_text_size(GtkSpinButton *spin, gpointer data G_GNUC_UNUSED)
{
  App *t = app_of(GTK_WIDGET(spin));

  set_font_scale(t, gtk_spin_button_get_value(spin) / 100);
  session_save_settings(t); /* kept even if t-code does not close normally */
}

static void on_preview_size(GtkSpinButton *spin, gpointer data G_GNUC_UNUSED)
{
  App *t = app_of(GTK_WIDGET(spin));

  set_preview_zoom_scale(t, gtk_spin_button_get_value(spin) / 100);
  session_save_settings(t);
}

/* Font buttons: data is the setting (&t->settings.font or files_font) they change. */
static void on_font(GtkFontDialogButton *button, GParamSpec *spec G_GNUC_UNUSED, gpointer data)
{
  App *t = app_of(GTK_WIDGET(button));
  PangoFontDescription *desc = gtk_font_dialog_button_get_font_desc(button);
  char **setting = data;

  if (!desc)
    return;
  g_free(*setting);
  *setting = pango_font_description_to_string(desc);
  apply_fonts(t);
  session_save_settings(t);
}

/* The font dialog's filter: fixed width font families (and their faces) only. */
static gboolean is_monospace(gpointer item, gpointer data G_GNUC_UNUSED)
{
  PangoFontFamily *family = PANGO_IS_FONT_FACE(item) ? pango_font_face_get_family(item) : item;

  return PANGO_IS_FONT_FAMILY(family) && pango_font_family_is_monospace(family);
}

/* A font button for a font setting. monospace: only fixed width fonts are listed (code is aligned by
 * columns, and a terminal puts every character in a cell of the same width); else every font (file names
 * need no columns, though a fixed width one is still there to pick). */
static GtkWidget *font_for(char **setting, gboolean monospace)
{
  GtkFontDialog *dialog = gtk_font_dialog_new();
  GtkWidget *button = gtk_font_dialog_button_new(dialog); /* takes the dialog */
  PangoFontDescription *desc = pango_font_description_from_string(*setting);

  gtk_font_dialog_set_title(dialog, "Font");
  if (monospace) {
    GtkCustomFilter *filter = gtk_custom_filter_new(is_monospace, NULL, NULL);
    gtk_font_dialog_set_filter(dialog, GTK_FILTER(filter));
    g_object_unref(filter);
  }
  gtk_font_dialog_button_set_font_desc(GTK_FONT_DIALOG_BUTTON(button), desc);
  pango_font_description_free(desc);
  g_signal_connect(button, "notify::font-desc", G_CALLBACK(on_font), setting);
  return button;
}

/* A page's heading, across both columns. */
static void add_heading(GtkGrid *grid, int *row, const char *title)
{
  GtkWidget *heading = gtk_label_new(title);

  gtk_widget_add_css_class(heading, "section");
  gtk_label_set_xalign(GTK_LABEL(heading), 0);
  gtk_grid_attach(grid, heading, 0, (*row)++, 2, 1);
}

/* A row: the name (with a note under it when given) and the control on the right. */
static void add_row(GtkGrid *grid, int *row, const char *name, const char *note, GtkWidget *control)
{
  GtkWidget *labels = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  GtkWidget *label = gtk_label_new(name);

  gtk_widget_add_css_class(label, "name");
  gtk_label_set_xalign(GTK_LABEL(label), 0);
  gtk_box_append(GTK_BOX(labels), label);
  if (note) {
    GtkWidget *small = gtk_label_new(note);
    gtk_widget_add_css_class(small, "note");
    gtk_label_set_xalign(GTK_LABEL(small), 0);
    gtk_label_set_wrap(GTK_LABEL(small), TRUE);
    gtk_box_append(GTK_BOX(labels), small);
  }
  gtk_widget_set_hexpand(labels, TRUE);
  gtk_widget_set_valign(control, GTK_ALIGN_CENTER);
  gtk_widget_set_halign(control, GTK_ALIGN_END);
  gtk_grid_attach(grid, labels, 0, *row, 1, 1);
  gtk_grid_attach(grid, control, 1, (*row)++, 1, 1);
}

/* A switch, set to on, that calls changed when it is flipped. */
static GtkWidget *switch_for(gboolean on, GCallback changed)
{
  GtkWidget *toggle = gtk_switch_new();

  gtk_switch_set_active(GTK_SWITCH(toggle), on);
  g_signal_connect(toggle, "notify::active", changed, NULL);
  return toggle;
}

/* A number field from low to high, set to value, that calls changed when it changes. */
static GtkWidget *spin_for(double low, double high, double step, double value, GCallback changed)
{
  GtkWidget *spin = gtk_spin_button_new_with_range(low, high, step);

  gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin), value);
  g_signal_connect(spin, "value-changed", changed, NULL);
  return spin;
}

void settings_changed(App *t, gboolean restart_lsp)
{
  if (window) { /* its switches show the old values; forgotten now, so it can open again at once */
    GtkWidget *old = window;
    g_object_remove_weak_pointer(G_OBJECT(old), (gpointer *)&window);
    window = NULL;
    gtk_window_destroy(GTK_WINDOW(old));
  }
  each_editor(t);
  apply_fonts(t);
  editor_view_set_selection(t);
  apply_theme(t);
  if (g_strcmp0(palette_id(), t->settings.color_theme) != 0)
    color_theme_apply(t, t->settings.color_theme);
  if (restart_lsp)
    lsp_restart_clangd(t);
  lsp_reattach(t); /* a built-in server turned on or off */
  lsp_idle_changed(t);
  apply_interface(t);
}

/* A panel's switch on the Interface page. */
static void on_panel_switch(GtkSwitch *toggle, GParamSpec *spec G_GNUC_UNUSED, gpointer data)
{
  set_panel_enabled(app_of(GTK_WIDGET(toggle)), panel_names[GPOINTER_TO_INT(data)], gtk_switch_get_active(toggle));
}

/* A built-in language server's switch: off, its editors move to a plugin's server or to none. */
static void on_server_switch(GtkSwitch *toggle, GParamSpec *spec G_GNUC_UNUSED, gpointer data)
{
  App *t = app_of(GTK_WIDGET(toggle));
  guint bit = 1u << GPOINTER_TO_UINT(data);

  t->settings.servers_off =
      gtk_switch_get_active(toggle) ? t->settings.servers_off & ~bit : t->settings.servers_off | bit;
  lsp_reattach(t);
  session_save_settings(t);
}

/* clangd starts again with a setting a second after its last change (a number field changes at each
 * step). */
static guint clangd_restart_id;

static gboolean restart_clangd_now(gpointer data)
{
  clangd_restart_id = 0;
  lsp_restart_clangd(data);
  return G_SOURCE_REMOVE;
}

static void restart_clangd_later(App *t)
{
  g_clear_handle_id(&clangd_restart_id, g_source_remove);
  clangd_restart_id = g_timeout_add_seconds(1, restart_clangd_now, t);
}

static void on_clangd_extra(GtkEditable *entry, gpointer data G_GNUC_UNUSED)
{
  App *t = app_of(GTK_WIDGET(entry));
  const char *text = gtk_editable_get_text(entry);

  g_free(t->settings.clangd.extra);
  t->settings.clangd.extra = *text ? g_strdup(text) : NULL;
  session_save_settings(t);
  restart_clangd_later(t);
}

static char *page_again; /* the page the window showed before a preset closed it */

static gboolean show_again(gpointer data)
{
  g_free(last_page);
  last_page = g_steal_pointer(&page_again); /* closing it may have shown another page */
  show_settings(data);
  return G_SOURCE_REMOVE;
}

/* "t-code defaults" (data 0): the measured ones in settings.ini's table (project/session.c); "Plain
 * clangd" (1): clangd and glibc as they come. Saved, clangd started again, the window shows them. */
static void on_clangd_preset(GtkButton *button, gpointer data)
{
  App *t = app_of(GTK_WIDGET(button));

  session_reset_settings(t, "clangd-");
  if (GPOINTER_TO_INT(data)) {
    t->settings.clangd.background_index = TRUE;
    t->settings.clangd.threads = 0;
    t->settings.clangd.pch_on_disk = FALSE;
    t->settings.clangd.preamble_dir = FALSE;
    t->settings.clangd.limit_results = 0;
    t->settings.clangd.malloc_trim = TRUE;
    t->settings.clangd.arenas = 0;
    t->settings.clangd.mmap_threshold_kb = 0;
  }
  g_clear_handle_id(&clangd_restart_id, g_source_remove);
  session_save_settings(t);
  g_set_str(&page_again, last_page);
  settings_changed(t, TRUE); /* closes the window */
  g_idle_add(show_again, t);
}

static GtkWidget *preset_buttons(void)
{
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  const char *const labels[] = { "t-code defaults", "Plain clangd" };

  for (int i = 0; i < 2; i++) {
    GtkWidget *button = gtk_button_new_with_label(labels[i]);
    g_signal_connect(button, "clicked", G_CALLBACK(on_clangd_preset), GINT_TO_POINTER(i));
    gtk_box_append(GTK_BOX(box), button);
  }
  return box;
}

/* A page of the window: its grid of rows (its heading in it), scrolling when the window is smaller. */
static GtkWidget *add_page(GtkStack *stack, const char *name, const char *title, int *row)
{
  GtkWidget *grid = gtk_grid_new(), *scroll = gtk_scrolled_window_new();

  gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
  gtk_grid_set_column_spacing(GTK_GRID(grid), 24);
  gtk_widget_set_margin_start(grid, 24);
  gtk_widget_set_margin_end(grid, 24);
  gtk_widget_set_margin_bottom(grid, 24);
  *row = 0;
  add_heading(GTK_GRID(grid), row, title);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), grid);
  gtk_stack_add_titled(stack, scroll, name, title);
  return grid;
}

/* The page shown changed: it is the one the window opens on next time. */
static void on_page(GtkStack *stack, GParamSpec *spec G_GNUC_UNUSED, gpointer data G_GNUC_UNUSED)
{
  g_set_str(&last_page, gtk_stack_get_visible_child_name(stack));
}

void show_settings(App *t)
{
  GtkWidget *grid, *stack, *sidebar, *panes;
  GtkEventController *keys;
  int row;

  if (window) {
    gtk_window_present(GTK_WINDOW(window));
    return;
  }
  palette_add_css(css);

  window = gtk_window_new();
  gtk_window_set_title(GTK_WINDOW(window), "Settings");
  gtk_window_set_transient_for(GTK_WINDOW(window), t->window);
  gtk_widget_add_css_class(window, "tcode-dialog");          /* window/theme.c: its edge */
  gtk_window_set_default_size(GTK_WINDOW(window), 720, 480); /* a long page scrolls */
  gtk_widget_add_css_class(window, "tcode-settings");
  g_object_set_data(G_OBJECT(window), "t-code-app", t);
  g_object_add_weak_pointer(G_OBJECT(window), (gpointer *)&window);
  stack = gtk_stack_new();
  gtk_widget_set_hexpand(stack, TRUE);

  grid = add_page(GTK_STACK(stack), "general", "General", &row);
  add_row(GTK_GRID(grid), &row, "Autosave", "Save a second after the last change",
          setting_switch(&t->settings.autosave, NULL));
  add_row(GTK_GRID(grid), &row, "Reopen the last project",
          "When started from the app menu (not from a folder in a terminal)",
          setting_switch(&t->settings.reopen_last, NULL));
  add_row(GTK_GRID(grid), &row, "Check for updates",
          "Once at start-up, the latest release on GitHub goes against t-code; a newer one asks (also"
          " \"Check for Updates Now\")",
          setting_switch(&t->settings.check_updates, NULL));
  add_row(GTK_GRID(grid), &row, "Trust all folders",
          "Run every project's own programs without asking: its venv's Python, its node_modules TypeScript,"
          " Rust build scripts, the test listing. Off: only in folders you trusted",
          setting_switch(&t->settings.trust_all, trust_changed));

  grid = add_page(GTK_STACK(stack), "editor", "Editor", &row);
  add_row(GTK_GRID(grid), &row, "Minimap", "A small picture of the file next to the scrollbar",
          setting_switch(&t->settings.minimap, each_editor));
  add_row(GTK_GRID(grid), &row, "Sticky scroll",
          "The first lines of the blocks around the top (namespace, function, loop) stay at the top",
          setting_switch(&t->settings.sticky_scroll, each_editor));
  add_row(GTK_GRID(grid), &row, "Code coloring", "Also F7", switch_for(!t->settings.plain, G_CALLBACK(on_coloring)));
  add_row(GTK_GRID(grid), &row, "Indent guides",
          "Lines at each indentation level; the block around the cursor in color",
          setting_switch(&t->settings.guides, each_editor));
  add_row(GTK_GRID(grid), &row, "Word wrap", "Lines longer than the editor continue on the next row",
          setting_switch(&t->settings.word_wrap, each_editor));
  add_row(GTK_GRID(grid), &row, "Inlay hints",
          "Parameter names and deduced types inside the lines, from the language servers",
          setting_switch(&t->settings.inlay_hints, each_editor));
  add_row(GTK_GRID(grid), &row, "Breadcrumbs", "The file's folders and the symbols around the cursor, over each editor",
          setting_switch(&t->settings.breadcrumbs, each_editor));
  add_row(GTK_GRID(grid), &row, "Line blame", "Who last changed the cursor's line, and in which commit, after its end",
          setting_switch(&t->settings.line_blame, each_editor));
  add_row(GTK_GRID(grid), &row, "Ruler column",
          "A vertical line after this column, to keep lines short. Type 0 to turn it off",
          setting_spin(&t->settings.ruler, 0, 300, each_editor));
  add_row(GTK_GRID(grid), &row, "Editor Autosave (ms)",
          "Set value of the autosave after editing (max 5 min (300'000 ms))",
          setting_spin(&t->settings.autosave_ms, 0, 300000, each_editor));

  grid = add_page(GTK_STACK(stack), "indentation", "Indentation", &row);
  add_row(GTK_GRID(grid), &row, "Tab size",
          "For projects without their own: the status bar's choice, .editorconfig or .clang-format come first",
          setting_spin(&t->settings.tab_size, 1, 16, each_editor));
  add_row(GTK_GRID(grid), &row, "Insert spaces for Tab", NULL,
          setting_switch(&t->settings.tabs_to_spaces, each_editor));
  add_row(GTK_GRID(grid), &row, "Detect indentation",
          "Files indented with tabs or another number of spaces keep it (applies to files opened from now on)",
          setting_switch(&t->settings.detect_indent, NULL));

  grid = add_page(GTK_STACK(stack), "appearance", "Appearance", &row);
  GtkWidget *theme = gtk_drop_down_new_from_strings((const char *const[]){ "Rounded", "Square", NULL });
  gtk_drop_down_set_selected(GTK_DROP_DOWN(theme), t->settings.square ? 1 : 0);
  g_signal_connect(theme, "notify::selected", G_CALLBACK(on_theme), NULL);
  add_row(GTK_GRID(grid), &row, "Color theme", "Also \"Preferences: Color Theme\" in the command palette",
          color_themes());
  add_row(GTK_GRID(grid), &row, "Theme", "Rounded panels on a darker frame, or flat ones with lines between", theme);
  add_row(GTK_GRID(grid), &row, "Corner radius",
          "The rounded theme's panel corners in pixels; buttons, tabs and menus follow",
          setting_spin(&t->settings.corner_radius, 0, 24, apply_theme));
  add_row(GTK_GRID(grid), &row, "Selection opacity (%)",
          "How strongly the editors' selection shows over the text, in the theme's accent color",
          setting_spin(&t->settings.selection_opacity, 0, 100, editor_view_set_selection));
  add_row(GTK_GRID(grid), &row, "Editor font", "Editors and the terminal", font_for(&t->settings.font, TRUE));
  add_row(GTK_GRID(grid), &row, "Files panel font", "The names in the Files panel and Open Tabs",
          font_for(&t->settings.files_font, FALSE));
  add_row(GTK_GRID(grid), &row, "Text size (%)", "Also Ctrl+= and Ctrl+-",
          spin_for(50, 300, 10, t->settings.font_scale * 100, G_CALLBACK(on_text_size)));
  add_row(GTK_GRID(grid), &row, "Preview size (%)", "Markdown previews; also Ctrl+= and Ctrl+- in a preview tab",
          spin_for(50, 300, 10, t->settings.preview_scale * 100, G_CALLBACK(on_preview_size)));
  add_row(GTK_GRID(grid), &row, "Web images in previews",
          "Markdown previews fetch the images their text links on the web (badges, screenshots); off: "
          "nothing is fetched, the image's text shows",
          setting_switch(&t->settings.web_images, NULL));

  grid = add_page(GTK_STACK(stack), "interface", "Interface", &row);
  add_row(GTK_GRID(grid), &row, "Menu bar", "Off: the menus open from t-code's icon in the title bar",
          setting_switch(&t->settings.menu_bar, apply_interface));
  add_row(GTK_GRID(grid), &row, "Status bar", "The bar along the window's bottom",
          setting_switch(&t->settings.status_bar, apply_interface));
  add_row(GTK_GRID(grid), &row, "Build and Run buttons",
          "In the status bar; Ctrl+Shift+B and Ctrl+F5 work without them",
          setting_switch(&t->settings.build_buttons, apply_interface));
  add_row(GTK_GRID(grid), &row, "Sidebar buttons", "The title bar's buttons that show or hide the sides",
          setting_switch(&t->settings.side_buttons, apply_interface));
  add_row(GTK_GRID(grid), &row, "Search bar",
          "In the title bar: files by name, commands after '#'; Ctrl+P and Ctrl+Shift+P work without it",
          setting_switch(&t->settings.search_bar, apply_interface));
  add_heading(GTK_GRID(grid), &row, "Panels");
  /* clang-format off */
  static const char *const panel_titles[PANELS][2] = {
    { "Files", NULL },
    { "Source Control", "Off: no git in the background either (no branch, file colors, diff marks or line blame)" },
    { "Search", NULL }, { "Terminal", "Build and Run still type into it" }, { "Debug", NULL },
    { "Run and Debug", NULL }, { "Problems", NULL }, { "Outline", NULL }, { "Hierarchy", NULL },
    { "Testing", NULL }, { "Plugins", NULL },
  };
  /* clang-format on */
  for (int i = 0; i < PANELS; i++) {
    GtkWidget *toggle = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(toggle), panel_enabled(t, panel_names[i]));
    g_signal_connect(toggle, "notify::active", G_CALLBACK(on_panel_switch), GINT_TO_POINTER(i));
    add_row(GTK_GRID(grid), &row, panel_titles[i][0],
            panel_titles[i][1] ? panel_titles[i][1] : "Off: it opens nowhere, its shortcut does nothing", toggle);
  }

  grid = add_page(GTK_STACK(stack), "c", "C / C++", &row);
  add_row(GTK_GRID(grid), &row, "Format on save",
          "Ctrl+S formats C and C++ files with clangd first (the project's .clang-format); autosave does not",
          setting_switch(&t->settings.format_on_save, NULL));
  add_row(GTK_GRID(grid), &row, "clang-tidy checks",
          "clangd also reports what the project's .clang-tidy asks for (more CPU while typing)",
          setting_switch(&t->settings.clang_tidy, lsp_restart_clangd));
  add_row(GTK_GRID(grid), &row, "Debug with lldb-dap",
          "F5 debugs C, C++ and Rust with lldb-dap instead of gdb (gdb when lldb-dap is not installed)",
          setting_switch(&t->settings.lldb, NULL));
  add_heading(GTK_GRID(grid), &row, "clangd: memory and speed");
  add_row(GTK_GRID(grid), &row, "Presets",
          "t-code's defaults are the smallest measured without losing speed: on a C++ project with Eigen, 25 files"
          " took clangd from 1000-1190 MB to 550 MB, its index the same. Plain clangd: clangd and glibc as they"
          " come. Each change starts clangd again",
          preset_buttons());
  add_row(GTK_GRID(grid), &row, "Background index",
          "The whole project indexed: references, symbols and call hierarchy across files not opened. Off: about"
          " 40 % less memory, those see open files only. t-code: on · clangd: on",
          setting_switch(&t->settings.clangd.background_index, restart_clangd_later));
  add_row(GTK_GRID(grid), &row, "Worker threads",
          "-j: each thread grows memory of its own; 1 takes about 10 % less and opens files twice as slowly. 0:"
          " every core. t-code: 2 · clangd: every core",
          setting_spin(&t->settings.clangd.threads, 0, 256, restart_clangd_later));
  add_row(GTK_GRID(grid), &row, "Compiled headers on disk",
          "--pch-storage=disk: each open file's headers, compiled (10-40 MB in C++), in a file instead of clangd's"
          " memory. t-code: on · clangd: off",
          setting_switch(&t->settings.clangd.pch_on_disk, restart_clangd_later));
  add_row(GTK_GRID(grid), &row, "Those files in t-code's cache",
          "~/.cache/t-code/preambles, not /tmp, which is RAM on many systems (tmpfs); what a stopped clangd left is"
          " deleted. t-code: on · clangd: /tmp",
          setting_switch(&t->settings.clangd.preamble_dir, restart_clangd_later));
  add_row(GTK_GRID(grid), &row, "Result limit",
          "At most this many completions and references. 0: clangd's own. t-code: 50 · clangd: 100",
          setting_spin(&t->settings.clangd.limit_results, 0, 10000, restart_clangd_later));
  add_row(GTK_GRID(grid), &row, "Give back freed memory", "--malloc-trim, now and then. t-code: on · clangd: on",
          setting_switch(&t->settings.clangd.malloc_trim, restart_clangd_later));
  add_row(GTK_GRID(grid), &row, "Malloc arenas",
          "glibc's: each holds the memory freed in it, so more of them keep more. 0: glibc's own. t-code: 1 ·"
          " glibc: 8 per core",
          setting_spin(&t->settings.clangd.arenas, 0, 64, restart_clangd_later));
  add_row(GTK_GRID(grid), &row, "mmap threshold (KB)",
          "Blocks from this size get memory of their own, given back when freed. 0: glibc's own, which moves up"
          " to 32 MB and leaves clangd about twice the size. t-code: 64 · glibc: 128, moving",
          setting_spin(&t->settings.clangd.mmap_threshold_kb, 0, 32768, restart_clangd_later));
  GtkWidget *extra = gtk_entry_new();
  gtk_editable_set_text(GTK_EDITABLE(extra), t->settings.clangd.extra ? t->settings.clangd.extra : "");
  gtk_entry_set_placeholder_text(GTK_ENTRY(extra), "--header-insertion=never");
  gtk_widget_set_size_request(extra, 240, -1);
  g_signal_connect(extra, "changed", G_CALLBACK(on_clangd_extra), NULL);
  add_row(GTK_GRID(grid), &row, "Extra flags", "More clangd flags, as in a shell; after t-code's, so they win", extra);

  grid = add_page(GTK_STACK(stack), "servers", "Language Servers", &row);
  add_row(GTK_GRID(grid), &row, "Stop when unused (s)",
          "Seconds a server keeps running after the last tab of its files closes (an open tab keeps it running);"
          " 30 by default. Type -1 to keep it running",
          setting_spin(&t->settings.server_idle, -1, 86400, lsp_idle_changed));
  add_row(GTK_GRID(grid), &row, "Files open on a server",
          "The other tabs are closed on their server until shown: each open file costs memory (a C++ file's"
          " headers 10-40 MB in clangd). t-code: 5",
          setting_spin(&t->settings.server_open_files, 1, 100, NULL));
  add_row(GTK_GRID(grid), &row, "Restart when grown by (MB)",
          "A quiet server this far above where it settled (or a quarter, if more) starts again once files were"
          " closed: memory it freed stays taken until it exits. Not gopls or rust-analyzer. 0: never. t-code: 150",
          setting_spin(&t->settings.server_restart_mb, 0, 100000, NULL));
  for (int kind = 0; kind < SERVER_NONE; kind++) {
    GtkWidget *toggle = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(toggle), server_enabled(t, kind));
    g_signal_connect(toggle, "notify::active", G_CALLBACK(on_server_switch), GUINT_TO_POINTER(kind));
    char *note =
        g_strdup_printf("%s. Off: a plugin's server for these files takes over, if one has any", server_title(kind));
    add_row(GTK_GRID(grid), &row, server_id(kind), note, toggle);
    g_free(note);
  }

  grid = add_page(GTK_STACK(stack), "keys", "Keyboard Shortcuts", &row);
  gtk_grid_attach(GTK_GRID(grid), keybinds_editor(t), 0, row++, 2, 1);

  grid = add_page(GTK_STACK(stack), "files", "Edit by Hand", &row);
  add_row(GTK_GRID(grid), &row, "Settings file", "settings.ini: these settings, each with a note",
          open_button(open_settings_file));
  add_row(GTK_GRID(grid), &row, "Theme CSS", "theme.css: your own CSS over t-code's look (colors, corners, sizes)",
          open_button(open_user_theme));
  add_row(GTK_GRID(grid), &row, "Editor colors", "Your copy of the editor's color scheme (dark-classic.xml)",
          open_button(open_editor_colors));
  add_row(GTK_GRID(grid), &row, "Plugins folder", "Installed plugins (~/.config/t-code/plugins)",
          open_button(open_plugins_folder));

  if (last_page && gtk_stack_get_child_by_name(GTK_STACK(stack), last_page))
    gtk_stack_set_visible_child_name(GTK_STACK(stack), last_page);
  g_signal_connect(stack, "notify::visible-child", G_CALLBACK(on_page), NULL);
  sidebar = gtk_stack_sidebar_new();
  gtk_stack_sidebar_set_stack(GTK_STACK_SIDEBAR(sidebar), GTK_STACK(stack));
  panes = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_append(GTK_BOX(panes), sidebar);
  gtk_box_append(GTK_BOX(panes), stack);
  gtk_window_set_child(GTK_WINDOW(window), panes);
  keys = gtk_shortcut_controller_new(); /* Escape closes it */
  gtk_shortcut_controller_add_shortcut(
      GTK_SHORTCUT_CONTROLLER(keys),
      gtk_shortcut_new(gtk_keyval_trigger_new(GDK_KEY_Escape, 0), gtk_named_action_new("window.close")));
  gtk_widget_add_controller(window, keys);
  gtk_window_present(GTK_WINDOW(window));
}
