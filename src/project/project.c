/*
 * Projects: the folder t-code works in. Open Folder (Ctrl+Shift+O, title bar button), the recent
 * folders menu, and "Open Folder as Project" in the Files panel all switch it. Switching remembers
 * and closes the current project's tabs, shows the new folder in the Files panel, restarts the
 * shell there, and brings back that folder's tabs from last time.
 */
#include "project/project.h"

#include "debug/debugger.h"
#include "dialogs/about.h"
#include "dialogs/keybinds.h"
#include "editor/recovery.h"
#include "editor/tabs.h"
#include "files/files.h"
#include "git/gitstatus.h"
#include "git/scm/scm.h"
#include "lsp/client.h"
#include "project/session.h"
#include "search/search.h"
#include "window/statusbar.h"
#include "window/terminal.h"

#ifdef __GLIBC__
#include <malloc.h>
#endif
#include <string.h>

/* Unsaved changes ---------------------------------------------------------------------------- */

typedef struct {
  App *app;
  ConfirmedFunc then;
  char *arg;
} Confirm;

/* The user answered unless_unsaved()'s question. */
static void on_confirm_answer(GObject *dialog, GAsyncResult *result, gpointer data)
{
  Confirm *c = data;

  if (gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(dialog), result, NULL) == 1) {
    recovery_forget_unsaved(c->app); /* their unsaved changes are given up on purpose */
    c->then(c->app, c->arg);
  }
  g_free(c->arg);
  g_free(c);
}

/* Asks before throwing away unsaved changes; runs then(t, arg) on "yes", or right away when
 * nothing is unsaved. */
void unless_unsaved(App *t, const char *question, const char *button, ConfirmedFunc then, const char *arg)
{
  if (!any_unsaved(t)) {
    then(t, arg);
    return;
  }

  GtkAlertDialog *dialog = gtk_alert_dialog_new("%s", question);
  const char *buttons[] = { "Cancel", button, NULL };
  Confirm *c = g_new0(Confirm, 1);

  c->app = t;
  c->then = then;
  c->arg = g_strdup(arg);
  gtk_alert_dialog_set_detail(dialog, "Some files have changes that are not saved yet.");
  gtk_alert_dialog_set_buttons(dialog, buttons);
  gtk_alert_dialog_set_cancel_button(dialog, 0);
  gtk_alert_dialog_set_default_button(dialog, 1);
  gtk_alert_dialog_choose(dialog, t->window, NULL, on_confirm_answer, c);
  g_object_unref(dialog);
}

/* Switching ---------------------------------------------------------------------------------- */

/* The folder's name. */
void update_title(App *t)
{
  char *name = g_path_get_basename(t->project.root);

  gtk_window_set_title(t->window, name);
  g_free(name);
}

/* path with the home folder written as "~", for the recent folders menu. */
static char *home_relative(const char *path)
{
  const char *home = g_get_home_dir();
  size_t n = strlen(home);

  if (strncmp(path, home, n) == 0 && (path[n] == '/' || path[n] == '\0'))
    return g_strconcat("~", path + n, NULL);
  return g_strdup(path);
}

void rebuild_recent_menu(App *t)
{
  char **recent = session_recent();
  int shown = 0;

  g_menu_remove_all(t->recent_menu);
  for (int i = 0; recent && recent[i]; i++) {
    if (strcmp(recent[i], t->project.root) == 0 || !g_file_test(recent[i], G_FILE_TEST_IS_DIR))
      continue;
    char *label = home_relative(recent[i]);
    GMenuItem *item = g_menu_item_new(label, NULL);
    g_menu_item_set_action_and_target_value(item, "win.open-recent", g_variant_new_string(recent[i]));
    g_menu_append_item(t->recent_menu, item);
    g_object_unref(item);
    g_free(label);
    shown++;
  }
  if (!shown)
    g_menu_append(t->recent_menu, "No recent folders", "win.none");
  g_strfreev(recent);
}

/* Switches to root: the current tabs are saved and closed, the Files panel shows root, the shell
 * restarts there, the panels empty, and root's tabs from last time come back. */
static void switch_project(App *t, const char *root)
{
  if (strcmp(root, t->project.root) == 0)
    return;
  session_save(t);
  debug_end(t); /* the program is the old project's */
  close_all_tabs(t);
  set_split(t, FALSE);
  lsp_stop(t); /* stop every server for the old project before loading the new one */
#ifdef __GLIBC__
  malloc_trim(0); /* the old project's tabs are gone: give their memory back */
#endif

  g_free(t->project.root);
  t->project.root = g_strdup(root);
  session_load_project(t);
  update_title(t);
  session_add_recent(t);
  rebuild_recent_menu(t);
  restart_shell_pane(t);
  scm_reset();
  search_reset();
  watch_git(t); /* t->project.git_top for the Files panel's git colors */
  files_set_root(t);

  session_restore_tabs(t);
  update_editor_area(t);
  update_status(t);
  if (!current_tab(t))
    files_focus(t);
}

void open_project(App *t, const char *root)
{
  unless_unsaved(t, "Open another folder?", "Open", switch_project, root);
}

/* Choosing a folder -------------------------------------------------------------------------- */

static void on_folder_chosen(GObject *source, GAsyncResult *result, gpointer data)
{
  GFile *folder = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(source), result, NULL);

  if (!folder)
    return;
  char *path = g_file_get_path(folder);
  if (path)
    open_project(data, path);
  g_free(path);
  g_object_unref(folder);
}

void choose_folder(App *t)
{
  GtkFileDialog *dialog = gtk_file_dialog_new();
  GFile *start = g_file_new_for_path(t->project.root);

  gtk_file_dialog_set_title(dialog, "Open Folder");
  gtk_file_dialog_set_initial_folder(dialog, start);
  gtk_file_dialog_select_folder(dialog, t->window, NULL, on_folder_chosen, t);
  g_object_unref(start);
  g_object_unref(dialog);
}

/* The window actions, for the title bar's buttons and menus. */
static void on_open_folder_action(GSimpleAction *action G_GNUC_UNUSED, GVariant *param G_GNUC_UNUSED, gpointer t)
{
  choose_folder(t);
}

static void on_keybinds_action(GSimpleAction *action G_GNUC_UNUSED, GVariant *param G_GNUC_UNUSED, gpointer t)
{
  show_keybinds(t);
}

static void on_about_action(GSimpleAction *action G_GNUC_UNUSED, GVariant *param G_GNUC_UNUSED, gpointer t)
{
  show_about(t);
}

static void on_open_recent_action(GSimpleAction *action G_GNUC_UNUSED, GVariant *param, gpointer t)
{
  open_project(t, g_variant_get_string(param, NULL));
}

/* win.open-folder, win.open-recent, win.keybinds and win.about, used by the title bar buttons and menus; and the recent menu. */
void add_project_actions(App *t)
{
  static const GActionEntry actions[] = {
    { .name = "open-folder", .activate = on_open_folder_action },
    { .name = "keybinds", .activate = on_keybinds_action },
    { .name = "about", .activate = on_about_action },
    { .name = "open-recent", .activate = on_open_recent_action, .parameter_type = "s" },
  };

  g_action_map_add_action_entries(G_ACTION_MAP(t->window), actions, G_N_ELEMENTS(actions), t);
  t->recent_menu = g_menu_new();
}
