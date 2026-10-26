/*
 * Tests for the log file (src/application/log.c, included for drop_old_runs): the last runs are
 * kept, each start and clean quit has its line, messages go in between (debug ones only with
 * G_MESSAGES_DEBUG). GLib-only. The messages are sent before g_test_init(), which takes them over
 * for its own output.
 */
#include "application/log.c"
#include "util.h"

static char *dir, *path;

static int count_of(const char *text, const char *line_start)
{
  int n = 0;

  for (const char *p = text; (p = strstr(p, line_start)); p++)
    n += p == text || p[-1] == '\n';
  return n;
}

/* 60 runs were there: the oldest 11 went, this one is the 50th; it has its message and quit line. */
static void test_log_runs(void)
{
  char *text = NULL;

  g_assert_cmpstr(log_path(), ==, path);
  log_stop();
  g_assert_true(g_file_get_contents(path, &text, NULL, NULL));
  g_assert_cmpint(count_of(text, "=== 20"), ==, LOG_RUNS);
  g_assert_true(g_str_has_prefix(text, "=== 2026-01-01 00:00:11 t-code old, pid 11\n"));
  g_assert_nonnull(strstr(text, " t-code " TCODE_VERSION ", pid "));
  g_assert_nonnull(strstr(text, "Message: written 42\n"));
  g_assert_null(strstr(text, "not written"));
  g_assert_cmpint(count_of(text, "=== quit"), ==, LOG_RUNS);
  g_free(text);
}

/* Runs over LOG_MAX_BYTES together: the oldest go until the rest fit. */
static void test_log_bytes(void)
{
  char *big = g_strnfill(LOG_MAX_BYTES / 2, 'x'), *text = NULL, *saved = file_path;
  char *runs = g_strdup_printf("=== 2026-01-01 a t-code old\n%s\n=== 2026-01-02 b t-code old\n%s\n"
                               "=== 2026-01-03 c t-code old\nlast\n",
                               big, big);

  file_path = write_at(dir, "big.log", runs, -1, 0);
  drop_old_runs();
  g_assert_true(g_file_get_contents(file_path, &text, NULL, NULL));
  g_assert_true(g_str_has_prefix(text, "=== 2026-01-02 b"));
  g_free(text);
  g_free(file_path);
  file_path = saved;
  g_free(runs);
  g_free(big);
}

int main(int argc, char **argv)
{
  GString *old = g_string_new(NULL);
  int status;

  dir = g_dir_make_tmp("t-code-log-XXXXXX", NULL);
  for (int i = 0; i < 60; i++)
    g_string_append_printf(old, "=== 2026-01-01 00:00:%02d t-code old, pid %d\nsomething\n=== quit 00:00:%02d\n", i, i,
                           i);
  path = write_at(dir, "state/t-code.log", old->str, -1, 0);
  g_string_free(old, TRUE);
  g_unsetenv("G_MESSAGES_DEBUG");
  log_start(path);
  g_debug("not written");
  g_message("written %d", 42);
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/log/runs", test_log_runs);
  g_test_add_func("/log/bytes", test_log_bytes);
  status = g_test_run();
  char *rm[] = { "rm", "-rf", dir, NULL };
  g_spawn_sync(NULL, rm, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, NULL, NULL);
  g_free(path);
  g_free(dir);
  return status;
}
