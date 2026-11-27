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

/* A run with a flood of short lines (a warning repeated a million times) is dropped at once: the
 * scan stays linear. */
static void test_log_flood(void)
{
  GString *runs = g_string_new("=== 2026-01-01 a t-code old\n");
  char *text = NULL, *saved = file_path;

  while (runs->len < 5 * LOG_MAX_BYTES)
    g_string_append(runs, "Tried to remove non-child\n");
  g_string_append(runs, "=== 2026-01-02 b t-code old\nlast\n");
  file_path = write_at(dir, "flood.log", runs->str, -1, 0);
  drop_old_runs();
  g_assert_true(g_file_get_contents(file_path, &text, NULL, NULL));
  g_assert_cmpstr(text, ==, "=== 2026-01-02 b t-code old\nlast\n");
  g_free(text);
  g_free(file_path);
  file_path = saved;
  g_string_free(runs, TRUE);
}

/* Another t-code cuts the file while this one has it open: this one's next lines still land in it. */
static void test_log_two_windows(void)
{
  char *big = g_strnfill(LOG_MAX_BYTES / 2, 'x'), *text = NULL, *saved = file_path;
  char *runs = g_strdup_printf("=== 2026-01-01 a t-code old\n%s\n=== 2026-01-02 b t-code old\n%s\n", big, big);
  FILE *other;

  file_path = write_at(dir, "shared.log", runs, -1, 0);
  other = fopen(file_path, "a"); /* the first window's open log */
  g_assert_nonnull(other);
  drop_old_runs(); /* the second window starts */
  fputs("from the first window\n", other);
  fclose(other);
  g_assert_true(g_file_get_contents(file_path, &text, NULL, NULL));
  g_assert_true(g_str_has_prefix(text, "=== 2026-01-02 b"));
  g_assert_true(g_str_has_suffix(text, "\nfrom the first window\n"));
  g_free(text);
  g_free(file_path);
  file_path = saved;
  g_free(runs);
  g_free(big);
}

static int hook_calls;
static char *hook_seen;

static void send(GLogLevelFlags level, const char *domain, const char *message)
{
  const GLogField fields[] = { { "GLIB_DOMAIN", domain, -1 }, { "MESSAGE", message, -1 } };

  write_log(level, fields, G_N_ELEMENTS(fields), NULL);
}

static void on_warning(const char *domain, const char *message)
{
  hook_calls++;
  g_free(hook_seen);
  hook_seen = g_strconcat(domain, ": ", message, NULL);
  send(G_LOG_LEVEL_WARNING, "Test", "the hook's own warning"); /* does not call it again */
}

/* The hook runs after warnings and criticals, not after notes, and not for its own warnings. */
static void test_log_hook(void)
{
  GLogLevelFlags fatal = g_log_set_always_fatal(G_LOG_FATAL_MASK); /* g_test_init made warnings fatal */

  log_set_hook(on_warning);
  send(G_LOG_LEVEL_MESSAGE, "Test", "a note");
  g_assert_cmpint(hook_calls, ==, 0);
  send(G_LOG_LEVEL_WARNING, "Gdk", "Tried to map a grabbing popup");
  g_assert_cmpint(hook_calls, ==, 1);
  g_assert_cmpstr(hook_seen, ==, "Gdk: Tried to map a grabbing popup");
  send(G_LOG_LEVEL_CRITICAL, "Gtk", "assertion failed");
  g_assert_cmpint(hook_calls, ==, 2);
  send(G_LOG_LEVEL_WARNING, "Gdk", "gdk_frame_timings_submitted() called on submitted frame."); /* GTK's own */
  g_assert_cmpint(hook_calls, ==, 2);
  send(G_LOG_LEVEL_CRITICAL, "Gtk", "gtk_text_view_paint: assertion 'priv->onscreen_validated' failed"); /* 4.25 */
  g_assert_cmpint(hook_calls, ==, 2);
  log_set_hook(NULL);
  send(G_LOG_LEVEL_WARNING, "Gdk", "after");
  g_assert_cmpint(hook_calls, ==, 2);
  g_log_set_always_fatal(fatal);
  g_free(hook_seen);
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
  g_test_add_func("/log/flood", test_log_flood);
  g_test_add_func("/log/two-windows", test_log_two_windows);
  g_test_add_func("/log/hook", test_log_hook);
  status = g_test_run();
  char *rm[] = { "rm", "-rf", dir, NULL };
  g_spawn_sync(NULL, rm, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, NULL, NULL);
  g_free(path);
  g_free(dir);
  return status;
}
