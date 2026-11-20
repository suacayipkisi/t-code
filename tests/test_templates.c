/*
 * Tests for the file templates (src/project/templates.c, GLib only): saving with a name and a
 * description, the list sorted by name, which new files a template suits, replacing one of the same
 * name, folders for names that look alike, deleting, and folders that are not templates.
 */
#include "project/templates.h"
#include "util.h"

#include <glib/gstdio.h>

static char *root;

static Template *nth(GPtrArray *list, guint i)
{
  g_assert_cmpuint(i, <, list->len);
  return list->pdata[i];
}

static char *contents(const Template *tpl)
{
  GError *error = NULL;
  char *text = template_contents(tpl, NULL, &error);

  g_assert_no_error(error);
  return text;
}

/* Removes everything saved in root (a folder per template, files in it). */
static void clear_root(void)
{
  GPtrArray *list = templates_list(root);

  for (guint i = 0; i < list->len; i++)
    g_assert_true(template_delete(nth(list, i), NULL));
  g_ptr_array_unref(list);
}

static void test_save_and_list(void)
{
  GError *error = NULL;

  g_assert_true(
      templates_save(root, "  C main ", "a program", "/home/me/src/main.c", "int main(void)\n{\n}\n", -1, &error));
  g_assert_no_error(error);
  g_assert_true(templates_save(root, "a header", NULL, "lib.h", "#pragma once\n", -1, NULL));
  g_assert_true(templates_save(root, "Build", "", "Makefile", "all:\n", -1, NULL));

  GPtrArray *list = templates_list(root);
  g_assert_cmpuint(list->len, ==, 3);
  g_assert_cmpstr(nth(list, 0)->name, ==, "a header"); /* by name, any case */
  g_assert_cmpstr(nth(list, 1)->name, ==, "Build");
  g_assert_cmpstr(nth(list, 2)->name, ==, "C main"); /* trimmed */
  g_assert_cmpstr(nth(list, 2)->description, ==, "a program");
  g_assert_cmpstr(nth(list, 0)->description, ==, "");
  g_assert_cmpstr(nth(list, 2)->file, ==, "main.c"); /* the name only */
  char *dir = g_build_filename(root, "c-main", NULL);
  g_assert_cmpstr(nth(list, 2)->dir, ==, dir);
  g_free(dir);
  char *text = contents(nth(list, 2));
  g_assert_cmpstr(text, ==, "int main(void)\n{\n}\n");
  g_free(text);
  g_ptr_array_unref(list);
  clear_root();
}

static void test_matches(void)
{
  Template c = { .file = "main.c" }, make = { .file = "Makefile" }, ignore = { .file = ".gitignore" };

  g_assert_true(template_matches(&c, "x.c"));
  g_assert_true(template_matches(&c, "src/lib/Other.C")); /* a path, any case */
  g_assert_false(template_matches(&c, "x.cc"));
  g_assert_false(template_matches(&c, "c"));
  g_assert_true(template_matches(&make, "sub/Makefile"));
  g_assert_false(template_matches(&make, "makefile.am"));
  g_assert_false(template_matches(&make, "README"));
  g_assert_true(template_matches(&ignore, "docs/.gitignore")); /* a leading dot is no extension */
  g_assert_false(template_matches(&ignore, "x.gitignore"));
}

/* The same name (any case) replaces the template, its old file too; a name that only looks the same
 * gets its own folder. */
static void test_replace(void)
{
  g_assert_true(templates_save(root, "Main", "first", "main.c", "one", -1, NULL));
  g_assert_true(templates_save(root, "main", "second", "start.c", "two", -1, NULL));
  GPtrArray *list = templates_list(root);
  g_assert_cmpuint(list->len, ==, 1);
  g_assert_cmpstr(nth(list, 0)->name, ==, "main");
  g_assert_cmpstr(nth(list, 0)->description, ==, "second");
  g_assert_cmpstr(nth(list, 0)->file, ==, "start.c");
  char *text = contents(nth(list, 0)), *old = g_build_filename(nth(list, 0)->dir, "main.c", NULL);
  g_assert_cmpstr(text, ==, "two");
  g_assert_false(g_file_test(old, G_FILE_TEST_EXISTS));
  g_free(old);
  g_free(text);
  g_ptr_array_unref(list);

  g_assert_true(templates_save(root, "main!", NULL, "x.c", "three", -1, NULL));
  list = templates_list(root);
  g_assert_cmpuint(list->len, ==, 2);
  g_assert_true(g_str_has_suffix(nth(list, 1)->dir, "/main-2"));
  g_ptr_array_unref(list);
  clear_root();
}

static void test_refused(void)
{
  GError *error = NULL;

  g_assert_false(templates_save(root, "   ", NULL, "x.c", "", -1, &error));
  g_assert_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL);
  g_clear_error(&error);
  g_assert_false(templates_save(root, "info", NULL, "template.ini", "", -1, &error));
  g_assert_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL);
  g_clear_error(&error);
  GPtrArray *list = templates_list(root);
  g_assert_cmpuint(list->len, ==, 0);
  g_ptr_array_unref(list);
}

/* Folders and files that are not templates are left out; deleting removes the folder. */
static void test_not_templates_and_delete(void)
{
  g_free(write_at(root, "stray.txt", "", -1, 0));
  g_free(write_at(root, "nameless/template.ini", "[template]\nfile=x.c\n", -1, 0));
  g_free(write_at(root, "escaping/template.ini", "[template]\nname=bad\nfile=../x.c\n", -1, 0));
  g_free(write_at(root, "broken/template.ini", "not a key file", -1, 0));
  g_assert_true(templates_save(root, "Good", NULL, "x.c", "x", -1, NULL));

  GPtrArray *list = templates_list(root);
  g_assert_cmpuint(list->len, ==, 1);
  char *dir = g_strdup(nth(list, 0)->dir);
  g_assert_true(template_delete(nth(list, 0), NULL));
  g_assert_false(g_file_test(dir, G_FILE_TEST_EXISTS));
  g_free(dir);
  g_ptr_array_unref(list);

  GPtrArray *none = templates_list("/nonexistent/t-code-templates");
  g_assert_cmpuint(none->len, ==, 0);
  g_ptr_array_unref(none);
}

int main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  char *tmp = g_dir_make_tmp("t-code-templates-XXXXXX", NULL);
  root = g_build_filename(tmp, "templates", NULL); /* made by the first save */

  g_test_add_func("/templates/save-and-list", test_save_and_list);
  g_test_add_func("/templates/matches", test_matches);
  g_test_add_func("/templates/replace", test_replace);
  g_test_add_func("/templates/refused", test_refused);
  g_test_add_func("/templates/not-templates-and-delete", test_not_templates_and_delete);
  int status = g_test_run();

  const char *rm[] = { "rm", "-rf", tmp, NULL };
  g_spawn_sync(NULL, (char **)rm, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, NULL, NULL);
  g_free(root);
  g_free(tmp);
  return status;
}
