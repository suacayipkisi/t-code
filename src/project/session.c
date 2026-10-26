/*
 * Remembers the window between runs, in ~/.config/t-code/session.ini:
 *   [window]            size, pane sizes, which panes are shown, text size
 *   [recent]            the last opened project folders
 *   [project <sha1>]    per project folder: the open tabs of both groups and the active ones, the
 *                       indentation chosen for it in the status bar, its breakpoints (and their
 *                       conditions), the debugger's watch expressions and the program it debugs
 *                       (a Makefile's)
 * and the settings in ~/.config/t-code/settings.ini ([settings], with a comment over each one, for
 * editing by hand). Before 0.3 the settings were in session.ini's [settings].
 */
#include "project/session.h"

#include "debug/view/debugview.h"
#include "editor/gutter/breakpoints.h"
#include "editor/editor.h"
#include "editor/preview/preview.h"
#include "editor/tabs.h"
#include "files/opentabs.h"
#include "git/scm/scm.h"
#include "window/palette.h"
#include "window/secondwindow.h"
#include "window/terminal.h"
#include "window/window.h"

#include <stddef.h>
#include <string.h>

#define PREVIEW_PREFIX "preview:"
#define MAX_RECENT 10

static const char *const tab_keys[GROUPS] = { "tabs", "tabs-side", "tabs-window" };
static const char *const active_keys[GROUPS] = { "active", "active-side", "active-window" };

/* ~/.config/t-code/session.ini */
static char *session_file(void)
{
  return g_build_filename(g_get_user_config_dir(), "t-code", "session.ini", NULL);
}

char *settings_file(void)
{
  return g_build_filename(g_get_user_config_dir(), "t-code", "settings.ini", NULL);
}

/* A key file as it is now (empty when there is none yet). */
static GKeyFile *load_from(const char *path)
{
  GKeyFile *kf = g_key_file_new();

  g_key_file_load_from_file(kf, path, G_KEY_FILE_KEEP_COMMENTS, NULL);
  return kf;
}

/* The session file as it is now (empty when there is none yet). */
static GKeyFile *load(void)
{
  char *path = session_file();
  GKeyFile *kf = load_from(path);

  g_free(path);
  return kf;
}

/* Writes a key file, making its folder first. */
static void store_to(GKeyFile *kf, const char *path)
{
  char *dir = g_path_get_dirname(path);

  g_mkdir_with_parents(dir, 0700);
  g_key_file_save_to_file(kf, path, NULL);
  g_free(dir);
}

/* Writes the session file. */
static void store(GKeyFile *kf)
{
  char *path = session_file();

  store_to(kf, path);
  g_free(path);
}

/* The group of the current project's tabs: "project <sha1 of the root>". */
static char *project_group(App *t)
{
  char *sum = g_compute_checksum_for_string(G_CHECKSUM_SHA1, t->project.root, -1);
  char *group = g_strdup_printf("project %s", sum);

  g_free(sum);
  return group;
}

/* A number from the session file, or fallback when it is missing or not a number. */
static int get_int(GKeyFile *kf, const char *group, const char *key, int fallback)
{
  GError *error = NULL;
  int value = g_key_file_get_integer(kf, group, key, &error);

  if (error) {
    g_error_free(error);
    return fallback;
  }
  return value;
}

/* true / false from the session file, or fallback when it is missing or not a boolean. */
static gboolean get_bool(GKeyFile *kf, const char *group, const char *key, gboolean fallback)
{
  GError *error = NULL;
  gboolean value = g_key_file_get_boolean(kf, group, key, &error);

  if (error) {
    g_error_free(error);
    return fallback;
  }
  return value;
}

static double get_double(GKeyFile *kf, const char *group, const char *key, double fallback)
{
  GError *error = NULL;
  double value = g_key_file_get_double(kf, group, key, &error);

  if (error) {
    g_error_free(error);
    return fallback;
  }
  return value;
}

/* The plain settings in settings.ini: each one's key, field in App (an int: a number from low to high,
 * or a gboolean), default and the comment over it. The theme, colors and fonts are read and written
 * by hand. */
typedef struct {
  const char *key;
  size_t field; /* offsetof(App, settings.<name>) */
  gboolean number;
  int fallback, low, high;
  const char *comment;
} Setting;

#define BOOL_SETTING(key, name, fallback, comment) { key, offsetof(App, settings.name), FALSE, fallback, 0, 0, comment }
#define INT_SETTING(key, name, fallback, low, high, comment)                                                           \
  { key, offsetof(App, settings.name), TRUE, fallback, low, high, comment }

/* clang-format off */
static const Setting settings[] = {
  BOOL_SETTING("minimap", minimap, TRUE, " A small picture of the file next to the scrollbar"),
  BOOL_SETTING("sticky-scroll", sticky_scroll, TRUE,
               " The first lines of the blocks around the editor's top (namespace, function, loop) stay\n"
               " at its top"),
  BOOL_SETTING("indent-guides", guides, TRUE, " Lines at each indentation level; the block around the cursor in color"),
  BOOL_SETTING("word-wrap", word_wrap, FALSE, " Lines longer than the editor continue on the next row"),
  BOOL_SETTING("inlay-hints", inlay_hints, TRUE,
               " Parameter names and deduced types shown inside the lines (from the language servers)"),
  BOOL_SETTING("breadcrumbs", breadcrumbs, TRUE, " The file's folders and the symbols around the cursor, over each editor"),
  BOOL_SETTING("line-blame", line_blame, TRUE, " Who last changed the cursor's line, and in which commit, after its end"),
  INT_SETTING("ruler", ruler, 80, 0, 300,
              " A vertical line after this column in editors, to keep lines short (80 by default, 120, ...); 0 turns"
              " it off"),
  INT_SETTING("tab-size", tab_size, 2, 1, 16,
              " 1 to 16, for projects without their own: the status bar's choice, .editorconfig or .clang-format come"
              " first"),
  BOOL_SETTING("tabs-to-spaces", tabs_to_spaces, TRUE, " The Tab key inserts spaces"),
  BOOL_SETTING("detect-indentation", detect_indent, TRUE, " Files indented with tabs or another number of spaces keep it"),
  BOOL_SETTING("autosave", autosave, TRUE, " Save a second after the last change"),
  BOOL_SETTING("format-on-save", format_on_save, FALSE,
               " Ctrl+S formats C and C++ files with clangd first (the project's .clang-format)"),
  BOOL_SETTING("clang-tidy", clang_tidy, FALSE,
               " clangd also reports what the project's .clang-tidy asks for (more CPU while typing)"),
  BOOL_SETTING("reopen-last-project", reopen_last, TRUE,
               " When started from the app menu (not from a folder in a terminal)"),
  BOOL_SETTING("check-updates", check_updates, TRUE,
               " Once at start-up, the latest release on GitHub goes against this version; a newer one asks"),
};
/* clang-format on */

static int *setting_field(App *t, const Setting *setting)
{
  return (int *)((char *)t + setting->field);
}

void session_load_settings(App *t)
{
  char *path = settings_file();
  gboolean moved = !g_file_test(path, G_FILE_TEST_EXISTS);
  GKeyFile *kf = moved ? load() : load_from(path); /* none yet: the ones session.ini kept before 0.3 */
  char *theme = g_key_file_get_string(kf, "settings", "theme", NULL);

  for (size_t i = 0; i < G_N_ELEMENTS(settings); i++) {
    const Setting *setting = &settings[i];
    *setting_field(t, setting) =
        setting->number ? CLAMP(get_int(kf, "settings", setting->key, setting->fallback), setting->low, setting->high)
                        : get_bool(kf, "settings", setting->key, setting->fallback);
  }
  t->settings.square = g_strcmp0(theme ? g_strstrip(theme) : NULL, "square") == 0;
  g_free(t->settings.color_theme);
  t->settings.color_theme = g_key_file_get_string(kf, "settings", "color-theme", NULL);
  if (!t->settings.color_theme || !*g_strstrip(t->settings.color_theme))
    g_set_str(&t->settings.color_theme, "dark-classic");
  else /* a renamed theme's old id */
    g_set_str(&t->settings.color_theme, palette_known_id(t->settings.color_theme));
  t->settings.plain = !get_bool(kf, "settings", "code-coloring", TRUE);
  g_free(t->settings.font);
  g_free(t->settings.files_font);
  t->settings.font = g_key_file_get_string(kf, "settings", "font", NULL);
  t->settings.files_font = g_key_file_get_string(kf, "settings", "file-manager-font", NULL);
  if (!t->settings.font || !*t->settings.font)
    g_set_str(&t->settings.font, DEFAULT_FONT);
  if (!t->settings.files_font || !*t->settings.files_font)
    g_set_str(&t->settings.files_font, DEFAULT_FONT);
  g_free(theme);
  g_key_file_free(kf);
  if (moved) {
    GKeyFile *session = load();
    gboolean had = g_key_file_remove_group(session, "settings", NULL);
    session_save_settings(t); /* settings.ini, with its comments */
    if (had)
      store(session);
    g_key_file_free(session);
  }
  g_free(path);
}

/* Puts comment over key in [settings] (the file is read and edited by hand). */
static void explain(GKeyFile *kf, const char *key, const char *comment)
{
  g_key_file_set_comment(kf, "settings", key, comment, NULL);
}

void session_save_settings(App *t)
{
  char *path = settings_file();
  GKeyFile *kf = load_from(path); /* keeps what was added by hand */
  GKeyFile *session = load();

  g_key_file_set_comment(kf, NULL, NULL,
                         " t-code's settings, also in the Settings window (Ctrl+,). A change saved here applies at"
                         " once.\n The look: theme.css next to this file (\"Preferences: Open Theme CSS\"), the"
                         " editor's colors:\n styles/dark-classic.xml (\"Preferences: Open Editor Colors\").",
                         NULL);
  g_key_file_set_string(kf, "settings", "theme", t->settings.square ? "square" : "rounded");
  explain(kf, "theme", " rounded: the panels as rounded cards on a darker frame; square: flat, lines between them");
  g_key_file_set_string(kf, "settings", "color-theme",
                        t->settings.color_theme ? t->settings.color_theme : "dark-classic");
  explain(kf, "color-theme",
          " dark-classic, light-classic, dracula, nord, ...: \"Preferences: Color Theme\" lists them all");
  g_key_file_set_boolean(kf, "settings", "code-coloring", !t->settings.plain);
  explain(kf, "code-coloring", " Syntax colors in the editors (also F7)");
  for (size_t i = 0; i < G_N_ELEMENTS(settings); i++) {
    const Setting *setting = &settings[i];
    if (setting->number)
      g_key_file_set_integer(kf, "settings", setting->key, *setting_field(t, setting));
    else
      g_key_file_set_boolean(kf, "settings", setting->key, *setting_field(t, setting));
    explain(kf, setting->key, setting->comment);
  }
  g_key_file_set_string(kf, "settings", "font", t->settings.font);
  explain(kf, "font", " Editors and the terminal: a font name and size, like Cascadia Code 11");
  g_key_file_set_string(kf, "settings", "file-manager-font", t->settings.files_font);
  explain(kf, "file-manager-font", " The Files panel (its file icons need a Nerd Font)");
  store_to(kf, path);
  g_key_file_set_double(session, "window", "font-scale", t->settings.font_scale);
  g_key_file_set_double(session, "window", "preview-scale", t->settings.preview_scale);
  store(session);
  g_key_file_free(session);
  g_key_file_free(kf);
  g_free(path);
}

void session_apply_window(App *t)
{
  GKeyFile *kf = load();
  double scale = get_double(kf, "window", "font-scale", 1.0);
  double preview_scale = get_double(kf, "window", "preview-scale", 1.0);
  int layout = get_int(kf, "window", "dock-layout", 0);
  char **order = g_key_file_get_string_list(kf, "window", "panel-order", NULL, NULL);

  gtk_window_set_default_size(t->window, get_int(kf, "window", "width", 1400), get_int(kf, "window", "height", 900));
  scm_show_commits(get_bool(kf, "window", "scm-commits", TRUE));
  open_tabs_show(get_bool(kf, "window", "open-tabs", TRUE));
  if (get_bool(kf, "window", "maximized", FALSE))
    gtk_window_maximize(t->window);
  for (int pass = 0; pass < 2; pass++) {
    for (int i = 0; (pass == 0) ? (order && order[i]) : (i < PANELS); i++) {
      const char *nm = (pass == 0) ? order[i] : panel_names[i];
      if (pass == 1 && order) {
        gboolean seen = FALSE;
        for (int j = 0; order[j]; j++)
          if (strcmp(order[j], nm) == 0)
            seen = TRUE;
        if (seen)
          continue;
      }
      int def = 0;
      for (int j = 0; j < PANELS; j++)
        if (strcmp(panel_names[j], nm) == 0)
          def = panel_default_docks[j];
      char key[32];
      g_snprintf(key, sizeof(key), "dock-%s", nm);
      int d = get_int(kf, "window", key, def);
      /* An older session.ini numbered the docks differently; a default is in today's numbering
       * (no session.ini yet: the first run) */
      if (layout != DOCKS && g_key_file_has_key(kf, "window", key, NULL)) {
        if (d == 2)
          d = 4;
        else if (d == 3)
          d = 8;
        else if (d == 4)
          d = 9;
      }
      if (d >= 0 && d < DOCKS)
        move_panel(t, nm, d);
    }
  }
  g_strfreev(order);
  int shell = panel_dock(t, "shell"); /* the panel moved in last shows: in the shell's dock, the shell */
  if (shell >= 0)
    gtk_stack_set_visible_child_name(t->dock.stacks[shell], "shell");
  gtk_paned_set_position(t->layout.hpaned, MAX(get_int(kf, "window", "files-width", 280), 80));
  gtk_paned_set_position(t->layout.rpaned, CLAMP(get_int(kf, "window", "right-position", 840), 200, 1600));
  gtk_paned_set_position(t->layout.vpaned, CLAMP(get_int(kf, "window", "editor-height", 620), 120, 1200));
  gtk_paned_set_position(GTK_PANED(t->layout.left), MAX(get_int(kf, "window", "sidebar-split", 400), 80));
  gtk_paned_set_position(GTK_PANED(t->layout.right), MAX(get_int(kf, "window", "right-split", 400), 80));
  if (gtk_widget_get_visible(t->layout.left))
    gtk_widget_set_visible(t->layout.left, get_bool(kf, "window", "files-visible", TRUE));
  if (gtk_widget_get_visible(t->layout.right))
    gtk_widget_set_visible(t->layout.right, get_bool(kf, "window", "right-visible", TRUE));
  if (gtk_widget_get_visible(t->layout.bottom))
    gtk_widget_set_visible(t->layout.bottom, get_bool(kf, "window", "shell-visible", TRUE));
  char *page = g_key_file_get_string(kf, "window", "sidebar-page", NULL);
  if (!page || !*page)
    g_set_str(&page, "files"); /* the first run: the panels were moved in, the last one shows */
  int d = panel_dock(t, page);
  if (d >= 0)
    gtk_stack_set_visible_child_name(t->dock.stacks[d], page);
  g_free(page);
  if (scale > 0)
    set_font_scale(t, scale);
  if (preview_scale > 0)
    set_preview_zoom_scale(t, preview_scale);
  g_key_file_free(kf);
}

void session_load_project(App *t)
{
  GKeyFile *kf = load();
  char *group = project_group(t);

  t->project.indent.width = CLAMP(get_int(kf, group, "indent-width", 0), 0, 16);
  t->project.indent.tabs = get_bool(kf, group, "indent-tabs", FALSE);
  char **breakpoints = g_key_file_get_string_list(kf, group, "breakpoints", NULL, NULL);
  char **conditions = g_key_file_get_string_list(kf, group, "breakpoint-conditions", NULL, NULL);
  breakpoints_restore(t, breakpoints, conditions);
  debug_view_breakpoints_changed(t);
  g_strfreev(conditions);
  g_strfreev(breakpoints);
  char **watches = g_key_file_get_string_list(kf, group, "watch", NULL, NULL);
  debug_view_set_watches(watches);
  g_strfreev(watches);
  char *message = g_key_file_get_string(kf, group, "commit-message", NULL);
  scm_set_message(message); /* none: the last project's goes */
  g_free(message);
  g_free(group);
  g_key_file_free(kf);
}

void session_save_project_indent(App *t)
{
  GKeyFile *kf = load();
  char *group = project_group(t);

  g_key_file_set_string(kf, group, "root", t->project.root);
  if (t->project.indent.width > 0) {
    g_key_file_set_integer(kf, group, "indent-width", t->project.indent.width);
    g_key_file_set_boolean(kf, group, "indent-tabs", t->project.indent.tabs);
  } else {
    g_key_file_remove_key(kf, group, "indent-width", NULL);
    g_key_file_remove_key(kf, group, "indent-tabs", NULL);
  }
  store(kf);
  g_free(group);
  g_key_file_free(kf);
}

char *session_project_value(App *t, const char *key)
{
  GKeyFile *kf = load();
  char *group = project_group(t);
  char *value = g_key_file_get_string(kf, group, key, NULL);

  g_free(group);
  g_key_file_free(kf);
  return value;
}

void session_set_project_value(App *t, const char *key, const char *value)
{
  GKeyFile *kf = load();
  char *group = project_group(t);

  g_key_file_set_string(kf, group, "root", t->project.root);
  if (value)
    g_key_file_set_string(kf, group, key, value);
  else
    g_key_file_remove_key(kf, group, key, NULL);
  store(kf);
  g_free(group);
  g_key_file_free(kf);
}

/* Opens the tabs saved for one group into the active group; returns how many opened. */
static int restore_group(App *t, GKeyFile *kf, const char *group_name, int g)
{
  char **tabs = g_key_file_get_string_list(kf, group_name, tab_keys[g], NULL, NULL);
  int opened = 0;

  for (int i = 0; tabs && tabs[i]; i++) {
    gboolean preview = g_str_has_prefix(tabs[i], PREVIEW_PREFIX);
    const char *path = preview ? tabs[i] + strlen(PREVIEW_PREFIX) : tabs[i];
    if (!g_file_test(path, G_FILE_TEST_IS_REGULAR))
      continue;
    if (preview)
      open_preview(t, path);
    else
      open_file(t, path);
    opened++;
  }
  if (opened)
    gtk_notebook_set_current_page(t->editors.groups[g], MIN(get_int(kf, group_name, active_keys[g], 0), opened - 1));
  g_strfreev(tabs);
  return opened;
}

gboolean session_restore_tabs(App *t)
{
  GKeyFile *kf = load();
  char *group = project_group(t);
  int opened;

  t->editors.active = 0;
  opened = restore_group(t, kf, group, 0);
  if (g_key_file_has_key(kf, group, tab_keys[1], NULL)) {
    char **side = g_key_file_get_string_list(kf, group, tab_keys[1], NULL, NULL);
    if (side && side[0]) {
      set_split(t, TRUE);
      t->editors.active = 1;
      if (restore_group(t, kf, group, 1) > 0) {
        int position = get_int(kf, "window", "split-position", 0);
        if (position > 0)
          gtk_paned_set_position(t->editors.split, position);
        opened++;
      } else {
        set_split(t, FALSE);
      }
    }
    g_strfreev(side);
  }
  char **window = g_key_file_get_string_list(kf, group, tab_keys[WINDOW_GROUP], NULL, NULL);
  if (window && window[0]) { /* the second window's tabs */
    show_second_window(t);
    t->editors.active = WINDOW_GROUP;
    int restored = restore_group(t, kf, group, WINDOW_GROUP);
    if (!restored) /* its files are gone */
      gtk_window_close(t->editors.second);
    opened += restored;
  }
  g_strfreev(window);
  t->editors.active = 0;
  if (opened)
    focus_current_tab(t);
  g_free(group);
  g_key_file_free(kf);
  return opened > 0;
}

void session_save(App *t)
{
  GKeyFile *kf = load();
  char *group = project_group(t);
  gboolean maximized = gtk_window_is_maximized(t->window);
  char *order[PANELS];
  int n_order = 0;

  if (!maximized) {
    g_key_file_set_integer(kf, "window", "width", gtk_widget_get_width(GTK_WIDGET(t->window)));
    g_key_file_set_integer(kf, "window", "height", gtk_widget_get_height(GTK_WIDGET(t->window)));
  }
  g_key_file_set_boolean(kf, "window", "maximized", maximized);
  g_key_file_set_integer(kf, "window", "dock-layout", DOCKS);
  for (int i = 0; i < PANELS; i++) {
    char key[32];
    g_snprintf(key, sizeof(key), "dock-%s", panel_names[i]);
    g_key_file_set_integer(kf, "window", key, panel_dock(t, panel_names[i]));
  }
  for (int d = 0; d < DOCKS; d++) {
    GtkSelectionModel *pages = gtk_stack_get_pages(t->dock.stacks[d]);
    guint n_items = g_list_model_get_n_items(G_LIST_MODEL(pages));
    for (guint i = 0; i < n_items; i++) {
      GtkStackPage *item = g_list_model_get_item(G_LIST_MODEL(pages), i);
      if (item && gtk_stack_page_get_name(item) && n_order < PANELS)
        order[n_order++] = g_strdup(gtk_stack_page_get_name(item));
      if (item)
        g_object_unref(item);
    }
    g_object_unref(pages);
  }
  g_key_file_set_string_list(kf, "window", "panel-order", (const char *const *)order, n_order);
  for (int i = 0; i < n_order; i++)
    g_free(order[i]);
  if (gtk_widget_get_visible(t->layout.left) && gtk_paned_get_position(t->layout.hpaned) >= 48)
    g_key_file_set_integer(kf, "window", "files-width", gtk_paned_get_position(t->layout.hpaned));
  if (gtk_widget_get_visible(t->layout.right) && gtk_widget_get_width(t->layout.right) >= 48)
    g_key_file_set_integer(kf, "window", "right-position", gtk_paned_get_position(t->layout.rpaned));
  if (gtk_widget_get_visible(t->layout.bottom) && gtk_widget_get_height(t->layout.bottom) >= 48)
    g_key_file_set_integer(kf, "window", "editor-height", gtk_paned_get_position(t->layout.vpaned));
  if (gtk_widget_get_visible(t->dock.cards[0]) && gtk_widget_get_visible(t->dock.cards[1]))
    g_key_file_set_integer(kf, "window", "sidebar-split", gtk_paned_get_position(GTK_PANED(t->layout.left)));
  if (gtk_widget_get_visible(t->dock.cards[8]) && gtk_widget_get_visible(t->dock.cards[9]))
    g_key_file_set_integer(kf, "window", "right-split", gtk_paned_get_position(GTK_PANED(t->layout.right)));
  if (gtk_widget_get_visible(GTK_WIDGET(t->editors.groups[1])))
    g_key_file_set_integer(kf, "window", "split-position", gtk_paned_get_position(t->editors.split));
  g_key_file_set_boolean(kf, "window", "files-visible", gtk_widget_get_visible(t->layout.left));
  g_key_file_set_boolean(kf, "window", "right-visible", gtk_widget_get_visible(t->layout.right));
  g_key_file_set_boolean(kf, "window", "shell-visible", gtk_widget_get_visible(t->layout.bottom));
  const char *vis = gtk_stack_get_visible_child_name(t->dock.stacks[0]);
  if (!vis)
    vis = gtk_stack_get_visible_child_name(t->dock.scm_stack);
  if (vis)
    g_key_file_set_string(kf, "window", "sidebar-page", vis);
  g_key_file_set_double(kf, "window", "font-scale", t->settings.font_scale);
  g_key_file_set_double(kf, "window", "preview-scale", t->settings.preview_scale);
  g_key_file_set_boolean(kf, "window", "scm-commits", scm_commits_shown());
  g_key_file_set_boolean(kf, "window", "open-tabs", open_tabs_shown());

  g_key_file_set_string(kf, group, "root", t->project.root);
  char *message = scm_message(); /* a commit message not committed yet comes back with the project */
  if (*message)
    g_key_file_set_string(kf, group, "commit-message", message);
  else
    g_key_file_remove_key(kf, group, "commit-message", NULL);
  g_free(message);
  for (int g = 0; g < GROUPS; g++) {
    GtkNotebook *notebook = t->editors.groups[g]; /* NULL: no second window */
    int n = notebook ? gtk_notebook_get_n_pages(notebook) : 0, kept = 0, active = 0;
    int current = notebook ? gtk_notebook_get_current_page(notebook) : -1;
    char **tabs = g_new0(char *, n + 1);
    for (int i = 0; i < n; i++) {
      Tab *tab = g_object_get_data(G_OBJECT(gtk_notebook_get_nth_page(notebook, i)), "t-code-tab");
      if (i == current) /* counted among the kept tabs (an old version: the one before it) */
        active = MAX(kept - (tab->compare.with ? 1 : 0), 0);
      if (tab->compare.with) /* old versions in a comparison live in a temp folder */
        continue;
      tabs[kept++] = tab->kind == TAB_PREVIEW ? g_strconcat(PREVIEW_PREFIX, tab->path, NULL) : g_strdup(tab->path);
    }
    g_key_file_set_string_list(kf, group, tab_keys[g], (const char *const *)tabs, kept);
    g_key_file_set_integer(kf, group, active_keys[g], active);
    g_strfreev(tabs);
  }
  char **conditions = NULL, **breakpoints = breakpoints_save(t, &conditions);
  gboolean any_condition = FALSE;
  for (int i = 0; conditions[i]; i++)
    any_condition |= *conditions[i] != '\0';
  if (breakpoints)
    g_key_file_set_string_list(kf, group, "breakpoints", (const char *const *)breakpoints, g_strv_length(breakpoints));
  else
    g_key_file_remove_key(kf, group, "breakpoints", NULL);
  if (any_condition) /* in the same order as breakpoints, "" for none */
    g_key_file_set_string_list(kf, group, "breakpoint-conditions", (const char *const *)conditions,
                               g_strv_length(conditions));
  else
    g_key_file_remove_key(kf, group, "breakpoint-conditions", NULL);
  g_strfreev(conditions);
  g_strfreev(breakpoints);
  char **watches = debug_view_watches();
  if (watches && *watches)
    g_key_file_set_string_list(kf, group, "watch", (const char *const *)watches, g_strv_length(watches));
  else
    g_key_file_remove_key(kf, group, "watch", NULL);
  g_strfreev(watches);

  store(kf);
  g_free(group);
  g_key_file_free(kf);
}

char **session_recent(void)
{
  GKeyFile *kf = load();
  char **recent = g_key_file_get_string_list(kf, "recent", "folders", NULL, NULL);

  g_key_file_free(kf);
  return recent;
}

/* Puts the current project at the top of the recent folders. */
void session_add_recent(App *t)
{
  GKeyFile *kf = load();
  char **old = g_key_file_get_string_list(kf, "recent", "folders", NULL, NULL);
  GPtrArray *list = g_ptr_array_new();

  g_ptr_array_add(list, t->project.root);
  for (int i = 0; old && old[i] && list->len < MAX_RECENT; i++)
    if (strcmp(old[i], t->project.root) != 0)
      g_ptr_array_add(list, old[i]);
  g_key_file_set_string_list(kf, "recent", "folders", (const char *const *)list->pdata, list->len);
  store(kf);

  g_ptr_array_free(list, TRUE);
  g_strfreev(old);
  g_key_file_free(kf);
}
