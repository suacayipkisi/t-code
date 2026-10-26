/*
 * The Source Control panel's Staged Changes and Changes lists (see state.h): a row per file with
 * its buttons and status letter; a click shows its changes side by side, its right-click menu stages,
 * discards, opens or copies.
 */
#include "git/scm/scm.h"
#include "git/scm/state.h"

#include "editor/editor.h"
#include "files/files.h"
#include "git/compare.h"
#include "popups/contextmenu.h"
#include "window/window.h"

#include <string.h>

/* Lists -------------------------------------------------------------------------------------- */

/* A small flat button with a symbol (+, −, ↶, ⟳), shown on hover. */
GtkWidget *scm_icon_button(const char *text, const char *tooltip)
{
  GtkWidget *button = gtk_button_new_with_label(text);

  gtk_button_set_has_frame(GTK_BUTTON(button), FALSE);
  gtk_widget_set_focusable(button, FALSE);
  gtk_widget_set_tooltip_text(button, tooltip);
  gtk_widget_add_css_class(button, "tool");
  return button;
}

/* Puts a row's main content in a flat button that activates the row: a click, Enter or the
 * accessibility "click" action all open it. */
GtkWidget *scm_open_button(GtkWidget *row, GtkWidget *content)
{
  GtkWidget *button = gtk_button_new();

  gtk_button_set_has_frame(GTK_BUTTON(button), FALSE);
  gtk_widget_add_css_class(button, "open");
  gtk_widget_set_hexpand(button, TRUE);
  gtk_button_set_child(GTK_BUTTON(button), content);
  g_signal_connect_swapped(button, "clicked", G_CALLBACK(gtk_widget_activate), row);
  return button;
}

/* The colored status letter on the right of a row (M, A, D, R, U, !). */
GtkWidget *scm_code_label(char code)
{
  char text[2] = { code, 0 };
  char *class = g_strdup_printf("code-%c", code == '!' ? 'x' : code);
  GtkWidget *label = gtk_label_new(text);

  gtk_widget_add_css_class(label, "code");
  gtk_widget_add_css_class(label, class);
  g_free(class);
  return label;
}

/* A row: name, its folder dimmed, then the buttons and the status letter on the right. */
GtkWidget *scm_file_row(const char *path, char code, GtkWidget **box_out)
{
  GtkWidget *row = gtk_list_box_row_new();
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *names = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  char *name = g_path_get_basename(path), *dir = g_path_get_dirname(path);
  GtkWidget *label = gtk_label_new(name);
  GtkWidget *folder = gtk_label_new(strcmp(dir, ".") == 0 ? "" : dir);

  gtk_label_set_ellipsize(GTK_LABEL(folder), PANGO_ELLIPSIZE_START);
  gtk_widget_add_css_class(folder, "dim");
  gtk_widget_set_hexpand(folder, TRUE);
  gtk_label_set_xalign(GTK_LABEL(folder), 0);
  gtk_box_append(GTK_BOX(names), label);
  gtk_box_append(GTK_BOX(names), folder);
  gtk_box_append(GTK_BOX(box), scm_open_button(row, names));
  gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
  gtk_widget_set_tooltip_text(row, path);
  *box_out = box;
  g_object_set_data(G_OBJECT(row), "code", GINT_TO_POINTER(code));
  g_free(dir);
  g_free(name);
  return row;
}

/* clang-format off */
static void on_stage_clicked(GtkButton *button G_GNUC_UNUSED, Change *c) { scm_stage(c->path); }
static void on_unstage_clicked(GtkButton *button G_GNUC_UNUSED, Change *c) { scm_unstage(c); }
static void on_discard_clicked(GtkButton *button G_GNUC_UNUSED, Change *c) { scm_discard(c); }
/* clang-format on */

/* Fills a list from scm.staged or scm.changes, with the buttons each row needs. */
void scm_fill_changes(GtkListBox *list, GPtrArray *changes, gboolean staged)
{
  gtk_list_box_remove_all(list);
  for (guint i = 0; i < changes->len; i++) {
    Change *c = g_ptr_array_index(changes, i);
    GtkWidget *box, *row = scm_file_row(c->path, c->code, &box);
    GtkWidget *button;

    g_object_set_data(G_OBJECT(row), "change", c);
    if (staged) {
      button = scm_icon_button("−", "Unstage");
      g_signal_connect(button, "clicked", G_CALLBACK(on_unstage_clicked), c);
      gtk_box_append(GTK_BOX(box), button);
    } else {
      button = scm_icon_button("↶", "Discard changes");
      g_signal_connect(button, "clicked", G_CALLBACK(on_discard_clicked), c);
      gtk_box_append(GTK_BOX(box), button);
      button = scm_icon_button("+", "Stage");
      g_signal_connect(button, "clicked", G_CALLBACK(on_stage_clicked), c);
      gtk_box_append(GTK_BOX(box), button);
    }
    gtk_box_append(GTK_BOX(box), scm_code_label(c->code));
    gtk_list_box_append(list, row);
  }
}

/* Whether path has staged changes too. */
static gboolean has_staged(const char *path)
{
  for (guint i = 0; i < scm.staged->len; i++)
    if (strcmp(((Change *)g_ptr_array_index(scm.staged, i))->path, path) == 0)
      return TRUE;
  return FALSE;
}

/* A staged change: the staged version (the index) next to the last commit's. */
static void show_staged(App *t, const Change *c)
{
  if (c->code == 'A')
    compare_revisions(t, NULL, NULL, INDEX_REV, c->path); /* new: only the staged version */
  else if (c->code == 'D')
    compare_revisions(t, "HEAD", c->path, NULL, NULL); /* only the old version is left */
  else                                                 /* renames and copies: next to where the file came from */
    compare_revisions(t, "HEAD", c->old_path ? c->old_path : c->path, INDEX_REV, c->path);
}

/* An unstaged change: the working file next to its staged version when it has staged changes too,
 * else next to the last scm_commit (the same then, and the gutter follows new commits). */
static void show_unstaged(App *t, const Change *c)
{
  char *full = g_build_filename(t->project.git_top, c->path, NULL);

  if (c->code == 'D')
    compare_revisions(t, INDEX_REV, c->path, NULL, NULL); /* only the staged version is left */
  else if (c->code != 'M')
    open_file(t, full); /* untracked, conflicted: nothing to compare with */
  else if (has_staged(c->path) || scm.initial)
    compare_with_index(t, full);
  else
    compare_with_head(t, full);
  g_free(full);
}

/* A file in Staged Changes or Changes was clicked: show its changes side by side. */
void scm_on_change_activated(GtkListBox *list, GtkListBoxRow *row, gpointer data G_GNUC_UNUSED)
{
  Change *c = g_object_get_data(G_OBJECT(row), "change");

  if (!c || !scm.app->project.git_top)
    return;
  if (list == scm.staged_list)
    show_staged(scm.app, c);
  else
    show_unstaged(scm.app, c);
}

/* A changed file's right-click menu: a copy of its change (the lists are made again at every git
 * status), and whether it is staged. */
typedef struct {
  Change change;
  gboolean staged;
} MenuChange;

static void menu_change_free(MenuChange *m)
{
  g_free(m->change.path);
  g_free(m->change.old_path);
  g_free(m);
}

/* The file's whole path, or NULL outside a repository. */
static char *change_full_path(const Change *c)
{
  return scm.app->project.git_top ? g_build_filename(scm.app->project.git_top, c->path, NULL) : NULL;
}

static void menu_open_changes(gpointer data)
{
  MenuChange *m = data;

  if (!scm.app->project.git_top)
    return;
  if (m->staged)
    show_staged(scm.app, &m->change);
  else
    show_unstaged(scm.app, &m->change);
}

static void menu_open_file(gpointer data)
{
  char *full = change_full_path(&((MenuChange *)data)->change);

  if (full && g_file_test(full, G_FILE_TEST_IS_REGULAR))
    open_file(scm.app, full);
  g_free(full);
}

/* clang-format off */
static void menu_stage(gpointer data) { scm_stage(((MenuChange *)data)->change.path); }
static void menu_unstage(gpointer data) { scm_unstage(&((MenuChange *)data)->change); }
static void menu_stage_hunk(gpointer data) { scm_choose_hunk(((MenuChange *)data)->change.path, FALSE); }
static void menu_unstage_hunk(gpointer data) { scm_choose_hunk(((MenuChange *)data)->change.path, TRUE); }
static void menu_discard(gpointer data) { scm_discard(&((MenuChange *)data)->change); }
static void menu_copy_relative(gpointer data) { copy_to_clipboard(((MenuChange *)data)->change.path); }
/* clang-format on */

static void menu_copy_path(gpointer data)
{
  char *full = change_full_path(&((MenuChange *)data)->change);

  copy_to_clipboard(full);
  g_free(full);
}

static void menu_reveal(gpointer data)
{
  char *full = change_full_path(&((MenuChange *)data)->change);

  if (full) {
    show_sidebar_page(scm.app, "files");
    files_reveal(scm.app, full);
  }
  g_free(full);
}

/* A right click in Staged Changes or Changes: the file's menu. */
void scm_on_changes_menu(GtkWidget *list, double x, double y, gpointer data G_GNUC_UNUSED)
{
  GtkListBoxRow *row = gtk_list_box_get_row_at_y(GTK_LIST_BOX(list), (int)y);
  Change *c = row ? g_object_get_data(G_OBJECT(row), "change") : NULL;
  MenuItem items[12];
  int n = 0;

  if (!c)
    return;
  MenuChange *m = g_new0(MenuChange, 1);
  m->change = (Change){ g_strdup(c->path), g_strdup(c->old_path), c->code };
  m->staged = list == GTK_WIDGET(scm.staged_list);
  items[n++] = (MenuItem){ "Open Changes", menu_open_changes };
  if (c->code != 'D')
    items[n++] = (MenuItem){ "Open File", menu_open_file };
  items[n++] = (MenuItem){ NULL, NULL };
  if (m->staged) {
    items[n++] = (MenuItem){ "Unstage Changes", menu_unstage };
    if (c->code == 'M')
      items[n++] = (MenuItem){ "Unstage Hunk…", menu_unstage_hunk };
  } else {
    items[n++] = (MenuItem){ "Stage Changes", menu_stage };
    if (c->code == 'M')
      items[n++] = (MenuItem){ "Stage Hunk…", menu_stage_hunk };
    items[n++] = (MenuItem){ "Discard Changes…", menu_discard };
  }
  items[n++] = (MenuItem){ NULL, NULL };
  items[n++] = (MenuItem){ "Copy Path", menu_copy_path };
  items[n++] = (MenuItem){ "Copy Relative Path", menu_copy_relative };
  if (c->code != 'D')
    items[n++] = (MenuItem){ "Reveal in Files", menu_reveal };
  context_menu(list, x, y, items, n, m, (GDestroyNotify)menu_change_free);
}
