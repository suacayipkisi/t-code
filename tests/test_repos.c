/* Tests for finding the git repositories inside a project (src/git/repos.c), against real ones: a
 * repository with clones nested in it, a submodule and an ignored clone; a folder of repositories. */
#include "git/repos.h"

#include <glib/gstdio.h>

static char *tmp;

static void git(const char *dir, const char *const *args)
{
  GPtrArray *argv = g_ptr_array_new();
  int status;

  g_ptr_array_add(argv, "git");
  g_ptr_array_add(argv, "-C");
  g_ptr_array_add(argv, (gpointer)dir);
  for (const char *const *config = (const char *const[]){ "user.name=t", "user.email=t@t", "protocol.file.allow=always",
                                                          "init.defaultBranch=main", NULL };
       *config; config++) {
    g_ptr_array_add(argv, "-c");
    g_ptr_array_add(argv, (gpointer)*config);
  }
  for (int i = 0; args[i]; i++)
    g_ptr_array_add(argv, (gpointer)args[i]);
  g_ptr_array_add(argv, NULL);
  g_assert_true(g_spawn_sync(NULL, (char **)argv->pdata, NULL,
                             G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL,
                             NULL, NULL, &status, NULL));
  g_assert_true(g_spawn_check_wait_status(status, NULL));
  g_ptr_array_free(argv, TRUE);
}

static char *path_of(const char *relative)
{
  return g_build_filename(tmp, relative, NULL);
}

/* A file at relative (its folders made). */
static void put(const char *relative, const char *text)
{
  char *path = path_of(relative), *dir = g_path_get_dirname(path);

  g_mkdir_with_parents(dir, 0755);
  g_assert_true(g_file_set_contents(path, text, -1, NULL));
  g_free(dir);
  g_free(path);
}

/* A repository at relative with one commit. */
static void repository(const char *relative)
{
  char *dir = path_of(relative), *file = g_strconcat(relative, "/file", NULL);

  put(file, "one\n");
  git(dir, (const char *const[]){ "init", "-q", NULL });
  git(dir, (const char *const[]){ "add", "-A", NULL });
  git(dir, (const char *const[]){ "commit", "-qm", "one", NULL });
  g_free(file);
  g_free(dir);
}

/* git status --porcelain=v2 -z --untracked-files=all in dir, NULs and all. */
static char *status_of(const char *dir, gsize *size)
{
  char *file = path_of("status.out"), *out = NULL, *quoted_dir = g_shell_quote(dir), *quoted_file = g_shell_quote(file);
  char *command =
      g_strdup_printf("git -C %s status --porcelain=v2 --branch -z --untracked-files=all --ignored=matching > %s",
                      quoted_dir, quoted_file);
  char *argv[] = { "sh", "-c", command, NULL };
  int status;

  g_assert_true(g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, &status, NULL));
  g_assert_true(g_spawn_check_wait_status(status, NULL));
  g_assert_true(g_file_get_contents(file, &out, size, NULL));
  g_unlink(file);
  g_free(command);
  g_free(quoted_file);
  g_free(quoted_dir);
  g_free(file);
  return out;
}

static void assert_repos(GPtrArray *repos, const char *const *expected)
{
  guint n = g_strv_length((char **)expected);

  for (guint i = 0; i < repos->len && i < n; i++) {
    char *path = path_of(expected[i]);
    g_assert_cmpstr(repos->pdata[i], ==, path);
    g_free(path);
  }
  g_assert_cmpuint(repos->len, ==, n);
}

/* Clones in a repository's work tree are listed, also deep down and in an ignored folder; a submodule,
 * a clone deeper in an ignored folder and a plain folder are not. */
static void test_nested(void)
{
  char *top = path_of("top"), *out;
  gsize size;
  GPtrArray *repos;

  repository("lib");
  repository("top");
  repository("top/inner");
  repository("top/deep/er/repo");
  repository("top/ignored");
  repository("top/build/_deps/fetched");
  put("top/.gitignore", "ignored/\nbuild/\n*.o\n");
  put("top/main.o", "object\n");
  put("top/plain/file", "text\n");
  git(top, (const char *const[]){ "submodule", "add", "-q", "../lib", "sub", NULL });
  git(top, (const char *const[]){ "commit", "-qm", "sub", NULL });

  out = status_of(top, &size);
  repos = git_nested_repos(top, out, size);
  assert_repos(repos, (const char *const[]){ "top/deep/er/repo", "top/ignored", "top/inner", NULL });
  g_ptr_array_unref(repos);
  g_free(out);

  /* Nothing nested: none (and no output at all: none). */
  char *inner = path_of("top/inner");
  out = status_of(inner, &size);
  repos = git_nested_repos(inner, out, size);
  g_assert_cmpuint(repos->len, ==, 0);
  g_ptr_array_unref(repos);
  g_free(out);
  repos = git_nested_repos(inner, NULL, 0);
  g_assert_cmpuint(repos->len, ==, 0);
  g_ptr_array_unref(repos);
  g_free(inner);
  g_free(top);
}

/* A rename's old path follows its record: never taken for a record of its own. */
static void test_rename_record(void)
{
  static const char out[] = "# branch.head main\0"
                            "2 R. N... 100644 100644 100644 1111 1111 R100 new\0? old/\0"
                            "? real/\0"
                            "? file\0";
  GPtrArray *repos = git_nested_repos("/top", out, sizeof out - 1);

  g_assert_cmpuint(repos->len, ==, 1);
  g_assert_cmpstr(repos->pdata[0], ==, "/top/real");
  g_ptr_array_unref(repos);
  g_assert_true(git_is_nested_record("? real/", 7));
  g_assert_false(git_is_nested_record("? file", 6));
  g_assert_false(git_is_nested_record("1 .M real/", 10));
}

/* A folder of repositories: each one found (not what is nested in them), not in hidden folders, down
 * to the depth asked, as many as asked at most. */
static void test_find(void)
{
  GPtrArray *repos;

  repository("folder/a");
  repository("folder/group/b");
  repository("folder/a/nested");
  repository("folder/.hidden/c");
  repository("folder/node_modules/d");
  repository("folder/x/y/z/deep");

  char *folder = path_of("folder");
  repos = git_find_repos(folder, 3, 32);
  assert_repos(repos, (const char *const[]){ "folder/a", "folder/group/b", NULL });
  g_ptr_array_unref(repos);
  repos = git_find_repos(folder, 4, 32);
  assert_repos(repos, (const char *const[]){ "folder/a", "folder/group/b", "folder/x/y/z/deep", NULL });
  g_ptr_array_unref(repos);
  repos = git_find_repos(folder, 3, 1);
  g_assert_cmpuint(repos->len, ==, 1);
  g_ptr_array_unref(repos);
  g_free(folder);
  folder = path_of("missing");
  repos = git_find_repos(folder, 3, 32);
  g_assert_cmpuint(repos->len, ==, 0);
  g_ptr_array_unref(repos);
  g_free(folder);
}

int main(int argc, char **argv)
{
  char *git_path = g_find_program_in_path("git");
  int status;

  g_test_init(&argc, &argv, NULL);
  if (!git_path) {
    g_test_skip("git is not installed");
    return g_test_run();
  }
  g_free(git_path);
  tmp = g_dir_make_tmp("t-code-repos-XXXXXX", NULL);
  g_setenv("GIT_CONFIG_NOSYSTEM", "1", TRUE);
  g_setenv("HOME", tmp, TRUE); /* no user configuration either */
  g_test_add_func("/repos/nested", test_nested);
  g_test_add_func("/repos/rename-record", test_rename_record);
  g_test_add_func("/repos/find", test_find);
  status = g_test_run();

  char *rm[] = { "rm", "-rf", tmp, NULL };
  g_spawn_sync(NULL, rm, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, NULL, NULL);
  g_free(tmp);
  return status;
}
