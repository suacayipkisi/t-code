/*
 * The status bar at the bottom:
 *   left   git branch ("*" when there are changes; a click switches branches), clangd errors and warnings in the open files,
 *          a warning when clangd has no compile flags, Build and Run buttons and what they would build
 *   right  what a language server is busy with ("rust-analyzer: Indexing 45%"), "No LSP" when the current file's language server is not installed (a click shows how to
 *          install it), the current editor's indentation ("Spaces: 4", a click chooses the
 *          project's), cursor line and column, and the Keybinds button
 */
#include "window/statusbar.h"

#include "dialogs/keybinds.h"
#include "dialogs/lspinstall.h"
#include "editor/editorview.h"
#include "editor/tabs.h"
#include "git/gitstatus.h"
#include "git/scm/scm.h"
#include "lsp/client.h"
#include "popups/picker.h"
#include "project/session.h"
#include "project/run/run.h"
#include "window/palette.h"

#include <stdlib.h>
#include <string.h>

static const char *const css =
    /* The title bar is in window/theme.c. */
    ".tcode-status { background: var(--tc-frame); padding: 3px 6px; }"
    ".tcode-status button { min-height: 0; padding: 1px 8px; border-radius: 6px; background: none;"
    "  box-shadow: none; border: none; color: var(--tc-fg); }"
    ".tcode-status button:hover { background: color-mix(in srgb, var(--tc-fg) 12%, transparent); }"
    ".tcode-status label { color: var(--tc-fg); padding: 0 8px; }"
    ".tcode-status label.dim { color: var(--tc-fg-dim); }"
    ".tcode-status button.warning { color: var(--tc-warning); }";

/* How long a status_say() message stays before the Build / Run target comes back. */
#define SAY_MS 3000
/* At most this often the language servers' progress is shown again. */
#define PROGRESS_MS 150

static void status_show_progress(App *t);

void update_status(App *t)
{
  int n, errors = 0, warnings = 0;
  char *text;

  if (!t->status.build || t->closing)
    return;

  if (!t->status.say_id) { /* a message keeps its place for a while */
    text = project_summary(t);
    gtk_label_set_text(t->status.build, text);
    g_free(text);
  }

  n = tab_count(t);
  for (int i = 0; i < n; i++) {
    Tab *each = tab_nth(t, i);
    if (!each->compare.with) { /* old versions in a comparison do not count */
      errors += each->problems.errors;
      warnings += each->problems.warnings;
    }
  }
  text = g_strdup_printf("⊗ %d  ⚠ %d", errors, warnings);
  gtk_label_set_text(t->status.problems, text);
  g_free(text);

  const char *problem = NULL;
  text = lsp_flags_hint(t, &problem);
  if (text) {
    char *label = g_strdup_printf("⚠ %s", problem);
    gtk_button_set_label(GTK_BUTTON(t->status.flags), label);
    g_free(label);
  }
  gtk_widget_set_tooltip_text(t->status.flags, text);
  gtk_widget_set_visible(t->status.flags, text != NULL);
  g_free(text);

  const char *server = lsp_missing_server(current_tab(t));
  if (server) {
    text = g_strdup_printf("%s is not installed: click for the commands that install it", server);
    gtk_widget_set_tooltip_text(t->status.no_lsp, text);
    g_free(text);
  }
  gtk_widget_set_visible(t->status.no_lsp, server != NULL);

  status_show_progress(t);
  status_show_indent(t);
  status_show_position(t);
}

void status_show_indent(App *t)
{
  Tab *tab = current_tab(t);

  if (!t->status.indent || t->closing)
    return;
  if (tab && tab->kind == TAB_EDITOR && tab->view->width > 0) {
    char *text = g_strdup_printf("%s: %d", tab->view->tabs ? "Tab Size" : "Spaces", tab->view->width);
    char *tip = g_strdup_printf("Indentation from %s; click to choose this project's", tab->view->indent_from);
    gtk_button_set_label(GTK_BUTTON(t->status.indent), text);
    gtk_widget_set_tooltip_text(t->status.indent, tip);
    gtk_widget_set_visible(t->status.indent, TRUE);
    g_free(tip);
    g_free(text);
  } else {
    gtk_widget_set_visible(t->status.indent, FALSE);
  }
}

/* The indentation picker ---------------------------------------------------------------------- */

/* The choices: "s4" spaces, "t4" tabs, "auto" the files' and config files' own. */
static const struct {
  const char *payload, *label;
} indents[] = {
  { "s2", "Indent Using Spaces: 2" },
  { "s4", "Indent Using Spaces: 4" },
  { "s8", "Indent Using Spaces: 8" },
  { "t2", "Indent Using Tabs: 2" },
  { "t4", "Indent Using Tabs: 4" },
  { "t8", "Indent Using Tabs: 8" },
  { "auto", "Automatic: .editorconfig, .clang-format or the file's own" },
};

static void indent_query(App *t, const char *text, guint generation)
{
  for (size_t i = 0; i < G_N_ELEMENTS(indents); i++) {
    gboolean chosen = t->project.indent.width > 0 && indents[i].payload[0] == (t->project.indent.tabs ? 't' : 's') &&
                      atoi(indents[i].payload + 1) == t->project.indent.width;
    gboolean automatic = t->project.indent.width == 0 && strcmp(indents[i].payload, "auto") == 0;
    if (!*text || fuzzy_score(text, indents[i].label) >= 0)
      picker_add(t, generation, indents[i].label, chosen || automatic ? "current" : NULL, indents[i].payload);
  }
}

/* PickerChoose: the project's indentation, for its open editors and the ones opened later. */
static void indent_choose(App *t, const char *payload)
{
  int n = tab_count(t);

  t->project.indent.tabs = payload[0] == 't';
  t->project.indent.width = strcmp(payload, "auto") == 0 ? 0 : atoi(payload + 1);
  session_save_project_indent(t);
  for (int i = 0; i < n; i++)
    if (tab_nth(t, i)->kind == TAB_EDITOR)
      editor_view_apply_settings(tab_nth(t, i));
  status_show_indent(t);
}

void status_pick_indent(App *t)
{
  picker_open(t, "Indentation for this project", indent_query, indent_choose);
}

void status_show_position(App *t)
{
  Tab *tab = current_tab(t);

  if (tab && tab->kind == TAB_EDITOR && tab->cursor.line > 0) {
    char *text = g_strdup_printf("Ln %d, Col %d", tab->cursor.line, tab->cursor.column);
    gtk_label_set_text(t->status.position, text);
    g_free(text);
    gtk_widget_set_visible(GTK_WIDGET(t->status.position), TRUE);
  } else {
    gtk_widget_set_visible(GTK_WIDGET(t->status.position), FALSE);
  }
}

void start_status_updates(App *t)
{
  git_status_start(t);
  update_status(t);
}

void status_show_branch(App *t, const char *branch, gboolean dirty)
{
  if (!t->status.branch)
    return;
  if (branch) {
    char *text = g_strdup_printf("⎇ %s%s", branch, dirty ? "*" : "");
    gtk_label_set_text(t->status.branch, text);
    g_free(text);
  }
  gtk_widget_set_visible(GTK_WIDGET(t->status.branch), branch != NULL);
}

static gboolean said(gpointer data)
{
  App *t = data;

  t->status.say_id = 0;
  update_status(t);
  return G_SOURCE_REMOVE;
}

static void status_show_progress(App *t)
{
  char *text = lsp_progress(t);

  if (text)
    gtk_label_set_text(t->status.progress, text);
  gtk_widget_set_tooltip_text(GTK_WIDGET(t->status.progress), text);
  gtk_widget_set_visible(GTK_WIDGET(t->status.progress), text != NULL);
  g_free(text);
}

static gboolean show_progress(gpointer data)
{
  App *t = data;

  t->status.progress_id = 0;
  if (t->status.progress && !t->closing)
    status_show_progress(t);
  return G_SOURCE_REMOVE;
}

void status_progress_changed(App *t)
{
  if (t && t->status.progress && !t->closing && !t->status.progress_id)
    t->status.progress_id = g_timeout_add(PROGRESS_MS, show_progress, t);
}

void status_say(App *t, const char *message)
{
  if (!t || !t->status.build || t->closing)
    return;
  gtk_label_set_text(t->status.build, message ? message : "");
  g_clear_handle_id(&t->status.say_id, g_source_remove);
  t->status.say_id = g_timeout_add(SAY_MS, said, t);
}

/* Widgets ------------------------------------------------------------------------------------ */

static GtkLabel *status_label(GtkWidget *bar, gboolean dim)
{
  GtkWidget *label = gtk_label_new(NULL);

  if (dim)
    gtk_widget_add_css_class(label, "dim");
  gtk_box_append(GTK_BOX(bar), label);
  return GTK_LABEL(label);
}

/* A flat button on the bar that calls clicked(t). */
static GtkWidget *status_button(GtkWidget *bar, const char *text, const char *tooltip, GCallback clicked, App *t)
{
  GtkWidget *button = gtk_button_new_with_label(text);

  gtk_button_set_has_frame(GTK_BUTTON(button), FALSE);
  gtk_widget_set_focusable(button, FALSE);
  gtk_widget_set_tooltip_text(button, tooltip);
  g_signal_connect_swapped(button, "clicked", clicked, t);
  gtk_box_append(GTK_BOX(bar), button);
  return button;
}

static void on_flags_answer(GObject *dialog, GAsyncResult *result, gpointer data)
{
  if (gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(dialog), result, NULL) == 1)
    lsp_restart_clangd(data);
}

/* "No compile flags" / "No C headers": what clangd lacks and how to give it, with a button that
 * restarts it. */
static void on_flags(App *t)
{
  const char *problem = NULL;
  char *hint = lsp_flags_hint(t, &problem);
  static const char *const buttons[] = { "Close", "Restart clangd", NULL };
  GtkAlertDialog *dialog;

  if (!hint)
    return;
  dialog = gtk_alert_dialog_new(strcmp(problem, "No C headers") == 0 ? "The C library's headers are not installed"
                                                                     : "clangd has no compile flags");
  gtk_alert_dialog_set_detail(dialog, hint);
  gtk_alert_dialog_set_buttons(dialog, buttons);
  gtk_alert_dialog_set_cancel_button(dialog, 0);
  gtk_alert_dialog_set_default_button(dialog, 1);
  gtk_alert_dialog_choose(dialog, t->window, NULL, on_flags_answer, t);
  g_object_unref(dialog);
  g_free(hint);
}

/* "No LSP": how to install the current file's language server. */
static void on_no_lsp(App *t)
{
  const char *server = lsp_missing_server(current_tab(t));

  if (server)
    show_lsp_install(t, server);
}

/* The Build and Run buttons. */
static void on_build(App *t)
{
  build_project(t, FALSE);
}

static void on_run(App *t)
{
  build_project(t, TRUE);
}

GtkWidget *status_bar(App *t)
{
  GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

  palette_add_css(css);
  gtk_widget_add_css_class(bar, "tcode-status");

  t->status.branch = status_label(bar, FALSE);
  gtk_widget_set_visible(GTK_WIDGET(t->status.branch), FALSE);
  /* A click switches branches (or makes one). */
  GtkGesture *branch_click = gtk_gesture_click_new();
  g_signal_connect_swapped(branch_click, "released", G_CALLBACK(scm_switch_branch), NULL);
  gtk_widget_add_controller(GTK_WIDGET(t->status.branch), GTK_EVENT_CONTROLLER(branch_click));
  gtk_widget_set_cursor_from_name(GTK_WIDGET(t->status.branch), "pointer");
  gtk_widget_set_tooltip_text(GTK_WIDGET(t->status.branch), "Switch to another branch, or create one");
  t->status.problems = status_label(bar, FALSE);
  gtk_widget_set_tooltip_text(GTK_WIDGET(t->status.problems), "clangd errors and warnings in the open files");
  t->status.flags = status_button(bar, "⚠ No compile flags", NULL, G_CALLBACK(on_flags), t);
  gtk_widget_add_css_class(t->status.flags, "warning");
  gtk_widget_set_visible(t->status.flags, FALSE);
  status_button(bar, "⚙ Build", "Build (Ctrl+Shift+B)", G_CALLBACK(on_build), t);
  status_button(bar, "▶ Run", "Build and run, without debugging (Ctrl+F5)", G_CALLBACK(on_run), t);
  t->status.build = status_label(bar, TRUE);

  gtk_widget_set_hexpand(spacer, TRUE);
  gtk_box_append(GTK_BOX(bar), spacer);
  t->status.progress = status_label(bar, TRUE);
  gtk_label_set_ellipsize(t->status.progress, PANGO_ELLIPSIZE_END);
  gtk_label_set_max_width_chars(t->status.progress, 48);
  gtk_widget_set_visible(GTK_WIDGET(t->status.progress), FALSE);
  t->status.no_lsp = status_button(bar, "⚠ No LSP", NULL, G_CALLBACK(on_no_lsp), t);
  gtk_widget_add_css_class(t->status.no_lsp, "warning");
  gtk_widget_set_visible(t->status.no_lsp, FALSE);
  t->status.indent = status_button(bar, "", NULL, G_CALLBACK(status_pick_indent), t);
  gtk_widget_set_visible(t->status.indent, FALSE);
  t->status.position = status_label(bar, FALSE);
  status_button(bar, "⌨ Keybinds", "All keyboard shortcuts", G_CALLBACK(show_keybinds), t);
  return bar;
}
