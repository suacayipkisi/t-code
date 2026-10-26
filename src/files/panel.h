/*
 * What the Files panel's files share (files.c: the panel, its tree, git colors and problems; rows.c:
 * a row's widgets, clicks and dragging; actions.c: file operations, the right-click menu and keys).
 * Only they include it.
 */
#pragma once

#include "app.h"

/* One row's widgets (set up once, then bound to the rows that scroll by). */
typedef struct {
  GtkWidget *expander, *icon, *name, *badge;
  GtkTreeListRow *row; /* NULL while unbound */
  char *path;
  gboolean dir;
  gulong expanded_handler;
} FileRow;

typedef void (*AskFunc)(App *t, const char *path, const char *answer);

typedef struct {
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
} FilesPanel;

extern FilesPanel files;

/* A tree row's file (a new reference), its path, and whether it is a folder. */
static inline GFileInfo *info_of(GtkTreeListRow *row)
{
  return G_FILE_INFO(gtk_tree_list_row_get_item(row));
}

static inline char *info_path(GFileInfo *info)
{
  return g_file_get_path(G_FILE(g_file_info_get_attribute_object(info, "standard::file")));
}

static inline gboolean info_is_dir(GFileInfo *info)
{
  return g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY;
}

/* files.c: the selected row (a new reference) or NULL; selecting (and scrolling to) a position; the
 * folder new files go into (path itself when it is a folder, else its folder). */
GtkTreeListRow *files_selected_row(void);
void files_select(guint position);
char *files_folder_of(const char *path);

/* files.c: a line under the tree for a few seconds, red when error (files_say_error frees it). */
void files_say(gboolean error, const char *text);
void files_say_error(GError *error);

/* files.c: a row's colors and badge drawn again, or every shown row's; an opened folder read. */
void files_paint(FileRow *r);
void files_paint_all(void);
void files_load(GtkTreeListRow *row);

/* files.c: path selected once its folders have loaded; the tree read again (keep_selection: the
 * selected path stays selected); git status read again shortly. */
void files_show_path(const char *path);
void files_build_tree(gboolean keep_selection);
void files_schedule_git(void);

/* rows.c: the list's row factory, and a file dropped on a row (r) or the empty space (r NULL). */
void files_setup_row(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer data);
void files_bind_row(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer data);
void files_unbind_row(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer data);
gboolean files_on_drop(GtkDropTarget *target, const GValue *value, double x, double y, FileRow *r);

/* actions.c: opens a file (source code in a temporary tab, unless keep) or opens / closes a folder;
 * the right-click menu for path (NULL: the empty space) at (x, y) in the panel. */
void files_activate_row(GtkTreeListRow *row, gboolean keep);
void files_show_menu(const char *path, double x, double y);

/* actions.c: copies or moves src into dir (and selects it there); Ctrl+X / Ctrl+C on path, Ctrl+V
 * into path's folder (NULL: the root). */
void files_move_or_copy(const char *src, const char *dir, gboolean cut);
void files_clip(const char *path, gboolean cut);
void files_paste(const char *path);

/* actions.c: New File… / New Folder… in path's folder (NULL: the root), Rename…, Delete. */
void files_ask_create(const char *path, gboolean folder);
void files_ask_rename(const char *path);
void files_ask_delete(const char *path);

/* actions.c: the name entry's popover, the menu's actions (files.*) and the panel's keys. */
GtkPopover *files_ask_new(void);
void files_add_actions(void);
gboolean files_on_key(GtkEventControllerKey *controller, guint key, guint code, GdkModifierType state, gpointer data);
