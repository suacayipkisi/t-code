/*
 * Tests for the Debug Adapter Protocol client (src/debug/dap.c) against a real gdb ("gdb -i dap") and
 * a small C program compiled for it: breakpoints, stopping, the stack, stepping, the program's end.
 * Skipped when gdb (14 or newer, with Python) or gcc is not there.
 */
#include "debug/dap.h"
#include "lsp/json.h"

#include <glib/gstdio.h>
#include <string.h>

#define TIMEOUT_MS 20000

static const char program[] = "#include <stdio.h>\n"      /* 1 */
                              "static int twice(int n)\n" /* 2 */
                              "{\n"                       /* 3 */
                              "  int m = n * 2;\n"        /* 4 */
                              "  return m;\n"             /* 5 */
                              "}\n"                       /* 6 */
                              "int main(void)\n"          /* 7 */
                              "{\n"                       /* 8 */
                              "  int a = twice(3);\n"     /* 9 */
                              "  printf(\"%d\\n\", a);\n" /* 10 */
                              "  return 0;\n"             /* 11 */
                              "}\n";                      /* 12 */

typedef struct {
  GMainLoop *loop;
  GPtrArray *events; /* the names of the events so far */
  char *wanted;      /* the loop stops at this event */
  JsonNode *body;    /* the last answer's body (a copy) */
  char *error;
  gboolean gone; /* gdb went */
  gboolean timed_out;
} Run;

static void on_event(gpointer owner, const char *event, JsonNode *body G_GNUC_UNUSED)
{
  Run *r = owner;

  if (!event) {
    r->gone = TRUE;
    g_main_loop_quit(r->loop);
    return;
  }
  g_ptr_array_add(r->events, g_strdup(event));
  if (g_strcmp0(event, r->wanted) == 0)
    g_main_loop_quit(r->loop);
}

static void on_reply(gpointer data, JsonNode *body, const char *error)
{
  Run *r = data;

  g_clear_pointer(&r->body, json_node_unref);
  g_clear_pointer(&r->error, g_free);
  r->body = body ? json_node_copy(body) : NULL;
  r->error = g_strdup(error);
  g_main_loop_quit(r->loop);
}

static gboolean on_timeout(gpointer data)
{
  Run *r = data;

  r->timed_out = TRUE;
  g_main_loop_quit(r->loop);
  return G_SOURCE_CONTINUE;
}

/* Runs the loop until it is quit (an answer, the wanted event); fails on the timeout. */
static void wait_for(Run *r)
{
  guint id = g_timeout_add(TIMEOUT_MS, on_timeout, r);

  g_main_loop_run(r->loop);
  g_source_remove(id);
  g_assert_false(r->timed_out);
}

/* Sends a request and waits for its answer. */
static void ask(Run *r, Dap *d, const char *command, const char *arguments)
{
  dap_request(d, command, arguments, on_reply, r);
  wait_for(r);
}

/* Waits until event comes (or came already since the last wait). */
static void wait_event(Run *r, const char *event)
{
  for (guint i = 0; i < r->events->len; i++)
    if (strcmp(g_ptr_array_index(r->events, i), event) == 0) {
      g_ptr_array_set_size(r->events, 0);
      return;
    }
  g_set_str(&r->wanted, event);
  wait_for(r);
  g_ptr_array_set_size(r->events, 0);
}

/* The line the program stopped at (the top frame's). */
static int stopped_line(Run *r, Dap *d)
{
  ask(r, d, "stackTrace", "{\"threadId\":1,\"levels\":1}");
  g_assert_null(r->error);
  return (int)json_int_at(r->body, "stackFrames.0.line", -1);
}

static gboolean have_tools(void)
{
  char *gdb = g_find_program_in_path("gdb"), *gcc = g_find_program_in_path("gcc");
  gboolean ok = gdb && gcc;

  g_free(gdb);
  g_free(gcc);
  return ok;
}

static void test_session(void)
{
  if (!have_tools()) {
    g_test_skip("no gdb or gcc");
    return;
  }
  char *dir = g_dir_make_tmp("t-code-debug-XXXXXX", NULL);
  char *source = g_build_filename(dir, "demo.c", NULL), *exe = g_build_filename(dir, "demo", NULL);
  int status = 0;
  g_file_set_contents(source, program, -1, NULL);
  const char *cc[] = { "gcc", "-g", "-O0", "-o", exe, source, NULL };
  g_assert_true(g_spawn_sync(dir, (char **)cc, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, &status, NULL));
  g_assert_cmpint(status, ==, 0);

  Run r = { .loop = g_main_loop_new(NULL, FALSE), .events = g_ptr_array_new_with_free_func(g_free) };
  const char *argv[] = { "gdb", "-q", "-nx", "-iex", "set debuginfod enabled off", "-i", "dap", NULL };
  GError *error = NULL;
  Dap *d = dap_start(argv, dir, NULL, on_event, &r, &error);
  g_assert_no_error(error);

  /* initialize went with dap_start: its answer comes before "initialized" */
  wait_event(&r, "initialized");
  if (!dap_capabilities(d)) { /* a gdb without DAP (older than 14, or without Python) */
    dap_free(d);
    g_test_skip("gdb has no DAP");
    goto out;
  }
  g_assert_nonnull(json_at(dap_capabilities(d), "supportsConfigurationDoneRequest"));

  char *args = g_strdup_printf("{\"program\":\"%s\",\"cwd\":\"%s\"}", exe, dir);
  dap_request(d, "launch", args, NULL, NULL); /* answered after configurationDone */
  char *where = g_strdup_printf("{\"source\":{\"path\":\"%s\"},\"breakpoints\":[{\"line\":4}]}", source);
  ask(&r, d, "setBreakpoints", where);
  g_assert_null(r.error);
  g_assert_cmpint(json_array_get_length(json_node_get_array(json_at(r.body, "breakpoints"))), ==, 1);
  ask(&r, d, "configurationDone", NULL);

  wait_event(&r, "stopped");
  g_assert_cmpint(stopped_line(&r, d), ==, 4);
  ask(&r, d, "stackTrace", "{\"threadId\":1,\"levels\":5}");
  g_assert_cmpstr(json_string_at(r.body, "stackFrames.0.name"), ==, "twice");
  g_assert_cmpstr(json_string_at(r.body, "stackFrames.0.source.path"), ==, source);
  g_assert_cmpstr(json_string_at(r.body, "stackFrames.1.name"), ==, "main");
  g_assert_cmpint(json_int_at(r.body, "stackFrames.1.line", -1), ==, 9);

  ask(&r, d, "next", "{\"threadId\":1}"); /* step over: the next line */
  wait_event(&r, "stopped");
  g_assert_cmpint(stopped_line(&r, d), ==, 5);

  ask(&r, d, "stepOut", "{\"threadId\":1}"); /* back in main, on the call's line */
  wait_event(&r, "stopped");
  g_assert_cmpint(stopped_line(&r, d), ==, 9);

  ask(&r, d, "continue", "{\"threadId\":1}"); /* to the end */
  wait_event(&r, "terminated");

  ask(&r, d, "disconnect", "{\"terminateDebuggee\":true}");
  dap_free(d);
  g_free(where);
  g_free(args);
out:
  g_clear_pointer(&r.body, json_node_unref);
  g_free(r.error);
  g_free(r.wanted);
  g_ptr_array_free(r.events, TRUE);
  g_main_loop_unref(r.loop);
  g_remove(exe);
  g_remove(source);
  g_rmdir(dir);
  g_free(exe);
  g_free(source);
  g_free(dir);
}

/* A debugger that cannot start says so, and nothing is left behind. */
static void test_missing(void)
{
  const char *argv[] = { "t-code-no-such-debugger", NULL };
  GError *error = NULL;
  Dap *d = dap_start(argv, "/", NULL, on_event, NULL, &error);

  g_assert_null(d);
  g_assert_nonnull(error);
  g_error_free(error);
}

int main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/dap/session", test_session);
  g_test_add_func("/dap/missing", test_missing);
  return g_test_run();
}
