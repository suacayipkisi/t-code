/*
 * Tests for project/trust.c (GLib only): the trusted folders' list.
 */
#include "project/trust.h"
#include <stdlib.h>

#include <gio/gio.h>
#include <glib/gstdio.h>

static char *root;

static char *in_root(const char *name)
{
  return g_build_filename(root, name, NULL);
}

/* Nothing is trusted at first; a trusted folder's own folders are, a sibling sharing its name's start
 * and the folder above it are not; adding it twice keeps one line; the list is the user's alone. */
static void test_folders(void)
{
  char *project = in_root("project"), *inner = in_root("project/src/lib"), *sibling = in_root("project-two");
  char *list = g_build_filename(g_get_user_config_dir(), "t-code", "trusted", NULL), *text = NULL;
  GError *error = NULL;
  GStatBuf st;

  g_assert_cmpint(g_mkdir_with_parents(inner, 0755), ==, 0);
  g_assert_cmpint(g_mkdir_with_parents(sibling, 0755), ==, 0);
  g_assert_false(trust_has(NULL));
  g_assert_false(trust_has(""));
  g_assert_false(trust_has(project));
  g_assert_true(trust_add(project, &error));
  g_assert_no_error(error);
  g_assert_true(trust_has(project));
  g_assert_true(trust_has(inner));
  g_assert_false(trust_has(sibling));
  g_assert_false(trust_has(root));
  g_assert_true(trust_add(inner, NULL)); /* inside one already there: no new line */
  g_assert_true(g_file_get_contents(list, &text, NULL, NULL));
  char *expected = g_strdup_printf("%s\n", project);
  g_assert_cmpstr(text, ==, expected);
  g_assert_cmpint(g_stat(list, &st), ==, 0);
  g_assert_cmpint(st.st_mode & 0777, ==, 0600);
  g_free(expected);
  g_free(text);
  g_free(list);
  g_free(sibling);
  g_free(inner);
  g_free(project);
}

/* A link to a trusted folder is that folder; a link inside a trusted folder pointing out of it is
 * not trusted for what it points to; a name with a line break is refused. */
static void test_links(void)
{
  char *project = in_root("project"), *outside = in_root("outside"), *alias = in_root("alias");
  char *escape = in_root("project/escape"), *odd = in_root("odd\nname");
  GError *error = NULL;
  GFile *file;

  g_assert_cmpint(g_mkdir_with_parents(outside, 0755), ==, 0);
  g_assert_cmpint(g_mkdir_with_parents(odd, 0755), ==, 0);
  g_assert_true(trust_add(project, NULL)); /* each test has its own config folder */
  file = g_file_new_for_path(alias);
  g_assert_true(g_file_make_symbolic_link(file, project, NULL, NULL));
  g_object_unref(file);
  file = g_file_new_for_path(escape);
  g_assert_true(g_file_make_symbolic_link(file, outside, NULL, NULL));
  g_object_unref(file);
  g_assert_true(trust_has(alias));
  g_assert_false(trust_has(escape));
  g_assert_false(trust_add(odd, &error));
  g_assert_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL);
  g_error_free(error);
  g_assert_false(trust_has(odd));
  g_free(odd);
  g_free(escape);
  g_free(alias);
  g_free(outside);
  g_free(project);
}

int main(int argc, char **argv)
{
  int status;

  g_test_init(&argc, &argv, G_TEST_OPTION_ISOLATE_DIRS, NULL);
  root = g_dir_make_tmp("t-code-trust-XXXXXX", NULL);
  g_test_add_func("/trust/folders", test_folders);
  g_test_add_func("/trust/links", test_links);
  status = g_test_run();
  char *cmd = g_strdup_printf("rm -rf '%s'", root);
  g_assert_cmpint(system(cmd), ==, 0);
  g_free(cmd);
  g_free(root);
  return status;
}
