/*
 * Every command of the window in one table: the keyboard shortcuts, the command palette
 * (Ctrl+Shift+P) and the Keybinds list are all made from it.
 */
#include "window/commands.h"

#include "application/identity.h"
#include "application/log.h"
#include "debug/debugger.h"
#include "debug/view/debugview.h"
#include "debug/disassembly.h"
#include "debug/memory.h"
#include "dialogs/about.h"
#include "dialogs/keybinds.h"
#include "dialogs/processes.h"
#include "dialogs/settings.h"
#include "dialogs/templates.h"
#include "editor/gutter/breakpoints.h"
#include "editor/editing/editing.h"
#include "editor/editor.h"
#include "editor/editing/findbar.h"
#include "editor/gutter/folding.h"
#include "editor/editing/multicursor.h"
#include "editor/preview/preview.h"
#include "editor/tabs.h"
#include "files/files.h"
#include "lsp/client/client.h"
#include "lsp/inline/completion.h"
#include "git/scm/scm.h"
#include "lsp/symbols/hierarchy.h"
#include "testing/testing.h"
#include "lsp/symbols/navigate.h"
#include "lsp/inline/signature.h"
#include "git/compare.h"
#include "popups/picker.h"
#include "popups/quickopen.h"
#include "project/project.h"
#include "project/run/run.h"
#include "project/toolchain.h"
#include "project/userfiles.h"
#include "update/update.h"
#include "plugins/manager.h"
#include "plugins/plugins.h"
#include "window/secondwindow.h"
#include "window/statusbar.h"
#include "window/terminal.h"
#include "window/window.h"
#include "window/theme.h"

#include <glib/gstdio.h>
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
static void focus_group_1(App *t) { focus_group(t, group_nth(t, 0)); }
static void focus_group_2(App *t) { focus_group(t, group_nth(t, 1)); }
static void focus_group_3(App *t) { focus_group(t, group_nth(t, 2)); }
static void focus_group_4(App *t) { focus_group(t, group_nth(t, 3)); }
static void focus_group_5(App *t) { focus_group(t, group_nth(t, 4)); }
static void focus_group_6(App *t) { focus_group(t, group_nth(t, 5)); }
static void focus_group_7(App *t) { focus_group(t, group_nth(t, 6)); }
static void focus_group_8(App *t) { focus_group(t, group_nth(t, 7)); }
static void focus_group_9(App *t) { focus_group(t, group_nth(t, 8)); }
static void split_down(App *t) { split_group(t->editors.active, SPLIT_DOWN); focus_current_tab(t); }
static void join_all_groups(App *t) { join_groups(t); focus_current_tab(t); }
static void close_editor_group(App *t) { close_group(t->editors.active); }
/* Ctrl+= / Ctrl+- / Ctrl+0: in a preview tab the previews' text size, elsewhere everything else's; a factor of 0
 * goes back to normal. */
static void zoom(App *t, double factor)
{
  Tab *tab = current_tab(t);

  if (tab && tab->kind == TAB_PREVIEW)
    set_preview_zoom_scale(t, factor ? t->settings.preview_scale * factor : 1.0);
  else
    set_font_scale(t, factor ? t->settings.font_scale * factor : 1.0);
}
static void zoom_in(App *t) { zoom(t, 1.1); }
static void zoom_out(App *t) { zoom(t, 1 / 1.1); }
static void zoom_reset(App *t) { zoom(t, 0); }
static void check_updates_now(App *t) { updates_check_now(t); }
static void toggle_files(App *t) { toggle_pane(t, t->layout.left); }
static void toggle_shell(App *t) { toggle_pane(t, t->shell.panel); }
static void new_terminal_cmd(App *t) { open_shell(t, NULL); }
static void show_scm(App *t) { show_sidebar_page(t, "scm"); }
static void show_search(App *t) { show_sidebar_page(t, "search"); }
static void save_as_template(App *t) { show_save_template(t, NULL); }
static void new_from_template(App *t) { show_sidebar_page(t, "files"); files_new_from_template(t); }
static void show_run(App *t) { show_sidebar_page(t, "run"); }
static void show_problems_cmd(App *t) { toggle_panel(t, "problems"); }
static void git_checkout(App *t G_GNUC_UNUSED) { scm_switch_branch(); }
static void git_create_branch(App *t G_GNUC_UNUSED) { scm_create_branch(); }
static void git_pull(App *t G_GNUC_UNUSED) { scm_pull(); }
static void git_push(App *t G_GNUC_UNUSED) { scm_push(); }
static void git_fetch(App *t G_GNUC_UNUSED) { scm_fetch(); }
static void git_stash(App *t G_GNUC_UNUSED) { scm_stash(); }
static void git_stash_pop(App *t G_GNUC_UNUSED) { scm_pick_stash(STASH_POP); }
static void git_stash_apply(App *t G_GNUC_UNUSED) { scm_pick_stash(STASH_APPLY); }
static void git_stash_drop(App *t G_GNUC_UNUSED) { scm_pick_stash(STASH_DROP); }
static void svn_changelists(App *t G_GNUC_UNUSED) { scm_changelists(); }
static void show_outline_cmd(App *t) { toggle_panel(t, "outline"); }
static void show_hierarchy_cmd(App *t) { toggle_panel(t, "hierarchy"); }
static void show_testing_cmd(App *t) { toggle_panel(t, "testing"); }
static void show_plugins_cmd(App *t) { toggle_panel(t, "plugins"); }
static void browse_plugins(App *t) { show_plugins_window_at(t, NULL, TRUE); }
static void run_all_tests(App *t) { show_sidebar_page(t, "testing"); testing_run_all(t); }
static void run_failed_tests(App *t) { show_sidebar_page(t, "testing"); testing_run_failed(t); }
static void refresh_tests(App *t) { show_sidebar_page(t, "testing"); testing_refresh(t); }
static void stop_tests(App *t G_GNUC_UNUSED) { testing_stop(); }
/* clang-format on */

/* "Help: Open Log File": t-code's warnings (application/log.h), in an editor. */
static void open_log(App *t)
{
  const char *path = log_path();

  if (path && g_file_test(path, G_FILE_TEST_EXISTS))
    open_file(t, path);
  else
    status_say(t, "The log file could not be written");
}

/* Alt+1: the file manager, showing the sidebar. */
static void focus_fm(App *t)
{
  show_sidebar_page(t, "files");
}

/* Alt+3: the shell, showing it. */
static void focus_shell(App *t)
{
  set_pane_visible(t, t->shell.panel, TRUE);
  gtk_widget_grab_focus(GTK_WIDGET(current_shell(t)));
}

/* Ctrl+\: a new group right of the active one; pressed in an empty group (just split off), it closes
 * that group again. */
static void toggle_split(App *t)
{
  GtkNotebook *group = t->editors.active;

  if (gtk_notebook_get_n_pages(group) == 0 && group_count(t) > 1)
    close_group(group);
  else
    split_group(group, SPLIT_RIGHT);
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
static void block_comment(App *t) { Tab *tab = current_editor(t); if (tab) editor_toggle_block_comment(tab); }
static void duplicate(App *t) { Tab *tab = current_editor(t); if (tab) editor_duplicate_lines(tab); }
static void add_next(App *t) { Tab *tab = current_editor(t); if (tab) multicursor_add_next(tab); }
static void select_occurrences(App *t) { Tab *tab = current_editor(t); if (tab) multicursor_select_all(tab); }
static void cursor_above(App *t) { Tab *tab = current_editor(t); if (tab) multicursor_add_line(tab, FALSE); }
static void cursor_below(App *t) { Tab *tab = current_editor(t); if (tab) multicursor_add_line(tab, TRUE); }
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
static void call_hierarchy(App *t) { Tab *tab = current_editor(t); if (tab) hierarchy_show(tab, FALSE); }
static void type_hierarchy(App *t) { Tab *tab = current_editor(t); if (tab) hierarchy_show(tab, TRUE); }
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

/* Ctrl+Shift+V: a preview of the current Markdown (or SVG) file. */
static void preview(App *t)
{
  Tab *tab = current_tab(t);

  if (tab && tab->kind == TAB_EDITOR && has_preview(tab->path))
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

static void command_palette(App *t);

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
  { "Toggle Block Comment", "<Alt>c", "Alt+C", block_comment, TRUE, FALSE, FALSE },
  { "Duplicate Line", "<Shift><Alt>Down", "Shift+Alt+↓", duplicate, TRUE, FALSE, FALSE },
  { "Add Selection to Next Find Match", "<Control>d", "Ctrl+D", add_next, TRUE, FALSE, FALSE },
  { "Select All Occurrences", "<Control><Shift>l", "Ctrl+Shift+L", select_occurrences, TRUE, FALSE, FALSE },
  { "Add Cursor Above", "<Control><Shift>Up", "Ctrl+Shift+↑", cursor_above, TRUE, FALSE, FALSE },
  { "Add Cursor Below", "<Control><Shift>Down", "Ctrl+Shift+↓", cursor_below, TRUE, FALSE, FALSE },
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
  { "Show Call Hierarchy", "<Shift><Alt>h", "Shift+Alt+H", call_hierarchy, TRUE, FALSE, FALSE },
  { "Show Type Hierarchy", NULL, NULL, type_hierarchy, TRUE, FALSE, FALSE },
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
  { "Test: Run All Tests", NULL, NULL, run_all_tests, FALSE, FALSE, FALSE },
  { "Test: Run Failed Tests", NULL, NULL, run_failed_tests, FALSE, FALSE, FALSE },
  { "Test: Refresh Tests", NULL, NULL, refresh_tests, FALSE, FALSE, FALSE },
  { "Test: Stop", NULL, NULL, stop_tests, FALSE, FALSE, FALSE },
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
  { "View: Toggle Outline", NULL, NULL, show_outline_cmd, FALSE, FALSE, FALSE },
  { "View: Toggle Hierarchy", NULL, NULL, show_hierarchy_cmd, FALSE, FALSE, FALSE },
  { "View: Toggle Testing", NULL, NULL, show_testing_cmd, FALSE, FALSE, FALSE },
  { "View: Toggle Plugins", NULL, NULL, show_plugins_cmd, FALSE, FALSE, FALSE },
  { "Debug: Add to Watch", NULL, NULL, add_to_watch, TRUE, FALSE, FALSE },
  { "Debug: Open Disassembly View", NULL, NULL, disassembly_open, FALSE, FALSE, FALSE },
  { "Debug: Open Memory View…", NULL, NULL, memory_open, FALSE, FALSE, FALSE },
  { "Debug: Attach to Process…", NULL, NULL, debug_attach, FALSE, FALSE, FALSE },
  { "Debug: Open Core Dump…", NULL, NULL, debug_open_core, FALSE, FALSE, FALSE },
  { "Debug: Remove All Breakpoints", NULL, NULL, breakpoints_remove_all, FALSE, FALSE, FALSE },
  { "Debug: Select Program to Debug…", NULL, NULL, debug_select_program, FALSE, FALSE, FALSE },
  { "Git: Checkout to…", NULL, NULL, git_checkout, FALSE, FALSE, FALSE },
  { "Git: Create Branch…", NULL, NULL, git_create_branch, FALSE, FALSE, FALSE },
  { "Git: Pull", NULL, NULL, git_pull, FALSE, FALSE, FALSE },
  { "Git: Push", NULL, NULL, git_push, FALSE, FALSE, FALSE },
  { "Git: Fetch", NULL, NULL, git_fetch, FALSE, FALSE, FALSE },
  { "Git: Stash…", NULL, NULL, git_stash, FALSE, FALSE, FALSE },
  { "Git: Pop Stash…", NULL, NULL, git_stash_pop, FALSE, FALSE, FALSE },
  { "Git: Apply Stash…", NULL, NULL, git_stash_apply, FALSE, FALSE, FALSE },
  { "Git: Drop Stash…", NULL, NULL, git_stash_drop, FALSE, FALSE, FALSE },
  { "Subversion: Changelists…", NULL, NULL, svn_changelists, FALSE, FALSE, FALSE },
  { "Git: Compare with HEAD", "<Control><Alt>d", "Ctrl+Alt+D", compare_head, FALSE, FALSE, FALSE },
  { "Git: File History…", "<Control><Alt>h", "Ctrl+Alt+H", history, FALSE, FALSE, FALSE },
  { "Git: Stop Comparing", NULL, NULL, stop_comparing, FALSE, TRUE, FALSE },
  { "Git: Show / Hide Unchanged Lines", NULL, NULL, toggle_unchanged_lines, FALSE, TRUE, FALSE },
  { "Markdown Preview", "<Control><Shift>v", "Ctrl+Shift+V", preview, FALSE, TRUE, FALSE },
  { "Split Editor", "<Control>backslash", "Ctrl+\\", toggle_split, FALSE, FALSE, FALSE },
  { "View: Move Editor into New Window", NULL, NULL, move_to_new_window, FALSE, TRUE, FALSE },
  { "View: Reset Panel Layout", NULL, NULL, reset_panel_layout, FALSE, FALSE, FALSE },
  { "Split Editor Down", NULL, NULL, split_down, FALSE, FALSE, FALSE },
  { "Close Editor Group", NULL, NULL, close_editor_group, FALSE, FALSE, FALSE },
  { "Join All Editor Groups", NULL, NULL, join_all_groups, FALSE, FALSE, FALSE },
  { "Focus Editor Group 1", "<Control>1", "Ctrl+1", focus_group_1, FALSE, FALSE, FALSE },
  { "Focus Editor Group 2", "<Control>2", "Ctrl+2", focus_group_2, FALSE, FALSE, FALSE },
  { "Focus Editor Group 3", "<Control>3", "Ctrl+3", focus_group_3, FALSE, FALSE, FALSE },
  { "Focus Editor Group 4", "<Control>4", "Ctrl+4", focus_group_4, FALSE, FALSE, FALSE },
  { "Focus Editor Group 5", "<Control>5", "Ctrl+5", focus_group_5, FALSE, FALSE, FALSE },
  { "Focus Editor Group 6", "<Control>6", "Ctrl+6", focus_group_6, FALSE, FALSE, FALSE },
  { "Focus Editor Group 7", "<Control>7", "Ctrl+7", focus_group_7, FALSE, FALSE, FALSE },
  { "Focus Editor Group 8", "<Control>8", "Ctrl+8", focus_group_8, FALSE, FALSE, FALSE },
  { "Focus Editor Group 9", "<Control>9", "Ctrl+9", focus_group_9, FALSE, FALSE, FALSE },
  { "Next Tab", "<Control>Page_Down|<Alt>period", "Ctrl+PageDown, Alt+.", next_tab, FALSE, TRUE, FALSE },
  { "Previous Tab", "<Control>Page_Up|<Alt>comma", "Ctrl+PageUp, Alt+,", prev_tab, FALSE, TRUE, FALSE },
  { "Focus File Manager", "<Alt>1", "Alt+1", focus_fm, FALSE, FALSE, FALSE },
  { "Focus Editor", "<Alt>2", "Alt+2", focus_current_tab, FALSE, TRUE, FALSE },
  { "Focus Shell", "<Alt>3", "Alt+3", focus_shell, FALSE, FALSE, FALSE },
  { "Toggle Sidebar", "<Control>b", "Ctrl+B", toggle_files, FALSE, FALSE, FALSE },
  { "Toggle Shell", "<Control>j", "Ctrl+J", toggle_shell, FALSE, FALSE, FALSE },
  { "Terminal: New Terminal", "<Control><Shift>grave|<Control><Shift>asciitilde|<Control><Shift>t",
    "Ctrl+Shift+`, Ctrl+Shift+T", new_terminal_cmd, FALSE, FALSE, FALSE },
  { "Terminal: Close Terminal", NULL, NULL, close_current_shell, FALSE, FALSE, FALSE },
  { "Terminal: Rename Terminal…", NULL, NULL, rename_current_shell, FALSE, FALSE, FALSE },
  { "Source Control", "<Control><Shift>g", "Ctrl+Shift+G", show_scm, FALSE, FALSE, FALSE },
  { "Toggle Code Coloring", "F7", "F7", toggle_syntax, FALSE, TRUE, FALSE },
  { "Toggle Relative Line Numbers", "<Control><Alt>l", "Ctrl+Alt+L", toggle_relative_numbers, FALSE, TRUE, FALSE },
  { "Restart Language Servers", NULL, NULL, lsp_restart, FALSE, FALSE, FALSE },
  { "Change Indentation for This Project…", NULL, NULL, status_pick_indent, FALSE, FALSE, FALSE },
  { "Zoom In", "<Control>equal|<Control>plus|<Control>KP_Add", "Ctrl+=", zoom_in, FALSE, TRUE, FALSE },
  { "Zoom Out", "<Control>minus|<Control>KP_Subtract", "Ctrl+-", zoom_out, FALSE, TRUE, FALSE },
  { "Reset Zoom", "<Control>0|<Control>KP_0", "Ctrl+0", zoom_reset, FALSE, TRUE, FALSE },
  { "Template: Save File as Template…", NULL, NULL, save_as_template, FALSE, FALSE, FALSE },
  { "Template: New File from Template…", NULL, NULL, new_from_template, FALSE, FALSE, FALSE },
  { "Template: Delete Template…", NULL, NULL, pick_template_to_delete, FALSE, FALSE, FALSE },
  { "Open Folder…", "<Control><Shift>o", "Ctrl+Shift+O", choose_folder, FALSE, FALSE, FALSE },
  { "Settings…", "<Control>comma", "Ctrl+,", show_settings, FALSE, FALSE, FALSE },
  { "Preferences: Color Theme…", NULL, NULL, color_theme_pick, FALSE, FALSE, FALSE },
  { "Preferences: Open Settings File", NULL, NULL, open_settings_file, FALSE, FALSE, FALSE },
  { "Preferences: Open Theme CSS", NULL, NULL, open_user_theme, FALSE, FALSE, FALSE },
  { "Preferences: Open Editor Colors", NULL, NULL, open_editor_colors, FALSE, FALSE, FALSE },
  { "Preferences: Open User Snippets", NULL, NULL, open_user_snippets, FALSE, FALSE, FALSE },
  { "Keyboard Shortcuts", NULL, NULL, show_keybinds, FALSE, FALSE, FALSE },
  { "Help: Open Log File", NULL, NULL, open_log, FALSE, FALSE, FALSE },
  { "Help: Process Monitor", NULL, NULL, show_processes, FALSE, FALSE, FALSE },
  { "About t-code", NULL, NULL, show_about, FALSE, FALSE, FALSE },
  { "Check for Updates Now", NULL, NULL, check_updates_now, FALSE, FALSE, FALSE },
  { "Plugins: Manage Plugins", NULL, NULL, show_plugins_window, FALSE, FALSE, FALSE },
  { "Plugins: Browse the Registry", NULL, NULL, browse_plugins, FALSE, FALSE, FALSE },
  { "Plugins: Reload Installed Plugins", NULL, NULL, plugins_reload, FALSE, FALSE, FALSE },
  { "Plugins: Open Plugins Folder", NULL, NULL, open_plugins_folder, FALSE, FALSE, FALSE },
};

/* The user's shortcuts -------------------------------------------------------------------------- */

/* keybindings.ini: [keybindings] with a command's name = its keys (GTK accelerators, '|' between
 * several; empty: none). Commands not in it keep the table's keys. */
#define KEYS_GROUP "keybindings"

static GHashTable *own_keys;                     /* command name -> trigger ("" : none) */
static char *own_labels[G_N_ELEMENTS(commands)]; /* the user's keys of command i as shown */

static char *keybindings_path(void)
{
  return g_build_filename(g_get_user_config_dir(), TCODE_NAME, "keybindings.ini", NULL);
}

static int command_index(const char *name)
{
  for (size_t i = 0; i < G_N_ELEMENTS(commands); i++)
    if (strcmp(commands[i].name, name) == 0)
      return (int)i;
  return -1;
}

/* Whether every alternative of trigger ("<Alt>c|F12") is a key GTK understands; "" is none. */
static gboolean valid_trigger(const char *trigger)
{
  char **alternatives = g_strsplit(trigger, "|", -1);
  gboolean valid = TRUE;

  for (int i = 0; alternatives[i] && valid; i++) {
    guint key = 0;
    valid = gtk_accelerator_parse(alternatives[i], &key, NULL) && key != 0;
  }
  g_strfreev(alternatives);
  return valid;
}

/* The keys of trigger as shown ("Alt+C, F12"); NULL for none. */
static char *trigger_label(const char *trigger)
{
  char **alternatives = g_strsplit(trigger, "|", -1);
  GString *label = g_string_new(NULL);

  for (int i = 0; alternatives[i]; i++) {
    guint key;
    GdkModifierType mods;
    if (!*alternatives[i] || !gtk_accelerator_parse(alternatives[i], &key, &mods))
      continue;
    char *one = gtk_accelerator_get_label(key, mods);
    g_string_append_printf(label, "%s%s", label->len ? ", " : "", one);
    g_free(one);
  }
  g_strfreev(alternatives);
  return label->len ? g_string_free(label, FALSE) : (g_string_free(label, TRUE), NULL);
}

/* Command i's keys are the user's trigger from now on (NULL: the table's again). */
static void set_own(size_t i, const char *trigger)
{
  g_clear_pointer(&own_labels[i], g_free);
  if (!trigger) {
    g_hash_table_remove(own_keys, commands[i].name);
    return;
  }
  g_hash_table_insert(own_keys, g_strdup(commands[i].name), g_strdup(trigger));
  own_labels[i] = trigger_label(trigger);
}

/* Reads keybindings.ini once; unknown commands and keys GTK does not understand go to the log. */
static void load_own_keys(void)
{
  GKeyFile *file;
  char *path, **names;

  if (own_keys)
    return;
  own_keys = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  file = g_key_file_new();
  path = keybindings_path();
  if (g_key_file_load_from_file(file, path, G_KEY_FILE_NONE, NULL)) {
    names = g_key_file_get_keys(file, KEYS_GROUP, NULL, NULL);
    for (int i = 0; names && names[i]; i++) {
      char *trigger = g_key_file_get_string(file, KEYS_GROUP, names[i], NULL);
      int index = command_index(names[i]);
      if (index < 0)
        g_warning("keybindings.ini: no command named \"%s\"", names[i]);
      else if (!trigger || !valid_trigger(trigger))
        g_warning("keybindings.ini: \"%s\" for \"%s\" is not a key", trigger ? trigger : "", names[i]);
      else
        set_own(index, trigger);
      g_free(trigger);
    }
    g_strfreev(names);
  }
  g_free(path);
  g_key_file_free(file);
}

/* Writes the user's keys to keybindings.ini (the file goes when there are none). */
static void save_own_keys(void)
{
  char *path = keybindings_path();
  GKeyFile *file;
  GError *error = NULL;

  if (g_hash_table_size(own_keys) == 0) {
    g_remove(path);
    g_free(path);
    return;
  }
  file = g_key_file_new();
  for (size_t i = 0; i < G_N_ELEMENTS(commands); i++) {
    const char *trigger = g_hash_table_lookup(own_keys, commands[i].name);
    if (trigger)
      g_key_file_set_string(file, KEYS_GROUP, commands[i].name, trigger);
  }
  g_key_file_set_comment(file, KEYS_GROUP, NULL,
                         " The keyboard shortcuts changed in Settings > Keyboard Shortcuts: a command's name\n"
                         " (as the command palette lists it) = its keys, GTK accelerators like <Control><Shift>k,\n"
                         " with | between several; empty: no shortcut. Read at start-up.",
                         NULL);
  char *dir = g_path_get_dirname(path);
  g_mkdir_with_parents(dir, 0700);
  if (!g_key_file_save_to_file(file, path, &error)) {
    g_warning("Could not save %s: %s", path, error->message);
    g_error_free(error);
  }
  g_free(dir);
  g_key_file_free(file);
  g_free(path);
}

/* Command i's trigger: the user's or the table's; NULL when it has none. */
static const char *trigger_of(size_t i)
{
  const char *own;

  load_own_keys();
  own = g_hash_table_lookup(own_keys, commands[i].name);
  if (own)
    return *own ? own : NULL;
  return commands[i].trigger;
}

int command_count(void)
{
  return G_N_ELEMENTS(commands);
}

const char *command_name(int i)
{
  return commands[i].name;
}

const char *command_shortcut(int i)
{
  load_own_keys();
  return g_hash_table_contains(own_keys, commands[i].name) ? own_labels[i] : commands[i].keys;
}

gboolean command_shortcut_changed(int i)
{
  load_own_keys();
  return g_hash_table_contains(own_keys, commands[i].name);
}

/* Takes key out of command i's trigger; TRUE when it was there. */
static gboolean take_key(size_t i, guint key, GdkModifierType mods)
{
  const char *trigger = trigger_of(i);
  char **alternatives;
  GString *rest;
  gboolean found = FALSE;

  if (!trigger)
    return FALSE;
  alternatives = g_strsplit(trigger, "|", -1);
  rest = g_string_new(NULL);
  for (int a = 0; alternatives[a]; a++) {
    guint other;
    GdkModifierType other_mods;
    if (gtk_accelerator_parse(alternatives[a], &other, &other_mods) && gdk_keyval_to_lower(other) == key &&
        other_mods == mods) {
      found = TRUE;
      continue;
    }
    g_string_append_printf(rest, "%s%s", rest->len ? "|" : "", alternatives[a]);
  }
  if (found)
    set_own(i, rest->str);
  g_string_free(rest, TRUE);
  g_strfreev(alternatives);
  return found;
}

int command_set_shortcut(App *t, int i, guint key, GdkModifierType mods)
{
  int taken = -1;

  load_own_keys();
  if (key == 0) {
    set_own(i, "");
  } else {
    key = gdk_keyval_to_lower(key);
    mods &= gtk_accelerator_get_default_mod_mask();
    for (size_t j = 0; j < G_N_ELEMENTS(commands); j++)
      if ((int)j != i && take_key(j, key, mods) && taken < 0)
        taken = (int)j;
    char *trigger = gtk_accelerator_name(key, mods);
    set_own(i, trigger);
    g_free(trigger);
  }
  save_own_keys();
  refresh_shortcuts(t);
  return taken;
}

void command_reset_shortcut(App *t, int i)
{
  load_own_keys();
  set_own(i, NULL);
  save_own_keys();
  refresh_shortcuts(t);
}

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
    keys[i] = (KeyHelp){ commands[i].name, command_shortcut((int)i) };
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
    if (!trigger_of(i))
      continue;
    gtk_shortcut_controller_add_shortcut(
        GTK_SHORTCUT_CONTROLLER(controller),
        gtk_shortcut_new(gtk_shortcut_trigger_parse_string(trigger_of(i)),
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

/* The table order when nothing is typed, otherwise the best match first; then the plugins' commands. */
void command_palette_query(App *t, const char *text, guint generation)
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
    picker_add(t, generation, c->name, command_shortcut(matches[i].index), index);
    g_free(index);
  }
  int plugin_matches = plugins_palette_query(t, text, generation);
  if (n == 0 && plugin_matches == 0)
    picker_message(t, generation, "No matching commands");
}

/* PickerChoose: runs the picked command (its index in the table, or a plugin's command). */
void command_palette_choose(App *t, const char *index)
{
  if (plugins_palette_choose(t, index))
    return;
  commands[atoi(index)].run(t);
}

/* Opens the command palette: Go to File's picker with "# " typed, as in the title bar's search. */
static void command_palette(App *t)
{
  quick_open_show_text(t, COMMANDS_PREFIX);
}
