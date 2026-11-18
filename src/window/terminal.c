/*
 * Terminals: their look, starting programs in them, and the Terminal panel: the shells as tabs, the
 * first one always there. In them, Ctrl+click on a "file.c:12:5" location (compiler output) opens
 * the file at that line.
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

/* Sets a terminal's font (a Pango description like "Cascadia Code 11"; NULL: DEFAULT_FONT) at the
 * window's text size. */
static void set_terminal_font(App *t, VteTerminal *term, const char *font)
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

static VteTerminal *nth_shell(App *t, int i);

/* Gives the terminals, the editors and the Files panel the fonts the settings choose for them. */
void apply_fonts(App *t)
{
  for (int i = 0; t->shell.tabs && i < shell_count(t); i++)
    set_terminal_font(t, nth_shell(t, i), t->settings.font);
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
  for (int i = 0; t->shell.tabs && i < shell_count(t); i++)
    color_terminal(nth_shell(t, i));
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

/* The terminal tabs -------------------------------------------------------------------------- */

typedef struct ShellRun ShellRun;

/* What t-code keeps about a terminal tab (its "t-code-shell" data; term_clear() checks for it). */
typedef struct {
  App *app;
  GPid pid;          /* its shell; 0 until it started and once it exited */
  gboolean first;    /* the first tab: never closed, its shell starts again when it exits */
  gboolean program;  /* a plugin's program, not a shell: not kept in session.ini */
  GtkLabel *label;   /* its name on the tab */
  ShellRun *waiting; /* a command waiting for the shell's prompt */
} Shell;

static struct {
  GtkPopover *rename; /* the entry that renames a tab */
  GtkEntry *entry;
  VteTerminal *renaming;
} ui;

static Shell *shell_of(VteTerminal *term)
{
  return g_object_get_data(G_OBJECT(term), "t-code-shell");
}

static VteTerminal *nth_shell(App *t, int i)
{
  return VTE_TERMINAL(gtk_notebook_get_nth_page(t->shell.tabs, i));
}

VteTerminal *current_shell(App *t)
{
  int i = t->shell.tabs ? gtk_notebook_get_current_page(t->shell.tabs) : -1;

  return i >= 0 ? nth_shell(t, i) : t->shell.term;
}

int shell_count(App *t)
{
  return gtk_notebook_get_n_pages(t->shell.tabs);
}

const char *shell_name(VteTerminal *term)
{
  return gtk_label_get_text(shell_of(term)->label);
}

void shell_rename(VteTerminal *term, const char *name)
{
  char *stripped = g_strstrip(g_strdup(name));

  if (*stripped) {
    gtk_label_set_text(shell_of(term)->label, stripped);
    gtk_widget_set_tooltip_text(gtk_widget_get_parent(GTK_WIDGET(shell_of(term)->label)), stripped);
  }
  g_free(stripped);
}

/* The shell started (its pid is kept), or the error shows in the terminal. A tab closed before
 * its shell started has no terminal any more: the shell is hung up. */
static void spawn_done(VteTerminal *term, GPid pid, GError *error, gpointer data G_GNUC_UNUSED)
{
  Shell *shell = term ? shell_of(term) : NULL;

  if (!term) {
    if (!error && pid > 0)
      kill(pid, SIGHUP);
    return;
  }
  if (error) {
    char *msg = g_strdup_printf("\r\nt-code: %s\r\n", error->message);
    vte_terminal_feed(term, msg, -1);
    g_free(msg);
    return;
  }
  if (shell)
    shell->pid = pid;
}

/* program (NULL: $SHELL first, like most terminals; then the login shell from the user database).
 * It runs in dir (NULL: the project root) with $TCODE_SOCKET set. */
static void spawn_program(App *t, VteTerminal *term, const char *dir, char **program)
{
  const char *var = g_getenv("SHELL");
  char *path = program ? NULL : var && *var ? g_strdup(var) : vte_get_user_shell();
  char *shell[] = { path ? path : "/bin/sh", NULL };
  char **env = g_environ_setenv(g_get_environ(), "TCODE_SOCKET", t->paths.socket, TRUE);

  vte_terminal_spawn_async(term, VTE_PTY_DEFAULT, dir ? dir : t->project.root, program ? program : shell, env,
                           G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, -1, NULL, spawn_done, NULL);
  g_strfreev(env);
  g_free(path);
}

static void spawn_shell(App *t, VteTerminal *term, const char *dir)
{
  spawn_program(t, term, dir, NULL);
}

/* Where a terminal's shell is now: its working folder, else what the shell reported (bash does on
 * Fedora), else the project root. */
static char *shell_directory(App *t, VteTerminal *term)
{
  Shell *shell = shell_of(term);
  char *dir = NULL;

  if (shell && shell->pid > 0) {
    char *link = g_strdup_printf("/proc/%d/cwd", shell->pid);
    dir = g_file_read_link(link, NULL);
    g_free(link);
  }
  GUri *guri = dir ? NULL : vte_terminal_ref_termprop_uri(term, VTE_TERMPROP_CURRENT_DIRECTORY_URI);
  if (guri) {
    char *uri = g_uri_to_string(guri);
    dir = uri ? g_filename_from_uri(uri, NULL, NULL) : NULL;
    g_free(uri);
    g_uri_unref(guri);
  }
  return dir ? dir : g_strdup(t->project.root);
}

/* Closes a terminal tab other than the first: its shell, and what runs in it, get SIGHUP. */
static void close_shell(App *t, VteTerminal *term)
{
  Shell *shell = shell_of(term);
  int page = gtk_notebook_page_num(t->shell.tabs, GTK_WIDGET(term));
  gboolean focused;

  if (!shell || shell->first || page < 0)
    return;
  if (ui.renaming == term)
    gtk_popover_popdown(ui.rename);
  focused = gtk_widget_has_focus(GTK_WIDGET(term));
  if (shell->pid > 0)
    kill(shell->pid, SIGHUP);
  shell->pid = 0;
  gtk_notebook_remove_page(t->shell.tabs, page);
  if (focused)
    gtk_widget_grab_focus(GTK_WIDGET(current_shell(t)));
}

void close_shell_tab(App *t, VteTerminal *term)
{
  close_shell(t, term);
}

void close_current_shell(App *t)
{
  close_shell(t, current_shell(t));
}

static gboolean close_later(gpointer term)
{
  Shell *shell = shell_of(term);

  if (shell && !shell->app->closing)
    close_shell(shell->app, term);
  return G_SOURCE_REMOVE;
}

/* A shell exited: the first one starts again (in the current project, also after
 * restart_shell_pane() hung it up), another one's tab closes. */
static void on_shell_exited(VteTerminal *term, int status G_GNUC_UNUSED, App *t)
{
  Shell *shell = shell_of(term);

  if (t->closing)
    return;
  shell->pid = 0; /* gone: nothing may signal its pid until a new one starts */
  if (!shell->first) {
    g_idle_add_full(G_PRIORITY_DEFAULT, close_later, g_object_ref(term), g_object_unref);
    return;
  }
  vte_terminal_reset(term, TRUE, TRUE);
  spawn_shell(t, term, NULL);
}

/* The rename entry: Enter names the tab. */
static void on_rename_activate(GtkEntry *entry, App *t)
{
  if (ui.renaming && gtk_notebook_page_num(t->shell.tabs, GTK_WIDGET(ui.renaming)) >= 0)
    shell_rename(ui.renaming, gtk_editable_get_text(GTK_EDITABLE(entry)));
  gtk_popover_popdown(ui.rename);
}

static void on_rename_closed(GtkPopover *popover G_GNUC_UNUSED, App *t)
{
  VteTerminal *term = ui.renaming;

  ui.renaming = NULL;
  if (term && gtk_notebook_page_num(t->shell.tabs, GTK_WIDGET(term)) >= 0)
    gtk_widget_grab_focus(GTK_WIDGET(term));
}

/* Asks for a terminal's new name in a small entry under its tab. */
static void ask_rename(App *t, VteTerminal *term)
{
  GtkWidget *tab = gtk_notebook_get_tab_label(t->shell.tabs, GTK_WIDGET(term));
  graphene_rect_t bounds;
  GdkRectangle rect = { 0, 0, 1, 1 };

  if (tab && gtk_widget_compute_bounds(tab, t->shell.panel, &bounds))
    rect =
        (GdkRectangle){ (int)bounds.origin.x, (int)bounds.origin.y, (int)bounds.size.width, (int)bounds.size.height };
  ui.renaming = term;
  gtk_editable_set_text(GTK_EDITABLE(ui.entry), shell_name(term));
  gtk_popover_set_pointing_to(ui.rename, &rect);
  gtk_popover_popup(ui.rename);
  gtk_widget_grab_focus(GTK_WIDGET(ui.entry));
  gtk_editable_select_region(GTK_EDITABLE(ui.entry), 0, -1);
}

void rename_current_shell(App *t)
{
  set_pane_visible(t, t->shell.panel, TRUE);
  ask_rename(t, current_shell(t));
}

/* A double click on a tab renames it. */
static void on_tab_pressed(GtkGestureClick *gesture, int n_press, double x G_GNUC_UNUSED, double y G_GNUC_UNUSED,
                           VteTerminal *term)
{
  Shell *shell = shell_of(term);

  if (n_press == 2) {
    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
    ask_rename(shell->app, term);
  }
}

static void on_close_clicked(GtkButton *button G_GNUC_UNUSED, VteTerminal *term)
{
  close_shell(shell_of(term)->app, term);
}

static void add_location_links(App *t, VteTerminal *term);

/* "Terminal 2", "Terminal 3", ...: the lowest number no tab's name has. */
static char *next_name(App *t)
{
  for (int number = 2;; number++) {
    char *name = g_strdup_printf("Terminal %d", number);
    gboolean used = FALSE;

    for (int i = 0; i < shell_count(t) && !used; i++)
      used = strcmp(shell_name(nth_shell(t, i)), name) == 0;
    if (!used)
      return name;
    g_free(name);
  }
}

/* A terminal tab named name (NULL: the next "Terminal N") at the end; its shell (or program) starts
 * in dir (NULL: the project root) unless it is the first, which starts with the window. */
static VteTerminal *add_tab(App *t, const char *name, const char *dir, gboolean first, char **program)
{
  VteTerminal *term = new_terminal(t);
  Shell *shell = g_new0(Shell, 1);
  GtkWidget *tab = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  GtkWidget *icon = gtk_image_new_from_icon_name("tcode-terminal-symbolic");
  GtkGesture *click = gtk_gesture_click_new();
  char *fallback = name ? NULL : next_name(t);

  shell->app = t;
  shell->first = first;
  shell->program = program != NULL;
  shell->label = GTK_LABEL(gtk_label_new(name ? name : fallback));
  g_object_set_data_full(G_OBJECT(term), "t-code-shell", shell, g_free);
  gtk_widget_add_css_class(GTK_WIDGET(term), "tcode-shell");
  vte_terminal_set_clear_background(term, FALSE);
  g_signal_connect(term, "child-exited", G_CALLBACK(on_shell_exited), t);
  add_location_links(t, term);

  gtk_widget_add_css_class(tab, "tcode-terminal-tab");
  gtk_widget_set_tooltip_text(tab, gtk_label_get_text(shell->label));
  gtk_box_append(GTK_BOX(tab), icon);
  gtk_box_append(GTK_BOX(tab), GTK_WIDGET(shell->label));
  if (!first) {
    GtkWidget *close = gtk_button_new_from_icon_name("tcode-close-symbolic");
    gtk_widget_set_tooltip_text(close, "Close Terminal");
    gtk_widget_set_focus_on_click(close, FALSE);
    g_signal_connect(close, "clicked", G_CALLBACK(on_close_clicked), term);
    gtk_box_append(GTK_BOX(tab), close);
  }
  g_signal_connect(click, "pressed", G_CALLBACK(on_tab_pressed), term);
  gtk_widget_add_controller(tab, GTK_EVENT_CONTROLLER(click));
  gtk_notebook_append_page(t->shell.tabs, GTK_WIDGET(term), tab);
  if (!first)
    spawn_program(t, term, dir, program);
  g_free(fallback);
  return term;
}

static VteTerminal *add_shell(App *t, const char *name, const char *dir, gboolean first)
{
  return add_tab(t, name, dir, first, NULL);
}

VteTerminal *open_shell(App *t, const char *dir)
{
  VteTerminal *term = add_shell(t, NULL, dir, FALSE);

  gtk_notebook_set_current_page(t->shell.tabs, gtk_notebook_page_num(t->shell.tabs, GTK_WIDGET(term)));
  set_pane_visible(t, t->shell.panel, TRUE);
  gtk_widget_grab_focus(GTK_WIDGET(term));
  return term;
}

VteTerminal *open_program(App *t, const char *name, char **argv)
{
  VteTerminal *term = add_tab(t, name, NULL, FALSE, argv);

  gtk_notebook_set_current_page(t->shell.tabs, gtk_notebook_page_num(t->shell.tabs, GTK_WIDGET(term)));
  set_pane_visible(t, t->shell.panel, TRUE);
  return term;
}

VteTerminal *shell_at(App *t, int i)
{
  return i >= 0 && i < shell_count(t) ? nth_shell(t, i) : NULL;
}

static void on_add_clicked(App *t)
{
  open_shell(t, NULL);
}

/* Another tab came up: its terminal takes the keys when the panel shows. */
static gboolean focus_current(gpointer data)
{
  App *t = data;

  if (gtk_widget_get_mapped(t->shell.panel) && !t->closing)
    gtk_widget_grab_focus(GTK_WIDGET(current_shell(t)));
  return G_SOURCE_REMOVE;
}

static void on_switch_page(GtkNotebook *tabs G_GNUC_UNUSED, GtkWidget *page G_GNUC_UNUSED, guint n G_GNUC_UNUSED,
                           App *t)
{
  if (gtk_widget_get_mapped(t->shell.panel))
    g_idle_add(focus_current, t);
}

void start_shell_pane(App *t)
{
  spawn_shell(t, t->shell.term, NULL);
}

void restart_shell_pane(App *t)
{
  for (int i = shell_count(t) - 1; i > 0; i--)
    close_shell(t, nth_shell(t, i));
  if (shell_of(t->shell.term)->pid > 0)
    kill(shell_of(t->shell.term)->pid, SIGHUP); /* it comes back in the current project: on_shell_exited */
}

char **shell_folders(App *t, char ***names)
{
  int n = shell_count(t) - 1, kept = 0;
  char **folders = g_new0(char *, n + 1);

  *names = g_new0(char *, n + 1);
  for (int i = 0; i < n; i++) {
    VteTerminal *term = nth_shell(t, i + 1);
    if (shell_of(term)->program) /* a plugin's: the plugin opens it again if it wants */
      continue;
    folders[kept] = shell_directory(t, term);
    (*names)[kept++] = g_strdup(shell_name(term));
  }
  return folders;
}

void shell_restore(App *t, char **folders, char **names)
{
  for (int i = 0; folders && folders[i]; i++) {
    gboolean named = names && g_strv_length(names) > (guint)i && *names[i];
    const char *dir = g_file_test(folders[i], G_FILE_TEST_IS_DIR) ? folders[i] : NULL;
    add_shell(t, named ? names[i] : NULL, dir, FALSE);
  }
}

/* Running commands in a shell ---------------------------------------------------------------- */

/* The process group in the foreground of a terminal: the shell itself when it waits at its prompt,
 * else the program it runs. 0 when unknown. */
static pid_t shell_foreground(VteTerminal *term)
{
  VtePty *pty = vte_terminal_get_pty(term);
  pid_t group = pty ? tcgetpgrp(vte_pty_get_fd(pty)) : -1;

  return group > 0 ? group : 0;
}

struct ShellRun {
  VteTerminal *term; /* a reference */
  char *command;
  int tries;
};

static void type_in_shell(VteTerminal *term, const char *command)
{
  char *line = g_strdup_printf("\x15%s\r", command);

  vte_terminal_feed_child(term, line, -1);
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
 * command. A closed tab drops it. */
static gboolean run_when_idle(gpointer data)
{
  ShellRun *run = data;
  Shell *shell = shell_of(run->term);
  App *t = shell->app;
  gboolean open = gtk_notebook_page_num(t->shell.tabs, GTK_WIDGET(run->term)) >= 0 && !t->closing;
  pid_t group = shell_foreground(run->term);
  gboolean busy = group && group != shell->pid;

  if (busy && open && ++run->tries < 40)
    return G_SOURCE_CONTINUE;
  if (busy && open)
    show_shell_busy(t, group);
  else if (open)
    type_in_shell(run->term, run->command);
  shell->waiting = NULL;
  g_object_unref(run->term);
  g_free(run->command);
  g_free(run);
  return G_SOURCE_REMOVE;
}

/* Types command into term, showing and focusing it. Ctrl+U first drops a half-typed line. When a
 * program runs in the shell (the last Run, gdb), it gets Ctrl+C first, so the command does not go
 * to its input. */
static void run_in(App *t, VteTerminal *term, const char *command)
{
  Shell *shell = shell_of(term);
  pid_t group = shell_foreground(term);
  gboolean busy = group && group != shell->pid;

  gtk_notebook_set_current_page(t->shell.tabs, gtk_notebook_page_num(t->shell.tabs, GTK_WIDGET(term)));
  set_pane_visible(t, t->shell.panel, TRUE);
  gtk_widget_grab_focus(GTK_WIDGET(term));
  if (shell->waiting) {
    /* Build or Run again while the last one waits: one command runs, the newest. */
    g_set_str(&shell->waiting->command, command);
    shell->waiting->tries = 0;
    if (busy)
      kill(-group, SIGINT);
    return;
  }
  if (!busy) {
    type_in_shell(term, command);
    return;
  }
  kill(-group, SIGINT);
  shell->waiting = g_new0(ShellRun, 1);
  shell->waiting->term = g_object_ref(term);
  shell->waiting->command = g_strdup(command);
  g_timeout_add(50, run_when_idle, shell->waiting);
}

void run_in_shell(App *t, const char *command)
{
  run_in(t, t->shell.term, command);
}

void cd_in_shell(App *t, const char *dir)
{
  char *quoted = g_shell_quote(dir);
  char *command = g_strdup_printf(" cd -- %s", quoted); /* the leading space keeps it out of history */

  run_in(t, current_shell(t), command);
  g_free(command);
  g_free(quoted);
}

/* Clickable compiler messages ---------------------------------------------------------------- */

#define PCRE2_MULTILINE 0x00000400u /* VTE wants its match patterns multiline */

/* path/to/file.ext:line or path/to/file.ext:line:column */
static const char *const location_pattern = "(?:[A-Za-z0-9_.~+-]*/)*[A-Za-z0-9_.+-]+\\.[A-Za-z0-9]+:[0-9]+(?::[0-9]+)?";

/* Opens a "file.c:12:5" location from the shell (relative to the shell's folder or the project). */
static void open_location(App *t, VteTerminal *term, const char *location)
{
  const char *colon = strchr(location, ':');
  char *name = g_strndup(location, colon - location);
  int line = 0, column = 0;
  char *dir = shell_directory(t, term);
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
  VteTerminal *term = VTE_TERMINAL(gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture)));
  char *match;

  if (!(state & GDK_CONTROL_MASK))
    return;
  match = vte_terminal_check_match_at(term, x, y, NULL);
  if (match) {
    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
    open_location(t, term, match);
    g_free(match);
  }
}

/* Makes "file.ext:line:col" in a terminal tab clickable (with Ctrl). */
static void add_location_links(App *t, VteTerminal *term)
{
  GError *error = NULL;
  VteRegex *regex = vte_regex_new_for_match(location_pattern, -1, PCRE2_MULTILINE, &error);
  GtkGesture *click = gtk_gesture_click_new();

  if (!regex) {
    g_warning("t-code: %s", error->message);
    g_error_free(error);
    return;
  }
  int tag = vte_terminal_match_add_regex(term, regex, 0);
  vte_terminal_match_set_cursor_name(term, tag, "pointer");
  vte_regex_unref(regex);

  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_PRIMARY);
  gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(click), GTK_PHASE_CAPTURE);
  g_signal_connect(click, "pressed", G_CALLBACK(on_shell_click), t);
  gtk_widget_add_controller(GTK_WIDGET(term), GTK_EVENT_CONTROLLER(click));
}

void create_shell_pane(App *t)
{
  static const char *const css =
      ".tcode-terminals > header { background: none; box-shadow: inset 0 -1px var(--tc-border); padding: 2px 4px 0; }"
      ".tcode-terminals > header > tabs > tab { min-height: 24px; padding: 0 4px 0 8px; margin: 0 1px;"
      "  border-radius: 7px 7px 0 0; border: none; box-shadow: none; color: var(--tc-fg-dim); }"
      ".tcode-terminals > header > tabs > tab:hover { background: var(--tc-raised); color: var(--tc-fg); }"
      ".tcode-terminals > header > tabs > tab:checked { color: var(--tc-fg-strong);"
      "  box-shadow: inset 0 -2px var(--tc-accent); }"
      ".tcode-terminal-tab { padding-right: 4px; }"
      ".tcode-terminal-tab button, .tcode-terminals > header button { min-width: 20px; min-height: 20px; padding: 0;"
      "  margin: 0; border-radius: 6px; background: none; box-shadow: none; }"
      ".tcode-terminal-tab button:hover, .tcode-terminals > header button:hover {"
      "  background: color-mix(in srgb, var(--tc-fg) 12%, transparent); }";
  GtkWidget *add = gtk_button_new_from_icon_name("tcode-add-symbolic");
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

  palette_add_css(css);
  t->shell.panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  t->shell.tabs = GTK_NOTEBOOK(gtk_notebook_new());
  gtk_notebook_set_scrollable(t->shell.tabs, TRUE);
  gtk_widget_add_css_class(GTK_WIDGET(t->shell.tabs), "tcode-terminals");
  gtk_widget_set_vexpand(GTK_WIDGET(t->shell.tabs), TRUE);
  gtk_widget_set_tooltip_text(add, "New Terminal (Ctrl+Shift+`)");
  gtk_widget_set_focus_on_click(add, FALSE);
  g_signal_connect_swapped(add, "clicked", G_CALLBACK(on_add_clicked), t);
  gtk_box_append(GTK_BOX(box), add);
  gtk_notebook_set_action_widget(t->shell.tabs, box, GTK_PACK_END);
  g_signal_connect(t->shell.tabs, "switch-page", G_CALLBACK(on_switch_page), t);
  gtk_box_append(GTK_BOX(t->shell.panel), GTK_WIDGET(t->shell.tabs));
  t->shell.term = add_shell(t, "Terminal", NULL, TRUE);

  ui.rename = GTK_POPOVER(gtk_popover_new());
  ui.entry = GTK_ENTRY(gtk_entry_new());
  gtk_widget_set_size_request(GTK_WIDGET(ui.entry), 220, -1);
  g_signal_connect(ui.entry, "activate", G_CALLBACK(on_rename_activate), t);
  g_signal_connect(ui.rename, "closed", G_CALLBACK(on_rename_closed), t);
  gtk_popover_set_child(ui.rename, GTK_WIDGET(ui.entry));
  gtk_popover_set_position(ui.rename, GTK_POS_BOTTOM);
  gtk_widget_set_parent(GTK_WIDGET(ui.rename), t->shell.panel);
}
