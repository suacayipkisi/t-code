/*
 * The editor page's files (src/editor and src/lsp), included by the tests that need their static
 * functions, with stand-ins for the rest of t-code they call (tabs, the status bar, the picker).
 */
#pragma once

#include "editor/text/detect.c"
#include "window/palette.c"
#include "editor/text/blocks.c"
#include "editor/gutter/diffgutter.c"
#include "editor/gutter/folding.c"
#include "editor/gutter/breakpoints.c"
#include "editor/decor/sticky.c"
#include "git/linediff.c"
#include "editor/editing/editing.c"
#include "editor/editorview.c"
#include "editor/recovery.c"
#include "editor/editing/findbar.c"
#include "editor/text/indentconf.c"
#include "editor/decor/guides.c"
#include "editor/decor/occurrences.c"
#include "editor/editing/multicursor.c"
#include "editor/editing/snippets.c"
#include "editor/decor/blame.c"
#include "editor/decor/breadcrumbs.c"
#include "lsp/channel.c"
#include "lsp/servers.c"
#include "lsp/json.c"
#include "lsp/client/client.c"
#include "lsp/client/documents.c"
#include "lsp/client/positions.c"
#include "lsp/client/server.c"
#include "lsp/compiledb.c"
#include "lsp/inline/completion.c"
#include "lsp/inline/diagnostics.c"
#include "lsp/problems.c"

#include "lsp/symbols/edits.c"
#include "lsp/inline/hover.c"
#include "lsp/inline/inlay.c"
#include "lsp/symbols/symbols.c"
#include "lsp/symbols/navigate.c"
#include "lsp/inline/semantic.c"
#include "lsp/inline/signature.c"
#include "project/langenv.c"

static PickerQuery picker_test_query;
static PickerChoose picker_test_choose;
static char *picker_test_label;
static char *picker_test_detail;
static char *picker_test_payload;
static char *picker_test_message;
static Tab *picker_test_tab;
static char *opened_path;
static int opened_line, opened_column;
static Tab *page_test_tabs[4]; /* the App's editors, for tab_count() / tab_nth() (the Problems panel) */
static int page_test_tab_count;

static void G_GNUC_UNUSED picker_test_reset(void)
{
  g_clear_pointer(&picker_test_label, g_free);
  g_clear_pointer(&picker_test_detail, g_free);
  g_clear_pointer(&picker_test_payload, g_free);
  g_clear_pointer(&picker_test_message, g_free);
  g_clear_pointer(&opened_path, g_free);
  picker_test_query = NULL;
  picker_test_choose = NULL;
  opened_line = opened_column = 0;
}
static void G_GNUC_UNUSED picker_test_clear_results(void)
{
  g_clear_pointer(&picker_test_label, g_free);
  g_clear_pointer(&picker_test_detail, g_free);
  g_clear_pointer(&picker_test_payload, g_free);
  g_clear_pointer(&picker_test_message, g_free);
}

/* clang-format off */
void update_status(App *t G_GNUC_UNUSED) {}
void debug_breakpoints_changed(App *t G_GNUC_UNUSED, const char *path G_GNUC_UNUSED) {}
void debug_hover_attach(Tab *tab G_GNUC_UNUSED) {}
gboolean disassembly_is(App *t G_GNUC_UNUSED, Tab *tab G_GNUC_UNUSED) { return FALSE; }
gboolean disassembly_breakpoint_at(App *t G_GNUC_UNUSED, int line G_GNUC_UNUSED) { return FALSE; }
void disassembly_toggle_breakpoint(App *t G_GNUC_UNUSED, int line G_GNUC_UNUSED) {}
void show_sidebar_page(App *t G_GNUC_UNUSED, const char *page G_GNUC_UNUSED) {}
void status_say(App *t G_GNUC_UNUSED, const char *message G_GNUC_UNUSED) {}
void status_progress_changed(App *t G_GNUC_UNUSED) {}
void outline_symbols_changed(Tab *tab G_GNUC_UNUSED) {}
void outline_cursor_moved(Tab *tab G_GNUC_UNUSED) {}
void files_problems_changed(App *t G_GNUC_UNUSED) {}
/* clang-format on */

/* The project's picked toolchain (toolchain.c's stand-in): set by the tests. */
static const char *toolchain_test_python, *toolchain_test_features, *toolchain_test_target;
char *toolchain_python(App *t G_GNUC_UNUSED, const char *root G_GNUC_UNUSED)
{
  return g_strdup(toolchain_test_python);
}
char *toolchain_rust_features(App *t G_GNUC_UNUSED)
{
  return g_strdup(toolchain_test_features);
}
char *toolchain_rust_target(App *t G_GNUC_UNUSED)
{
  return g_strdup(toolchain_test_target);
}
/* clang-format off */

void status_show_indent(App *t G_GNUC_UNUSED) {}
int tab_count(App *t G_GNUC_UNUSED) { return page_test_tab_count; }
Tab *current_tab(App *t G_GNUC_UNUSED) { return page_test_tab_count ? page_test_tabs[0] : NULL; }
Tab *tab_nth(App *t G_GNUC_UNUSED, int index) { return page_test_tabs[index]; }
Tab *find_tab(App *t G_GNUC_UNUSED, TabKind kind, const char *path)
{
  return picker_test_tab && picker_test_tab->kind == kind && g_strcmp0(picker_test_tab->path, path) == 0
             ? picker_test_tab
             : NULL;
}
void select_tab(Tab *tab G_GNUC_UNUSED) {}
void open_file_at(App *t G_GNUC_UNUSED, const char *path, int line, int column)
{
  g_free(opened_path);
  opened_path = g_strdup(path);
  opened_line = line;
  opened_column = column;
}
void editor_goto(Tab *tab G_GNUC_UNUSED, int line G_GNUC_UNUSED, int column G_GNUC_UNUSED) {}
void open_file(App *t G_GNUC_UNUSED, const char *path G_GNUC_UNUSED) {}
void run_command(App *t G_GNUC_UNUSED, const char *name G_GNUC_UNUSED) {}
void save_editor(Tab *tab G_GNUC_UNUSED) {}
void refresh_branch(App *t G_GNUC_UNUSED) {}
void picker_open(App *t G_GNUC_UNUSED, const char *placeholder G_GNUC_UNUSED, PickerQuery query, PickerChoose choose)
{
  picker_test_query = query;
  picker_test_choose = choose;
}
void picker_add(App *t G_GNUC_UNUSED, guint generation G_GNUC_UNUSED, const char *label, const char *detail,
                const char *payload)
{
  g_free(picker_test_label);
  g_free(picker_test_detail);
  g_free(picker_test_payload);
  picker_test_label = g_strdup(label);
  picker_test_detail = g_strdup(detail);
  picker_test_payload = g_strdup(payload);
}
void picker_message(App *t G_GNUC_UNUSED, guint generation G_GNUC_UNUSED, const char *text)
{
  g_free(picker_test_message);
  picker_test_message = g_strdup(text);
}
int fuzzy_score(const char *query G_GNUC_UNUSED, const char *text G_GNUC_UNUSED) { return 0; }
/* clang-format on */
