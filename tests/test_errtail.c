/*
 * Tests for the end of a program's stderr (src/application/errtail.c), GLib-only: a program that
 * writes a lot keeps only the last ERR_TAIL_BYTES, cut at a line; the report comes when stderr ends,
 * or after the wait when a child it left keeps the pipe open; a stopped one reports nothing.
 */
#include "application/errtail.h"
#include "util.h"

#include <string.h>

typedef struct {
  gboolean done;
  char *text;
} Report;

static void on_done(const char *text, gpointer data)
{
  Report *r = data;

  r->done = TRUE;
  r->text = g_strdup(text);
}

static gboolean is_done(gconstpointer data)
{
  return ((const Report *)data)->done;
}

static GSubprocess *run_sh(const char *script)
{
  GSubprocess *p = g_subprocess_new(G_SUBPROCESS_FLAGS_STDERR_PIPE, NULL, "sh", "-c", script, NULL);

  g_assert_nonnull(p);
  return p;
}

/* Ten thousand lines, then the reason: the reason is there, the start is not, memory stays small. */
static void test_tail(void)
{
  GSubprocess *p = run_sh("i=0; while [ $i -lt 10000 ]; do echo \"noise line $i\" >&2; i=$((i+1)); done; "
                          "echo 'error: rust-analyzer is not installed for the toolchain' >&2; exit 1");
  ErrTail *t = err_tail_new(g_subprocess_get_stderr_pipe(p));
  Report r = { 0 };

  err_tail_finish(t, 5000, on_done, &r); /* the report waits for the end of stderr */
  run_until(is_done, &r);
  g_assert_true(r.done);
  g_subprocess_wait(p, NULL, NULL);
  g_assert_cmpuint(strlen(r.text), <=, ERR_TAIL_BYTES);
  g_assert_true(g_str_has_suffix(r.text, "error: rust-analyzer is not installed for the toolchain"));
  g_assert_null(strstr(r.text, "noise line 0\n"));
  g_assert_true(g_str_has_prefix(r.text, "noise line ")); /* cut at a line start */
  char *last = err_tail_last_line(r.text);
  g_assert_cmpstr(last, ==, "error: rust-analyzer is not installed for the toolchain");
  g_free(last);
  g_free(r.text);
  g_object_unref(p);
}

/* The program is gone but a child it started keeps stderr open: the report comes after the wait,
 * with what was written. */
static void test_held_open(void)
{
  GSubprocess *p = run_sh("echo 'Cannot find module typescript' >&2; sleep 3 & exit 3");
  ErrTail *t = err_tail_new(g_subprocess_get_stderr_pipe(p));
  Report r = { 0 };
  gint64 start = g_get_monotonic_time();

  g_subprocess_wait(p, NULL, NULL);
  run_for(100);
  char *so_far = err_tail_text(t);
  g_assert_cmpstr(so_far, ==, "Cannot find module typescript");
  g_free(so_far);
  err_tail_finish(t, 300, on_done, &r);
  run_until(is_done, &r);
  g_assert_true(r.done);
  g_assert_cmpint(g_get_monotonic_time() - start, <, 3 * G_USEC_PER_SEC);
  g_assert_cmpstr(r.text, ==, "Cannot find module typescript");
  g_free(r.text);
  g_object_unref(p); /* the sleep ends by itself */
}

/* Stopped on purpose: no report, nothing left behind (ASan's leak check). */
static void test_free(void)
{
  GSubprocess *p = run_sh("echo started >&2; sleep 1");
  ErrTail *t = err_tail_new(g_subprocess_get_stderr_pipe(p));

  run_for(50);
  err_tail_free(t);
  g_subprocess_force_exit(p);
  g_subprocess_wait(p, NULL, NULL);
  run_for(50);
  g_object_unref(p);
  char *last = err_tail_last_line("");
  g_assert_cmpstr(last, ==, "");
  g_free(last);
}

int main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/errtail/tail", test_tail);
  g_test_add_func("/errtail/held-open", test_held_open);
  g_test_add_func("/errtail/free", test_free);
  return g_test_run();
}
