/*
 * The GTK application (see application.h): which window opens, and quitting cleanly on SIGTERM /
 * SIGINT.
 */
#define _DEFAULT_SOURCE /* kill() under -std=c23 with musl */
#include "application/application.h"

#include "application/identity.h"
#include "application/log.h"
#include "application/shellenv.h"
#include "debug/debugger.h"
#include "dialogs/welcome.h"
#include "editor/editor.h"
#include "files/files.h"
#include "git/compare.h"
#include "ipc/socket.h"
#include "lsp/client/client.h"
#include "plugins/plugins.h"
#include "project/session.h"
#include "window/terminal.h"
#include "window/theme.h"
#include "window/window.h"

#include <glib-unix.h>
#ifdef __GLIBC__
#include <malloc.h>
#endif
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* TRUE when another t-code window is already running (has a live socket in XDG_RUNTIME_DIR). */
static gboolean already_running(void)
{
  GDir *dir = g_dir_open(g_get_user_runtime_dir(), 0, NULL);
  const char *name;
  gboolean found = FALSE;

  while (dir && !found && (name = g_dir_read_name(dir))) {
    int pid = 0;
    if (sscanf(name, TCODE_NAME "-%d.sock", &pid) == 1 && pid > 0 && pid != getpid() && kill(pid, 0) == 0)
      found = TRUE;
  }
  if (dir)
    g_dir_close(dir);
  return found;
}

/* Started with no folder from the home directory (the app menu does that): reopen the last project
 * instead. Started elsewhere (t-code in a terminal), the current directory is meant. */
static void reopen_last_project(App *t)
{
  char **recent;

  if (!t->settings.reopen_last) /* turned off in Settings */
    return;
  if (strcmp(t->project.root, g_get_home_dir()) != 0) /* started in a folder: that one is meant */
    return;
  recent = session_recent();
  if (recent && recent[0] && g_file_test(recent[0], G_FILE_TEST_IS_DIR)) /* still there */
    g_set_str(&t->project.root, recent[0]);
  g_strfreev(recent);
}

/* The welcome window picked a folder: the main window opens on it, with its tabs from last time. */
static void on_welcome_chosen(const char *folder, gpointer data)
{
  App *t = data;

  g_set_str(&t->project.root, folder);
  build_window(t, GTK_APPLICATION(g_application_get_default()));
  session_restore_tabs(t);
}

/* "activate": started without folder/file arguments. With another window open, a new one from the
 * app menu (or --new-window) asks for its folder in the welcome window: the last project is probably
 * open over there, and two editors on one file overwrite each other. Otherwise the current folder,
 * or the last project (see above), comes back with its tabs; with neither (the first start), the
 * welcome window asks too. */
#define TRIM_LATER_SECONDS 5 /* files load and color themselves in the background meanwhile */

static guint trim_id;

static gboolean trim_now(gpointer data G_GNUC_UNUSED)
{
  trim_id = 0;
#ifdef __GLIBC__
  malloc_trim(0);
#endif
  return G_SOURCE_REMOVE;
}

void application_trim_later(void)
{
  g_clear_handle_id(&trim_id, g_source_remove);
  trim_id = g_timeout_add_seconds(TRIM_LATER_SECONDS, trim_now, NULL);
}

static void on_activate(GApplication *app, App *t)
{
  gboolean second = already_running() && (cli_new_window() || strcmp(t->project.root, g_get_home_dir()) == 0);

  if (!second)
    reopen_last_project(t);
  if (second || strcmp(t->project.root, g_get_home_dir()) == 0) { /* no folder to open */
    char **recent = session_recent();
    load_theme(t);
    show_welcome(GTK_APPLICATION(app), (const char *const *)recent, on_welcome_chosen, t);
    g_strfreev(recent);
    return;
  }
  build_window(t, GTK_APPLICATION(app));
  session_restore_tabs(t);
}

/* t-code [dir] [file...]: a directory becomes the project root, files open as tabs.
 * With no files, the tabs from the last session in that directory come back. */
static void on_open(GApplication *app, GFile **files, int n, const char *hint G_GNUC_UNUSED, App *t)
{
  gboolean any_file = FALSE;

  for (int i = 0; i < n; i++)
    if (g_file_query_file_type(files[i], G_FILE_QUERY_INFO_NONE, NULL) == G_FILE_TYPE_DIRECTORY) {
      g_free(t->project.root);
      t->project.root = g_file_get_path(files[i]);
    }
  build_window(t, GTK_APPLICATION(app));
  for (int i = 0; i < n; i++)
    if (g_file_query_file_type(files[i], G_FILE_QUERY_INFO_NONE, NULL) != G_FILE_TYPE_DIRECTORY) {
      char *path = g_file_get_path(files[i]);
      open_file(t, path);
      g_free(path);
      any_file = TRUE;
    }
  if (!any_file)
    session_restore_tabs(t);
}

/* A widget's place for the log: its type, name and CSS classes, then its parents'. */
static char *widget_place(GtkWidget *widget)
{
  GString *place = g_string_new(NULL);

  for (int depth = 0; widget && depth < 6; widget = gtk_widget_get_parent(widget), depth++) {
    const char *type = G_OBJECT_TYPE_NAME(widget), *name = gtk_widget_get_name(widget);
    char **classes = gtk_widget_get_css_classes(widget);
    g_string_append_printf(place, "%s%s", depth ? " < " : "", type);
    if (name && strcmp(name, type) != 0)
      g_string_append_printf(place, "#%s", name);
    for (int i = 0; classes && classes[i]; i++)
      g_string_append_printf(place, ".%s", classes[i]);
    g_strfreev(classes);
  }
  return g_string_free(place, FALSE);
}

static void log_popovers_in(GtkWidget *widget, int *count)
{
  if (GTK_IS_POPOVER(widget) && gtk_widget_get_visible(widget)) {
    GdkSurface *surface = gtk_native_get_surface(GTK_NATIVE(widget));
    char *place = widget_place(gtk_widget_get_parent(widget));
    g_message("  %s%s, %s, its surface %s, on %s", G_OBJECT_TYPE_NAME(widget),
              gtk_popover_get_autohide(GTK_POPOVER(widget)) ? " (grabbing)" : "",
              gtk_widget_get_mapped(widget) ? "mapped" : "not mapped",
              surface && gdk_surface_get_mapped(surface) ? "shown" : "not shown", place);
    g_free(place);
    (*count)++;
  }
  for (GtkWidget *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child))
    log_popovers_in(child, count);
}

/* GDK or GTK warned about a popup or a grab (a window that then took no clicks came after "Tried
 * to map a grabbing popup with a non-top most parent"): the log gets the popovers open then. */
static void on_log_warning(const char *domain, const char *message)
{
  if ((g_strcmp0(domain, "Gdk") != 0 && g_strcmp0(domain, "Gtk") != 0) ||
      (!strstr(message, "popup") && !strstr(message, "grab")) || !g_main_context_is_owner(g_main_context_default()))
    return;
  GList *windows = gtk_window_list_toplevels();
  int count = 0;

  g_message("The popovers open at this warning:");
  for (GList *l = windows; l; l = l->next)
    log_popovers_in(l->data, &count);
  if (!count)
    g_message("  none");
  g_list_free(windows);
}

/* After cli.c's handle-local-options, which ends the emission for --version and --send: t-code
 * really starts, so the log file gets this run's line (before GTK opens the display, whose failure
 * is worth logging too). */
static int start_log(void)
{
  log_start(NULL);
  log_set_hook(on_log_warning);
  return -1; /* go on */
}

/* The login shell's environment came, or did not (see shellenv.h): the language servers it makes
 * findable start for the open tabs ("No LSP" goes), and the project's plugins, which waited for it,
 * start. Before the window is built nothing needs doing: it starts them itself. */
static void on_shell_env(gboolean changed, gpointer data)
{
  App *t = data;

  if (!t->window || t->closing)
    return;
  if (changed)
    lsp_open_installed(t);
  plugins_start_project(t);
}

/* "startup": this process really is t-code. Inside Flatpak and Snap the user's shell is not the
 * one their programs run with, and its folders are not there. */
static void on_startup(GApplication *app G_GNUC_UNUSED, App *t)
{
  const char *shell = g_getenv("SHELL");

  if (shell && *shell && !g_getenv("FLATPAK_ID") && !g_getenv("SNAP"))
    shell_env_load(shell, 5000, on_shell_env, t);
}

/* "shutdown": the window is gone. The sockets' files and the old file versions are removed, and the
 * language servers stop. */
static void on_shutdown(GApplication *app G_GNUC_UNUSED, App *t)
{
  t->closing = TRUE;
  debug_end(t);
  stop_socket(t);
  files_stop(t);
  lsp_stop(t);
  remove_temp_dir(t);
  plugins_stop(t);
  g_clear_pointer(&t->plugin_diagnostics, g_hash_table_unref);
}

static gboolean ending; /* the plugins' programs end, the window is hidden but still there */

static void quit_now(App *t)
{
  if (ending) {
    ending = FALSE;
    if (t->editors.second)
      gtk_window_destroy(t->editors.second);
    gtk_window_destroy(t->window);
  }
  g_application_quit(g_application_get_default());
}

/* SIGTERM / SIGINT: save the session and quit normally, so the socket file is removed. The window
 * goes like on a normal close: the plugins' programs in terminal tabs end first, its tabs stop their
 * editors, the terminals hang up on theirs. A second signal while they end quits at once. */
static gboolean on_signal(gpointer data)
{
  App *t = data;

  if (t->window && !t->closing) {
    session_save(t);
    t->closing = TRUE;
    ending = TRUE;
    end_programs(t, quit_now);
    return G_SOURCE_REMOVE;
  }
  t->closing = TRUE; /* closing: the window is gone already, unless its programs are still ending */
  quit_now(t);
  return G_SOURCE_REMOVE;
}

/* app.new-window, on the session bus: the docks (GNOME's, Dash to Dock) open a new window on a
 * middle click only when the running app exports it. Every window is its own process, so this
 * starts another t-code with --new-window from the home directory. */
static void on_new_window_action(GSimpleAction *action G_GNUC_UNUSED, GVariant *param G_GNUC_UNUSED,
                                 gpointer data G_GNUC_UNUSED)
{
  char *exe = g_file_read_link("/proc/self/exe", NULL);
  char *argv[] = { exe, "--new-window", NULL };
  GError *error = NULL;

  if (!exe || !g_spawn_async(g_get_home_dir(), argv, NULL, G_SPAWN_DEFAULT, NULL, NULL, NULL, &error))
    g_warning("t-code: cannot open a new window: %s", error ? error->message : "no /proc/self/exe");
  g_clear_error(&error);
  g_free(exe);
}

/* What is left of the App once the window and the services are gone. */
static void app_free(App *t)
{
  g_clear_object(&t->service);
  plugins_stop(t);
  g_clear_pointer(&t->plugin_diagnostics, g_hash_table_unref);
  g_free(t->paths.socket);
  g_free(t->paths.styles);
  g_free(t->paths.icons);
  g_free(t->paths.temp);
  g_free(t->debug.disassembly);
  g_free(t->project.repo_top);
  g_free(t->project.scm_top);
  g_free(t->project.root);
  g_free(t->settings.font);
  g_free(t->settings.files_font);
  g_free(t->settings.clangd.extra);
  g_free(t->settings.color_theme);
  g_strfreev(t->settings.divider_colors);
}

/* Everything happens inside g_application_run(): it parses the command line, calls on_activate or
 * on_open, runs the main loop until the window closes, then calls on_shutdown. */
int application_run(int argc, char **argv)
{
  static const GActionEntry actions[] = { { .name = "new-window", .activate = on_new_window_action } };
  App t = { .project.root = g_get_current_dir(), .settings.font_scale = 1.0, .settings.preview_scale = 1.0 };
  GtkApplication *app = gtk_application_new(TCODE_APP_ID, G_APPLICATION_HANDLES_OPEN | G_APPLICATION_NON_UNIQUE);
  int status;

  cli_add_options(G_APPLICATION(app));
  g_signal_connect(app, "handle-local-options", G_CALLBACK(start_log), NULL);
  g_action_map_add_action_entries(G_ACTION_MAP(app), actions, G_N_ELEMENTS(actions), NULL);
  session_load_settings(&t);
  g_signal_connect(app, "startup", G_CALLBACK(on_startup), &t);
  g_signal_connect(app, "activate", G_CALLBACK(on_activate), &t);
  g_signal_connect(app, "open", G_CALLBACK(on_open), &t);
  g_signal_connect(app, "shutdown", G_CALLBACK(on_shutdown), &t);
  g_unix_signal_add(SIGTERM, on_signal, &t);
  g_unix_signal_add(SIGINT, on_signal, &t);
  status = g_application_run(G_APPLICATION(app), argc, argv);
  g_object_unref(app);
  app_free(&t);
  log_stop();
  return status;
}
