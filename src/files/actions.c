/*
 * What the Files panel does to files (see panel.h): New File / Folder, Rename and Delete (asked in
 * a small entry or a dialog), cut / copy / paste, the right-click menu and the keys.
 */
#include "files/panel.h"

#include "editor/editor.h"
#include "editor/preview/preview.h"
#include "files/fileops.h"
#include "git/compare.h"
#include "project/project.h"
#include "window/secondwindow.h"
#include "window/terminal.h"

#include <string.h>

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
    files_say_error(error);
    return;
  }
  files_show_path(made);
  files_schedule_git();
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
static void files_ask_create(const char *path, gboolean folder)
{
  char *dir = path ? files_folder_of(path) : g_strdup(files.app->project.root);
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
    files_say_error(error);
    return;
  }
  if (g_hash_table_remove(files.expanded, path))
    g_hash_table_add(files.expanded, g_strdup(dest));
  files_show_path(dest);
  files_schedule_git();
  g_free(dest);
}

static void files_ask_rename(const char *path)
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
      files_say_error(error);
    files_schedule_git();
  }
  g_free(path);
}

static void files_ask_delete(const char *path)
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

void files_clip(const char *path, gboolean cut)
{
  char *name = g_path_get_basename(path),
       *text = g_strdup_printf("%s %s, Ctrl+V pastes it", cut ? "Cut" : "Copied", name);

  g_set_str(&files.clip, path);
  files.clip_cut = cut;
  files_say(FALSE, text);
  files_paint_all();
  g_free(text);
  g_free(name);
}

/* Copies or moves src into dir and selects it there. */
void files_move_or_copy(const char *src, const char *dir, gboolean cut)
{
  GError *error = NULL;
  char *dest;

  if (files_put(src, dir, cut, &dest, &error)) {
    files_show_path(dest);
    files_schedule_git();
    g_free(dest);
  }
  if (error)
    files_say_error(error);
}

/* Paste into path's folder (NULL: the root). */
void files_paste(const char *path)
{
  char *dir = path ? files_folder_of(path) : g_strdup(files.app->project.root);

  if (!files.clip) {
    files_say(FALSE, "Nothing to paste: Ctrl+C or Ctrl+X a file first");
  } else {
    if (!files.clip_cut && strcmp(files.clip, dir) == 0) { /* a copied folder pasted on itself: beside it */
      char *parent = g_path_get_dirname(dir);
      g_free(dir);
      dir = parent;
    }
    files_move_or_copy(files.clip, dir, files.clip_cut);
    if (files.clip_cut)
      g_clear_pointer(&files.clip, g_free);
    files_paint_all();
  }
  g_free(dir);
}

/* Opens a file (source code in a temporary tab, unless keep), or opens / closes a folder. */
void files_activate_row(GtkTreeListRow *row, gboolean keep)
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
  char *dir = path ? files_folder_of(path) : g_strdup(t->project.root);

  if (strcmp(name, "open") == 0) {
    GtkTreeListRow *row = files_selected_row();
    if (row) {
      files_activate_row(row, TRUE);
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
    files_ask_create(path, strcmp(name, "new-folder") == 0);
  } else if (strcmp(name, "terminal") == 0) {
    cd_in_shell(t, dir);
  } else if (strcmp(name, "cut") == 0 || strcmp(name, "copy") == 0) {
    files_clip(path, strcmp(name, "cut") == 0);
  } else if (strcmp(name, "paste") == 0) {
    files_paste(path);
  } else if (strcmp(name, "copy-path") == 0) {
    copy_text(path);
  } else if (strcmp(name, "copy-relative") == 0) {
    copy_text(files_relative(path, t->project.root));
  } else if (strcmp(name, "rename") == 0) {
    files_ask_rename(path);
  } else if (strcmp(name, "delete") == 0) {
    files_ask_delete(path);
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
void files_show_menu(const char *path, double x, double y)
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

/* Keys --------------------------------------------------------------------------------------- */

/* Enter, l: open; →: opens a folder or goes into it; ←, h, Backspace: closes a folder or goes to its
 * parent; j k: move; g G: first / last; a: new file; r, F2: rename; d, Delete: delete; Ctrl+X C V;
 * ".": dotfiles; R: read again. ↑ ↓ Home End PgUp PgDn are the list's own. */
gboolean files_on_key(GtkEventControllerKey *controller G_GNUC_UNUSED, guint key, guint code G_GNUC_UNUSED,
                      GdkModifierType state, gpointer data G_GNUC_UNUSED)
{
  GtkTreeListRow *row = files_selected_row();
  GFileInfo *info = row ? info_of(row) : NULL;
  char *path = info ? info_path(info) : NULL;
  gboolean dir = info && info_is_dir(info), open = dir && gtk_tree_list_row_get_expanded(row);
  guint position = row ? gtk_tree_list_row_get_position(row) : 0;
  guint last = g_list_model_get_n_items(G_LIST_MODEL(files.tree)) - 1;
  gboolean ctrl = state & GDK_CONTROL_MASK, handled = TRUE;

  if (state & (GDK_ALT_MASK | GDK_SUPER_MASK) || (ctrl && key != GDK_KEY_x && key != GDK_KEY_c && key != GDK_KEY_v))
    handled = FALSE;
  else if (ctrl && key == GDK_KEY_v)
    files_paste(path);
  else if (ctrl && path)
    files_clip(path, key == GDK_KEY_x);
  else if (key == GDK_KEY_a)
    files_ask_create(path, FALSE);
  else if (key == GDK_KEY_period) {
    files.show_hidden = !files.show_hidden;
    gtk_filter_changed(files.filter, GTK_FILTER_CHANGE_DIFFERENT);
    files_say(FALSE, files.show_hidden ? "Dotfiles shown" : "Dotfiles hidden");
  } else if (key == GDK_KEY_R) {
    files_build_tree(TRUE);
    files_schedule_git();
  } else if (key == GDK_KEY_j || key == GDK_KEY_k || key == GDK_KEY_g || key == GDK_KEY_G) {
    guint to = key == GDK_KEY_g || !row ? 0
               : key == GDK_KEY_G       ? last
               : key == GDK_KEY_j       ? MIN(position + 1, last)
               : position               ? position - 1
                                        : 0;
    if (last != G_MAXUINT) /* not empty */
      files_select(to);
  } else if (!row) {
    handled = FALSE;
  } else if (key == GDK_KEY_Return || key == GDK_KEY_KP_Enter || key == GDK_KEY_l) {
    files_activate_row(row, FALSE);
  } else if (key == GDK_KEY_Right) {
    if (dir && !open)
      gtk_tree_list_row_set_expanded(row, TRUE);
    else if (open && position < last)
      files_select(position + 1);
  } else if (key == GDK_KEY_Left || key == GDK_KEY_h || key == GDK_KEY_BackSpace) {
    GtkTreeListRow *parent = gtk_tree_list_row_get_parent(row);
    if (open)
      gtk_tree_list_row_set_expanded(row, FALSE);
    else if (parent)
      files_select(gtk_tree_list_row_get_position(parent));
    if (parent)
      g_object_unref(parent);
  } else if (key == GDK_KEY_r || key == GDK_KEY_F2) {
    files_ask_rename(path);
  } else if (key == GDK_KEY_d || key == GDK_KEY_Delete) {
    files_ask_delete(path);
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

GtkPopover *files_ask_new(void)
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

void files_add_actions(void)
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
