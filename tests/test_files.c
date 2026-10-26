/*
 * Tests for the Files panel (src/files/files.c, rows.c and actions.c, included for their static functions) on a temporary
 * project: the tree's order, opening files (temporary tabs), folders read only once open and freed when closed, watched folders
 * (but not build output) updating by themselves, open folders kept
 * when the tree is read again, new files revealed and opened, moving into a folder, dotfiles and git
 * colors. Needs a display; skipped without one.
 */
#include "files/actions.c"
#include "files/files.c"
#include "files/rows.c"
#include "util.h"

#include <glib/gstdio.h>

/* What the panel asked the rest of t-code to do (these stand in for editor.c, preview.c, ...):
 * "open <path>", "temporary <path>" or "side <path>". */
static char *opened;

void open_file(App *t G_GNUC_UNUSED, const char *path)
{
  g_free(opened);
  opened = g_strconcat("open ", path, NULL);
}

void open_file_temporary(App *t G_GNUC_UNUSED, const char *path)
{
  g_free(opened);
  opened = g_strconcat("temporary ", path, NULL);
}

void open_to_side(App *t G_GNUC_UNUSED, const char *path)
{
  g_free(opened);
  opened = g_strconcat("side ", path, NULL);
}

/* clang-format off */
void open_preview(App *t G_GNUC_UNUSED, const char *path G_GNUC_UNUSED) {}
void open_in_new_window(App *t G_GNUC_UNUSED, const char *path G_GNUC_UNUSED) {}
GtkWidget *open_tabs_new(App *t G_GNUC_UNUSED) { return gtk_box_new(GTK_ORIENTATION_VERTICAL, 0); }
gboolean is_markdown(const char *path) { return g_str_has_suffix(path, ".md"); }
void compare_with_head(App *t G_GNUC_UNUSED, const char *path G_GNUC_UNUSED) {}
void file_history(App *t G_GNUC_UNUSED, const char *path G_GNUC_UNUSED) {}
void open_project(App *t G_GNUC_UNUSED, const char *root G_GNUC_UNUSED) {}
void cd_in_shell(App *t G_GNUC_UNUSED, const char *dir G_GNUC_UNUSED) {}
/* clang-format on */

/* The open editors (tabs.c's stand-in), for the problems' colors. */
static Tab *open_editors[2];
static int open_editor_count;

int tab_count(App *t G_GNUC_UNUSED)
{
  return open_editor_count;
}
Tab *tab_nth(App *t G_GNUC_UNUSED, int index)
{
  return open_editors[index];
}

static App app;
static GtkWidget *window;

static void write_file(const char *name)
{
  g_free(write_at(app.project.root, name, "", -1, 0));
}

/* Runs the main loop until done() says so (5 seconds at most). */
static gboolean wait_for(gboolean (*done)(void))
{
  gint64 end = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;

  while (!done() && g_get_monotonic_time() < end)
    g_main_context_iteration(NULL, FALSE);
  return done();
}

/* The tree's rows as "name/" (folders) and "name", indented by depth: "src/ |  a.c | Makefile". */
static char *rows(void)
{
  GString *text = g_string_new(NULL);

  for (guint i = 0; i < g_list_model_get_n_items(G_LIST_MODEL(files.tree)); i++) {
    GtkTreeListRow *row = gtk_tree_list_model_get_row(files.tree, i);
    GFileInfo *info = info_of(row);
    if (i)
      g_string_append(text, " | ");
    for (guint d = 0; d < gtk_tree_list_row_get_depth(row); d++)
      g_string_append(text, " ");
    g_string_append_printf(text, "%s%s", g_file_info_get_name(info), info_is_dir(info) ? "/" : "");
    g_object_unref(info);
    g_object_unref(row);
  }
  return g_string_free(text, FALSE);
}

static const char *expected;

static gboolean rows_match(void)
{
  char *now = rows();
  gboolean same = strcmp(now, expected) == 0;

  g_free(now);
  return same;
}

/* Waits until the tree shows want; fails showing what it shows instead. */
static void assert_rows(const char *want)
{
  expected = want;
  if (!wait_for(rows_match)) {
    char *now = rows();
    g_assert_cmpstr(now, ==, want);
    g_free(now);
  }
}

/* The row showing name (a new reference), or NULL. */
static GtkTreeListRow *row_named(const char *name)
{
  for (guint i = 0; i < g_list_model_get_n_items(G_LIST_MODEL(files.tree)); i++) {
    GtkTreeListRow *row = gtk_tree_list_model_get_row(files.tree, i);
    GFileInfo *info = info_of(row);
    gboolean found = strcmp(g_file_info_get_name(info), name) == 0;
    g_object_unref(info);
    if (found)
      return row;
    g_object_unref(row);
  }
  return NULL;
}

static void set_open(const char *name, gboolean open)
{
  GtkTreeListRow *row = row_named(name);

  g_assert_nonnull(row);
  gtk_tree_list_row_set_expanded(row, open);
  files_load(row); /* what a shown row's notify::expanded does */
  g_object_unref(row);
}

static char *selected_name(void)
{
  char *path = selected_path(), *name = path ? g_path_get_basename(path) : NULL;

  g_free(path);
  return name;
}

static const char *wanted_selection;

static gboolean selection_is(void)
{
  char *name = selected_name();
  gboolean yes = g_strcmp0(name, wanted_selection) == 0;

  g_free(name);
  return yes;
}

static void project(void)
{
  app.project.root = g_dir_make_tmp("t-code-files-XXXXXX", NULL);
  write_file("Makefile");
  write_file("main.c");
  write_file("README.md");
  write_file(".clang-format");
  write_file("src/util.c");
  write_file("src/lib/deep.c");
  write_file("build/main.o");
  files_set_root(&app);
}

static void forget_project(void)
{
  g_assert_true(files_delete(app.project.root, NULL));
  g_clear_pointer(&app.project.root, g_free);
  g_clear_pointer(&app.project.git_top, g_free);
  g_clear_pointer(&opened, g_free);
}

/* Folders first, then names without regard to case; dotfiles show; closed folders are not read. */
static void test_tree(void)
{
  GtkTreeListRow *src;

  project();
  assert_rows("build/ | src/ | .clang-format | main.c | Makefile | README.md");
  src = row_named("src");
  g_assert_null(gtk_tree_list_row_get_children(src)); /* closed: no list */
  g_object_unref(src);

  set_open("src", TRUE);
  assert_rows("build/ | src/ |  lib/ |  util.c | .clang-format | main.c | Makefile | README.md");
  set_open("src", FALSE);
  src = row_named("src");
  g_assert_null(gtk_tree_list_row_get_children(src)); /* closed again: its list is freed */
  g_object_unref(src);
  forget_project();
}

/* Build output is listed but not watched (it churns while compiling); the rest is. */
static void test_busy_folders(void)
{
  GtkTreeListRow *row;

  project();
  assert_rows("build/ | src/ | .clang-format | main.c | Makefile | README.md");
  set_open("build", TRUE);
  set_open("src", TRUE);
  assert_rows("build/ |  main.o | src/ |  lib/ |  util.c | .clang-format | main.c | Makefile | README.md");
  row = row_named("build");
  g_assert_false(folder_watched(entries_of(gtk_tree_list_row_get_children(row))));
  g_object_unref(row);
  row = row_named("src");
  g_assert_true(folder_watched(entries_of(gtk_tree_list_row_get_children(row))));
  g_object_unref(row);

  write_file("src/new.c"); /* a watched folder shows it by itself (written by a rename, like editors do) */
  assert_rows("build/ |  main.o | src/ |  lib/ |  new.c |  util.c | .clang-format | main.c | Makefile | README.md");
  forget_project();
}

/* R reads the tree again: open folders stay open and the selection stays on its file. */
static void test_read_again(void)
{
  char *deep;

  project();
  assert_rows("build/ | src/ | .clang-format | main.c | Makefile | README.md");
  set_open("src", TRUE);
  assert_rows("build/ | src/ |  lib/ |  util.c | .clang-format | main.c | Makefile | README.md");
  set_open("lib", TRUE);
  assert_rows("build/ | src/ |  lib/ |   deep.c |  util.c | .clang-format | main.c | Makefile | README.md");
  files_show_path(deep = g_build_filename(app.project.root, "src/lib/deep.c", NULL));
  g_free(deep);
  wanted_selection = "deep.c";
  g_assert_true(wait_for(selection_is));

  files_build_tree(TRUE);
  assert_rows("build/ | src/ |  lib/ |   deep.c |  util.c | .clang-format | main.c | Makefile | README.md");
  g_assert_true(wait_for(selection_is));
  forget_project();
}

/* A new file in a closed folder: the folders on the way open, it is selected and opened. */
static void test_create(void)
{
  char *dir;

  project();
  assert_rows("build/ | src/ | .clang-format | main.c | Makefile | README.md");
  dir = g_build_filename(app.project.root, "src", NULL);
  on_new_file(&app, dir, "lib/fresh.c");
  wanted_selection = "fresh.c";
  g_assert_true(wait_for(selection_is));
  assert_rows("build/ | src/ |  lib/ |   deep.c |   fresh.c |  util.c | .clang-format | main.c | Makefile | README.md");
  g_assert_true(g_str_has_prefix(opened, "open ") && g_str_has_suffix(opened, "/src/lib/fresh.c"));

  on_new_folder(&app, app.project.root, "docs");
  wanted_selection = "docs";
  g_assert_true(wait_for(selection_is));
  g_free(dir);
  forget_project();
}

/* Cut and paste into a folder moves the file there; a copy pasted where it is gets "copy". */
static void test_cut_paste(void)
{
  char *main_c, *src;

  project();
  assert_rows("build/ | src/ | .clang-format | main.c | Makefile | README.md");
  main_c = g_build_filename(app.project.root, "main.c", NULL);
  src = g_build_filename(app.project.root, "src", NULL);

  files_clip(main_c, TRUE);
  files_paste(src);
  g_assert_null(files.clip); /* a cut file is pasted once */
  wanted_selection = "main.c";
  g_assert_true(wait_for(selection_is));
  assert_rows("build/ | src/ |  lib/ |  main.c |  util.c | .clang-format | Makefile | README.md");

  g_free(main_c);
  main_c = g_build_filename(app.project.root, "src/main.c", NULL);
  files_clip(main_c, FALSE);
  files_paste(main_c); /* onto a file: into its folder */
  wanted_selection = "main copy.c";
  g_assert_true(wait_for(selection_is));
  g_free(main_c);
  g_free(src);
  forget_project();
}

/* A click (or Enter) opens source code in a temporary tab, other files (README, Makefile) for good;
 * the menu's Open always for good; a click on a folder opens it. */
static void test_open(void)
{
  GtkTreeListRow *row;

  project();
  assert_rows("build/ | src/ | .clang-format | main.c | Makefile | README.md");
  row = row_named("main.c");
  files_activate_row(row, FALSE);
  g_assert_true(g_str_has_prefix(opened, "temporary ") && g_str_has_suffix(opened, "/main.c"));
  files_activate_row(row, TRUE);
  g_assert_true(g_str_has_prefix(opened, "open ") && g_str_has_suffix(opened, "/main.c"));
  g_object_unref(row);
  row = row_named("README.md");
  files_activate_row(row, FALSE);
  g_assert_true(g_str_has_prefix(opened, "open ") && g_str_has_suffix(opened, "/README.md"));
  g_object_unref(row);

  g_clear_pointer(&opened, g_free);
  row = row_named("src");
  files_activate_row(row, FALSE);
  g_assert_true(gtk_tree_list_row_get_expanded(row));
  g_assert_null(opened);
  g_object_unref(row);
  forget_project();
}

/* A second click on the same file within the double-click time (by the clicks' own times, however
 * late they are handled) is a double click; a third starts again; another file or a late click is a
 * first one. */
static void test_double_click(void)
{
  GtkWidget *widget = files.panel;

  g_assert_false(is_double_click(widget, "/p/a.c", 1000));
  g_usleep(450 * 1000); /* opening the first click's file kept the main loop busy */
  g_assert_true(is_double_click(widget, "/p/a.c", 1300));
  g_assert_false(is_double_click(widget, "/p/a.c", 1400)); /* a third */
  g_assert_false(is_double_click(widget, "/p/b.c", 1500));
  g_assert_false(is_double_click(widget, "/p/b.c", 2000)); /* 500 ms: too late */
  g_assert_true(is_double_click(widget, "/p/b.c", 2400));
}

/* The rows are not activatable: the list would take a double click's second click for itself, and
 * the row's own gesture would never see it. */
static void test_rows_not_activatable(void)
{
  GtkListItem *item = g_object_new(GTK_TYPE_LIST_ITEM, NULL);

  files_setup_row(NULL, item, NULL);
  g_assert_false(gtk_list_item_get_activatable(item));
  g_object_unref(item);
}

/* "." hides dotfiles (and shows them again); version control folders never show. */
static void test_dotfiles(void)
{
  project();
  g_mkdir(g_build_filename(app.project.root, ".git", NULL), 0755);
  assert_rows("build/ | src/ | .clang-format | main.c | Makefile | README.md");
  files.show_hidden = FALSE;
  gtk_filter_changed(files.filter, GTK_FILTER_CHANGE_DIFFERENT);
  assert_rows("build/ | src/ | main.c | Makefile | README.md");
  files.show_hidden = TRUE;
  gtk_filter_changed(files.filter, GTK_FILTER_CHANGE_DIFFERENT);
  assert_rows("build/ | src/ | .clang-format | main.c | Makefile | README.md");
  forget_project();
}

static gboolean marks_read(void)
{
  return files.marks != NULL && files.git_cancel == NULL;
}

/* git status in a repository: untracked files and their folders, ignored build output. */
static void test_git(void)
{
  char *argv[] = { "git", "init", "-q", NULL }, *path;

  project();
  write_file(".gitignore");
  g_assert_true(
      g_file_set_contents((path = g_build_filename(app.project.root, ".gitignore", NULL)), "build/\n", -1, NULL));
  g_free(path);
  if (!g_spawn_sync(app.project.root, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL, NULL, NULL, NULL,
                    NULL, NULL, NULL)) {
    g_test_skip("no git");
    forget_project();
    return;
  }
  app.project.git_top = g_canonicalize_filename(app.project.root, NULL);
  files_git_changed(&app);
  g_assert_true(wait_for(marks_read));
  path = g_build_filename(app.project.git_top, "src", NULL);
  g_assert_cmpint(git_marks_state(files.marks, path), ==, 'U');
  g_free(path);
  path = g_build_filename(app.project.git_top, "build/main.o", NULL);
  g_assert_cmpint(git_marks_state(files.marks, path), ==, 'I');
  g_free(path);
  forget_project();
}

static gboolean problems_read(void)
{
  return files.problems_idle == 0;
}

/* An open file with errors is named in the error color, one with only warnings in the warning
 * color, and so are the folders holding them (the worse wins); a closed file loses its color. */
static void test_problems(void)
{
  Tab deep = { .kind = TAB_EDITOR }, util = { .kind = TAB_EDITOR };
  char *src, *lib;

  project();
  deep.path = g_build_filename(app.project.root, "src/lib/deep.c", NULL);
  util.path = g_build_filename(app.project.root, "src/util.c", NULL);
  src = g_build_filename(app.project.root, "src", NULL);
  lib = g_build_filename(app.project.root, "src/lib", NULL);
  deep.problems.errors = 1;
  util.problems.warnings = 2;
  open_editors[0] = &util;
  open_editors[1] = &deep;
  open_editor_count = 2;
  files_problems_changed(&app);
  g_assert_true(wait_for(problems_read));
  g_assert_cmpstr(name_color(deep.path, 0), ==, palette_hex(PAL_ERROR));
  g_assert_cmpstr(name_color(lib, 0), ==, palette_hex(PAL_ERROR));
  g_assert_cmpstr(name_color(src, 0), ==, palette_hex(PAL_ERROR));
  g_assert_cmpstr(name_color(util.path, 0), ==, palette_hex(PAL_WARNING));
  g_assert_cmpstr(name_color(app.project.root, 0), ==, palette_hex(PAL_FG)); /* not the root */

  open_editor_count = 1; /* deep.c closed */
  files_problems_changed(&app);
  g_assert_true(wait_for(problems_read));
  g_assert_cmpstr(name_color(deep.path, 0), ==, palette_hex(PAL_FG));
  g_assert_cmpstr(name_color(lib, 0), ==, palette_hex(PAL_FG));
  g_assert_cmpstr(name_color(src, 0), ==, palette_hex(PAL_WARNING));

  util.problems.warnings = 0; /* fixed */
  files_problems_changed(&app);
  g_assert_true(wait_for(problems_read));
  g_assert_cmpstr(name_color(util.path, 0), ==, palette_hex(PAL_FG));
  g_assert_cmpstr(name_color(src, 0), ==, palette_hex(PAL_FG));
  open_editor_count = 0;
  g_free(lib);
  g_free(src);
  g_free(util.path);
  g_free(deep.path);
  forget_project();
}

int main(int argc, char **argv)
{
  gtk_test_init(&argc, &argv, NULL);
  app.settings.font_scale = 1;
  app.settings.files_font = "Cascadia Code 11";
  app.project.root = g_strdup(g_get_tmp_dir());
  window = gtk_window_new();
  gtk_window_set_child(GTK_WINDOW(window), files_panel(&app));
  g_clear_pointer(&app.project.root, g_free);

  g_test_add_func("/files/tree", test_tree);
  g_test_add_func("/files/busy-folders", test_busy_folders);
  g_test_add_func("/files/read-again", test_read_again);
  g_test_add_func("/files/create", test_create);
  g_test_add_func("/files/cut-paste", test_cut_paste);
  g_test_add_func("/files/open", test_open);
  g_test_add_func("/files/double-click", test_double_click);
  g_test_add_func("/files/rows-not-activatable", test_rows_not_activatable);
  g_test_add_func("/files/dotfiles", test_dotfiles);
  g_test_add_func("/files/git", test_git);
  g_test_add_func("/files/problems", test_problems);
  return g_test_run();
}
