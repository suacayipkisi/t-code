/*
 * Tests for the log file (src/application/log.c): a big one from before is kept as .1, and messages
 * go to the file after a line for the run, debug ones only with G_MESSAGES_DEBUG. GLib-only. The
 * messages are sent before g_test_init(), which takes them over for its own output.
 */
#include "application/log.h"
#include "util.h"

#include <string.h>

static char *dir, *path;

static void test_log_file(void)
{
  char *text = NULL, *old = g_strconcat(path, ".1", NULL);

  g_assert_true(g_file_test(old, G_FILE_TEST_EXISTS)); /* the big one, kept once */
  g_assert_cmpstr(log_path(), ==, path);
  g_assert_true(g_file_get_contents(path, &text, NULL, NULL));
  g_assert_true(g_str_has_prefix(text, "=== "));
  g_assert_nonnull(strstr(text, " t-code "));
  g_assert_nonnull(strstr(text, "Message: written 42\n"));
  g_assert_null(strstr(text, "not written"));
  g_free(text);
  g_free(old);
}

int main(int argc, char **argv)
{
  int status;

  dir = g_dir_make_tmp("t-code-log-XXXXXX", NULL);
  char *big = g_strnfill(LOG_MAX_BYTES + 1, 'x');
  path = write_at(dir, "state/t-code.log", big, -1, 0);
  g_free(big);
  g_unsetenv("G_MESSAGES_DEBUG");
  log_start(path);
  g_assert_false(g_file_test(path, G_FILE_TEST_EXISTS)); /* made at the first message */
  g_debug("not written");
  g_message("written %d", 42);
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/log/file", test_log_file);
  status = g_test_run();
  char *rm[] = { "rm", "-rf", dir, NULL };
  g_spawn_sync(NULL, rm, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, NULL, NULL);
  g_free(path);
  g_free(dir);
  return status;
}
