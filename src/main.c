/*
 * t-code's start and end: the GTK application, the command line (t-code [dir] [file...],
 * --version, --send), and quitting cleanly on SIGTERM / SIGINT. The window itself is built by
 * build_window() (window/window.c); see app.h for how the source folders divide the work.
 */
#define _DEFAULT_SOURCE /* kill() under -std=c23 with musl */
#include "app.h"

#include "debug/debugger.h"
#include "editor/editor.h"
#include "files/files.h"
#include "git/compare.h"
#include "lsp/client.h"
#include "ipc/socket.h"
#include "project/project.h"
#include "project/session.h"
#include "window/window.h"
#include "plugins/plugins.h"

#include <gio/gunixsocketaddress.h>
#include <glib-unix.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#ifndef TCODE_VERSION /* set by the Makefile */
#define TCODE_VERSION "unknown"
#endif

/* Application -------------------------------------------------------------------------------- */

/* --new-window (the desktop file's "New Window" action, e.g. a middle click on the dock): an empty
 * window that asks for its folder, instead of the last project, which may be open in another one. */
static gboolean new_window;

/* TRUE when another t-code window is already running (has a live socket in XDG_RUNTIME_DIR). */
static gboolean already_running(void)
{
  GDir *dir = g_dir_open(g_get_user_runtime_dir(), 0, NULL);
  const char *name;
  gboolean found = FALSE;

  while (dir && !found && (name = g_dir_read_name(dir))) {
    int pid = 0;
    if (sscanf(name, "t-code-%d.sock", &pid) == 1 && pid > 0 && pid != getpid() && kill(pid, 0) == 0)
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
  if (recent && recent[0] && g_file_test(recent[0], G_FILE_TEST_IS_DIR)) { /* still there */
    g_free(t->project.root);
    t->project.root = g_strdup(recent[0]);
  }
  g_strfreev(recent);
}

/* "activate": started without folder/file arguments. With another window open, a new one from the
 * app menu (or --new-window) asks for its folder: the last project is probably open over there, and
 * two editors on one file overwrite each other. Otherwise the current folder, or the last project
 * (see above), comes back with its tabs. */
static void on_activate(GApplication *app, App *t)
{
  if (already_running() && (new_window || strcmp(t->project.root, g_get_home_dir()) == 0)) {
    build_window(t, GTK_APPLICATION(app));
    choose_folder(t);
    return;
  }
  reopen_last_project(t);
  build_window(t, GTK_APPLICATION(app));
  session_restore_tabs(t);
}

/* t-code [dir] [file...]: a directory becomes the project root, files open as tabs.
 * With no files, the tabs from the last session in that directory come back. */
static void on_open(GApplication *app, GFile **files, int n, const char *hint G_GNUC_UNUSED, App *t)
{
  gboolean any_file = FALSE;

  for (int i = 0; i < n; i++) {
    if (g_file_query_file_type(files[i], G_FILE_QUERY_INFO_NONE, NULL) == G_FILE_TYPE_DIRECTORY) {
      g_free(t->project.root);
      t->project.root = g_file_get_path(files[i]);
    }
  }
  build_window(t, GTK_APPLICATION(app));
  for (int i = 0; i < n; i++) {
    if (g_file_query_file_type(files[i], G_FILE_QUERY_INFO_NONE, NULL) != G_FILE_TYPE_DIRECTORY) {
      char *path = g_file_get_path(files[i]);
      open_file(t, path);
      g_free(path);
      any_file = TRUE;
    }
  }
  if (!any_file)
    session_restore_tabs(t);
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
}

/* SIGTERM / SIGINT: save the session and quit normally, so the socket file is removed. The window
 * goes like on a normal close: its tabs stop their editors, the terminals hang up on theirs. */
static gboolean on_signal(gpointer data)
{
  App *t = data;

  if (t->window && !t->closing) { /* closing: the window is gone already */
    session_save(t);
    t->closing = TRUE;
    if (t->editors.second)
      gtk_window_destroy(t->editors.second);
    gtk_window_destroy(t->window);
  }
  t->closing = TRUE;
  g_application_quit(g_application_get_default());
  return G_SOURCE_REMOVE;
}

/* --send "<command> <arg>": one request to the t-code whose shell this runs in ($TCODE_SOCKET), ended
 * with a NUL (a path may hold a line break). The build for debugging ends with it (project/run.c). */
static int send_request(const char *request)
{
  const char *path = g_getenv("TCODE_SOCKET");
  GSocketClient *client = g_socket_client_new();
  GSocketAddress *address = path ? g_unix_socket_address_new(path) : NULL;
  GError *error = NULL;
  GSocketConnection *conn =
      address ? g_socket_client_connect(client, G_SOCKET_CONNECTABLE(address), NULL, &error) : NULL;
  gboolean ok = conn && g_output_stream_write_all(g_io_stream_get_output_stream(G_IO_STREAM(conn)), request,
                                                  strlen(request) + 1, NULL, NULL, &error);

  if (!ok)
    g_printerr("t-code --send: %s\n", error ? error->message : "TCODE_SOCKET is not set (run it in t-code's shell)");
  g_clear_error(&error);
  g_clear_object(&conn);
  g_clear_object(&address);
  g_object_unref(client);
  return ok ? 0 : 1;
}

/* --version and --send: answered before GTK starts, so they work without a display. */
static int on_local_options(GApplication *app G_GNUC_UNUSED, GVariantDict *options, App *t G_GNUC_UNUSED)
{
  const char *request = NULL;

  if (g_variant_dict_contains(options, "version")) {
    g_print("t-code %s\n", TCODE_VERSION);
    return 0;
  }
  if (g_variant_dict_lookup(options, "send", "&s", &request))
    return send_request(request);
  new_window = g_variant_dict_contains(options, "new-window");
  return -1;
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

/* Everything happens inside g_application_run(): it parses the command line, calls on_activate or
 * on_open, runs the main loop until the window closes, then calls on_shutdown. */
int main(int argc, char **argv)
{
  App t = { .project.root = g_get_current_dir(), .font_scale = 1.0, .preview_scale = 1.0 };
  GtkApplication *app = gtk_application_new("io.github.tcode", G_APPLICATION_HANDLES_OPEN | G_APPLICATION_NON_UNIQUE);
  int status;

  g_application_add_main_option(G_APPLICATION(app), "version", 'v', G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE,
                                "Show the version and exit", NULL);
  g_application_add_main_option(G_APPLICATION(app), "new-window", 0, G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE,
                                "Open an empty window that asks for a folder", NULL);
  g_application_add_main_option(G_APPLICATION(app), "send", 0, G_OPTION_FLAG_NONE, G_OPTION_ARG_STRING,
                                "Send a request to the t-code this runs in", "\"COMMAND ARG\"");
  static const GActionEntry app_actions[] = { { .name = "new-window", .activate = on_new_window_action } };
  g_action_map_add_action_entries(G_ACTION_MAP(app), app_actions, G_N_ELEMENTS(app_actions), NULL);
  g_signal_connect(app, "handle-local-options", G_CALLBACK(on_local_options), &t);
  session_load_settings(&t);
  g_signal_connect(app, "activate", G_CALLBACK(on_activate), &t);
  g_signal_connect(app, "open", G_CALLBACK(on_open), &t);
  g_signal_connect(app, "shutdown", G_CALLBACK(on_shutdown), &t);
  g_unix_signal_add(SIGTERM, on_signal, &t);
  g_unix_signal_add(SIGINT, on_signal, &t);
  status = g_application_run(G_APPLICATION(app), argc, argv);

  g_object_unref(app);
  g_clear_object(&t.service);
  plugins_stop(&t);
  g_free(t.paths.socket);
  g_free(t.paths.styles);
  g_free(t.paths.icons);
  g_free(t.paths.temp);
  g_free(t.debug.disassembly);
  g_free(t.project.git_top);
  g_free(t.project.root);
  g_free(t.settings.font);
  g_free(t.settings.files_font);
  g_free(t.settings.color_theme);
  return status;
}
