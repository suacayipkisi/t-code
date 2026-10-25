/*
 * The files in ~/.config/t-code edited by hand (see userfiles.h): each is watched, and read again
 * RELOAD_MS after it changed (an editor's save is a write and a rename).
 */
#include "project/userfiles.h"

#include "dialogs/settings.h"
#include "editor/editor.h"
#include "editor/editorview.h"
#include "editor/tabs.h"
#include "project/session.h"
#include "window/theme.h"

#include <glib/gstdio.h>
#include <string.h>

#define RELOAD_MS 200

typedef enum { USER_SETTINGS, USER_THEME, USER_COLORS, USER_FILES } UserFile;

static GFileMonitor *monitors[USER_FILES];
static guint reload_ids[USER_FILES];
static App *app;

/* What theme.css starts with: how it works, and examples to uncomment. */
static const char *const theme_template =
    "/*\n"
    " * Your own look for t-code: GTK CSS over t-code's own theme. Saving this file applies it at once;\n"
    " * delete the file to go back. Some examples (take them out of this comment):\n"
    " *\n"
    " * The panels' and the editor's background:\n"
    " *   .tcode-card, notebook > stack { background: #1e1e2e; }\n"
    " * The frame around them (title bar, status bar, the gaps):\n"
    " *   window.background, headerbar, .tcode-status, .tcode-tabs { background: #11111b; }\n"
    " * The line on the current tab:\n"
    " *   .tcode-tab:checked { box-shadow: inset 0 2px #f38ba8; }\n"
    " * Rounder panels (theme = rounded in settings.ini):\n"
    " *   .tcode-card { border-radius: 16px; }\n"
    " * Wider gaps between the panels:\n"
    " *   paned > separator { min-width: 10px; min-height: 10px; }\n"
    " *\n"
    " * The editor's text colors are in styles/dark-classic.xml next to this file\n"
    " * (\"Preferences: Open Editor Colors\" in the command palette).\n"
    " */\n";

/* The file for which (free it). */
static char *path_of(UserFile which)
{
  switch (which) {
    case USER_SETTINGS: return settings_file();
    case USER_THEME: return user_theme_file();
    default: return user_colors_file();
  }
}

/* settings.ini changed: the settings read again, and applied when they are not what t-code has
 * (its own saves from the Settings window come back the same). */
static void reload_settings(App *t)
{
  typeof(t->settings) old = t->settings;
  gboolean plain = t->plain;
  gboolean same;

  old.font = g_strdup(t->settings.font);
  old.files_font = g_strdup(t->settings.files_font);
  old.color_theme = g_strdup(t->settings.color_theme);
  session_load_settings(t);
  same = plain == t->plain && old.minimap == t->settings.minimap && old.sticky_scroll == t->settings.sticky_scroll &&
         old.reopen_last == t->settings.reopen_last && old.autosave == t->settings.autosave &&
         old.tabs_to_spaces == t->settings.tabs_to_spaces && old.tab_size == t->settings.tab_size &&
         old.detect_indent == t->settings.detect_indent && old.guides == t->settings.guides &&
         old.word_wrap == t->settings.word_wrap && old.ruler == t->settings.ruler &&
         old.clang_tidy == t->settings.clang_tidy && old.format_on_save == t->settings.format_on_save &&
         old.square == t->settings.square && g_strcmp0(old.font, t->settings.font) == 0 &&
         g_strcmp0(old.files_font, t->settings.files_font) == 0 &&
         g_strcmp0(old.color_theme, t->settings.color_theme) == 0;
  if (!same)
    settings_changed(t, old.clang_tidy != t->settings.clang_tidy);
  g_free(old.font);
  g_free(old.files_font);
  g_free(old.color_theme);
}

/* The editor's colors changed: every editor gets the scheme read again. */
static void reload_colors(App *t)
{
  int n = tab_count(t);

  editor_view_reload_colors();
  for (int i = 0; i < n; i++)
    if (tab_nth(t, i)->kind == TAB_EDITOR)
      editor_view_apply_settings(tab_nth(t, i));
}

static gboolean reload(gpointer data)
{
  UserFile which = GPOINTER_TO_INT(data);

  reload_ids[which] = 0;
  if (which == USER_SETTINGS)
    reload_settings(app);
  else if (which == USER_THEME)
    load_user_theme();
  else
    reload_colors(app);
  return G_SOURCE_REMOVE;
}

static void on_changed(GFileMonitor *monitor G_GNUC_UNUSED, GFile *file G_GNUC_UNUSED, GFile *other G_GNUC_UNUSED,
                       GFileMonitorEvent event, gpointer data)
{
  UserFile which = GPOINTER_TO_INT(data);

  if (event == G_FILE_MONITOR_EVENT_ATTRIBUTE_CHANGED || event == G_FILE_MONITOR_EVENT_PRE_UNMOUNT)
    return;
  if (reload_ids[which])
    g_source_remove(reload_ids[which]);
  reload_ids[which] = g_timeout_add(RELOAD_MS, reload, data);
}

/* Watches which (again: its folder may have been made since). */
static void watch(UserFile which)
{
  char *path = path_of(which);
  GFile *file = g_file_new_for_path(path);

  g_clear_object(&monitors[which]);
  monitors[which] = g_file_monitor_file(file, G_FILE_MONITOR_NONE, NULL, NULL);
  if (monitors[which])
    g_signal_connect(monitors[which], "changed", G_CALLBACK(on_changed), GINT_TO_POINTER(which));
  g_object_unref(file);
  g_free(path);
}

void watch_user_files(App *t)
{
  app = t;
  for (int i = 0; i < USER_FILES; i++)
    watch(i);
}

void open_settings_file(App *t)
{
  char *path = settings_file();

  if (!find_tab(t, TAB_EDITOR, path)) /* every setting there, with its comment (keeps the rest) */
    session_save_settings(t);
  watch(USER_SETTINGS);
  open_file(t, path);
  g_free(path);
}

void open_user_theme(App *t)
{
  char *path = user_theme_file(), *dir = g_path_get_dirname(path);

  if (!g_file_test(path, G_FILE_TEST_EXISTS)) {
    g_mkdir_with_parents(dir, 0700);
    g_file_set_contents(path, theme_template, -1, NULL);
  }
  watch(USER_THEME);
  open_file(t, path);
  g_free(dir);
  g_free(path);
}

/* The user's copy was called dark-modern.xml before 0.3, its scheme t-code-dark-modern: it moves to
 * the new name, with the new id (else the editors would not find it). */
void move_old_colors(void)
{
  char *path = user_colors_file(), *dir = g_path_get_dirname(path);
  char *old = g_build_filename(dir, "dark-modern.xml", NULL), *text = NULL;

  if (!g_file_test(path, G_FILE_TEST_EXISTS) && g_file_get_contents(old, &text, NULL, NULL)) {
    GString *now = g_string_new(text);
    g_string_replace(now, "id=\"t-code-dark-modern\"", "id=\"t-code-dark-classic\"", 1);
    if (g_file_set_contents(path, now->str, -1, NULL))
      g_remove(old);
    g_string_free(now, TRUE);
  }
  g_free(text);
  g_free(old);
  g_free(dir);
  g_free(path);
}

void open_editor_colors(App *t)
{
  char *path = user_colors_file(), *dir = g_path_get_dirname(path);
  char *ours = g_build_filename(t->paths.styles, "dark-classic.xml", NULL), *text = NULL;

  if (!g_file_test(path, G_FILE_TEST_EXISTS) && g_file_get_contents(ours, &text, NULL, NULL)) {
    const char *after = strstr(text, "?>"); /* below the XML declaration */
    char *head = g_strndup(text, after ? after + 2 - text : 0);
    char *copy = g_strconcat(head,
                             "\n<!--\n  Your copy of t-code's editor colors, used instead of its own: saving it "
                             "applies it at once;\n  delete it to go back. Each style names a GtkSourceView "
                             "style (def:comment, c:keyword, ...)\n  and gives it a foreground, background, "
                             "bold or italic.\n-->",
                             after ? after + 2 : text, NULL);
    g_mkdir_with_parents(dir, 0700);
    g_file_set_contents(path, copy, -1, NULL);
    g_free(copy);
    g_free(head);
    reload_colors(t);
  }
  watch(USER_COLORS);
  open_file(t, path);
  g_free(text);
  g_free(ours);
  g_free(dir);
  g_free(path);
}
