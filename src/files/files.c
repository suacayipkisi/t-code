/*
 * The Files panel: a GtkListView over a GtkTreeListModel whose folders are files/folder.c lists
 * (filtered: dotfiles; sorted: folders first). A folder is read and watched only while it is open;
 * GtkTreeListModel frees its list when it closes. Git colors come from one `git status` per change
 * (files/gitmarks.c), file operations from files/fileops.c. The names of open files with errors or
 * warnings (and of the folders holding them) are drawn in those colors instead.
 */
#include "files/files.h"

#include "editor/editor.h"
#include "editor/preview/preview.h"
#include "editor/tabs.h"
#include "files/fileops.h"
#include "files/folder.h"
#include "files/gitmarks.h"
#include "files/icons.h"
#include "git/compare.h"
#include "project/project.h"
#include "files/opentabs.h"
#include "window/secondwindow.h"
#include "window/terminal.h"
#include "window/palette.h"

#include <string.h>

#define GIT_DELAY_MS 250 /* changes come in bursts (a build, a save, a checkout): read git status once */
#define MESSAGE_MS 4000  /* how long a message stays at the bottom */
#define REVEAL_MS 2000   /* how long a file to select is waited for (its folders load first) */

/* One row's widgets (set up once, then bound to the rows that scroll by). */
typedef struct {
  GtkWidget *expander, *icon, *name, *badge;
  GtkTreeListRow *row; /* NULL while unbound */
  char *path;
  gboolean dir;
  gulong expanded_handler;
} FileRow;

typedef void (*AskFunc)(App *t, const char *path, const char *answer);

static struct {
  App *app;
  GtkWidget *panel;
  GtkLabel *title, *message;
  GtkListView *list;
  GtkSingleSelection *selection;
  GtkTreeListModel *tree;
  GtkFilter *filter; /* shared by every folder's list */
  GtkSorter *sorter;
  GtkPopover *menu, *ask;
  GtkEntry *ask_entry;
  GtkLabel *ask_label;
  AskFunc ask_then;
  char *ask_path;
  char *menu_path;      /* what the right-click menu is for (NULL: the empty space, the root) */
  GHashTable *expanded; /* paths of the open folders, kept when a folder's list is read again */
  GPtrArray *rows;      /* the bound FileRows */
  GHashTable *marks;    /* git states (NULL outside a repository) */
  GHashTable *problems; /* path -> PROBLEM_ERROR / PROBLEM_WARNING: open files and their folders */
  guint problems_idle;
  GCancellable *git_cancel;
  guint git_timer, message_timer, settle_idle;
  char *reveal; /* a path to select once its folders have loaded */
  gint64 reveal_until;
  char *clip; /* a cut or copied path */
  gboolean clip_cut, show_hidden;
  GtkCssProvider *font_css;
} files;

/* Paths and rows ----------------------------------------------------------------------------- */

static GFileInfo *info_of(GtkTreeListRow *row)
{
  return G_FILE_INFO(gtk_tree_list_row_get_item(row)); /* a new reference */
}

static char *info_path(GFileInfo *info)
{
  return g_file_get_path(G_FILE(g_file_info_get_attribute_object(info, "standard::file")));
}

static gboolean info_is_dir(GFileInfo *info)
{
  return g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY;
}

static char *row_path(GtkTreeListRow *row)
{
  GFileInfo *info = info_of(row);
  char *path = info_path(info);

  g_object_unref(info);
  return path;
}

/* The selected row (a new reference), or NULL. */
static GtkTreeListRow *selected_row(void)
{
  GtkTreeListRow *row = gtk_single_selection_get_selected_item(files.selection);

  return row ? g_object_ref(row) : NULL;
}

static char *selected_path(void)
{
  GtkTreeListRow *row = selected_row();
  char *path = row ? row_path(row) : NULL;

  if (row)
    g_object_unref(row);
  return path;
}

static void select_position(guint position)
{
  gtk_list_view_scroll_to(files.list, position, GTK_LIST_SCROLL_SELECT | GTK_LIST_SCROLL_FOCUS, NULL);
}

/* The folder new files go into: path itself when it is a folder, else its folder. */
static char *folder_of(const char *path)
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
static void say(gboolean error, const char *text)
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

static void say_error(GError *error)
{
  say(TRUE, error->message);
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
static void paint(FileRow *r)
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

static void paint_all(void)
{
  for (guint i = 0; i < files.rows->len; i++)
    paint(g_ptr_array_index(files.rows, i));
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

/* A folder's rows: its entries, filtered and sorted. They are read only when load() is called: GTK
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
static void load(GtkTreeListRow *row)
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
        load(row);
    }
    if (files.reveal && strcmp(files.reveal, path) == 0) {
      g_clear_pointer(&files.reveal, g_free);
      select_position(i);
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
static void reveal(const char *path)
{
  g_set_str(&files.reveal, path);
  files.reveal_until = g_get_monotonic_time() + REVEAL_MS * 1000;
  settle_later();
}

void files_reveal(App *t G_GNUC_UNUSED, const char *path)
{
  reveal(path);
}

static void on_tree_changed(GListModel *model G_GNUC_UNUSED, guint position G_GNUC_UNUSED, guint removed G_GNUC_UNUSED,
                            guint added, gpointer data G_GNUC_UNUSED)
{
  if (added)
    settle_later();
}

/* A new tree of t->project.root; open folders stay open, and with keep_selection the selected file selected. */
static void build_tree(gboolean keep_selection)
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
    reveal(keep);
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
  paint_all();
}

static gboolean read_git(gpointer data G_GNUC_UNUSED)
{
  App *t = files.app;
  const char *argv[] = {
    "git", "-C", t->project.root, "status", "--porcelain=v1", "-z", "--untracked-files=all", "--ignored=matching", NULL
  };
  GSubprocess *git;

  files.git_timer = 0;
  if (files.git_cancel)
    g_cancellable_cancel(files.git_cancel);
  g_clear_object(&files.git_cancel);
  if (!t->project.git_top) {
    g_clear_pointer(&files.marks, g_hash_table_unref);
    paint_all();
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

static void schedule_git(void)
{
  g_clear_handle_id(&files.git_timer, g_source_remove);
  files.git_timer = g_timeout_add(GIT_DELAY_MS, read_git, NULL);
}

/* Asking for a name -------------------------------------------------------------------------- */

/* The row widget showing path, if it is on screen. */
static FileRow *shown_row(const char *path)
{
  for (guint i = 0; path && i < files.rows->len; i++) {
    FileRow *r = g_ptr_array_index(files.rows, i);
    if (strcmp(r->path, path) == 0)
      return r;
  }
  return NULL;
}

/* A small entry next to the row of path (or at the top): then(t, path, answer) on Enter. With
 * select_stem the name before its extension is selected. */
static void ask(const char *question, const char *path, const char *initial, gboolean select_stem, AskFunc then)
{
  FileRow *r = shown_row(path);
  GtkWidget *at = r ? r->expander : GTK_WIDGET(files.list);
  graphene_rect_t bounds;
  GdkRectangle rect = { 0, 0, 1, 1 };

  if (gtk_widget_compute_bounds(at, files.panel, &bounds))
    rect = (GdkRectangle){ (int)bounds.origin.x + 24, (int)bounds.origin.y, 1, (int)bounds.size.height };
  gtk_label_set_text(files.ask_label, question);
  gtk_editable_set_text(GTK_EDITABLE(files.ask_entry), initial ? initial : "");
  files.ask_then = then;
  g_set_str(&files.ask_path, path);
  gtk_popover_set_pointing_to(files.ask, &rect);
  gtk_popover_popup(files.ask);
  gtk_widget_grab_focus(GTK_WIDGET(files.ask_entry));
  if (initial) {
    const char *dot = select_stem ? strrchr(initial, '.') : NULL;
    int end = dot && dot != initial ? (int)g_utf8_pointer_to_offset(initial, dot) : -1;
    gtk_editable_select_region(GTK_EDITABLE(files.ask_entry), 0, end);
  }
}

static void on_ask_activate(GtkEntry *entry, gpointer data G_GNUC_UNUSED)
{
  char *answer = g_strdup(gtk_editable_get_text(GTK_EDITABLE(entry)));
  char *path = g_strdup(files.ask_path);

  gtk_popover_popdown(files.ask);
  if (*answer)
    files.ask_then(files.app, path, answer);
  g_free(answer);
  g_free(path);
}

/* File operations ---------------------------------------------------------------------------- */

static void create_answered(App *t, const char *dir, const char *name, gboolean folder)
{
  GError *error = NULL;
  char *made;

  if (!files_create(dir, name, folder, &made, &error)) {
    say_error(error);
    return;
  }
  reveal(made);
  schedule_git();
  if (!folder && !g_str_has_suffix(name, "/"))
    open_file(t, made);
  g_free(made);
}

static void on_new_file(App *t, const char *dir, const char *name)
{
  create_answered(t, dir, name, FALSE);
}

static void on_new_folder(App *t, const char *dir, const char *name)
{
  create_answered(t, dir, name, TRUE);
}

/* New File… / New Folder… in path's folder (NULL: the root). */
static void create_in(const char *path, gboolean folder)
{
  char *dir = path ? folder_of(path) : g_strdup(files.app->project.root);
  const char *rel = files_relative(dir, files.app->project.root);
  char *question =
      g_strdup_printf("%s in %s/  (a/b.c makes folders)", folder ? "New folder" : "New file", *rel ? rel : ".");

  ask(question, dir, NULL, FALSE, folder ? on_new_folder : on_new_file);
  g_free(question);
  g_free(dir);
}

static void on_rename(App *t G_GNUC_UNUSED, const char *path, const char *name)
{
  GError *error = NULL;
  char *dest;

  if (!files_rename(path, name, &dest, &error)) {
    say_error(error);
    return;
  }
  if (g_hash_table_remove(files.expanded, path))
    g_hash_table_add(files.expanded, g_strdup(dest));
  reveal(dest);
  schedule_git();
  g_free(dest);
}

static void rename_path(const char *path)
{
  char *name = g_path_get_basename(path);

  ask("Rename to", path, name, TRUE, on_rename);
  g_free(name);
}

static void on_delete_answer(GObject *dialog, GAsyncResult *result, gpointer data)
{
  char *path = data;
  GError *error = NULL;

  if (gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(dialog), result, NULL) == 1) {
    if (!files_delete(path, &error))
      say_error(error);
    schedule_git();
  }
  g_free(path);
}

static void delete_path(const char *path)
{
  char *name = g_path_get_basename(path);
  gboolean dir = g_file_test(path, G_FILE_TEST_IS_DIR);
  GtkAlertDialog *dialog = gtk_alert_dialog_new("Delete %s%s?", name, dir ? " and everything in it" : "");
  const char *buttons[] = { "Cancel", "Delete", NULL };

  gtk_alert_dialog_set_buttons(dialog, buttons);
  gtk_alert_dialog_set_cancel_button(dialog, 0);
  gtk_alert_dialog_set_default_button(dialog, 1);
  gtk_alert_dialog_choose(dialog, files.app->window, NULL, on_delete_answer, g_strdup(path));
  g_object_unref(dialog);
  g_free(name);
}

static void clip(const char *path, gboolean cut)
{
  char *name = g_path_get_basename(path),
       *text = g_strdup_printf("%s %s, Ctrl+V pastes it", cut ? "Cut" : "Copied", name);

  g_set_str(&files.clip, path);
  files.clip_cut = cut;
  say(FALSE, text);
  paint_all();
  g_free(text);
  g_free(name);
}

/* Copies or moves src into dir and selects it there. */
static void put(const char *src, const char *dir, gboolean cut)
{
  GError *error = NULL;
  char *dest;

  if (files_put(src, dir, cut, &dest, &error)) {
    reveal(dest);
    schedule_git();
    g_free(dest);
  }
  if (error)
    say_error(error);
}

/* Paste into path's folder (NULL: the root). */
static void paste(const char *path)
{
  char *dir = path ? folder_of(path) : g_strdup(files.app->project.root);

  if (!files.clip) {
    say(FALSE, "Nothing to paste: Ctrl+C or Ctrl+X a file first");
  } else {
    if (!files.clip_cut && strcmp(files.clip, dir) == 0) { /* a copied folder pasted on itself: beside it */
      char *parent = g_path_get_dirname(dir);
      g_free(dir);
      dir = parent;
    }
    put(files.clip, dir, files.clip_cut);
    if (files.clip_cut)
      g_clear_pointer(&files.clip, g_free);
    paint_all();
  }
  g_free(dir);
}

/* Opens a file (source code in a temporary tab, unless keep), or opens / closes a folder. */
static void activate_row(GtkTreeListRow *row, gboolean keep)
{
  GFileInfo *info = info_of(row);
  char *path = info_path(info);

  if (info_is_dir(info))
    gtk_tree_list_row_set_expanded(row, !gtk_tree_list_row_get_expanded(row));
  else if (keep || !files_is_source(g_file_info_get_name(info)))
    open_file(files.app, path);
  else
    open_file_temporary(files.app, path);
  g_free(path);
  g_object_unref(info);
}

/* The right-click menu ----------------------------------------------------------------------- */

static void copy_text(const char *text)
{
  gdk_clipboard_set_text(gtk_widget_get_clipboard(files.panel), text);
}

static void on_menu_action(GSimpleAction *action, GVariant *param G_GNUC_UNUSED, gpointer data G_GNUC_UNUSED)
{
  const char *name = g_action_get_name(G_ACTION(action));
  App *t = files.app;
  char *path = g_strdup(files.menu_path); /* NULL: the root */
  char *dir = path ? folder_of(path) : g_strdup(t->project.root);

  if (strcmp(name, "open") == 0) {
    GtkTreeListRow *row = selected_row();
    if (row) {
      activate_row(row, TRUE);
      g_object_unref(row);
    }
  } else if (strcmp(name, "open-side") == 0) {
    open_to_side(t, path);
  } else if (strcmp(name, "open-window") == 0) {
    open_in_new_window(t, path);
  } else if (strcmp(name, "preview") == 0) {
    open_preview(t, path);
  } else if (strcmp(name, "project") == 0) {
    open_project(t, path);
  } else if (strcmp(name, "compare") == 0) {
    compare_with_head(t, path);
  } else if (strcmp(name, "history") == 0) {
    file_history(t, path);
  } else if (strcmp(name, "new-file") == 0 || strcmp(name, "new-folder") == 0) {
    create_in(path, strcmp(name, "new-folder") == 0);
  } else if (strcmp(name, "terminal") == 0) {
    cd_in_shell(t, dir);
  } else if (strcmp(name, "cut") == 0 || strcmp(name, "copy") == 0) {
    clip(path, strcmp(name, "cut") == 0);
  } else if (strcmp(name, "paste") == 0) {
    paste(path);
  } else if (strcmp(name, "copy-path") == 0) {
    copy_text(path);
  } else if (strcmp(name, "copy-relative") == 0) {
    copy_text(files_relative(path, t->project.root));
  } else if (strcmp(name, "rename") == 0) {
    rename_path(path);
  } else if (strcmp(name, "delete") == 0) {
    delete_path(path);
  }
  g_free(dir);
  g_free(path);
}

static void add_section(GMenu *menu, const char *const *items)
{
  GMenu *section = g_menu_new();

  for (int i = 0; items[i]; i += 2) {
    char *action = g_strconcat("files.", items[i + 1], NULL);
    g_menu_append(section, items[i], action);
    g_free(action);
  }
  g_menu_append_section(menu, NULL, G_MENU_MODEL(section));
  g_object_unref(section);
}

/* The menu for path (NULL: the empty space) at (x, y) in the panel. */
static void show_menu(const char *path, double x, double y)
{
  static const char *const create[] = { "New File…",        "new-file", "New Folder…", "new-folder",
                                        "Open in Terminal", "terminal", NULL };
  static const char *const clipboard[] = { "Cut", "cut", "Copy", "copy", "Paste", "paste", NULL };
  static const char *const paths[] = { "Copy Path", "copy-path", "Copy Relative Path", "copy-relative", NULL };
  static const char *const change[] = { "Rename…", "rename", "Delete", "delete", NULL };
  static const char *const paste_only[] = { "Paste", "paste", NULL };
  GMenu *menu = g_menu_new();
  GdkRectangle rect = { (int)x, (int)y, 1, 1 };
  gboolean dir = path && g_file_test(path, G_FILE_TEST_IS_DIR);

  g_set_str(&files.menu_path, path);
  if (dir) {
    add_section(menu, (const char *const[]){ "Open", "open", "Open Folder as Project", "project", NULL });
  } else if (path) {
    if (is_markdown(path))
      add_section(menu, (const char *const[]){ "Open", "open", "Open to the Side", "open-side", "Open in New Window",
                                               "open-window", "Open Preview", "preview", NULL });
    else
      add_section(menu, (const char *const[]){ "Open", "open", "Open to the Side", "open-side", "Open in New Window",
                                               "open-window", NULL });
    if (files.app->project.git_top)
      add_section(menu, (const char *const[]){ "Compare with HEAD", "compare", "File History…", "history", NULL });
  }
  add_section(menu, create);
  if (path) {
    add_section(menu, clipboard);
    add_section(menu, paths);
    add_section(menu, change);
  } else {
    add_section(menu, paste_only);
  }
  gtk_popover_menu_set_menu_model(GTK_POPOVER_MENU(files.menu), G_MENU_MODEL(menu));
  gtk_popover_set_pointing_to(files.menu, &rect);
  gtk_popover_popup(files.menu);
  g_object_unref(menu);
}

/* Rows: set up, bound to a tree row, clicked, dragged, dropped on ---------------------------- */

static void on_row_expanded(GtkTreeListRow *row, GParamSpec *pspec G_GNUC_UNUSED, FileRow *r)
{
  if (gtk_tree_list_row_get_expanded(row)) {
    g_hash_table_add(files.expanded, g_strdup(r->path));
    load(row);
  } else {
    g_hash_table_remove(files.expanded, r->path);
  }
  paint(r);
}

/* A right click on a row selects it and shows its menu (claimed: not the empty space's menu too). */
static void on_row_menu(GtkGestureClick *gesture, int n_press G_GNUC_UNUSED, double x, double y, FileRow *r)
{
  graphene_point_t at;

  if (!r->row)
    return;
  gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
  select_position(gtk_tree_list_row_get_position(r->row));
  if (gtk_widget_compute_point(r->expander, files.panel, &GRAPHENE_POINT_INIT((float)x, (float)y), &at))
    show_menu(r->path, at.x, at.y);
}

/* A second left click on the same file within the double-click time (GTK's own count starts again
 * once the first click has moved the focus to the editor). time is the click's own (ms): opening the
 * first click's file keeps the main loop busy, so the second is handled later than it happened. */
static gboolean is_double_click(GtkWidget *widget, const char *path, guint32 time)
{
  static char *last_path;
  static guint32 last_time;
  int interval = 400; /* ms, GTK's default */
  gboolean twice;

  g_object_get(gtk_widget_get_settings(widget), "gtk-double-click-time", &interval, NULL);
  twice = g_strcmp0(path, last_path) == 0 && time - last_time <= (guint32)interval;
  g_set_str(&last_path, twice ? NULL : path); /* a third click is a first one again */
  last_time = time;
  return twice;
}

/* A left click opens a file (source code in a temporary tab; a double click keeps it) or opens /
 * closes a folder, on release: dragging the row cancels it. A middle click opens a file for good. */
static void on_row_click(GtkGestureClick *gesture, int n_press G_GNUC_UNUSED, double x G_GNUC_UNUSED,
                         double y G_GNUC_UNUSED, FileRow *r)
{
  guint button = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture));

  if (!r->row || button == GDK_BUTTON_SECONDARY) /* the menu's */
    return;
  if (r->dir)
    activate_row(r->row, FALSE);
  else if (button == GDK_BUTTON_MIDDLE ||
           is_double_click(r->expander, r->path,
                           gtk_event_controller_get_current_event_time(GTK_EVENT_CONTROLLER(gesture))))
    open_file(files.app, r->path);
  else
    activate_row(r->row, FALSE);
}

static GdkContentProvider *on_drag_prepare(GtkDragSource *source G_GNUC_UNUSED, double x G_GNUC_UNUSED,
                                           double y G_GNUC_UNUSED, FileRow *r)
{
  GFile *file = g_file_new_for_path(r->path);
  GdkContentProvider *content = gdk_content_provider_new_typed(G_TYPE_FILE, file);

  g_object_unref(file);
  return content;
}

static void on_drag_begin(GtkDragSource *source, GdkDrag *drag G_GNUC_UNUSED, FileRow *r)
{
  GdkPaintable *icon = gtk_widget_paintable_new(r->name);

  gtk_drag_source_set_icon(source, icon, 0, 0);
  g_object_unref(icon);
}

/* A file dropped on a row (its folder) or on the empty space (the root): from inside the project
 * it moves there, from elsewhere (a file manager) it is copied. */
static gboolean on_drop(GtkDropTarget *target G_GNUC_UNUSED, const GValue *value, double x G_GNUC_UNUSED,
                        double y G_GNUC_UNUSED, FileRow *r)
{
  char *src = g_file_get_path(g_value_get_object(value));
  char *dir = r ? folder_of(r->path) : g_strdup(files.app->project.root);
  gboolean dropped = src && (!r || strcmp(src, r->path) != 0); /* a row let go on itself: nothing */

  if (dropped)
    put(src, dir, files_inside(src, files.app->project.root));
  g_free(src);
  g_free(dir);
  return dropped;
}

static void free_row(FileRow *r)
{
  g_free(r->path);
  g_free(r);
}

static void setup_row(GtkSignalListItemFactory *factory G_GNUC_UNUSED, GtkListItem *item, gpointer data G_GNUC_UNUSED)
{
  FileRow *r = g_new0(FileRow, 1);
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  GtkGesture *click = gtk_gesture_click_new(), *menu = gtk_gesture_click_new();
  GtkDragSource *drag = gtk_drag_source_new();
  GtkDropTarget *drop = gtk_drop_target_new(G_TYPE_FILE, GDK_ACTION_COPY | GDK_ACTION_MOVE);

  r->expander = gtk_tree_expander_new();
  r->icon = gtk_label_new(NULL);
  r->name = gtk_label_new(NULL);
  r->badge = gtk_label_new(NULL);
  gtk_widget_add_css_class(r->icon, "icon");
  gtk_widget_add_css_class(r->badge, "badge");
  gtk_label_set_xalign(GTK_LABEL(r->name), 0);
  gtk_label_set_ellipsize(GTK_LABEL(r->name), PANGO_ELLIPSIZE_END);
  gtk_widget_set_hexpand(r->name, TRUE);
  gtk_box_append(GTK_BOX(box), r->icon);
  gtk_box_append(GTK_BOX(box), r->name);
  gtk_box_append(GTK_BOX(box), r->badge);
  gtk_tree_expander_set_child(GTK_TREE_EXPANDER(r->expander), box);
  gtk_tree_expander_set_indent_for_icon(GTK_TREE_EXPANDER(r->expander), TRUE);

  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 0); /* left and middle; the right one: menu */
  g_signal_connect(click, "released", G_CALLBACK(on_row_click), r);
  gtk_widget_add_controller(box, GTK_EVENT_CONTROLLER(click));
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(menu), GDK_BUTTON_SECONDARY);
  g_signal_connect(menu, "pressed", G_CALLBACK(on_row_menu), r);
  gtk_widget_add_controller(box, GTK_EVENT_CONTROLLER(menu));
  gtk_drag_source_set_actions(drag, GDK_ACTION_MOVE | GDK_ACTION_COPY);
  g_signal_connect(drag, "prepare", G_CALLBACK(on_drag_prepare), r);
  g_signal_connect(drag, "drag-begin", G_CALLBACK(on_drag_begin), r);
  gtk_widget_add_controller(box, GTK_EVENT_CONTROLLER(drag));
  g_signal_connect(drop, "drop", G_CALLBACK(on_drop), r);
  gtk_widget_add_controller(r->expander, GTK_EVENT_CONTROLLER(drop));

  /* Not activatable: the list's own double click would claim the second click's events from the
   * row's gesture (on_row_click), which then never sees it. Enter is on_key's. */
  gtk_list_item_set_activatable(item, FALSE);
  g_object_set_data_full(G_OBJECT(item), "row", r, (GDestroyNotify)free_row);
  gtk_list_item_set_child(item, r->expander);
}

static void bind_row(GtkSignalListItemFactory *factory G_GNUC_UNUSED, GtkListItem *item, gpointer data G_GNUC_UNUSED)
{
  FileRow *r = g_object_get_data(G_OBJECT(item), "row");
  GFileInfo *info;

  r->row = gtk_list_item_get_item(item);
  info = info_of(r->row);
  r->path = info_path(info);
  r->dir = info_is_dir(info);
  g_object_unref(info);
  gtk_tree_expander_set_list_row(GTK_TREE_EXPANDER(r->expander), r->row);
  r->expanded_handler = g_signal_connect(r->row, "notify::expanded", G_CALLBACK(on_row_expanded), r);
  g_ptr_array_add(files.rows, r);
  paint(r);
}

static void unbind_row(GtkSignalListItemFactory *factory G_GNUC_UNUSED, GtkListItem *item, gpointer data G_GNUC_UNUSED)
{
  FileRow *r = g_object_get_data(G_OBJECT(item), "row");

  g_ptr_array_remove_fast(files.rows, r);
  g_clear_signal_handler(&r->expanded_handler, r->row);
  gtk_tree_expander_set_list_row(GTK_TREE_EXPANDER(r->expander), NULL);
  g_clear_pointer(&r->path, g_free);
  r->row = NULL;
}

/* Keys --------------------------------------------------------------------------------------- */

/* Enter, l: open; →: opens a folder or goes into it; ←, h, Backspace: closes a folder or goes to its
 * parent; j k: move; g G: first / last; a: new file; r, F2: rename; d, Delete: delete; Ctrl+X C V;
 * ".": dotfiles; R: read again. ↑ ↓ Home End PgUp PgDn are the list's own. */
static gboolean on_key(GtkEventControllerKey *controller G_GNUC_UNUSED, guint key, guint code G_GNUC_UNUSED,
                       GdkModifierType state, gpointer data G_GNUC_UNUSED)
{
  GtkTreeListRow *row = selected_row();
  GFileInfo *info = row ? info_of(row) : NULL;
  char *path = info ? info_path(info) : NULL;
  gboolean dir = info && info_is_dir(info), open = dir && gtk_tree_list_row_get_expanded(row);
  guint position = row ? gtk_tree_list_row_get_position(row) : 0;
  guint last = g_list_model_get_n_items(G_LIST_MODEL(files.tree)) - 1;
  gboolean ctrl = state & GDK_CONTROL_MASK, handled = TRUE;

  if (state & (GDK_ALT_MASK | GDK_SUPER_MASK) || (ctrl && key != GDK_KEY_x && key != GDK_KEY_c && key != GDK_KEY_v))
    handled = FALSE;
  else if (ctrl && key == GDK_KEY_v)
    paste(path);
  else if (ctrl && path)
    clip(path, key == GDK_KEY_x);
  else if (key == GDK_KEY_a)
    create_in(path, FALSE);
  else if (key == GDK_KEY_period) {
    files.show_hidden = !files.show_hidden;
    gtk_filter_changed(files.filter, GTK_FILTER_CHANGE_DIFFERENT);
    say(FALSE, files.show_hidden ? "Dotfiles shown" : "Dotfiles hidden");
  } else if (key == GDK_KEY_R) {
    build_tree(TRUE);
    schedule_git();
  } else if (key == GDK_KEY_j || key == GDK_KEY_k || key == GDK_KEY_g || key == GDK_KEY_G) {
    guint to = key == GDK_KEY_g || !row ? 0
               : key == GDK_KEY_G       ? last
               : key == GDK_KEY_j       ? MIN(position + 1, last)
               : position               ? position - 1
                                        : 0;
    if (last != G_MAXUINT) /* not empty */
      select_position(to);
  } else if (!row) {
    handled = FALSE;
  } else if (key == GDK_KEY_Return || key == GDK_KEY_KP_Enter || key == GDK_KEY_l) {
    activate_row(row, FALSE);
  } else if (key == GDK_KEY_Right) {
    if (dir && !open)
      gtk_tree_list_row_set_expanded(row, TRUE);
    else if (open && position < last)
      select_position(position + 1);
  } else if (key == GDK_KEY_Left || key == GDK_KEY_h || key == GDK_KEY_BackSpace) {
    GtkTreeListRow *parent = gtk_tree_list_row_get_parent(row);
    if (open)
      gtk_tree_list_row_set_expanded(row, FALSE);
    else if (parent)
      select_position(gtk_tree_list_row_get_position(parent));
    if (parent)
      g_object_unref(parent);
  } else if (key == GDK_KEY_r || key == GDK_KEY_F2) {
    rename_path(path);
  } else if (key == GDK_KEY_d || key == GDK_KEY_Delete) {
    delete_path(path);
  } else {
    handled = FALSE;
  }
  g_free(path);
  if (info)
    g_object_unref(info);
  if (row)
    g_object_unref(row);
  return handled;
}

/* The panel ---------------------------------------------------------------------------------- */

static void on_space_click(GtkGestureClick *gesture, int n_press G_GNUC_UNUSED, double x, double y,
                           gpointer data G_GNUC_UNUSED)
{
  graphene_point_t at;
  GtkWidget *widget = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));

  if (gtk_widget_compute_point(widget, files.panel, &GRAPHENE_POINT_INIT((float)x, (float)y), &at))
    show_menu(NULL, at.x, at.y);
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

static GtkPopover *build_ask(void)
{
  GtkWidget *popover = gtk_popover_new(), *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);

  files.ask_label = GTK_LABEL(gtk_label_new(NULL));
  files.ask_entry = GTK_ENTRY(gtk_entry_new());
  gtk_label_set_xalign(files.ask_label, 0);
  gtk_widget_set_size_request(GTK_WIDGET(files.ask_entry), 260, -1);
  g_signal_connect(files.ask_entry, "activate", G_CALLBACK(on_ask_activate), NULL);
  gtk_box_append(GTK_BOX(box), GTK_WIDGET(files.ask_label));
  gtk_box_append(GTK_BOX(box), GTK_WIDGET(files.ask_entry));
  gtk_popover_set_child(GTK_POPOVER(popover), box);
  gtk_popover_set_position(GTK_POPOVER(popover), GTK_POS_BOTTOM);
  gtk_widget_set_parent(popover, files.panel);
  return GTK_POPOVER(popover);
}

static void add_actions(void)
{
  static const char *const names[] = { "open",    "open-side", "open-window",   "preview",  "project", "compare",
                                       "history", "new-file",  "new-folder",    "terminal", "cut",     "copy",
                                       "paste",   "copy-path", "copy-relative", "rename",   "delete" };
  GSimpleActionGroup *group = g_simple_action_group_new();

  for (size_t i = 0; i < G_N_ELEMENTS(names); i++) {
    GSimpleAction *action = g_simple_action_new(names[i], NULL);
    g_signal_connect(action, "activate", G_CALLBACK(on_menu_action), NULL);
    g_action_map_add_action(G_ACTION_MAP(group), G_ACTION(action));
    g_object_unref(action);
  }
  gtk_widget_insert_action_group(files.panel, "files", G_ACTION_GROUP(group));
  g_object_unref(group);
}

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

  g_signal_connect(factory, "setup", G_CALLBACK(setup_row), NULL);
  g_signal_connect(factory, "bind", G_CALLBACK(bind_row), NULL);
  g_signal_connect(factory, "unbind", G_CALLBACK(unbind_row), NULL);
  files.list = GTK_LIST_VIEW(gtk_list_view_new(GTK_SELECTION_MODEL(g_object_ref(files.selection)), factory));
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(files.list));
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_vexpand(scroll, TRUE);

  gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
  g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key), NULL);
  gtk_widget_add_controller(GTK_WIDGET(files.list), keys);
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(space), GDK_BUTTON_SECONDARY);
  g_signal_connect(space, "pressed", G_CALLBACK(on_space_click), NULL);
  gtk_widget_add_controller(files.panel, GTK_EVENT_CONTROLLER(space));
  g_signal_connect(drop, "drop", G_CALLBACK(on_drop), NULL);
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
  files.ask = build_ask();
  add_actions();

  build_tree(FALSE);
  files_apply_font(t);
  schedule_git();
  t->files = files.panel;
  return files.panel;
}

void files_set_root(App *t G_GNUC_UNUSED)
{
  g_hash_table_remove_all(files.expanded);
  g_clear_pointer(&files.reveal, g_free);
  build_tree(FALSE);
  schedule_git();
}

void files_git_changed(App *t G_GNUC_UNUSED)
{
  if (files.panel)
    schedule_git();
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
  paint_all();
  return G_SOURCE_REMOVE;
}

void files_problems_changed(App *t)
{
  if (files.rows && !t->closing && !files.app->closing && !files.problems_idle)
    files.problems_idle = g_idle_add(problems_now, NULL);
}

void files_restyle(App *t G_GNUC_UNUSED)
{
  paint_all();
}

void files_stop(App *t G_GNUC_UNUSED)
{
  g_clear_handle_id(&files.git_timer, g_source_remove);
  g_clear_handle_id(&files.message_timer, g_source_remove);
  g_clear_handle_id(&files.settle_idle, g_source_remove);
  g_clear_handle_id(&files.problems_idle, g_source_remove);
  if (files.git_cancel)
    g_cancellable_cancel(files.git_cancel);
}
