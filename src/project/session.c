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

#include "application/identity.h"
#include "debug/view/debugview.h"
#include "editor/gutter/breakpoints.h"
#include "editor/editor.h"
#include "editor/preview/preview.h"
#include "editor/tabs.h"
#include "files/opentabs.h"
#include "git/gitstatus.h"
#include "git/scm/scm.h"
#include "lsp/client/client.h"
#include "lsp/servers.h"
#include "window/palette.h"
#include "window/secondwindow.h"
#include "window/terminal.h"
#include "window/window.h"

#include <stddef.h>
#include <string.h>

#define PREVIEW_PREFIX "preview:"
#define MAX_RECENT 10

/* ~/.config/t-code/session.ini */
static char *session_file(void)
{
  return g_build_filename(g_get_user_config_dir(), TCODE_NAME, "session.ini", NULL);
}

char *settings_file(void)
{
  return g_build_filename(g_get_user_config_dir(), TCODE_NAME, "settings.ini", NULL);
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
  INT_SETTING("autosave-ms", autosave_ms, 1000, 0, 300000,
              " 0ms to 300'000ms, default 1000ms. saveing files after delay in ms after change"),
  INT_SETTING("server-idle-seconds", server_idle, 30, -1, 86400,
              " -1 to 86400, seconds a language server keeps running after its last file closes (30 by default);"
              " -1: it never stops by itself"),
  INT_SETTING("server-open-files", server_open_files, 5, 1, 100,
              " 1 to 100, files open on a language server at once (5 by default); the other tabs are closed on it"
              " until shown, which keeps the server small"),
  INT_SETTING("server-restart-mb", server_restart_mb, 150, 0, 100000,
              " 0 to 100000, a quiet server (not gopls, rust-analyzer) this many MB (or a quarter, if more) above"
              " where it settled\n starts again, once files were closed: memory it freed stays taken until it"
              " exits (150 by default); 0: never"),
  BOOL_SETTING("clangd-background-index", clangd.background_index, TRUE,
               " clangd indexes the whole project: references, symbols and call hierarchy across files it has not"
               " opened\n (true by default, as in clangd; off: about 40 % less memory, those features see open"
               " files only)"),
  INT_SETTING("clangd-threads", clangd.threads, 2, 0, 256,
              " 0 to 256, clangd's worker threads (-j; 2 by default); 0: clangd's own (every core). Each thread"
              " grows memory\n of its own; 1 takes about 10 % less and opens files twice as slowly"),
  BOOL_SETTING("clangd-pch-on-disk", clangd.pch_on_disk, TRUE,
               " clangd keeps each open file's headers compiled in a file (--pch-storage=disk; true by default),"
               " not in its\n memory (clangd's own)"),
  BOOL_SETTING("clangd-preamble-folder", clangd.preamble_dir, TRUE,
               " Those files in t-code's cache folder (~/.cache/t-code/preambles), not /tmp, which is RAM on many"
               " systems\n (tmpfs); the ones a stopped clangd left are deleted (true by default)"),
  INT_SETTING("clangd-limit-results", clangd.limit_results, 50, 0, 10000,
              " 0 to 10000, at most this many completions and references from clangd (50 by default); 0: clangd's"
              " own (100)"),
  BOOL_SETTING("clangd-malloc-trim", clangd.malloc_trim, TRUE,
               " clangd gives freed memory back to the system now and then (true by default, as in clangd)"),
  INT_SETTING("clangd-malloc-arenas", clangd.arenas, 1, 0, 64,
              " 0 to 64, glibc malloc arenas for clangd (1 by default): each holds memory freed in it; 0: glibc's"
              " own (8 per core)"),
  INT_SETTING("clangd-mmap-threshold-kb", clangd.mmap_threshold_kb, 64, 0, 32768,
              " 0 to 32768, clangd's blocks from this size (KB) get memory of their own, given back when freed"
              " (64 by default);\n 0: glibc's own, which moves up to 32 MB and keeps clangd at about twice the"
              " size"),
  INT_SETTING("tab-size", tab_size, 2, 1, 16,
              " 1 to 16, for projects without their own: the status bar's choice, .editorconfig or .clang-format come"
              " first"),
  INT_SETTING("corner-radius", corner_radius, 10, 0, 24,
              " 0 to 24, the rounded theme's panel corners in pixels (10 by default); buttons, tabs and menus follow"),
  INT_SETTING("selection-opacity", selection_opacity, 40, 0, 100,
              " 0 to 100, how strongly the editors' selection (the theme's accent color, over the text) shows;"
              " 40 by default"),
  BOOL_SETTING("tabs-to-spaces", tabs_to_spaces, TRUE, " The Tab key inserts spaces"),
  BOOL_SETTING("detect-indentation", detect_indent, TRUE, " Files indented with tabs or another number of spaces keep it"),
  BOOL_SETTING("autosave", autosave, TRUE, " Save a second after the last change"),
  BOOL_SETTING("format-on-save", format_on_save, FALSE,
               " Ctrl+S formats C and C++ files with clangd first (the project's .clang-format)"),
  BOOL_SETTING("clang-tidy", clang_tidy, FALSE,
               " clangd also reports what the project's .clang-tidy asks for (more CPU while typing)"),
  BOOL_SETTING("debug-with-lldb", lldb, FALSE,
               " F5 debugs C, C++ and Rust with lldb-dap instead of gdb (gdb when lldb-dap is not installed)"),
  BOOL_SETTING("reopen-last-project", reopen_last, TRUE,
               " When started from the app menu (not from a folder in a terminal)"),
  BOOL_SETTING("check-updates", check_updates, TRUE,
               " Once at start-up, the latest release on GitHub goes against this version; a newer one asks"),
  BOOL_SETTING("menu-bar", menu_bar, TRUE,
               " The menus in the title bar (Files, T-Code, Settings, ...); off: they open from t-code's icon"),
  BOOL_SETTING("status-bar", status_bar, TRUE, " The bar along the window's bottom (branch, problems, Ln / Col, ...)"),
  BOOL_SETTING("build-buttons", build_buttons, TRUE,
               " The status bar's Build and Run buttons (Ctrl+Shift+B and Ctrl+F5 work without them)"),
  BOOL_SETTING("sidebar-buttons", side_buttons, TRUE, " The title bar's buttons that show or hide the sides"),
  BOOL_SETTING("search-bar", search_bar, TRUE,
               " The title bar's search: files by name, commands after '#' (Ctrl+P and Ctrl+Shift+P work without it)"),
};
/* clang-format on */

static int *setting_field(App *t, const Setting *setting)
{
  return (int *)((char *)t + setting->field);
}

void session_reset_settings(App *t, const char *prefix)
{
  for (size_t i = 0; i < G_N_ELEMENTS(settings); i++)
    if (g_str_has_prefix(settings[i].key, prefix))
      *setting_field(t, &settings[i]) = settings[i].fallback;
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
  char **off = g_key_file_get_string_list(kf, "settings", "language-servers-off", NULL, NULL);
  t->settings.servers_off = 0;
  for (int i = 0; off && off[i]; i++) {
    ServerKind kind = server_by_id(g_strstrip(off[i]));
    if (kind != SERVER_NONE)
      t->settings.servers_off |= 1u << kind;
  }
  g_strfreev(off);
  g_strfreev(t->settings.status_off);
  t->settings.status_off = g_key_file_get_string_list(kf, "settings", "status-items-off", NULL, NULL);
  for (int i = 0; t->settings.status_off && t->settings.status_off[i]; i++)
    g_strstrip(t->settings.status_off[i]);
  off = g_key_file_get_string_list(kf, "settings", "panels-off", NULL, NULL);
  t->settings.panels_off = 0;
  for (int i = 0; off && off[i]; i++)
    for (int p = 0; p < PANELS; p++)
      if (strcmp(g_strstrip(off[i]), panel_names[p]) == 0)
        t->settings.panels_off |= 1u << p;
  g_strfreev(off);
  g_free(t->settings.font);
  g_free(t->settings.files_font);
  t->settings.font = g_key_file_get_string(kf, "settings", "font", NULL);
  t->settings.files_font = g_key_file_get_string(kf, "settings", "file-manager-font", NULL);
  if (!t->settings.font || !*t->settings.font)
    g_set_str(&t->settings.font, DEFAULT_FONT);
  if (!t->settings.files_font || !*t->settings.files_font)
    g_set_str(&t->settings.files_font, DEFAULT_FONT);
  g_free(t->settings.clangd.extra);
  t->settings.clangd.extra = g_key_file_get_string(kf, "settings", "clangd-extra-flags", NULL);
  if (t->settings.clangd.extra && !*g_strstrip(t->settings.clangd.extra))
    g_clear_pointer(&t->settings.clangd.extra, g_free);
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
  GPtrArray *off = g_ptr_array_new();
  for (int kind = 0; kind < SERVER_NONE; kind++)
    if (!server_enabled(t, kind))
      g_ptr_array_add(off, (gpointer)server_id(kind));
  g_key_file_set_string_list(kf, "settings", "language-servers-off", (const char *const *)off->pdata, off->len);
  g_ptr_array_free(off, TRUE);
  explain(kf, "language-servers-off",
          " Built-in language servers turned off, by name: clangd;pylsp;gopls;rust-analyzer;\n"
          " typescript-language-server;vscode-json-language-server;yaml-language-server;marksman;neocmakelsp.\n"
          " A plugin's server for the same files takes their place");
  off = g_ptr_array_new();
  for (int p = 0; p < PANELS; p++)
    if (t->settings.panels_off & (1u << p))
      g_ptr_array_add(off, (gpointer)panel_names[p]);
  g_key_file_set_string_list(kf, "settings", "panels-off", (const char *const *)off->pdata, off->len);
  g_ptr_array_free(off, TRUE);
  g_key_file_set_string_list(
      kf, "settings", "status-items-off",
      (const char *const *)(t->settings.status_off ? t->settings.status_off : (char *[]){ NULL }),
      t->settings.status_off ? g_strv_length(t->settings.status_off) : 0);
  explain(kf, "status-items-off",
          " Status bar items turned off (right-click the status bar): branch;problems;flags;build-status;progress;\n"
          " no-lsp;font-scale;indent;position;keybinds, and a plugin's item as plugin:<its key>");
  explain(kf, "panels-off",
          " Panels turned off, by name: files;scm;search;shell;debug;run;problems;outline;hierarchy;testing;plugins.\n"
          " They open nowhere (scm: no git in the background either); the Panels menu turns them on again");
  g_key_file_set_string(kf, "settings", "font", t->settings.font);
  explain(kf, "font", " Editors and the terminal: a font name and size, like Cascadia Code 11");
  g_key_file_set_string(kf, "settings", "file-manager-font", t->settings.files_font);
  explain(kf, "file-manager-font", " The Files panel");
  g_key_file_set_string(kf, "settings", "clangd-extra-flags", t->settings.clangd.extra ? t->settings.clangd.extra : "");
  explain(kf, "clangd-extra-flags",
          " More clangd flags, as in a shell (--header-insertion=never --log=verbose ...), after t-code's: they"
          " win");
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

  gtk_window_set_default_size(t->window, get_int(kf, "window", "width", 1400), get_int(kf, "window", "height", 900));
  scm_show_commits(get_bool(kf, "window", "scm-commits", TRUE));
  open_tabs_show(get_bool(kf, "window", "open-tabs", TRUE));
  if (get_bool(kf, "window", "maximized", FALSE))
    gtk_window_maximize(t->window);
  docks_restore(t, kf);
  gtk_paned_set_position(t->layout.hpaned, MAX(get_int(kf, "window", "files-width", 280), 80));
  t->layout.sizes[1] = CLAMP(get_int(kf, "window", "bottom-size", 240), 100, 1200); /* given at the first size */
  t->layout.sizes[2] = CLAMP(get_int(kf, "window", "right-size", 280), 170, 1600);
  if (gtk_widget_get_visible(t->layout.left))
    gtk_widget_set_visible(t->layout.left, get_bool(kf, "window", "files-visible", TRUE));
  if (gtk_widget_get_visible(t->layout.right))
    gtk_widget_set_visible(t->layout.right, get_bool(kf, "window", "right-visible", TRUE));
  if (gtk_widget_get_visible(t->layout.bottom))
    gtk_widget_set_visible(t->layout.bottom, get_bool(kf, "window", "shell-visible", TRUE));
  char *page = g_key_file_get_string(kf, "window", "sidebar-page", NULL);
  if (!page || !*page)
    g_set_str(&page, "files"); /* the first run: the panels were moved in, the last one shows */
  GtkStack *stack = panel_stack(t, page);
  if (stack && !g_key_file_has_key(kf, "window", "docks-left", NULL)) /* each dock keeps its own page now */
    gtk_stack_set_visible_child_name(stack, page);
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
  char **logs = g_key_file_get_string_list(kf, group, "breakpoint-logs", NULL, NULL);
  breakpoints_restore(t, breakpoints, conditions, logs);
  debug_view_breakpoints_changed(t);
  g_strfreev(logs);
  g_strfreev(conditions);
  g_strfreev(breakpoints);
  char **watches = g_key_file_get_string_list(kf, group, "watch", NULL, NULL);
  debug_view_set_watches(watches);
  g_strfreev(watches);
  char *vcs = g_key_file_get_string(kf, group, "vcs", NULL);
  git_set_chosen_kind(vcs_kind_named(vcs)); /* several repositories hold the project: the one chosen */
  g_free(vcs);
  char *message = g_key_file_get_string(kf, group, "commit-message", NULL);
  scm_set_message(message); /* none: the last project's goes */
  g_free(message);
  char **terminals = g_key_file_get_string_list(kf, group, "terminals", NULL, NULL);
  char **names = g_key_file_get_string_list(kf, group, "terminal-names", NULL, NULL);
  shell_restore(t, terminals, names);
  g_strfreev(names);
  g_strfreev(terminals);
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

/* A group's keys: window 0's first group "tabs" / "active", its next ones "tabs-1" / "active-1", ...; the
 * second window's "tabs-window", "tabs-window-1", ... */
static char *group_key(const char *what, int window, int index)
{
  const char *suffix = window ? "-window" : "";

  return index ? g_strdup_printf("%s%s-%d", what, suffix, index) : g_strconcat(what, suffix, NULL);
}

/* Opens the tabs saved under key into group; returns how many opened. */
static int restore_group(App *t, GKeyFile *kf, const char *group_name, GtkNotebook *group, const char *key,
                         const char *active_key)
{
  char **tabs = g_key_file_get_string_list(kf, group_name, key, NULL, NULL);
  int opened = 0;

  t->editors.active = group;
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
  if (opened && gtk_notebook_get_n_pages(group) > 0)
    gtk_notebook_set_current_page(group,
                                  MIN(get_int(kf, group_name, active_key, 0), gtk_notebook_get_n_pages(group) - 1));
  g_strfreev(tabs);
  return opened;
}

/* Gives a window its saved groups and their tabs; returns how many tabs opened. Before layouts were
 * saved, the main window had "tabs" and "tabs-side" (a group right of it). */
static int restore_window(App *t, GKeyFile *kf, const char *group_name, int window)
{
  const char *layout_key = window ? "layout-window" : "layout";
  char *layout = g_key_file_get_string(kf, group_name, layout_key, NULL);
  int opened = 0;

  if (!layout || !set_groups_layout(t, window, layout)) {
    char **side = window ? NULL : g_key_file_get_string_list(kf, group_name, "tabs-side", NULL, NULL);
    if (side && side[0]) {
      GtkNotebook *first = window_first_group(t, 0);
      opened += restore_group(t, kf, group_name, first, "tabs", "active");
      opened += restore_group(t, kf, group_name, split_group(first, SPLIT_RIGHT), "tabs-side", "active-side");
      g_strfreev(side);
      g_free(layout);
      prune_empty_groups(t, window);
      return opened;
    }
    g_strfreev(side);
  }
  g_free(layout);
  GPtrArray *groups = g_ptr_array_new();
  for (int g = 0; g < group_count(t); g++)
    if (group_window(t, group_nth(t, g)) == window)
      g_ptr_array_add(groups, group_nth(t, g));
  for (guint g = 0; g < groups->len; g++) {
    char *key = group_key("tabs", window, g), *active_key = group_key("active", window, g);
    opened += restore_group(t, kf, group_name, g_ptr_array_index(groups, g), key, active_key);
    g_free(key);
    g_free(active_key);
  }
  g_ptr_array_unref(groups);
  prune_empty_groups(t, window);
  return opened;
}

gboolean session_restore_tabs(App *t)
{
  GKeyFile *kf = load();
  char *group = project_group(t);
  int opened;

  opened = restore_window(t, kf, group, 0);
  char **window = g_key_file_get_string_list(kf, group, "tabs-window", NULL, NULL);
  if (window && window[0]) { /* the second window's tabs */
    show_second_window(t);
    int restored = restore_window(t, kf, group, 1);
    if (!restored) /* its files are gone */
      gtk_window_close(t->editors.second);
    opened += restored;
  }
  g_strfreev(window);
  t->editors.active = window_first_group(t, 0);
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

  if (!maximized) {
    g_key_file_set_integer(kf, "window", "width", gtk_widget_get_width(GTK_WIDGET(t->window)));
    g_key_file_set_integer(kf, "window", "height", gtk_widget_get_height(GTK_WIDGET(t->window)));
  }
  g_key_file_set_boolean(kf, "window", "maximized", maximized);
  docks_save(t, kf);
  if (gtk_widget_get_visible(t->layout.left) && gtk_paned_get_position(t->layout.hpaned) >= 48)
    g_key_file_set_integer(kf, "window", "files-width", gtk_paned_get_position(t->layout.hpaned));
  if (gtk_widget_get_visible(t->layout.right) && gtk_widget_get_width(t->layout.right) >= 170)
    g_key_file_set_integer(kf, "window", "right-size", gtk_widget_get_width(t->layout.right));
  if (gtk_widget_get_visible(t->layout.bottom) && gtk_widget_get_height(t->layout.bottom) >= 100)
    g_key_file_set_integer(kf, "window", "bottom-size", gtk_widget_get_height(t->layout.bottom));
  g_key_file_set_boolean(kf, "window", "files-visible", gtk_widget_get_visible(t->layout.left));
  g_key_file_set_boolean(kf, "window", "right-visible", gtk_widget_get_visible(t->layout.right));
  g_key_file_set_boolean(kf, "window", "shell-visible", gtk_widget_get_visible(t->layout.bottom));
  g_key_file_set_double(kf, "window", "font-scale", t->settings.font_scale);
  g_key_file_set_double(kf, "window", "preview-scale", t->settings.preview_scale);
  g_key_file_set_boolean(kf, "window", "scm-commits", scm_commits_shown());
  g_key_file_set_boolean(kf, "window", "open-tabs", open_tabs_shown());

  g_key_file_set_string(kf, group, "root", t->project.root);
  if (git_chosen_kind() != VCS_NONE)
    g_key_file_set_string(kf, group, "vcs", vcs_program(git_chosen_kind()));
  else
    g_key_file_remove_key(kf, group, "vcs", NULL);
  char *message = scm_message(); /* a commit message not committed yet comes back with the project */
  if (*message)
    g_key_file_set_string(kf, group, "commit-message", message);
  else
    g_key_file_remove_key(kf, group, "commit-message", NULL);
  g_free(message);
  char **keys = g_key_file_get_keys(kf, group, NULL, NULL); /* the groups as they were last time go */
  for (int i = 0; keys && keys[i]; i++)
    if (g_str_has_prefix(keys[i], "tabs") || g_str_has_prefix(keys[i], "active") || g_str_has_prefix(keys[i], "layout"))
      g_key_file_remove_key(kf, group, keys[i], NULL);
  g_strfreev(keys);
  for (int w = 0; w < 2; w++) {
    char *layout = groups_layout(t, w);
    if (layout)
      g_key_file_set_string(kf, group, w ? "layout-window" : "layout", layout);
    g_free(layout);
  }
  for (int g = 0, in_window[2] = { 0, 0 }; g < group_count(t); g++) {
    GtkNotebook *notebook = group_nth(t, g);
    int window = group_window(t, notebook), index = in_window[window]++;
    int n = gtk_notebook_get_n_pages(notebook), kept = 0, active = 0;
    int current = gtk_notebook_get_current_page(notebook);
    char **tabs = g_new0(char *, n + 1);
    for (int i = 0; i < n; i++) {
      Tab *tab = g_object_get_data(G_OBJECT(gtk_notebook_get_nth_page(notebook, i)), "t-code-tab");
      if (i == current) /* counted among the kept tabs (an old version: the one before it) */
        active = MAX(kept - (tab->compare.with ? 1 : 0), 0);
      if (tab->compare.with) /* old versions in a comparison live in a temp folder */
        continue;
      /* a library's source (the server gave it), a plugin's view (the plugin opens it again) */
      if ((tab->kind == TAB_EDITOR && lsp_is_alias(tab->path)) || tab->kind == TAB_VIEW)
        continue;
      tabs[kept++] = tab->kind == TAB_PREVIEW ? g_strconcat(PREVIEW_PREFIX, tab->path, NULL) : g_strdup(tab->path);
    }
    char *key = group_key("tabs", window, index), *active_key = group_key("active", window, index);
    g_key_file_set_string_list(kf, group, key, (const char *const *)tabs, kept);
    g_key_file_set_integer(kf, group, active_key, active);
    g_free(key);
    g_free(active_key);
    g_strfreev(tabs);
  }
  char **conditions = NULL, **logs = NULL, **breakpoints = breakpoints_save(t, &conditions, &logs);
  gboolean any_condition = FALSE, any_log = FALSE;
  for (int i = 0; conditions[i]; i++)
    any_condition |= *conditions[i] != '\0';
  for (int i = 0; logs[i]; i++)
    any_log |= *logs[i] != '\0';
  if (breakpoints)
    g_key_file_set_string_list(kf, group, "breakpoints", (const char *const *)breakpoints, g_strv_length(breakpoints));
  else
    g_key_file_remove_key(kf, group, "breakpoints", NULL);
  if (any_condition) /* in the same order as breakpoints, "" for none */
    g_key_file_set_string_list(kf, group, "breakpoint-conditions", (const char *const *)conditions,
                               g_strv_length(conditions));
  else
    g_key_file_remove_key(kf, group, "breakpoint-conditions", NULL);
  if (any_log)
    g_key_file_set_string_list(kf, group, "breakpoint-logs", (const char *const *)logs, g_strv_length(logs));
  else
    g_key_file_remove_key(kf, group, "breakpoint-logs", NULL);
  g_strfreev(logs);
  g_strfreev(conditions);
  g_strfreev(breakpoints);
  char **watches = debug_view_watches();
  if (watches && *watches)
    g_key_file_set_string_list(kf, group, "watch", (const char *const *)watches, g_strv_length(watches));
  else
    g_key_file_remove_key(kf, group, "watch", NULL);
  g_strfreev(watches);
  char **names = NULL, **terminals = shell_folders(t, &names);
  if (*terminals) { /* the terminal tabs after the first one */
    g_key_file_set_string_list(kf, group, "terminals", (const char *const *)terminals, g_strv_length(terminals));
    g_key_file_set_string_list(kf, group, "terminal-names", (const char *const *)names, g_strv_length(names));
  } else {
    g_key_file_remove_key(kf, group, "terminals", NULL);
    g_key_file_remove_key(kf, group, "terminal-names", NULL);
  }
  g_strfreev(names);
  g_strfreev(terminals);

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
