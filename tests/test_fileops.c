/*
 * Tests for the Files panel's half without a display: src/files/fileops.c (names, order, create,
 * rename, delete, copy / move) and src/files/folder.c (a folder's entries, read again on changes) in
 * a temporary folder, src/files/gitmarks.c (git status -> colors) and src/files/icons.c.
 */
#include "files/fileops.h"
#include "files/folder.h"
#include "files/gitmarks.h"
#include "files/icons.h"
#include "util.h"

#include <string.h>

#include <glib/gstdio.h>

static char *tmp;

static char *at(const char *name)
{
  return g_build_filename(tmp, name, NULL);
}

static void write_file(const char *name, const char *text)
{
  char *path = at(name);

  g_assert_true(g_file_set_contents(path, text, -1, NULL));
  g_free(path);
}

static gboolean exists(const char *name)
{
  char *path = at(name);
  gboolean yes = g_file_test(path, G_FILE_TEST_EXISTS);

  g_free(path);
  return yes;
}

static void setup(void)
{
  tmp = g_dir_make_tmp("t-code-fileops-XXXXXX", NULL);
  g_assert_nonnull(tmp);
}

static void teardown(void)
{
  g_assert_true(files_delete(tmp, NULL));
  g_clear_pointer(&tmp, g_free);
}

/* Rules -------------------------------------------------------------------------------------- */

static void test_shown(void)
{
  g_assert_true(files_shown("main.c", FALSE));
  g_assert_true(files_shown(".clang-format", TRUE));
  g_assert_false(files_shown(".clang-format", FALSE));
  g_assert_false(files_shown(".git", TRUE));
  g_assert_false(files_shown(".hg", TRUE));
}

static void test_order(void)
{
  g_assert_cmpint(files_compare("zeta", TRUE, "alpha.c", FALSE), <, 0); /* folders first */
  g_assert_cmpint(files_compare("b.c", FALSE, "A.c", FALSE), >, 0);     /* case does not matter */
  g_assert_cmpint(files_compare("Makefile", FALSE, "makefile", FALSE), <, 0);
  g_assert_cmpint(files_compare("a", FALSE, "a", FALSE), ==, 0);
}

/* Source code opens in a temporary tab; READMEs, Makefiles, CMakeLists.txt, configs do not. */
static void test_is_source(void)
{
  g_assert_true(files_is_source("main.c"));
  g_assert_true(files_is_source("view.HPP"));
  g_assert_true(files_is_source("tool.py"));
  g_assert_false(files_is_source("README.md"));
  g_assert_false(files_is_source("CMakeLists.txt"));
  g_assert_false(files_is_source("Makefile"));
  g_assert_false(files_is_source("rules.cmake"));
  g_assert_false(files_is_source(".c")); /* a dotfile, not an extension */
}

static void test_paths(void)
{
  g_assert_true(files_inside("/p/src/a.c", "/p/src"));
  g_assert_true(files_inside("/p/src", "/p/src"));
  g_assert_false(files_inside("/p/src2/a.c", "/p/src"));
  g_assert_cmpstr(files_relative("/p/src/a.c", "/p"), ==, "src/a.c");
  g_assert_cmpstr(files_relative("/p", "/p"), ==, "");
  g_assert_cmpstr(files_relative("/q/a.c", "/p"), ==, "/q/a.c");
}

/* Operations --------------------------------------------------------------------------------- */

static void test_create(void)
{
  GError *error = NULL;
  char *made;

  setup();
  g_assert_true(files_create(tmp, "a.c", FALSE, &made, &error));
  g_assert_true(g_str_has_suffix(made, "/a.c"));
  g_free(made);
  g_assert_false(files_create(tmp, "a.c", FALSE, &made, &error)); /* taken */
  g_assert_error(error, G_FILE_ERROR, G_FILE_ERROR_EXIST);
  g_assert_null(made);
  g_clear_error(&error);

  g_assert_true(files_create(tmp, "src/lib/b.c", FALSE, &made, NULL)); /* the folders on the way */
  g_free(made);
  g_assert_true(g_file_test(made = at("src/lib"), G_FILE_TEST_IS_DIR));
  g_free(made);
  g_assert_true(files_create(tmp, "docs/", FALSE, &made, NULL)); /* ends in "/": a folder */
  g_assert_true(g_str_has_suffix(made, "/docs"));
  g_free(made);
  g_assert_true(files_create(tmp, "include", TRUE, &made, NULL));
  g_free(made);
  g_assert_true(g_file_test(made = at("include"), G_FILE_TEST_IS_DIR));
  g_free(made);
  teardown();
}

static void test_rename(void)
{
  GError *error = NULL;
  char *path, *dest;

  setup();
  write_file("a.c", "a");
  write_file("b.c", "b");
  path = at("a.c");
  g_assert_false(files_rename(path, "b.c", &dest, &error)); /* taken */
  g_assert_error(error, G_FILE_ERROR, G_FILE_ERROR_EXIST);
  g_clear_error(&error);
  g_assert_false(files_rename(path, "x/y.c", &dest, &error));
  g_assert_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL);
  g_clear_error(&error);
  g_assert_true(files_rename(path, "c.c", &dest, NULL));
  g_assert_true(g_str_has_suffix(dest, "/c.c"));
  g_assert_false(exists("a.c"));
  g_assert_true(exists("c.c"));
  g_free(dest);
  g_free(path);
  teardown();
}

static void test_free_name(void)
{
  char *name;

  setup();
  write_file("a.c", "");
  write_file("a copy.c", "");
  write_file("Makefile", "");
  g_assert_true(g_str_has_suffix(name = files_free_name(tmp, "a.c", FALSE), "/a copy 2.c"));
  g_free(name);
  g_assert_true(g_str_has_suffix(name = files_free_name(tmp, "Makefile", FALSE), "/Makefile copy"));
  g_free(name);
  g_assert_true(g_str_has_suffix(name = files_free_name(tmp, "new.c", FALSE), "/new.c"));
  g_free(name);
  teardown();
}

static void test_put(void)
{
  GError *error = NULL;
  char *src, *dir, *dest, *text;

  setup();
  write_file("a.c", "int a;");
  g_assert_true(files_create(tmp, "src/", FALSE, &dir, NULL));
  src = at("a.c");

  g_assert_true(files_put(src, tmp, FALSE, &dest, NULL)); /* a copy beside it */
  g_assert_true(g_str_has_suffix(dest, "/a copy.c"));
  g_free(dest);
  g_assert_false(files_put(src, tmp, TRUE, &dest, &error)); /* moved where it is: nothing to do */
  g_assert_no_error(error);
  g_assert_null(dest);

  g_assert_true(files_put(src, dir, TRUE, &dest, NULL)); /* moved into src/ */
  g_assert_false(exists("a.c"));
  g_assert_true(g_file_get_contents(dest, &text, NULL, NULL));
  g_assert_cmpstr(text, ==, "int a;");
  g_free(text);
  g_free(dest);

  g_assert_true(files_put(dir, tmp, FALSE, &dest, NULL)); /* a folder copied with what is in it */
  g_assert_true(g_str_has_suffix(dest, "/src copy"));
  g_assert_true(exists("src copy/a.c"));
  g_free(dest);
  g_assert_false(files_put(tmp, dir, TRUE, &dest, &error)); /* into itself */
  g_assert_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL);
  g_clear_error(&error);

  g_assert_true(files_delete(dir, NULL));
  g_assert_false(exists("src"));
  g_free(src);
  g_free(dir);
  teardown();
}

/* Folder lists ------------------------------------------------------------------------------- */

static GListModel *list;
static const char *wanted;

/* The list's names, sorted, joined by spaces ("a.c src/"; folders end in "/"). */
static char *names(void)
{
  GPtrArray *all = g_ptr_array_new_with_free_func(g_free);

  for (guint i = 0; i < g_list_model_get_n_items(list); i++) {
    GFileInfo *info = g_list_model_get_item(list, i);
    gboolean dir = g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY;
    g_ptr_array_add(all, g_strconcat(g_file_info_get_name(info), dir ? "/" : "", NULL));
    g_object_unref(info);
  }
  g_ptr_array_sort_values(all, (GCompareFunc)strcmp);
  g_ptr_array_add(all, NULL);
  char *text = g_strjoinv(" ", (char **)all->pdata);
  g_ptr_array_unref(all);
  return text;
}

static gboolean names_are(void)
{
  char *now = names();
  gboolean same = strcmp(now, wanted) == 0;

  g_free(now);
  return same;
}

/* Runs the main loop until the list holds want (3 seconds at most). */
static void assert_names(const char *want)
{
  gint64 end = g_get_monotonic_time() + 3 * G_TIME_SPAN_SECOND;

  wanted = want;
  while (!names_are() && g_get_monotonic_time() < end)
    g_main_context_iteration(NULL, FALSE);
  char *now = names();
  g_assert_cmpstr(now, ==, want);
  g_free(now);
}

/* Read only once loaded; each entry knows its file. */
static void test_folder_load(void)
{
  GFileInfo *info;
  char *path;

  setup();
  write_file("a.c", "");
  g_assert_true(files_create(tmp, "src/", FALSE, &path, NULL));
  g_free(path);
  list = folder_new(tmp, TRUE);
  run_for(50);
  g_assert_cmpuint(g_list_model_get_n_items(list), ==, 0);
  g_assert_false(folder_loaded(list));
  g_assert_false(folder_watched(list));

  folder_load(list);
  assert_names("a.c src/");
  g_assert_true(folder_watched(list));
  g_assert_cmpstr(folder_path(list), ==, tmp);
  info = g_list_model_get_item(list, 0);
  path = g_file_get_path(G_FILE(g_file_info_get_attribute_object(info, "standard::file")));
  g_assert_true(g_str_has_prefix(path, tmp));
  g_free(path);
  g_object_unref(info);
  g_object_unref(list);
  teardown();
}

/* Files that come by a rename (editors' and git's way of writing) and go; the rest stay the same
 * objects. */
static void test_folder_changes(void)
{
  GFileInfo *kept, *again;
  char *from, *to;

  setup();
  write_file("kept.c", "");
  list = folder_new(tmp, TRUE);
  folder_load(list);
  assert_names("kept.c");
  kept = g_list_model_get_item(list, 0);

  write_file("new.c", "int x;"); /* g_file_set_contents: a temporary file renamed */
  assert_names("kept.c new.c");
  from = g_build_filename(tmp, "new.c", NULL);
  to = g_build_filename(tmp, "moved.c", NULL);
  g_assert_cmpint(g_rename(from, to), ==, 0);
  assert_names("kept.c moved.c");
  g_assert_cmpint(g_unlink(to), ==, 0);
  assert_names("kept.c");

  again = g_list_model_get_item(list, 0);
  g_assert_true(again == kept);
  g_object_unref(again);
  g_object_unref(kept);
  g_free(from);
  g_free(to);
  g_object_unref(list);
  teardown();
}

/* Not watched: changes show only when the list is made again. */
static void test_folder_unwatched(void)
{
  setup();
  write_file("early.c", "");
  list = folder_new(tmp, FALSE);
  folder_load(list);
  assert_names("early.c"); /* read */
  g_assert_false(folder_watched(list));
  write_file("late.c", "");
  run_for(FOLDER_DELAY_MS + 200);
  assert_names("early.c");
  g_object_unref(list);
  teardown();
}

/* Freed while its folder is being read, or a change waits: nothing touches it afterwards. */
static void test_folder_freed_early(void)
{
  setup();
  list = folder_new(tmp, TRUE);
  folder_load(list);
  g_object_unref(list);
  run_for(100);
  write_file("a.c", "");
  list = folder_new(tmp, TRUE);
  folder_load(list);
  assert_names("a.c");
  write_file("x.c", "");
  run_for(FOLDER_DELAY_MS / 5); /* the change waits for its delay */
  g_object_unref(list);
  run_for(FOLDER_DELAY_MS + 100);
  teardown();
}

/* Git colors --------------------------------------------------------------------------------- */

static void test_git_marks(void)
{
  /* git status --porcelain=v1 -z: "XY path\0", a rename "R  new\0old\0" */
  static const char out[] = " M src/main.c\0"
                            "?? notes/todo.md\0"
                            "D  src/old.c\0"
                            "R  src/new.c\0src/was.c\0"
                            "UU src/both.c\0"
                            "!! build/\0";
  GHashTable *marks = git_marks_parse("/p", out, sizeof out - 1);

  g_assert_cmpint(git_marks_state(marks, "/p/src/main.c"), ==, 'M');
  g_assert_cmpint(git_marks_state(marks, "/p/notes/todo.md"), ==, 'U');
  g_assert_cmpint(git_marks_state(marks, "/p/notes"), ==, 'U'); /* a folder shows what is in it */
  g_assert_cmpint(git_marks_state(marks, "/p/src/old.c"), ==, 'D');
  g_assert_cmpint(git_marks_state(marks, "/p/src/new.c"), ==, 'M'); /* renamed: modified, like t-code-fm */
  g_assert_cmpint(git_marks_state(marks, "/p/src/was.c"), ==, 0);
  g_assert_cmpint(git_marks_state(marks, "/p/src"), ==, 'C'); /* the strongest inside */
  g_assert_cmpint(git_marks_state(marks, "/p/build"), ==, 'I');
  g_assert_cmpint(git_marks_state(marks, "/p/build/obj/a.o"), ==, 'I'); /* inside an ignored folder */
  g_assert_cmpint(git_marks_state(marks, "/p/README.md"), ==, 0);
  g_assert_cmpint(git_marks_state(marks, "/p"), ==, 0); /* the top itself: no color */
  g_assert_cmpint(git_marks_state(NULL, "/p/src/main.c"), ==, 0);
  g_assert_cmpstr(git_marks_badge('C'), ==, "!");
  g_assert_null(git_marks_badge('I'));
  g_assert_cmpstr(git_marks_color(0), ==, "#cccccc");
  g_hash_table_unref(marks);
}

/* A deleted file makes its folder modified, not deleted (the folder is still there). */
static void test_git_deleted_folder(void)
{
  static const char out[] = " D lib/gone.c\0";
  GHashTable *marks = git_marks_parse("/p", out, sizeof out - 1);

  g_assert_cmpint(git_marks_state(marks, "/p/lib"), ==, 'M');
  g_hash_table_unref(marks);
}

/* Icons ------------------------------------------------------------------------------------- */

/* By whole name first, then by extension (any case); folders by whether they are open. */
static void test_icons(void)
{
  const FileIcon *c = file_icon("main.c", FALSE, FALSE);

  g_assert_true(file_icon("MAIN.C", FALSE, FALSE) == c);
  g_assert_true(file_icon("Makefile", FALSE, FALSE) == file_icon("rules.mk", FALSE, FALSE));
  g_assert_true(file_icon("notes", FALSE, FALSE) == file_icon(".hidden", FALSE, FALSE)); /* the plain file */
  g_assert_true(file_icon("src", TRUE, FALSE) != file_icon("src", TRUE, TRUE));
  g_assert_true(file_icon("a.c", TRUE, FALSE) == file_icon("src", TRUE, FALSE)); /* a folder named a.c */
}

/* No glyph from the Material Design range Nerd Fonts 3 dropped (U+F500..U+FD46): fontconfig would
 * draw it from some other font (LICENSE showed a Tibetan letter). */
static void test_icon_glyphs(void)
{
  const char *const names[] = { "LICENSE", "a.c",  "a.cpp",      "a.h",    "Makefile", ".clang-format", "a.md",
                                "a.json",  "a.sh", "a.py",       "a.lua",  "a.js",     "a.ts",          "a.html",
                                "a.css",   "a.rs", "a.go",       "a.java", "a.png",    "a.zip",         "a.pdf",
                                "a.txt",   "a.o",  ".gitignore", "a.lock", "x" };

  for (size_t i = 0; i < G_N_ELEMENTS(names); i++) {
    gunichar c = g_utf8_get_char(file_icon(names[i], FALSE, FALSE)->glyph);
    g_assert_false(c >= 0xF500 && c <= 0xFD46);
  }
}

int main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/fileops/shown", test_shown);
  g_test_add_func("/fileops/order", test_order);
  g_test_add_func("/fileops/is-source", test_is_source);
  g_test_add_func("/fileops/paths", test_paths);
  g_test_add_func("/fileops/create", test_create);
  g_test_add_func("/fileops/rename", test_rename);
  g_test_add_func("/fileops/free-name", test_free_name);
  g_test_add_func("/fileops/put", test_put);
  g_test_add_func("/folder/load", test_folder_load);
  g_test_add_func("/folder/changes", test_folder_changes);
  g_test_add_func("/folder/unwatched", test_folder_unwatched);
  g_test_add_func("/folder/freed-early", test_folder_freed_early);
  g_test_add_func("/gitmarks/states", test_git_marks);
  g_test_add_func("/gitmarks/deleted-folder", test_git_deleted_folder);
  g_test_add_func("/icons/lookup", test_icons);
  g_test_add_func("/icons/glyphs", test_icon_glyphs);
  return g_test_run();
}
