/*
 * Terminals: their look, starting programs in them, and the shell pane, which is always there. In
 * the shell, Ctrl+click on a "file.c:12:5" location (compiler output) opens the file at that line.
 */
#define _DEFAULT_SOURCE /* kill() under -std=c23 with musl */
#include "window/terminal.h"

#include "editor/editor.h"
#include "editor/editorview.h"
#include "files/files.h"
#include "window/window.h"
#include "window/palette.h"

#include <signal.h>
#include <string.h>
#include <unistd.h>

void set_terminal_font(App *t, VteTerminal *term, const char *font)
{
  PangoFontDescription *desc = pango_font_description_from_string(font ? font : DEFAULT_FONT);
  const char *family = pango_font_description_get_family(desc);
  /* a font that is not installed falls back to monospace, not to fontconfig's default (proportional) */
  char *families = g_strconcat(family ? family : "monospace", ",monospace", NULL);

  pango_font_description_set_family(desc, families);
  vte_terminal_set_font(term, desc);
  vte_terminal_set_font_scale(term, t->settings.font_scale);
  pango_font_description_free(desc);
  g_free(families);
}

/* Gives the terminals, the editors and the Files panel the fonts the settings choose for them. */
void apply_fonts(App *t)
{
  set_terminal_font(t, t->shell.term, t->settings.font);
  if (t->debug.term)
    set_terminal_font(t, t->debug.term, t->settings.font);
  editor_view_set_font(t);
  files_apply_font(t);
}

/* The color theme's text, background and 16 colors. */
static void color_terminal(VteTerminal *term)
{
  vte_terminal_set_colors(term, palette_rgba(PAL_FG), palette_rgba(PAL_PANEL), palette_terminal(), 16);
}

void terminal_apply_colors(App *t)
{
  if (t->shell.term)
    color_terminal(t->shell.term);
  if (t->debug.term)
    color_terminal(t->debug.term);
}

/* A terminal in the editor font. */
/* clang-format off */
static void term_copy(GSimpleAction *a G_GNUC_UNUSED, GVariant *p G_GNUC_UNUSED, gpointer term) { vte_terminal_copy_clipboard_format(term, VTE_FORMAT_TEXT); }
static void term_paste(GSimpleAction *a G_GNUC_UNUSED, GVariant *p G_GNUC_UNUSED, gpointer term) { vte_terminal_paste_clipboard(term); }
static void term_select_all(GSimpleAction *a G_GNUC_UNUSED, GVariant *p G_GNUC_UNUSED, gpointer term) { vte_terminal_select_all(term); }
/* clang-format on */

/* A shell (a prompt in it) is cleared by Ctrl+L, which draws the prompt again; a program's output
 * is wiped with its scrollback. */
static void term_clear(GSimpleAction *a G_GNUC_UNUSED, GVariant *p G_GNUC_UNUSED, gpointer term)
{
  if (g_object_get_data(G_OBJECT(term), "t-code-shell"))
    vte_terminal_feed_child(term, "\x0c", 1);
  else
    vte_terminal_reset(term, TRUE, TRUE);
}

/* The right-click menu is the terminal's own (a popover put on it by hand came up small and
 * scrolling): items are actions of its "term" group. */
static void add_terminal_menu(VteTerminal *term)
{
  static const GActionEntry entries[] = {
    { .name = "copy", .activate = term_copy },
    { .name = "paste", .activate = term_paste },
    { .name = "select-all", .activate = term_select_all },
    { .name = "clear", .activate = term_clear },
  };
  GSimpleActionGroup *group = g_simple_action_group_new();
  GMenu *menu = g_menu_new(), *edit = g_menu_new(), *rest = g_menu_new();

  g_action_map_add_action_entries(G_ACTION_MAP(group), entries, G_N_ELEMENTS(entries), term);
  gtk_widget_insert_action_group(GTK_WIDGET(term), "term", G_ACTION_GROUP(group));
  g_menu_append(edit, "Copy", "term.copy");
  g_menu_append(edit, "Paste", "term.paste");
  g_menu_append(rest, "Select All", "term.select-all");
  g_menu_append(rest, "Clear", "term.clear");
  g_menu_append_section(menu, NULL, G_MENU_MODEL(edit));
  g_menu_append_section(menu, NULL, G_MENU_MODEL(rest));
  vte_terminal_set_context_menu_model(term, G_MENU_MODEL(menu));
  g_object_unref(edit);
  g_object_unref(rest);
  g_object_unref(menu);
  g_object_unref(group);
}

VteTerminal *new_terminal(App *t)
{
  VteTerminal *term = VTE_TERMINAL(vte_terminal_new());

  set_terminal_font(t, term, t->settings.font);
  color_terminal(term);
  vte_terminal_set_scrollback_lines(term, 10000);
  vte_terminal_set_mouse_autohide(term, TRUE);
  gtk_widget_set_hexpand(GTK_WIDGET(term), TRUE);
  gtk_widget_set_vexpand(GTK_WIDGET(term), TRUE);
  add_terminal_menu(term);
  return term;
}

/* The program started (its pid goes to *data), or the error shows in the terminal. */
static void spawn_done(VteTerminal *term, GPid pid, GError *error, gpointer data)
{
  GPid *out = data;

  if (error) {
    char *msg = g_strdup_printf("\r\nt-code: %s\r\n", error->message);
    vte_terminal_feed(term, msg, -1);
    g_free(msg);
    return;
  }
  if (out)
    *out = pid;
}

/* Runs argv in term, in the project root, with $TCODE_SOCKET and extra_env ("NAME=value", may be
 * NULL) set; the child's pid goes to *pid. */
void spawn(App *t, VteTerminal *term, char **argv, char **extra_env, GPid *pid)
{
  char **env = g_get_environ();

  env = g_environ_setenv(env, "TCODE_SOCKET", t->paths.socket, TRUE);
  for (int i = 0; extra_env && extra_env[i]; i++) {
    char **pair = g_strsplit(extra_env[i], "=", 2);
    if (pair[0] && pair[1])
      env = g_environ_setenv(env, pair[0], pair[1], TRUE);
    g_strfreev(pair);
  }
  vte_terminal_spawn_async(term, VTE_PTY_DEFAULT, t->project.root, argv, env, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, -1,
                           NULL, spawn_done, pid);
  g_strfreev(env);
}

/* The shell pane ----------------------------------------------------------------------------- */

/* $SHELL first, like most terminals; then the login shell from the user database. */
void start_shell_pane(App *t)
{
  const char *env = g_getenv("SHELL");
  char *shell = env && *env ? g_strdup(env) : vte_get_user_shell();
  char *argv[] = { shell ? shell : "/bin/sh", NULL };

  spawn(t, t->shell.term, argv, NULL, &t->shell.pid);
  g_free(shell);
}

/* It comes back when it exits, in the current project (see on_shell_exited). */
void restart_shell_pane(App *t)
{
  if (t->shell.pid > 0)
    kill(t->shell.pid, SIGHUP);
}

/* The shell exited (or restart_shell_pane() hung it up): it starts again, in the current project. */
static void on_shell_exited(VteTerminal *term, int status G_GNUC_UNUSED, App *t)
{
  if (t->closing)
    return;
  t->shell.pid = 0; /* gone: nothing may signal its pid until the new one starts */
  vte_terminal_reset(term, TRUE, TRUE);
  start_shell_pane(t);
}

/* The process group in the foreground of the shell pane: the shell itself when it waits at its
 * prompt, else the program it runs. 0 when unknown. */
static pid_t shell_foreground(App *t)
{
  VtePty *pty = vte_terminal_get_pty(t->shell.term);
  pid_t group = pty ? tcgetpgrp(vte_pty_get_fd(pty)) : -1;

  return group > 0 ? group : 0;
}

typedef struct {
  App *app;
  char *command;
  int tries;
} ShellRun;

static ShellRun *waiting; /* the command that waits for the shell's prompt */

static void type_in_shell(App *t, const char *command)
{
  char *line = g_strdup_printf("\x15%s\r", command);

  vte_terminal_feed_child(t->shell.term, line, -1);
  g_free(line);
}

/* A program in the shell did not stop on Ctrl+C: says which, instead of typing into it. */
static void show_shell_busy(App *t, pid_t group)
{
  char *path = g_strdup_printf("/proc/%d/comm", group), *name = NULL;
  GtkAlertDialog *dialog = gtk_alert_dialog_new("The terminal is busy");
  char *detail;

  g_file_get_contents(path, &name, NULL, NULL);
  detail = g_strdup_printf("%s is still running in the terminal and did not stop on Ctrl+C. "
                           "Close it, then try again.",
                           name ? g_strstrip(name) : "A program");
  gtk_alert_dialog_set_detail(dialog, detail);
  gtk_alert_dialog_show(dialog, t->window);
  g_object_unref(dialog);
  g_free(detail);
  g_free(name);
  g_free(path);
}

/* Waits for an interrupted program to give the prompt back (2 seconds at most), then types the
 * command. */
static gboolean run_when_idle(gpointer data)
{
  ShellRun *run = data;
  App *t = run->app;
  pid_t group = shell_foreground(t);
  gboolean busy = group && group != t->shell.pid;

  if (busy && !t->closing && ++run->tries < 40)
    return G_SOURCE_CONTINUE;
  if (busy && !t->closing)
    show_shell_busy(t, group);
  else if (!t->closing)
    type_in_shell(t, run->command);
  waiting = NULL;
  g_free(run->command);
  g_free(run);
  return G_SOURCE_REMOVE;
}

/* Types command into the shell pane and runs it. Ctrl+U first drops a half-typed line. When a
 * program runs in the shell (the last Run, gdb), it gets Ctrl+C first, so the command does not go
 * to its input. */
void run_in_shell(App *t, const char *command)
{
  pid_t group = shell_foreground(t);
  gboolean busy = group && group != t->shell.pid;

  set_pane_visible(t, GTK_WIDGET(t->shell.term), TRUE);
  gtk_widget_grab_focus(GTK_WIDGET(t->shell.term));
  if (waiting) {
    /* Build or Run again while the last one waits: one command runs, the newest. */
    g_free(waiting->command);
    waiting->command = g_strdup(command);
    waiting->tries = 0;
    if (busy)
      kill(-group, SIGINT);
    return;
  }
  if (!busy) {
    type_in_shell(t, command);
    return;
  }
  kill(-group, SIGINT);
  waiting = g_new0(ShellRun, 1);
  waiting->app = t;
  waiting->command = g_strdup(command);
  g_timeout_add(50, run_when_idle, waiting);
}

void cd_in_shell(App *t, const char *dir)
{
  char *quoted = g_shell_quote(dir);
  char *command = g_strdup_printf(" cd -- %s", quoted); /* the leading space keeps it out of history */

  run_in_shell(t, command);
  g_free(command);
  g_free(quoted);
}

/* Clickable compiler messages ---------------------------------------------------------------- */

#define PCRE2_MULTILINE 0x00000400u /* VTE wants its match patterns multiline */

/* path/to/file.ext:line or path/to/file.ext:line:column */
static const char *const location_pattern = "(?:[A-Za-z0-9_.~+-]*/)*[A-Za-z0-9_.+-]+\\.[A-Za-z0-9]+:[0-9]+(?::[0-9]+)?";

/* Where the shell is now (bash reports it on Fedora), or else the project root. */
static char *shell_directory(App *t)
{
  const char *uri = vte_terminal_get_termprop_string(t->shell.term, VTE_TERMPROP_CURRENT_DIRECTORY_URI, NULL);
  char *dir = uri ? g_filename_from_uri(uri, NULL, NULL) : NULL;

  return dir ? dir : g_strdup(t->project.root);
}

/* Opens a "file.c:12:5" location from the shell (relative to the shell's folder or the project). */
static void open_location(App *t, const char *location)
{
  const char *colon = strchr(location, ':');
  char *name = g_strndup(location, colon - location);
  int line = 0, column = 0;
  char *dir = shell_directory(t);
  const char *const bases[] = { dir, t->project.root };
  char *path = NULL;

  sscanf(colon + 1, "%d:%d", &line, &column);
  if (g_path_is_absolute(name)) {
    path = g_strdup(name);
  } else {
    for (size_t i = 0; i < G_N_ELEMENTS(bases) && !path; i++) {
      char *candidate = g_build_filename(bases[i], name, NULL);
      if (g_file_test(candidate, G_FILE_TEST_IS_REGULAR))
        path = g_canonicalize_filename(candidate, NULL);
      g_free(candidate);
    }
  }
  if (path && g_file_test(path, G_FILE_TEST_IS_REGULAR))
    open_file_at(t, path, line, column);
  g_free(path);
  g_free(dir);
  g_free(name);
}

/* Ctrl+Click on a location in the shell opens it. */
static void on_shell_click(GtkGestureClick *gesture, int n_press G_GNUC_UNUSED, double x, double y, App *t)
{
  GdkModifierType state = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(gesture));
  char *match;

  if (!(state & GDK_CONTROL_MASK))
    return;
  match = vte_terminal_check_match_at(t->shell.term, x, y, NULL);
  if (match) {
    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
    open_location(t, match);
    g_free(match);
  }
}

/* Makes "file.ext:line:col" in the shell clickable (with Ctrl). */
static void add_location_links(App *t)
{
  GError *error = NULL;
  VteRegex *regex = vte_regex_new_for_match(location_pattern, -1, PCRE2_MULTILINE, &error);
  GtkGesture *click = gtk_gesture_click_new();

  if (!regex) {
    g_warning("t-code: %s", error->message);
    g_error_free(error);
    return;
  }
  int tag = vte_terminal_match_add_regex(t->shell.term, regex, 0);
  vte_terminal_match_set_cursor_name(t->shell.term, tag, "pointer");
  vte_regex_unref(regex);

  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_PRIMARY);
  gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(click), GTK_PHASE_CAPTURE);
  g_signal_connect(click, "pressed", G_CALLBACK(on_shell_click), t);
  gtk_widget_add_controller(GTK_WIDGET(t->shell.term), GTK_EVENT_CONTROLLER(click));
}

void create_shell_pane(App *t)
{
  t->shell.term = new_terminal(t);
  g_object_set_data(G_OBJECT(t->shell.term), "t-code-shell", GINT_TO_POINTER(TRUE)); /* Clear: Ctrl+L */
  g_signal_connect(t->shell.term, "child-exited", G_CALLBACK(on_shell_exited), t);
  add_location_links(t);
}
