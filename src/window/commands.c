/*
 * Every command of the window in one table: the keyboard shortcuts, the command palette
 * (Ctrl+Shift+P) and the Keybinds list are all made from it.
 */
#include "window/commands.h"

#include "debug/debugger.h"
#include "debug/debugview.h"
#include "debug/disassembly.h"
#include "dialogs/about.h"
#include "dialogs/keybinds.h"
#include "dialogs/settings.h"
#include "editor/breakpoints.h"
#include "editor/editing.h"
#include "editor/editor.h"
#include "editor/findbar.h"
#include "editor/folding.h"
#include "editor/preview.h"
#include "editor/tabs.h"
#include "lsp/client.h"
#include "lsp/completion.h"
#include "lsp/navigate.h"
#include "lsp/signature.h"
#include "git/compare.h"
#include "popups/picker.h"
#include "popups/quickopen.h"
#include "project/project.h"
#include "project/run.h"
#include "project/toolchain.h"
#include "project/userfiles.h"
#include "update/update.h"
#include "plugins/plugins.h"
#include "window/secondwindow.h"
#include "window/statusbar.h"
#include "window/window.h"
#include "window/theme.h"

#include <stdlib.h>

typedef struct {
  const char *name;
  const char *trigger; /* GTK shortcut trigger, NULL when there is none */
  const char *keys;    /* the shortcut as shown */
  void (*run)(App *t);
  gboolean editor;    /* the shortcut works in an editor (or its find bar) only: elsewhere the key goes on
                       * (Ctrl+W deletes a word in the shell) */
  gboolean here;      /* from the second window, it stays there (it is about its tabs) instead of bringing
                       * the main window up */
  gboolean debugging; /* the shortcut works only while debugging: elsewhere the key goes on (F10 in the shell) */
} Command;

/* What the commands do ----------------------------------------------------------------------- */

/* clang-format off */
static void build(App *t) { build_project(t, FALSE); }
static void run(App *t) { build_project(t, TRUE); }
static void test_project(App *t) { run_project_tests(t); }
static void next_tab(App *t) { step_tab(t, 1); }
static void prev_tab(App *t) { step_tab(t, -1); }
static void focus_left(App *t) { focus_group(t, 0); }
static void focus_right(App *t) { focus_group(t, 1); }
/* Ctrl+= / Ctrl+- / Ctrl+0: in a preview tab the previews' text size, elsewhere everything else's; a factor of 0
 * goes back to normal. */
static void zoom(App *t, double factor)
{
  Tab *tab = current_tab(t);

  if (tab && tab->kind == TAB_PREVIEW)
    set_preview_zoom_scale(t, factor ? t->preview_scale * factor : 1.0);
  else
    set_font_scale(t, factor ? t->font_scale * factor : 1.0);
}
static void zoom_in(App *t) { zoom(t, 1.1); }
static void zoom_out(App *t) { zoom(t, 1 / 1.1); }
static void zoom_reset(App *t) { zoom(t, 0); }
static void check_updates_now(App *t) { updates_check_now(t); }
static void toggle_files(App *t) { toggle_pane(t, t->layout.left); }
static void toggle_shell(App *t) { toggle_pane(t, GTK_WIDGET(t->shell.term)); }
static void show_scm(App *t) { show_sidebar_page(t, "scm"); }
static void show_search(App *t) { show_sidebar_page(t, "search"); }
static void show_run(App *t) { show_sidebar_page(t, "run"); }
static void show_problems_cmd(App *t) { toggle_panel(t, "problems"); }
/* clang-format on */

/* Alt+1: the file manager, showing the sidebar. */
static void focus_fm(App *t)
{
  show_sidebar_page(t, "files");
}

/* Alt+3: the shell, showing it. */
static void focus_shell(App *t)
{
  set_pane_visible(t, GTK_WIDGET(t->shell.term), TRUE);
  gtk_widget_grab_focus(GTK_WIDGET(t->shell.term));
}

/* Ctrl+\: split the editor area, or join it again. */
static void toggle_split(App *t)
{
  set_split(t, !gtk_widget_get_visible(GTK_WIDGET(t->editors.groups[1])));
  focus_current_tab(t);
}

/* The current editor tab, or NULL. */
static Tab *current_editor(App *t)
{
  Tab *tab = current_tab(t);

  return tab && tab->kind == TAB_EDITOR ? tab : NULL;
}

/* Ctrl+S */
static void save(App *t)
{
  Tab *tab = current_editor(t);

  if (tab)
    save_with_format(tab);
}

/* Ctrl+W: closes the current tab (an editor asks about unsaved changes). */
static void close_current(App *t)
{
  Tab *tab = current_tab(t);

  if (tab && tab->kind == TAB_EDITOR)
    quit_editor(tab);
  else if (tab)
    remove_tab(tab);
}

/* The editing commands, on the current editor. */
/* clang-format off */
static void find(App *t) { Tab *tab = current_editor(t); if (tab) findbar_show(tab, FALSE); }
static void replace(App *t) { Tab *tab = current_editor(t); if (tab) findbar_show(tab, TRUE); }
static void find_next(App *t) { Tab *tab = current_editor(t); if (tab) findbar_next(tab, FALSE); }
static void find_previous(App *t) { Tab *tab = current_editor(t); if (tab) findbar_next(tab, TRUE); }
static void comment(App *t) { Tab *tab = current_editor(t); if (tab) editor_toggle_comment(tab); }
static void duplicate(App *t) { Tab *tab = current_editor(t); if (tab) editor_duplicate_lines(tab); }
static void fold(App *t) { Tab *tab = current_editor(t); if (tab) fold_at_cursor(tab); }
static void unfold(App *t) { Tab *tab = current_editor(t); if (tab) unfold_at_cursor(tab); }
static void fold_everything(App *t) { Tab *tab = current_editor(t); if (tab) fold_all(tab); }
static void unfold_everything(App *t) { Tab *tab = current_editor(t); if (tab) unfold_all(tab); }
static void line_up(App *t) { Tab *tab = current_editor(t); if (tab) editor_move_lines(tab, FALSE); }
static void line_down(App *t) { Tab *tab = current_editor(t); if (tab) editor_move_lines(tab, TRUE); }
static void delete_line(App *t) { Tab *tab = current_editor(t); if (tab) editor_delete_lines(tab); }
static void redo(App *t) { Tab *tab = current_editor(t); if (tab) editor_redo(tab); }
static void definition(App *t) { Tab *tab = current_editor(t); if (tab) go_to_definition(tab); }
static void references(App *t) { Tab *tab = current_editor(t); if (tab) find_references(tab); }
static void format(App *t) { Tab *tab = current_editor(t); if (tab) format_document(tab); }
static void rename_it(App *t) { Tab *tab = current_editor(t); if (tab) rename_symbol(tab); }
static void switch_header(App *t) { Tab *tab = current_editor(t); if (tab) switch_source_header(tab); }
static void fix(App *t) { Tab *tab = current_editor(t); if (tab) quick_fix(tab); }
static void symbols(App *t) { Tab *tab = current_editor(t); if (tab) go_to_symbol(tab); }
static void workspace_symbols(App *t) { Tab *tab = current_editor(t); if (tab) go_to_workspace_symbol(tab); }
static void suggest(App *t) { Tab *tab = current_editor(t); if (tab) completion_show(tab); }
static void parameter_hints(App *t) { Tab *tab = current_editor(t); if (tab) signature_show(tab); }
static void toggle_breakpoint(App *t) { Tab *tab = current_editor(t); if (tab) breakpoint_toggle_at_cursor(tab); }
/* clang-format on */

/* The selection, or the name at the cursor, goes to the Run and Debug panel's Watch. */
static void add_to_watch(App *t)
{
  Tab *tab = current_editor(t);
  GtkTextBuffer *buffer = tab ? GTK_TEXT_BUFFER(tab->buffer) : NULL;
  GtkTextIter start, end;
  char *text = NULL;

  if (!buffer)
    return;
  if (gtk_text_buffer_get_selection_bounds(buffer, &start, &end))
    text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
  else {
    gtk_text_buffer_get_iter_at_mark(buffer, &start, gtk_text_buffer_get_insert(buffer));
    if (editor_word_at(&start, &start, &end))
      text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
  }
  if (text) {
    debug_view_add_watch(t, text);
    show_sidebar_page(t, "run");
  }
  g_free(text);
}

/* Go to Line: the picker offers the line typed so far. */
static void line_query(App *t, const char *text, guint generation)
{
  Tab *tab = current_editor(t);
  int lines = tab ? gtk_text_buffer_get_line_count(GTK_TEXT_BUFFER(tab->buffer)) : 0;
  char *end;
  long line = strtol(text, &end, 10);

  if (*text && *end == '\0' && line >= 1 && line <= lines) {
    char *label = g_strdup_printf("Go to line %ld", line), *payload = g_strdup_printf("%ld", line);
    picker_add(t, generation, label, NULL, payload);
    g_free(payload);
    g_free(label);
  } else {
    char *message = g_strdup_printf("Type a line number between 1 and %d", lines);
    picker_message(t, generation, message);
    g_free(message);
  }
}

static void line_choose(App *t, const char *line)
{
  Tab *tab = current_editor(t);

  if (tab) {
    select_tab(tab);
    editor_goto(tab, atoi(line), 1);
  }
}

/* Ctrl+G */
static void go_to_line(App *t)
{
  if (current_editor(t))
    picker_open(t, "Line number", line_query, line_choose);
}

/* Ctrl+Shift+V: a preview of the current Markdown file. */
static void preview(App *t)
{
  Tab *tab = current_tab(t);

  if (tab && tab->kind == TAB_EDITOR && is_markdown(tab->path))
    open_preview(t, tab->path);
}

/* Ctrl+Alt+D: the current file next to its last committed version. */
static void compare_head(App *t)
{
  const char *path = comparable_path(t);

  if (path)
    compare_with_head(t, path);
}

/* Ctrl+Alt+H: the current file's commits, to compare with one. */
static void history(App *t)
{
  const char *path = comparable_path(t);

  if (path)
    file_history(t, path);
}

/* The table ---------------------------------------------------------------------------------- */

static const Command commands[] = {
  { "Save", "<Control>s", "Ctrl+S", save, TRUE, FALSE, FALSE },
  { "Close Tab", "<Control>w|<Control>q", "Ctrl+W", close_current, TRUE, FALSE, FALSE },
  { "Go to File…", "<Control>p", "Ctrl+P", quick_open_show, FALSE, FALSE, FALSE },
  { "Search in Files", "<Control><Shift>f", "Ctrl+Shift+F", show_search, FALSE, FALSE, FALSE },
  { "Command Palette…", "<Control><Shift>p|F1", "Ctrl+Shift+P, F1", command_palette, FALSE, FALSE, FALSE },
  { "Go to Line…", "<Control>g", "Ctrl+G", go_to_line, TRUE, FALSE, FALSE },
  { "Find", "<Control>f", "Ctrl+F", find, TRUE, FALSE, FALSE },
  { "Replace", "<Control>h", "Ctrl+H", replace, TRUE, FALSE, FALSE },
  { "Find Next", "F3", "F3, Enter", find_next, TRUE, FALSE, FALSE },
  { "Find Previous", "<Shift>F3", "Shift+F3, Shift+Enter", find_previous, TRUE, FALSE, FALSE },
  { "Toggle Line Comment", "<Control>slash|<Control>KP_Divide|<Alt>slash", "Ctrl+/", comment, TRUE, FALSE, FALSE },
  { "Duplicate Line", "<Control>d|<Shift><Alt>Down", "Ctrl+D", duplicate, TRUE, FALSE, FALSE },
  { "Move Line Up", "<Alt>Up", "Alt+↑", line_up, TRUE, FALSE, FALSE },
  { "Move Line Down", "<Alt>Down", "Alt+↓", line_down, TRUE, FALSE, FALSE },
  { "Fold", "<Control><Shift>bracketleft|<Control><Shift>braceleft", "Ctrl+Shift+[", fold, TRUE, FALSE, FALSE },
  { "Unfold", "<Control><Shift>bracketright|<Control><Shift>braceright", "Ctrl+Shift+]", unfold, TRUE, FALSE, FALSE },
  { "Fold All", NULL, NULL, fold_everything, TRUE, FALSE, FALSE },
  { "Unfold All", NULL, NULL, unfold_everything, TRUE, FALSE, FALSE },
  { "Delete Line", "<Control><Shift>k", "Ctrl+Shift+K", delete_line, TRUE, FALSE, FALSE },
  { "Redo", "<Control>y", "Ctrl+Y, Ctrl+Shift+Z", redo, TRUE, FALSE, FALSE },
  { "Go to Definition", "F12|<Alt>d", "F12, Ctrl+Click", definition, TRUE, FALSE, FALSE },
  { "Go Back", "<Alt>Left", "Alt+←", go_back, TRUE, FALSE, FALSE },
  { "Find All References", "<Shift>F12|<Alt>r", "Shift+F12", references, TRUE, FALSE, FALSE },
  { "Quick Fix…", "<Control>period", "Ctrl+.", fix, TRUE, FALSE, FALSE },
  { "Go to Symbol in File…", "<Control><Shift>period|<Control><Shift>greater", "Ctrl+Shift+.", symbols, TRUE, FALSE,
    FALSE },
  { "Go to Symbol in Workspace…", NULL, NULL, workspace_symbols, TRUE, FALSE, FALSE },
  { "Switch Source / Header", "<Alt>o", "Alt+O", switch_header, TRUE, FALSE, FALSE },
  { "Rename Symbol", "F2", "F2", rename_it, TRUE, FALSE, FALSE },
  { "Format Document", "<Shift><Alt>f|<Alt>f", "Shift+Alt+F", format, TRUE, FALSE, FALSE },
  { "Trigger Suggest", "<Control>space", "Ctrl+Space", suggest, TRUE, FALSE, FALSE },
  { "Trigger Parameter Hints", "<Control><Shift>space", "Ctrl+Shift+Space", parameter_hints, TRUE, FALSE, FALSE },
  { "Build", "<Control><Shift>b", "Ctrl+Shift+B", build, FALSE, FALSE, FALSE },
  { "Test", NULL, NULL, test_project, FALSE, FALSE, FALSE },
  { "Python: Select Interpreter…", NULL, NULL, toolchain_pick_python, FALSE, FALSE, FALSE },
  { "Rust: Select Features…", NULL, NULL, toolchain_pick_features, FALSE, FALSE, FALSE },
  { "Rust: Select Target…", NULL, NULL, toolchain_pick_target, FALSE, FALSE, FALSE },
  { "Run Without Debugging", "<Control>F5", "Ctrl+F5", run, FALSE, FALSE, FALSE },
  { "Debug: Start Debugging / Continue", "F5", "F5", debug_start_or_continue, FALSE, FALSE, FALSE },
  { "Debug: Toggle Breakpoint", "F9", "F9", toggle_breakpoint, TRUE, FALSE, FALSE },
  { "Debug: Step Over", "F10", "F10", debug_step_over, FALSE, FALSE, TRUE },
  { "Debug: Step Into", "F11", "F11", debug_step_into, FALSE, FALSE, TRUE },
  { "Debug: Step Out", "<Shift>F11", "Shift+F11", debug_step_out, FALSE, FALSE, TRUE },
  { "Debug: Pause", "F6", "F6", debug_pause, FALSE, FALSE, TRUE },
  { "Debug: Stop", "<Shift>F5", "Shift+F5", debug_stop, FALSE, FALSE, TRUE },
  { "Debug: Restart", "<Control><Shift>F5", "Ctrl+Shift+F5", debug_restart, FALSE, FALSE, TRUE },
  { "View: Run and Debug", "<Control><Shift>d", "Ctrl+Shift+D", show_run, FALSE, FALSE, FALSE },
  { "View: Toggle Problems", "<Control><Shift>m", "Ctrl+Shift+M", show_problems_cmd, FALSE, FALSE, FALSE },
  { "Debug: Add to Watch", NULL, NULL, add_to_watch, TRUE, FALSE, FALSE },
  { "Debug: Open Disassembly View", NULL, NULL, disassembly_open, FALSE, FALSE, FALSE },
  { "Debug: Remove All Breakpoints", NULL, NULL, breakpoints_remove_all, FALSE, FALSE, FALSE },
  { "Debug: Select Program to Debug…", NULL, NULL, debug_select_program, FALSE, FALSE, FALSE },
  { "Git: Compare with HEAD", "<Control><Alt>d", "Ctrl+Alt+D", compare_head, FALSE, FALSE, FALSE },
  { "Git: File History…", "<Control><Alt>h", "Ctrl+Alt+H", history, FALSE, FALSE, FALSE },
  { "Git: Stop Comparing", NULL, NULL, stop_comparing, FALSE, TRUE, FALSE },
  { "Git: Show / Hide Unchanged Lines", NULL, NULL, toggle_unchanged_lines, FALSE, TRUE, FALSE },
  { "Markdown Preview", "<Control><Shift>v", "Ctrl+Shift+V", preview, FALSE, TRUE, FALSE },
  { "Split Editor", "<Control>backslash", "Ctrl+\\", toggle_split, FALSE, FALSE, FALSE },
  { "View: Move Editor into New Window", NULL, NULL, move_to_new_window, FALSE, TRUE, FALSE },
  { "View: Reset Panel Layout", NULL, NULL, reset_panel_layout, FALSE, FALSE, FALSE },
  { "Focus Left Editor Group", "<Control>1", "Ctrl+1", focus_left, FALSE, FALSE, FALSE },
  { "Focus Right Editor Group", "<Control>2", "Ctrl+2", focus_right, FALSE, FALSE, FALSE },
  { "Next Tab", "<Control>Page_Down|<Alt>period", "Ctrl+PageDown, Alt+.", next_tab, FALSE, TRUE, FALSE },
  { "Previous Tab", "<Control>Page_Up|<Alt>comma", "Ctrl+PageUp, Alt+,", prev_tab, FALSE, TRUE, FALSE },
  { "Focus File Manager", "<Alt>1", "Alt+1", focus_fm, FALSE, FALSE, FALSE },
  { "Focus Editor", "<Alt>2", "Alt+2", focus_current_tab, FALSE, TRUE, FALSE },
  { "Focus Shell", "<Alt>3", "Alt+3", focus_shell, FALSE, FALSE, FALSE },
  { "Toggle Sidebar", "<Control>b", "Ctrl+B", toggle_files, FALSE, FALSE, FALSE },
  { "Toggle Shell", "<Control>j", "Ctrl+J", toggle_shell, FALSE, FALSE, FALSE },
  { "Source Control", "<Control><Shift>g", "Ctrl+Shift+G", show_scm, FALSE, FALSE, FALSE },
  { "Toggle Code Coloring", "F7", "F7", toggle_syntax, FALSE, TRUE, FALSE },
  { "Restart Language Servers", NULL, NULL, lsp_restart, FALSE, FALSE, FALSE },
  { "Change Indentation for This Project…", NULL, NULL, status_pick_indent, FALSE, FALSE, FALSE },
  { "Zoom In", "<Control>equal|<Control>plus|<Control>KP_Add", "Ctrl+=", zoom_in, FALSE, TRUE, FALSE },
  { "Zoom Out", "<Control>minus|<Control>KP_Subtract", "Ctrl+-", zoom_out, FALSE, TRUE, FALSE },
  { "Reset Zoom", "<Control>0|<Control>KP_0", "Ctrl+0", zoom_reset, FALSE, TRUE, FALSE },
  { "Open Folder…", "<Control><Shift>o", "Ctrl+Shift+O", choose_folder, FALSE, FALSE, FALSE },
  { "Settings…", "<Control>comma", "Ctrl+,", show_settings, FALSE, FALSE, FALSE },
  { "Preferences: Color Theme…", NULL, NULL, color_theme_pick, FALSE, FALSE, FALSE },
  { "Preferences: Open Settings File", NULL, NULL, open_settings_file, FALSE, FALSE, FALSE },
  { "Preferences: Open Theme CSS", NULL, NULL, open_user_theme, FALSE, FALSE, FALSE },
  { "Preferences: Open Editor Colors", NULL, NULL, open_editor_colors, FALSE, FALSE, FALSE },
  { "Keyboard Shortcuts", NULL, NULL, show_keybinds, FALSE, FALSE, FALSE },
  { "About t-code", NULL, NULL, show_about, FALSE, FALSE, FALSE },
  { "Check for Updates Now", NULL, NULL, check_updates_now, FALSE, FALSE, FALSE },
  { "Plugins: Reload Installed Plugins", NULL, NULL, plugins_reload, FALSE, FALSE, FALSE },
  { "Plugins: Open Plugins Folder", NULL, NULL, open_plugins_folder, FALSE, FALSE, FALSE },
};

void run_command(App *t, const char *name)
{
  for (size_t i = 0; i < G_N_ELEMENTS(commands); i++)
    if (strcmp(commands[i].name, name) == 0) {
      commands[i].run(t);
      return;
    }
}

const KeyHelp *window_keys(int *n)
{
  static KeyHelp keys[G_N_ELEMENTS(commands)];

  for (size_t i = 0; i < G_N_ELEMENTS(commands); i++)
    keys[i] = (KeyHelp){ commands[i].name, commands[i].keys };
  *n = G_N_ELEMENTS(commands);
  return keys;
}

/* Shortcuts ---------------------------------------------------------------------------------- */

/* Whether the keyboard is in an editor's page: its text or its find bar. */
static gboolean in_editor(GtkWidget *focus)
{
  for (; focus; focus = gtk_widget_get_parent(focus))
    if (gtk_widget_has_css_class(focus, "tcode-editor"))
      return TRUE;
  return FALSE;
}

static gboolean on_shortcut(GtkWidget *window, GVariant *args G_GNUC_UNUSED, gpointer data)
{
  const Command *command = data;
  App *t = g_object_get_data(G_OBJECT(window), "t-code-app");

  if (command->editor && !in_editor(gtk_root_get_focus(GTK_ROOT(window))))
    return FALSE;
  if (command->debugging && !debug_active(t))
    return FALSE;
  /* In the second window: the panels, pickers and the shell are in the main window. */
  if (!command->editor && window != GTK_WIDGET(t->window) && !command->here)
    gtk_window_present(t->window);
  command->run(t);
  return TRUE;
}

void add_shortcuts(App *t)
{
  add_shortcuts_to(t, t->window);
}

void add_shortcuts_to(App *t, GtkWindow *window)
{
  GtkEventController *controller = gtk_shortcut_controller_new();

  g_object_set_data(G_OBJECT(window), "t-code-app", t);
  /* Capture phase: the window sees these keys before the focused terminal or editor does. */
  gtk_event_controller_set_propagation_phase(controller, GTK_PHASE_CAPTURE);
  for (size_t i = 0; i < G_N_ELEMENTS(commands); i++) {
    if (!commands[i].trigger)
      continue;
    gtk_shortcut_controller_add_shortcut(
        GTK_SHORTCUT_CONTROLLER(controller),
        gtk_shortcut_new(gtk_shortcut_trigger_parse_string(commands[i].trigger),
                         gtk_callback_action_new(on_shortcut, (gpointer)&commands[i], NULL)));
  }
  plugins_add_shortcuts(GTK_SHORTCUT_CONTROLLER(controller), t);
  g_object_set_data(G_OBJECT(window), "t-code-shortcut-controller", controller);
  gtk_widget_add_controller(GTK_WIDGET(window), controller);
}

void refresh_shortcuts(App *t)
{
  GtkWindow *windows[] = { t->window, t->editors.second };

  for (guint i = 0; i < G_N_ELEMENTS(windows); i++) {
    if (!windows[i])
      continue;
    GtkEventController *controller = g_object_get_data(G_OBJECT(windows[i]), "t-code-shortcut-controller");
    if (controller) {
      g_object_set_data(G_OBJECT(windows[i]), "t-code-shortcut-controller", NULL);
      gtk_widget_remove_controller(GTK_WIDGET(windows[i]), controller);
    }
    add_shortcuts_to(t, windows[i]);
  }
}

/* Command palette ---------------------------------------------------------------------------- */

typedef struct {
  int index, score;
} Match;

/* Best match first; equal ones in table order. */
static int by_score(gconstpointer a, gconstpointer b)
{
  const Match *x = a, *y = b;

  return x->score != y->score ? y->score - x->score : x->index - y->index;
}

/* The table order when nothing is typed, otherwise the best match first. */
static void query(App *t, const char *text, guint generation)
{
  Match matches[G_N_ELEMENTS(commands)];
  int n = 0;

  for (size_t i = 0; i < G_N_ELEMENTS(commands); i++) {
    int score = *text ? fuzzy_score(text, commands[i].name) : 0;
    if (score >= 0)
      matches[n++] = (Match){ (int)i, score };
  }
  qsort(matches, n, sizeof *matches, (int (*)(const void *, const void *))by_score);
  for (int i = 0; i < n; i++) {
    const Command *c = &commands[matches[i].index];
    char *index = g_strdup_printf("%d", matches[i].index);
    picker_add(t, generation, c->name, c->keys, index);
    g_free(index);
  }
  int plugin_matches = plugins_palette_query(t, text, generation);
  if (n == 0 && plugin_matches == 0)
    picker_message(t, generation, "No matching commands");
}

/* PickerChoose: runs the picked command (its index in the table). */
static void choose(App *t, const char *index)
{
  if (plugins_palette_choose(t, index))
    return;
  commands[atoi(index)].run(t);
}

void command_palette(App *t)
{
  picker_open(t, "Type a command", query, choose);
}
