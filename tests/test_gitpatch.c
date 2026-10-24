/* Tests for splitting and applying individual hunks from Git diffs. */
#include "git/patch.h"

#include <glib/gstdio.h>
#include <string.h>

static char *git(const char *root, const char *const *args)
{
  GPtrArray *argv = g_ptr_array_new();
  char *out = NULL, *err = NULL;
  int status;
  GError *error = NULL;

  g_ptr_array_add(argv, "git");
  g_ptr_array_add(argv, "-C");
  g_ptr_array_add(argv, (gpointer)root);
  for (int i = 0; args[i]; i++)
    g_ptr_array_add(argv, (gpointer)args[i]);
  g_ptr_array_add(argv, NULL);
  g_assert_true(g_spawn_sync(NULL, (char **)argv->pdata, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, &out, &err, &status,
                             &error));
  g_assert_no_error(error);
  g_assert_true(g_spawn_check_wait_status(status, &error));
  g_assert_no_error(error);
  g_ptr_array_free(argv, TRUE);
  g_free(err);
  return out;
}

static void test_partial_stage_and_unstage(void)
{
  char *git_path = g_find_program_in_path("git");
  char *root, *path, *patch_path, *output;
  GString *text;
  GPtrArray *patches;
  int status;

  if (!git_path) {
    g_test_skip("git is not installed");
    return;
  }
  g_free(git_path);
  root = g_dir_make_tmp("t-code-gitpatch-XXXXXX", NULL);
  path = g_build_filename(root, "file.txt", NULL);
  patch_path = g_build_filename(root, "hunk.patch", NULL);
  text = g_string_new(NULL);
  for (int i = 1; i <= 20; i++)
    g_string_append_printf(text, "line %d\n", i);
  g_assert_true(g_file_set_contents(path, text->str, -1, NULL));
  g_string_free(text, TRUE);
  g_free(git(root, (const char *[]){ "init", "-q", NULL }));
  g_free(git(root, (const char *[]){ "add", "file.txt", NULL }));
  g_free(git(root, (const char *[]){ "-c", "user.name=Test", "-c", "user.email=test@example.com", "commit", "-q", "-m",
                                     "base", NULL }));

  g_assert_true(g_file_set_contents(path,
                                    "line 1\nline 2 changed\nline 3\nline 4\nline 5\nline 6\nline 7\nline 8\n"
                                    "line 9\nline 10\nline 11\nline 12\nline 13\nline 14\nline 15\nline 16\n"
                                    "line 17\nline 18 changed\nline 19\nline 20\n",
                                    -1, NULL));
  output = git(root, (const char *[]){ "diff", "--no-color", "--no-ext-diff", "--unified=3", "--", "file.txt",
                                        NULL });
  patches = git_patch_hunks(output);
  g_assert_cmpuint(patches->len, ==, 2);
  g_assert_true(g_file_set_contents(patch_path, g_ptr_array_index(patches, 0), -1, NULL));
  g_free(git(root, (const char *[]){ "apply", "--cached", "hunk.patch", NULL }));
  g_ptr_array_free(patches, TRUE);
  g_free(output);

  output = git(root, (const char *[]){ "diff", "--cached", "--no-color", NULL });
  g_assert_nonnull(strstr(output, "+line 2 changed"));
  g_assert_null(strstr(output, "+line 18 changed"));
  g_free(output);
  output = git(root, (const char *[]){ "diff", "--no-color", NULL });
  g_assert_null(strstr(output, "+line 2 changed"));
  g_assert_nonnull(strstr(output, "+line 18 changed"));
  g_free(output);

  g_free(git(root, (const char *[]){ "reset", "-q", NULL }));
  g_free(git(root, (const char *[]){ "add", "file.txt", NULL }));
  output = git(root, (const char *[]){ "diff", "--cached", "--no-color", "--no-ext-diff", "--unified=3", "--",
                                        "file.txt", NULL });
  patches = git_patch_hunks(output);
  g_assert_cmpuint(patches->len, ==, 2);
  g_assert_true(g_file_set_contents(patch_path, g_ptr_array_index(patches, 1), -1, NULL));
  g_free(git(root, (const char *[]){ "apply", "--cached", "--reverse", "hunk.patch", NULL }));
  g_ptr_array_free(patches, TRUE);
  g_free(output);

  output = git(root, (const char *[]){ "diff", "--cached", "--no-color", NULL });
  g_assert_nonnull(strstr(output, "+line 2 changed"));
  g_assert_null(strstr(output, "+line 18 changed"));
  g_free(output);
  output = git(root, (const char *[]){ "diff", "--no-color", NULL });
  g_assert_null(strstr(output, "+line 2 changed"));
  g_assert_nonnull(strstr(output, "+line 18 changed"));
  g_free(output);

  char *argv[] = { "rm", "-rf", root, NULL };
  g_assert_true(g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, &status, NULL));
  g_assert_true(g_spawn_check_wait_status(status, NULL));
  g_free(patch_path);
  g_free(path);
  g_free(root);
}

int main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/gitpatch/partial-stage-unstage", test_partial_stage_and_unstage);
  return g_test_run();
}
