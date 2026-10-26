/*
 * The Files panel: a GtkListView over a GtkTreeListModel whose folders are files/folder.c lists
 * (filtered: dotfiles; sorted: folders first). A folder is read and watched only while it is open;
 * GtkTreeListModel frees its list when it closes. Git colors come from one `git status` per change
 * (files/gitmarks.c). The names of open files with errors or warnings (and of the folders holding
 * them) are drawn in those colors instead. A row's widgets are in rows.c, file operations, the
 * right-click menu and the keys in actions.c (on files/fileops.c).
 */
#include "files/files.h"

#include "editor/tabs.h"
#include "files/fileops.h"
#include "files/folder.h"
#include "files/gitmarks.h"
#include "files/icons.h"
#include "files/opentabs.h"
#include "files/panel.h"
#include "window/palette.h"

#include <string.h>

#define GIT_DELAY_MS 250 /* changes come in bursts (a build, a save, a checkout): read git status once */
#define MESSAGE_MS 4000  /* how long a message stays at the bottom */
#define REVEAL_MS 2000   /* how long a file to select is waited for (its folders load first) */

FilesPanel files;

/* Paths and rows ----------------------------------------------------------------------------- */

static char *row_path(GtkTreeListRow *row)
{
  GFileInfo *info = info_of(row);
  char *path = info_path(info);

  g_object_unref(info);
  return path;
}

/* The selected row (a new reference), or NULL. */
GtkTreeListRow *files_selected_row(void)
{
  GtkTreeListRow *row = gtk_single_selection_get_selected_item(files.selection);

  return row ? g_object_ref(row) : NULL;
}

static char *selected_path(void)
{
  GtkTreeListRow *row = files_selected_row();
  char *path = row ? row_path(row) : NULL;

  if (row)
    g_object_unref(row);
  return path;
}

void files_select(guint position)
{
  gtk_list_view_scroll_to(files.list, position, GTK_LIST_SCROLL_SELECT | GTK_LIST_SCROLL_FOCUS, NULL);
}

/* The folder new files go into: path itself when it is a folder, else its folder. */
char *files_folder_of(const char *path)
{
  return g_file_test(path, G_FILE_TEST_IS_DIR) ? g_strdup(path) : g_path_get_dirname(path);
}

/* Messages ----------------------------------------------------------------------------------- */

static gboolean hide_message(gpointer data G_GNUC_UNUSED)
{
  files.message_timer = 0;
  gtk_widget_set_visible(GTK_WIDGET(files.message), FALSE);
  return G_SOURCE_REMOVE;
}

/* A line under the tree for a few seconds; red when error. */
void files_say(gboolean error, const char *text)
{
  gtk_label_set_text(files.message, text);
  if (error)
    gtk_widget_add_css_class(GTK_WIDGET(files.message), "error");
  else
    gtk_widget_remove_css_class(GTK_WIDGET(files.message), "error");
  gtk_widget_set_visible(GTK_WIDGET(files.message), TRUE);
  g_clear_handle_id(&files.message_timer, g_source_remove);
  files.message_timer = g_timeout_add(MESSAGE_MS, hide_message, NULL);
}

void files_say_error(GError *error)
{
  files_say(TRUE, error->message);
  g_error_free(error);
}

/* Drawing a row ------------------------------------------------------------------------------ */

static void set_colored(GtkWidget *label, const char *text, const char *color)
{
  char *markup = g_markup_printf_escaped("<span foreground=\"%s\">%s</span>", color, text);

  gtk_label_set_markup(GTK_LABEL(label), markup);
  g_free(markup);
}

/* A git state's color in the color theme (gitmarks.c has Dark Classic's). */
static const char *git_color(char state)
{
  switch (state) {
    case 'M': return palette_hex(PAL_GIT_MODIFIED);
    case 'A': return palette_hex(PAL_GIT_ADDED);
    case 'U': return palette_hex(PAL_GIT_UNTRACKED);
    case 'D': return palette_hex(PAL_GIT_DELETED);
    case 'C': return palette_hex(PAL_GIT_CONFLICT);
    case 'I': return palette_hex(PAL_GIT_IGNORED);
    default: return palette_hex(PAL_FG);
  }
}

enum { PROBLEM_WARNING = 1, PROBLEM_ERROR };

/* The name's color: its problems' over its git state's. */
static const char *name_color(const char *path, char state)
{
  switch (files.problems ? GPOINTER_TO_INT(g_hash_table_lookup(files.problems, path)) : 0) {
    case PROBLEM_ERROR: return palette_hex(PAL_ERROR);
    case PROBLEM_WARNING: return palette_hex(PAL_WARNING);
    default: return git_color(state);
  }
}

/* Icon, name in its problems' or git color (dimmed when cut) and the git letter of a file. */
void files_paint(FileRow *r)
{
  const char *name = strrchr(r->path, '/') + 1;
  gboolean open = r->dir && gtk_tree_list_row_get_expanded(r->row);
  gboolean cut = files.clip_cut && g_strcmp0(files.clip, r->path) == 0;
  char state = git_marks_state(files.marks, r->path);
  const FileIcon *icon = file_icon(name, r->dir, open);
  const char *badge = r->dir ? NULL : git_marks_badge(state);

  set_colored(r->icon, icon->glyph, cut ? palette_hex(PAL_FG_FAINT) : icon->color);
  set_colored(r->name, name, cut ? palette_hex(PAL_FG_FAINT) : name_color(r->path, state));
  gtk_widget_set_visible(r->badge, badge != NULL);
  if (badge)
    set_colored(r->badge, badge, git_color(state));
}

void files_paint_all(void)
{
  for (guint i = 0; i < files.rows->len; i++)
    files_paint(g_ptr_array_index(files.rows, i));
}

/* The folder model ------------------------------------------------------------------------------ */

/* Build output and caches churn while compiling: they are not watched (R reads them again). */
static gboolean is_busy_folder(const char *path)
{
  static const char *const names[] = { "build", "target", "node_modules", ".git", ".cache" };
  char **parts = g_strsplit(files_relative(path, files.app->project.root), "/", -1);
  gboolean busy = FALSE;

  for (int i = 0; parts[i] && !busy; i++)
    for (size_t j = 0; j < G_N_ELEMENTS(names) && !busy; j++)
      busy = strcmp(parts[i], names[j]) == 0;
  g_strfreev(parts);
  return busy;
}

/* A folder's rows: its entries, filtered and sorted. They are read only when files_load() is called: GTK
 * also asks for a folder's model just to know whether its row gets an arrow, and reading every
 * folder shown for that would be wasted. */
static GListModel *folder_model(const char *path)
{
  GListModel *entries = folder_new(path, !is_busy_folder(path));
  GtkFilterListModel *filtered = gtk_filter_list_model_new(entries, g_object_ref(files.filter));
  GtkSortListModel *sorted = gtk_sort_list_model_new(G_LIST_MODEL(filtered), g_object_ref(files.sorter));

  g_object_set_data(G_OBJECT(sorted), "entries", entries);
  return G_LIST_MODEL(sorted);
}

static GListModel *entries_of(GListModel *model)
{
  return g_object_get_data(G_OBJECT(model), "entries");
}

/* Reads an open folder, the first time it is open. */
void files_load(GtkTreeListRow *row)
{
  GListModel *children = gtk_tree_list_row_get_children(row);

  if (children)
    folder_load(entries_of(children));
}

static GListModel *create_children(gpointer item, gpointer data G_GNUC_UNUSED)
{
  GFileInfo *info = item;
  char *path;
  GListModel *model;

  if (!info_is_dir(info))
    return NULL;
  path = info_path(info);
  model = folder_model(path);
  g_free(path);
  return model;
}

static gboolean filter_shown(gpointer item, gpointer data G_GNUC_UNUSED)
{
  return files_shown(g_file_info_get_name(item), files.show_hidden);
}

static int sort_rows(gconstpointer a, gconstpointer b, gpointer data G_GNUC_UNUSED)
{
  GFileInfo *x = (GFileInfo *)a, *y = (GFileInfo *)b;

  return files_compare(g_file_info_get_name(x), info_is_dir(x), g_file_info_get_name(y), info_is_dir(y));
}

/* Opens the folders that were open (their lists come back closed when read again), the folders on the
 * way to files.reveal, loads what is open, and selects files.reveal once it shows. */
static gboolean settle(gpointer data G_GNUC_UNUSED)
{
  files.settle_idle = 0;
  if (files.reveal && g_get_monotonic_time() > files.reveal_until)
    g_clear_pointer(&files.reveal, g_free);
  for (guint i = 0; i < g_list_model_get_n_items(G_LIST_MODEL(files.tree)); i++) {
    GtkTreeListRow *row = gtk_tree_list_model_get_row(files.tree, i);
    GFileInfo *info = info_of(row);
    char *path = info_path(info);

    if (info_is_dir(info)) {
      gboolean on_the_way = files.reveal && files_inside(files.reveal, path) && strcmp(files.reveal, path) != 0;
      if (!gtk_tree_list_row_get_expanded(row) && (on_the_way || g_hash_table_contains(files.expanded, path))) {
        g_hash_table_add(files.expanded, g_strdup(path));
        gtk_tree_list_row_set_expanded(row, TRUE);
      }
      if (gtk_tree_list_row_get_expanded(row))
        files_load(row);
    }
    if (files.reveal && strcmp(files.reveal, path) == 0) {
      g_clear_pointer(&files.reveal, g_free);
      files_select(i);
    }
    g_free(path);
    g_object_unref(info);
    g_object_unref(row);
  }
  return G_SOURCE_REMOVE;
}

static void settle_later(void)
{
  if (!files.settle_idle)
    files.settle_idle = g_idle_add(settle, NULL);
}

/* Selects path once it shows (its folders open on the way). */
void files_show_path(const char *path)
{
  g_set_str(&files.reveal, path);
  files.reveal_until = g_get_monotonic_time() + REVEAL_MS * 1000;
  settle_later();
}

void files_reveal(App *t G_GNUC_UNUSED, const char *path)
{
  files_show_path(path);
}

static void on_tree_changed(GListModel *model G_GNUC_UNUSED, guint position G_GNUC_UNUSED, guint removed G_GNUC_UNUSED,
                            guint added, gpointer data G_GNUC_UNUSED)
{
  if (added)
    settle_later();
}

/* A new tree of t->project.root; open folders stay open, and with keep_selection the selected file selected. */
void files_build_tree(gboolean keep_selection)
{
  App *t = files.app;
  char *keep = keep_selection ? selected_path() : NULL;
  char *name = g_path_get_basename(t->project.root);
  GtkTreeListModel *old = files.tree;
  GListModel *top = folder_model(t->project.root);

  folder_load(entries_of(top));
  files.tree = gtk_tree_list_model_new(top, FALSE, FALSE, create_children, NULL, NULL);
  g_signal_connect(files.tree, "items-changed", G_CALLBACK(on_tree_changed), NULL);
  gtk_single_selection_set_model(files.selection, G_LIST_MODEL(files.tree));
  if (old) {
    g_signal_handlers_disconnect_by_func(old, on_tree_changed, NULL);
    g_object_unref(old);
  }
  gtk_label_set_text(files.title, name);
  if (keep)
    files_show_path(keep);
  g_free(keep);
  g_free(name);
}

/* Git colors --------------------------------------------------------------------------------- */

static void on_git_status(GObject *source, GAsyncResult *result, gpointer data G_GNUC_UNUSED)
{
  GBytes *out = NULL;
  GError *error = NULL;

  if (!g_subprocess_communicate_finish(G_SUBPROCESS(source), result, &out, NULL, &error)) {
    gboolean cancelled = g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_error_free(error);
    if (cancelled)
      return; /* files may be gone (shutdown), or a newer one runs */
  }
  g_clear_object(&files.git_cancel);
  g_clear_pointer(&files.marks, g_hash_table_unref);
  if (out && g_subprocess_get_successful(G_SUBPROCESS(source)) && files.app->project.git_top) {
    gsize len;
    const char *text = g_bytes_get_data(out, &len);
    files.marks = git_marks_parse(files.app->project.git_top, text, len);
  }
  if (out)
    g_bytes_unref(out);
  files_paint_all();
}

static gboolean read_git(gpointer data G_GNUC_UNUSED)
{
  App *t = files.app;
  const char *argv[] = {
    "git", "-C", t->project.root, "status", "--porcelain=v1", "-z", "--untracked-files=all", "--ignored=matching", NULL
  };
  GSubprocess *git;

  files.git_timer = 0;
  g_cancellable_cancel(files.git_cancel);
  g_clear_object(&files.git_cancel);
  if (!t->project.git_top) {
    g_clear_pointer(&files.marks, g_hash_table_unref);
    files_paint_all();
    return G_SOURCE_REMOVE;
  }
  git = g_subprocess_newv(argv, G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, NULL);
  if (git) {
    files.git_cancel = g_cancellable_new();
    g_subprocess_communicate_async(git, NULL, files.git_cancel, on_git_status, NULL);
    g_object_unref(git);
  }
  return G_SOURCE_REMOVE;
}

void files_schedule_git(void)
{
  g_clear_handle_id(&files.git_timer, g_source_remove);
  files.git_timer = g_timeout_add(GIT_DELAY_MS, read_git, NULL);
}

/* The panel ---------------------------------------------------------------------------------- */

static void on_space_click(GtkGestureClick *gesture, int n_press G_GNUC_UNUSED, double x, double y,
                           gpointer data G_GNUC_UNUSED)
{
  graphene_point_t at;
  GtkWidget *widget = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));

  if (gtk_widget_compute_point(widget, files.panel, &GRAPHENE_POINT_INIT((float)x, (float)y), &at))
    files_show_menu(NULL, at.x, at.y);
}

static const char *const css =
    ".tcode-files { background: var(--tc-frame); }"
    ".tcode-files .title { font-weight: bold; color: var(--tc-fg); padding: 4px 12px 4px 20px; }"
    ".tcode-files listview { background: transparent; }"
    ".tcode-files listview row { min-height: 22px; padding: 0 8px 0 4px; border-radius: 0; }"
    ".tcode-files listview row:hover { background: var(--tc-hover); }"
    ".tcode-files listview row:selected { background: var(--tc-inactive-selection); }"
    ".tcode-files listview:focus-within row:selected { background: var(--tc-selection); }"
    ".tcode-files treeexpander indent { -gtk-icon-size: 8px; }"
    ".tcode-files treeexpander expander { -gtk-icon-size: 12px; color: var(--tc-fg); }"
    ".tcode-files treeexpander:drop(active) { background: var(--tc-hover); box-shadow: none; }"
    ".tcode-files .badge { margin-right: 4px; }"
    ".tcode-files .message { padding: 4px 12px; color: var(--tc-fg); }"
    ".tcode-files .message.error { color: var(--tc-error); }";

GtkWidget *files_panel(App *t)
{
  GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
  GtkWidget *scroll = gtk_scrolled_window_new();
  GtkEventController *keys = gtk_event_controller_key_new();
  GtkGesture *space = gtk_gesture_click_new();
  GtkDropTarget *drop = gtk_drop_target_new(G_TYPE_FILE, GDK_ACTION_COPY | GDK_ACTION_MOVE);

  files.app = t;
  files.show_hidden = TRUE;
  files.expanded = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  files.rows = g_ptr_array_new();
  files.problems = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  files.filter = GTK_FILTER(gtk_custom_filter_new(filter_shown, NULL, NULL));
  files.sorter = GTK_SORTER(gtk_custom_sorter_new(sort_rows, NULL, NULL));
  files.selection = gtk_single_selection_new(NULL);
  gtk_single_selection_set_autoselect(files.selection, FALSE);

  palette_add_css(css);

  files.panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class(files.panel, "tcode-files");
  files.title = GTK_LABEL(gtk_label_new(NULL));
  gtk_label_set_xalign(files.title, 0);
  gtk_label_set_ellipsize(files.title, PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class(GTK_WIDGET(files.title), "title");
  files.message = GTK_LABEL(gtk_label_new(NULL));
  gtk_label_set_xalign(files.message, 0);
  gtk_label_set_wrap(files.message, TRUE);
  gtk_widget_add_css_class(GTK_WIDGET(files.message), "message");
  gtk_widget_set_visible(GTK_WIDGET(files.message), FALSE);

  g_signal_connect(factory, "setup", G_CALLBACK(files_setup_row), NULL);
  g_signal_connect(factory, "bind", G_CALLBACK(files_bind_row), NULL);
  g_signal_connect(factory, "unbind", G_CALLBACK(files_unbind_row), NULL);
  files.list = GTK_LIST_VIEW(gtk_list_view_new(GTK_SELECTION_MODEL(g_object_ref(files.selection)), factory));
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(files.list));
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_vexpand(scroll, TRUE);

  gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
  g_signal_connect(keys, "key-pressed", G_CALLBACK(files_on_key), NULL);
  gtk_widget_add_controller(GTK_WIDGET(files.list), keys);
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(space), GDK_BUTTON_SECONDARY);
  g_signal_connect(space, "pressed", G_CALLBACK(on_space_click), NULL);
  gtk_widget_add_controller(files.panel, GTK_EVENT_CONTROLLER(space));
  g_signal_connect(drop, "drop", G_CALLBACK(files_on_drop), NULL);
  gtk_widget_add_controller(files.panel, GTK_EVENT_CONTROLLER(drop));

  gtk_box_append(GTK_BOX(files.panel), open_tabs_new(t));
  gtk_box_append(GTK_BOX(files.panel), GTK_WIDGET(files.title));
  gtk_box_append(GTK_BOX(files.panel), scroll);
  gtk_box_append(GTK_BOX(files.panel), GTK_WIDGET(files.message));
  files.menu = GTK_POPOVER(gtk_popover_menu_new_from_model(NULL));
  gtk_popover_set_has_arrow(files.menu, FALSE);
  gtk_popover_set_position(files.menu, GTK_POS_BOTTOM);
  gtk_widget_set_halign(GTK_WIDGET(files.menu), GTK_ALIGN_START);
  gtk_widget_set_parent(GTK_WIDGET(files.menu), files.panel);
  files.ask = files_ask_new();
  files_add_actions();

  files_build_tree(FALSE);
  files_apply_font(t);
  files_schedule_git();
  t->files = files.panel;
  return files.panel;
}

void files_set_root(App *t G_GNUC_UNUSED)
{
  g_hash_table_remove_all(files.expanded);
  g_clear_pointer(&files.reveal, g_free);
  files_build_tree(FALSE);
  files_schedule_git();
}

void files_git_changed(App *t G_GNUC_UNUSED)
{
  if (files.panel)
    files_schedule_git();
}

void files_focus(App *t G_GNUC_UNUSED)
{
  gtk_widget_grab_focus(GTK_WIDGET(files.list));
}

void files_apply_font(App *t)
{
  PangoFontDescription *desc = pango_font_description_from_string(t->settings.files_font);
  const char *family = pango_font_description_get_family(desc);
  int size = pango_font_description_get_size(desc);
  double points = (size > 0 ? size / (double)PANGO_SCALE : 11) * t->settings.font_scale;
  char number[G_ASCII_DTOSTR_BUF_SIZE]; /* "11.5", not the locale's "11,5": CSS wants a dot */
  char *css_text =
      g_strdup_printf(".tcode-files listview, .tcode-files .title, .tcode-opentabs list"
                      " { font-family: \"%s\", monospace; font-size: %spt; }",
                      family ? family : "Cascadia Code", g_ascii_formatd(number, sizeof number, "%.1f", points));

  if (!files.font_css) {
    files.font_css = gtk_css_provider_new();
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(files.font_css),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
  }
  gtk_css_provider_load_from_string(files.font_css, css_text);
  g_free(css_text);
  pango_font_description_free(desc);
}

/* Marks path and its folders up to the project's root with problem, unless they have a worse one. */
static void mark_problem(const char *root, const char *path, int problem)
{
  char *at = g_strdup(path);

  while (at && GPOINTER_TO_INT(g_hash_table_lookup(files.problems, at)) < problem) {
    char *up = g_path_get_dirname(at);
    gboolean inside = root && g_str_has_prefix(up, root) && strlen(up) > strlen(root);
    g_hash_table_insert(files.problems, at, GINT_TO_POINTER(problem));
    at = inside ? up : NULL;
    if (!inside)
      g_free(up);
  }
  g_free(at);
}

static gboolean problems_now(gpointer data G_GNUC_UNUSED)
{
  App *t = files.app; /* the panel's window, whichever window's file changed */
  const char *root = t->project.root;

  files.problems_idle = 0;
  g_hash_table_remove_all(files.problems);
  for (int i = 0; i < tab_count(t); i++) {
    Tab *tab = tab_nth(t, i);
    if (tab->kind == TAB_EDITOR && tab->path && (tab->problems.errors || tab->problems.warnings))
      mark_problem(root, tab->path, tab->problems.errors ? PROBLEM_ERROR : PROBLEM_WARNING);
  }
  files_paint_all();
  return G_SOURCE_REMOVE;
}

void files_problems_changed(App *t)
{
  if (files.rows && !t->closing && !files.app->closing && !files.problems_idle)
    files.problems_idle = g_idle_add(problems_now, NULL);
}

void files_restyle(App *t G_GNUC_UNUSED)
{
  files_paint_all();
}

void files_stop(App *t G_GNUC_UNUSED)
{
  g_clear_handle_id(&files.git_timer, g_source_remove);
  g_clear_handle_id(&files.message_timer, g_source_remove);
  g_clear_handle_id(&files.settle_idle, g_source_remove);
  g_clear_handle_id(&files.problems_idle, g_source_remove);
  g_cancellable_cancel(files.git_cancel);
}
