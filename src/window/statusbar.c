/*
 * The status bar at the bottom:
 *   left   git branch ("*" when there are changes; a click switches branches), clangd errors and warnings in the open files,
 *          a warning when clangd has no compile flags, Build and Run buttons and what they would build
 *   right  what a language server is busy with ("rust-analyzer: Indexing 45%"), "No LSP" when the current file's language server is not installed (a click shows how to
 *          install it), the current editor's indentation ("Spaces: 4", a click chooses the
 *          project's), cursor line and column, and the Keybinds button
 * Each item sits in its own slot, which right-clicking the bar turns off and on (settings.status_off; the
 * Build and Run buttons follow settings.build_buttons); a plugin's items one by one.
 */
#include "window/statusbar.h"

#include "application/shellenv.h"
#include "dialogs/keybinds.h"
#include "dialogs/lspinstall.h"
#include "editor/editorview.h"
#include "editor/tabs.h"
#include "git/gitstatus.h"
#include "git/scm/scm.h"
#include "glib-object.h"
#include "lsp/client/client.h"
#include "popups/picker.h"
#include "project/session.h"
#include "project/run/run.h"
#include "window/palette.h"
#include "window/window.h"

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
    ".tcode-status button.warning { color: var(--tc-warning); }"
    /* Icons before the text: one gap between them, the label's own padding goes. */
    ".tcode-status image.icon { -gtk-icon-size: 14px; color: var(--tc-fg); }"
    ".tcode-status button > box > label { padding: 0; }"
    ".tcode-status button.warning label, .tcode-status button.warning image.icon { color: var(--tc-warning); }"
    ".tcode-status .problems { padding: 0 8px; }"
    ".tcode-status .problems label { padding: 0 6px 0 0; }";

/* How long a status_say() message stays before the Build / Run target comes back. */
#define SAY_MS 3000
/* At most this often the language servers' progress is shown again. */
#define PROGRESS_MS 150

static void status_show_progress(App *t);

/* "No LSP" shown: the log says once per server where it was looked for (not while the login shell's
 * PATH is still coming: it may be found then). */
static void log_missing(const char *server)
{
  static GHashTable *logged;

  if (shell_env_loading())
    return;
  if (!logged)
    logged = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  if (g_hash_table_contains(logged, server))
    return;
  g_hash_table_add(logged, g_strdup(server)); /* a plugin's name goes when the plugins load again */
  g_message("t-code: %s not found: not on PATH (%s) nor in ~/.local/bin, ~/.cargo/bin or Go's bin folder", server,
            g_getenv("PATH") ? g_getenv("PATH") : "");
}

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
  text = g_strdup_printf("%d", errors);
  gtk_label_set_text(t->status.errors, text);
  g_free(text);
  text = g_strdup_printf("%d", warnings);
  gtk_label_set_text(t->status.warnings, text);
  g_free(text);

  const char *problem = NULL;
  text = lsp_flags_hint(t, &problem);
  if (text) {
    gtk_label_set_text(g_object_get_data(G_OBJECT(t->status.flags), "t-code-label"), problem);
  }
  gtk_widget_set_tooltip_text(t->status.flags, text);
  gtk_widget_set_visible(t->status.flags, text != NULL);
  g_free(text);

  const char *server = lsp_missing_server(current_tab(t));
  gboolean was_missing = gtk_widget_get_visible(t->status.no_lsp);
  if (server) {
    log_missing(server);
    text = g_strdup_printf("%s is not installed: click for the commands that install it", server);
    gtk_widget_set_tooltip_text(t->status.no_lsp, text);
    g_free(text);
  }
  gtk_widget_set_visible(t->status.no_lsp, server != NULL);
  /* Installed (or put on PATH) while t-code runs: the notice goes, and the server starts for the open
   * files instead of only after a restart. */
  if (was_missing && !server)
    lsp_open_installed(t);

  status_show_progress(t);
  status_show_font_scale(t);
  status_show_indent(t);
  status_show_position(t);
}

void status_show_font_scale(App *t)
{
  if (!t->status.font_scale)
    return;

  double current_font_scale = t->settings.font_scale;
  int fs = (int)(current_font_scale * 100.0 + 0.5);
  if (fs == 100) {
    gtk_widget_set_visible(GTK_WIDGET(t->status.font_scale), FALSE);
  } else {
    char *text = g_strdup_printf("Text Scale: %d%%", fs);
    char *tip = g_strdup_printf("CTRL + OR CTRL -");
    gtk_button_set_label(GTK_BUTTON(t->status.font_scale), text);
    gtk_widget_set_tooltip_text(GTK_WIDGET(t->status.font_scale), tip);
    g_free(text);
    g_free(tip);
    gtk_widget_set_visible(GTK_WIDGET(t->status.font_scale), TRUE);
  }
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
  char *text = lsp_progress(t), *failure = text ? lsp_failure(t) : NULL;

  if (text)
    gtk_label_set_text(t->status.progress, text);
  gtk_widget_set_tooltip_text(GTK_WIDGET(t->status.progress), failure ? failure : text);
  g_free(failure);
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

/* An icon (t-code's own, see window/theme.h) on the bar. */
static void status_icon(GtkWidget *box, const char *icon)
{
  GtkWidget *image = gtk_image_new_from_icon_name(icon);

  gtk_widget_add_css_class(image, "icon");
  gtk_box_append(GTK_BOX(box), image);
}

/* A flat button on the bar that calls clicked(t): its text, after an icon unless icon is NULL (then
 * gtk_button_set_label() changes the text; with an icon, the label is its "t-code-label" data). */
static GtkWidget *status_button(GtkWidget *bar, const char *icon, const char *text, const char *tooltip,
                                GCallback clicked, App *t)
{
  GtkWidget *button = icon ? gtk_button_new() : gtk_button_new_with_label(text);

  if (icon) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4), *label = gtk_label_new(text);
    status_icon(box, icon);
    gtk_box_append(GTK_BOX(box), label);
    gtk_button_set_child(GTK_BUTTON(button), box);
    g_object_set_data(G_OBJECT(button), "t-code-label", label);
  }

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

/* A plugin's item: the label in t->status.plugins named key. */
static GtkLabel *plugin_item(App *t, const char *key)
{
  for (GtkWidget *child = t->status.plugins ? gtk_widget_get_first_child(GTK_WIDGET(t->status.plugins)) : NULL; child;
       child = gtk_widget_get_next_sibling(child))
    if (g_strcmp0(gtk_widget_get_name(child), key) == 0)
      return GTK_LABEL(child);
  return NULL;
}

void status_plugin_set(App *t, const char *key, const char *text)
{
  GtkLabel *label = plugin_item(t, key);

  if (!t->status.plugins)
    return;
  if (!label) {
    label = status_label(GTK_WIDGET(t->status.plugins), FALSE);
    gtk_widget_set_name(GTK_WIDGET(label), key);
    gtk_label_set_ellipsize(label, PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(label, 40);
    char *id = g_strconcat("plugin:", key, NULL);
    gtk_widget_set_visible(GTK_WIDGET(label), !status_item_off(t, id));
    g_free(id);
  }
  gtk_label_set_text(label, text);
  gtk_widget_set_tooltip_text(GTK_WIDGET(label), text);
}

void status_plugin_clear(App *t, const char *key)
{
  GtkLabel *label = plugin_item(t, key);

  if (label)
    gtk_box_remove(t->status.plugins, GTK_WIDGET(label));
}

const char *status_plugin_text(App *t, const char *key)
{
  GtkLabel *label = plugin_item(t, key);

  return label ? gtk_label_get_text(label) : NULL;
}

static void on_font_scale_reset(App *t)
{
  set_font_scale(t, 1.0);
}

/* The bar's items, in its order: each one's id (settings.status_off) and its name in the right-click
 * menu. */
static const struct {
  const char *id, *label;
} items[] = {
  { "branch", "Git Branch" },
  { "problems", "Errors and Warnings" },
  { "flags", "No Compile Flags Warning" },
  { "build", "Build and Run Buttons" },
  { "build-status", "Build Target and Messages" },
  { "plugins", NULL }, /* each plugin item on its own */
  { "progress", "Language Server Progress" },
  { "no-lsp", "No LSP Warning" },
  { "font-scale", "Text Size" },
  { "indent", "Indentation" },
  { "position", "Line and Column" },
  { "keybinds", "Keybinds Button" },
};

gboolean status_item_off(App *t, const char *id)
{
  return t->settings.status_off && g_strv_contains((const char *const *)t->settings.status_off, id);
}

/* A box on the bar holding item id's widgets: they show or hide themselves, the slot follows the
 * setting. */
static GtkWidget *status_slot(GtkWidget *bar, const char *id)
{
  GtkWidget *slot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

  g_object_set_data(G_OBJECT(slot), "t-code-status-item", (gpointer)id);
  gtk_box_append(GTK_BOX(bar), slot);
  return slot;
}

void status_bar_apply(App *t)
{
  if (!t->status.bar)
    return;
  gtk_widget_set_visible(t->status.bar, t->settings.status_bar);
  for (GtkWidget *child = gtk_widget_get_first_child(t->status.bar); child;
       child = gtk_widget_get_next_sibling(child)) {
    const char *id = g_object_get_data(G_OBJECT(child), "t-code-status-item");
    if (id)
      gtk_widget_set_visible(child, strcmp(id, "build") == 0 ? t->settings.build_buttons : !status_item_off(t, id));
  }
  for (GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(t->status.plugins)); child;
       child = gtk_widget_get_next_sibling(child)) {
    char *id = g_strconcat("plugin:", gtk_widget_get_name(child), NULL);
    gtk_widget_set_visible(child, !status_item_off(t, id));
    g_free(id);
  }
}

/* status.item: turns the item of that id off or on. */
static void on_item_action(GSimpleAction *action, GVariant *param G_GNUC_UNUSED, gpointer data)
{
  App *t = data;
  const char *id = g_object_get_data(G_OBJECT(action), "t-code-id");
  GStrvBuilder *builder = g_strv_builder_new();
  gboolean was_off = status_item_off(t, id);

  for (int i = 0; t->settings.status_off && t->settings.status_off[i]; i++)
    if (strcmp(t->settings.status_off[i], id) != 0)
      g_strv_builder_add(builder, t->settings.status_off[i]);
  if (!was_off)
    g_strv_builder_add(builder, id);
  g_strfreev(t->settings.status_off);
  t->settings.status_off = g_strv_builder_end(builder);
  g_strv_builder_unref(builder);
  status_bar_apply(t);
  session_save_settings(t);
}

/* One check item of the menu for id (its action in group). */
static void add_item_check(App *t, GMenu *menu, GSimpleActionGroup *group, int n, const char *id, const char *label)
{
  char name[32], detailed[48];
  GSimpleAction *action;

  g_snprintf(name, sizeof(name), "item-%d", n);
  action = g_simple_action_new_stateful(name, NULL, g_variant_new_boolean(!status_item_off(t, id)));
  g_object_set_data_full(G_OBJECT(action), "t-code-id", g_strdup(id), g_free);
  g_signal_connect(action, "activate", G_CALLBACK(on_item_action), t);
  g_action_map_add_action(G_ACTION_MAP(group), G_ACTION(action));
  g_object_unref(action);
  g_snprintf(detailed, sizeof(detailed), "status.%s", name);
  g_menu_append(menu, label, detailed);
}

/* Right-click on the bar: a check per item (each plugin item too, as it is now), and the bar itself. */
static void on_bar_right_click(GtkGestureClick *gesture, int n_press G_GNUC_UNUSED, double x, double y, App *t)
{
  GtkWidget *bar = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
  GtkPopover *pop = g_object_get_data(G_OBJECT(bar), "t-code-items-pop");
  GSimpleActionGroup *group = g_simple_action_group_new();
  GMenu *menu = g_menu_new(), *section = g_menu_new(), *rest = g_menu_new();
  int n = 0;

  for (size_t i = 0; i < G_N_ELEMENTS(items); i++) {
    if (strcmp(items[i].id, "build") == 0) {
      g_menu_append(section, items[i].label, "win.ui-build-buttons");
      continue;
    }
    if (strcmp(items[i].id, "plugins") != 0) {
      add_item_check(t, section, group, n++, items[i].id, items[i].label);
      continue;
    }
    for (GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(t->status.plugins)); child;
         child = gtk_widget_get_next_sibling(child)) {
      char *id = g_strconcat("plugin:", gtk_widget_get_name(child), NULL);
      char *label = g_strdup_printf("Plugin: %s", gtk_widget_get_name(child));
      add_item_check(t, section, group, n++, id, label);
      g_free(label);
      g_free(id);
    }
  }
  g_menu_append_section(menu, NULL, G_MENU_MODEL(section));
  g_menu_append(rest, "Status Bar", "win.ui-status-bar");
  g_menu_append_section(menu, NULL, G_MENU_MODEL(rest));
  gtk_widget_insert_action_group(bar, "status", G_ACTION_GROUP(group));
  if (!pop) {
    pop = GTK_POPOVER(gtk_popover_menu_new_from_model(NULL));
    gtk_popover_set_has_arrow(pop, FALSE);
    gtk_widget_set_parent(GTK_WIDGET(pop), bar);
    g_object_set_data(G_OBJECT(bar), "t-code-items-pop", pop);
  }
  gtk_popover_menu_set_menu_model(GTK_POPOVER_MENU(pop), G_MENU_MODEL(menu));
  gtk_popover_set_pointing_to(pop, &(const GdkRectangle){ (int)x, (int)y, 1, 1 });
  gtk_popover_popup(pop);
  g_object_unref(rest);
  g_object_unref(section);
  g_object_unref(menu);
  g_object_unref(group);
}

GtkWidget *status_bar(App *t)
{
  GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  GtkGesture *right = gtk_gesture_click_new();

  palette_add_css(css);
  gtk_widget_add_css_class(bar, "tcode-status");
  t->status.bar = bar;
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(right), GDK_BUTTON_SECONDARY);
  g_signal_connect(right, "pressed", G_CALLBACK(on_bar_right_click), t);
  gtk_widget_add_controller(bar, GTK_EVENT_CONTROLLER(right));

  t->status.branch = status_label(status_slot(bar, "branch"), FALSE);
  gtk_widget_set_visible(GTK_WIDGET(t->status.branch), FALSE);
  /* A click switches branches (or makes one). */
  GtkGesture *branch_click = gtk_gesture_click_new();
  g_signal_connect_swapped(branch_click, "released", G_CALLBACK(scm_switch_branch), NULL);
  gtk_widget_add_controller(GTK_WIDGET(t->status.branch), GTK_EVENT_CONTROLLER(branch_click));
  gtk_widget_set_cursor_from_name(GTK_WIDGET(t->status.branch), "pointer");
  gtk_widget_set_tooltip_text(GTK_WIDGET(t->status.branch), "Switch to another branch, or create one");
  GtkWidget *problems = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_widget_add_css_class(problems, "problems");
  gtk_widget_set_tooltip_text(problems, "clangd errors and warnings in the open files");
  status_icon(problems, "tcode-error-symbolic");
  t->status.errors = status_label(problems, FALSE);
  status_icon(problems, "tcode-warning-symbolic");
  t->status.warnings = status_label(problems, FALSE);
  gtk_box_append(GTK_BOX(status_slot(bar, "problems")), problems);
  t->status.flags = status_button(status_slot(bar, "flags"), "tcode-warning-symbolic", "No compile flags", NULL,
                                  G_CALLBACK(on_flags), t);
  gtk_widget_add_css_class(t->status.flags, "warning");
  gtk_widget_set_visible(t->status.flags, FALSE);
  GtkWidget *build = status_slot(bar, "build");
  t->status.build_buttons[0] =
      status_button(build, "tcode-build-symbolic", "Build", "Build (Ctrl+Shift+B)", G_CALLBACK(on_build), t);
  t->status.build_buttons[1] = status_button(build, "tcode-run-symbolic", "Run",
                                             "Build and run, without debugging (Ctrl+F5)", G_CALLBACK(on_run), t);
  t->status.build = status_label(status_slot(bar, "build-status"), TRUE);

  gtk_widget_set_hexpand(spacer, TRUE);
  gtk_box_append(GTK_BOX(bar), spacer);
  t->status.plugins = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0));
  gtk_box_append(GTK_BOX(bar), GTK_WIDGET(t->status.plugins));
  t->status.progress = status_label(status_slot(bar, "progress"), TRUE);
  gtk_label_set_ellipsize(t->status.progress, PANGO_ELLIPSIZE_END);
  gtk_label_set_max_width_chars(t->status.progress, 48);
  gtk_widget_set_visible(GTK_WIDGET(t->status.progress), FALSE);
  t->status.no_lsp =
      status_button(status_slot(bar, "no-lsp"), "tcode-warning-symbolic", "No LSP", NULL, G_CALLBACK(on_no_lsp), t);
  gtk_widget_add_css_class(t->status.no_lsp, "warning");
  gtk_widget_set_visible(t->status.no_lsp, FALSE);
  t->status.font_scale = status_button(status_slot(bar, "font-scale"), NULL, "", "Reset Text Size (Ctrl + 0)",
                                       G_CALLBACK(on_font_scale_reset), t);
  gtk_widget_set_visible(t->status.font_scale, FALSE);
  t->status.indent = status_button(status_slot(bar, "indent"), NULL, "", NULL, G_CALLBACK(status_pick_indent), t);
  gtk_widget_set_visible(t->status.indent, FALSE);
  t->status.position = status_label(status_slot(bar, "position"), FALSE);
  status_button(status_slot(bar, "keybinds"), "tcode-keyboard-symbolic", "Keybinds", "All keyboard shortcuts",
                G_CALLBACK(show_keybinds), t);
  return bar;
}
