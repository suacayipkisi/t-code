/*
 * Tests for the login shell's environment (src/application/shellenv.c), GLib-only: reading it
 * between what start-up files print, adding only the missing PATH folders at the end, a fake shell
 * that sets PATH and GOPATH the way lines in .bashrc do (a variable t-code already has stays), and a
 * shell that hangs (killed with what it started).
 */
#define _DEFAULT_SOURCE
#include "application/shellenv.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
  gboolean done, changed;
} Result;

static void on_done(gboolean changed, gpointer data)
{
  Result *r = data;

  r->done = TRUE;
  r->changed = changed;
}

static gboolean is_done(gconstpointer data)
{
  return ((const Result *)data)->done;
}

/* Writes a shell script to dir/name and returns its path. */
static char *script(const char *dir, const char *name, const char *body)
{
  char *text = g_strconcat("#!/bin/sh\n", body, NULL), *path = write_at(dir, name, text, -1, 0755);

  g_free(text);
  return path;
}

static void test_parse(void)
{
  GHashTable *env = shell_env_parse("Welcome!\n__TCODE_ENV_START__\036PATH=/a/bin:/usr/bin\n\036GOPATH=\036"
                                    "JAVA_HOME=/opt/jdk\n\036X=a\nb\n__TCODE_ENV_END__bye\n");

  g_assert_nonnull(env);
  g_assert_cmpstr(g_hash_table_lookup(env, "PATH"), ==, "/a/bin:/usr/bin");
  g_assert_cmpstr(g_hash_table_lookup(env, "JAVA_HOME"), ==, "/opt/jdk");
  g_assert_false(g_hash_table_contains(env, "GOPATH")); /* not set in the shell */
  g_assert_false(g_hash_table_contains(env, "X"));      /* several lines */
  g_hash_table_unref(env);
  g_assert_null(shell_env_parse("no markers here"));
  g_assert_null(shell_env_parse("__TCODE_ENV_START__\036PATH=/a/bin")); /* killed before the end */
  g_assert_null(shell_env_parse(NULL));
  g_assert_cmpstr(shell_env_names[0], ==, "PATH");
}

static void test_merge(void)
{
  char *merged =
      shell_env_merge_path("/usr/bin:/bin", "/home/u/node/bin:/usr/bin::/bin:/home/u/go/bin:/home/u/node/bin");

  g_assert_cmpstr(merged, ==, "/usr/bin:/bin:/home/u/node/bin:/home/u/go/bin");
  g_free(merged);
  merged = shell_env_merge_path("/usr/bin:/bin", "/bin:/usr/bin");
  g_assert_cmpstr(merged, ==, "/usr/bin:/bin"); /* nothing new: the order stays */
  g_free(merged);
  merged = shell_env_merge_path(NULL, "/a:/b");
  g_assert_cmpstr(merged, ==, "/a:/b");
  g_free(merged);
}

static char *logged; /* the last message, for what the log gets */

static void on_message(const char *domain G_GNUC_UNUSED, GLogLevelFlags level G_GNUC_UNUSED, const char *message,
                       gpointer data G_GNUC_UNUSED)
{
  g_free(logged);
  logged = g_strdup(message);
}

/* The way a Node.js unpacked in the home folder and Go come: .bashrc puts their bin first and sets
 * GOPATH; JAVA_HOME, which t-code already has, stays. */
static void test_load(void)
{
  char *dir = g_dir_make_tmp("t-code-shellenv-XXXXXX", NULL);
  char *shell = script(dir, "fakesh",
                       "[ \"$1 $2 $3\" = '-l -i -c' ] || exit 1\n"
                       "echo 'Welcome to your shell'\n"
                       "echo 'stty: not a tty' >&2\n"
                       "PATH=\"/t-code-test/node/bin:$PATH\"; export PATH\n"
                       "GOPATH=/t-code-test/go; JAVA_HOME=/t-code-test/other-jdk; export GOPATH JAVA_HOME\n"
                       "eval \"$4\"\n");
  char *before = g_strdup(g_getenv("PATH")), *expected = g_strconcat(before, ":/t-code-test/node/bin", NULL);
  Result r = { 0 };

  g_unsetenv("GOPATH");
  g_setenv("JAVA_HOME", "/t-code-test/jdk", TRUE);
  guint handler = g_log_set_handler(NULL, G_LOG_LEVEL_MESSAGE, on_message, NULL);
  shell_env_load(shell, 5000, on_done, &r);
  g_assert_true(shell_env_loading()); /* the plugins wait */
  run_until(is_done, &r);
  g_assert_true(r.done);
  g_assert_false(shell_env_loading());
  g_assert_true(r.changed);
  g_assert_cmpstr(g_getenv("PATH"), ==, expected); /* what was found before still is */
  g_assert_cmpstr(g_getenv("GOPATH"), ==, "/t-code-test/go");
  g_assert_cmpstr(g_getenv("JAVA_HOME"), ==, "/t-code-test/jdk");
  char *expected_log =
      g_strdup_printf("t-code: login shell %s: PATH + /t-code-test/node/bin; GOPATH=/t-code-test/go", shell);
  g_assert_cmpstr(logged, ==, expected_log); /* what the log file gets */
  g_free(expected_log);

  r = (Result){ 0 };
  shell_env_load(shell, 5000, on_done, &r);
  run_until(is_done, &r);
  g_assert_true(r.done);
  g_assert_false(r.changed);
  g_assert_cmpstr(g_getenv("PATH"), ==, expected);

  g_log_remove_handler(NULL, handler);
  g_clear_pointer(&logged, g_free);
  g_setenv("PATH", before, TRUE);
  g_unsetenv("GOPATH");
  g_unsetenv("JAVA_HOME");
  g_free(expected);
  g_free(before);
  g_unlink(shell);
  g_free(shell);
  g_rmdir(dir);
  g_free(dir);
}

/* Gone or a zombie: killed, but in a container whose PID 1 reaps nothing it stays a zombie, which
 * kill(pid, 0) still finds. */
static gboolean alive(pid_t pid)
{
  char *path = g_strdup_printf("/proc/%d/stat", (int)pid), *stat = NULL;
  gboolean found = g_file_get_contents(path, &stat, NULL, NULL);
  const char *paren = found ? strrchr(stat, ')') : NULL;
  gboolean running = paren && paren[1] == ' ' && paren[2] != 'Z' && paren[2] != 'X';

  g_free(stat);
  g_free(path);
  return running;
}

/* A start-up file that waits forever (and a program it started holds the output open): given up
 * on in time, both killed, PATH unchanged. */
static void test_timeout(void)
{
  char *dir = g_dir_make_tmp("t-code-shellenv-XXXXXX", NULL), *pid_file = g_build_filename(dir, "pid", NULL);
  char *body = g_strdup_printf("sleep 30 &\necho $! > '%s'\nwait\n", pid_file);
  char *shell = script(dir, "hangsh", body), *before = g_strdup(g_getenv("PATH")), *text = NULL;
  Result r = { 0 };
  gint64 start = g_get_monotonic_time();

  g_test_expect_message(NULL, G_LOG_LEVEL_WARNING, "*the login shell */hangsh gave no environment in 0 s*");
  shell_env_load(shell, 300, on_done, &r);
  run_until(is_done, &r);
  g_assert_true(r.done);
  g_test_assert_expected_messages();
  g_assert_false(r.changed);
  g_assert_cmpint(g_get_monotonic_time() - start, <, 3 * G_USEC_PER_SEC);
  g_assert_cmpstr(g_getenv("PATH"), ==, before);

  g_assert_true(g_file_get_contents(pid_file, &text, NULL, NULL));
  pid_t sleeper = atoi(text);
  g_assert_cmpint(sleeper, >, 0);
  gint64 end = g_get_monotonic_time() + 2 * G_USEC_PER_SEC;
  while (alive(sleeper) && g_get_monotonic_time() < end)
    run_for(20);
  g_assert_false(alive(sleeper));

  g_free(text);
  g_free(before);
  g_unlink(shell);
  g_unlink(pid_file);
  g_free(shell);
  g_free(body);
  g_free(pid_file);
  g_rmdir(dir);
  g_free(dir);
}

static void test_missing_shell(void)
{
  Result r = { 0 };

  g_test_expect_message(NULL, G_LOG_LEVEL_WARNING, "*cannot run the login shell /nonexistent/t-code-shell*");
  shell_env_load("/nonexistent/t-code-shell", 5000, on_done, &r);
  g_test_assert_expected_messages();
  run_until(is_done, &r);
  g_assert_true(r.done);
  g_assert_false(r.changed);
}

int main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/shellenv/parse", test_parse);
  g_test_add_func("/shellenv/merge", test_merge);
  g_test_add_func("/shellenv/load", test_load);
  g_test_add_func("/shellenv/timeout", test_timeout);
  g_test_add_func("/shellenv/missing-shell", test_missing_shell);
  return g_test_run();
}
