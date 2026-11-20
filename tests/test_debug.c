/*
 * Tests for the Debug Adapter Protocol client (src/debug/dap.c) against a real gdb ("gdb -i dap") and
 * a small C program compiled for it: breakpoints, logpoints, stopping, the stack, stepping, memory, a
 * core dump, attaching, the program's end; Delve and debugpy the same way when they are installed.
 * Skipped when gdb (14 or newer, with Python) or gcc is not there. The debuggers' table
 * (src/debug/adapters.c) too.
 */
#define _DEFAULT_SOURCE   /* kill() */
#define _XOPEN_SOURCE 700 /* posix_openpt() */
#include "debug/adapters.h"
#include "debug/dap.h"
#include "lsp/json.h"

#include <fcntl.h>
#include <glib/gstdio.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
  GString *console; /* the "output" events' text (logpoints) */
  GPid child;       /* what runInTerminal started */
} Run;

static void on_event(gpointer owner, const char *event, JsonNode *body)
{
  Run *r = owner;

  if (!event) {
    r->gone = TRUE;
    g_main_loop_quit(r->loop);
    return;
  }
  if (strcmp(event, "output") == 0 && r->console && json_string_at(body, "output"))
    g_string_append(r->console, json_string_at(body, "output"));
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

/* runInTerminal: the program is started (its output dropped), its pid the answer. */
static char *on_request(gpointer owner, const char *command, JsonNode *arguments, char **error)
{
  Run *r = owner;
  JsonNode *args = json_at(arguments, "args");
  GPtrArray *argv = g_ptr_array_new();
  GError *failed = NULL;
  char *answer = NULL;

  g_assert_cmpstr(command, ==, "runInTerminal");
  for (guint i = 0; args && i < json_array_get_length(json_node_get_array(args)); i++)
    g_ptr_array_add(argv, (gpointer)json_array_get_string_element(json_node_get_array(args), i));
  g_ptr_array_add(argv, NULL);
  if (g_spawn_async(json_string_at(arguments, "cwd"), (char **)argv->pdata, NULL,
                    G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_DO_NOT_REAP_CHILD, NULL, NULL, &r->child,
                    &failed))
    answer = g_strdup_printf("{\"processId\":%d}", (int)r->child);
  else
    *error = g_strdup(failed->message);
  g_clear_error(&failed);
  g_ptr_array_free(argv, TRUE);
  return answer;
}

static Run new_run(void)
{
  return (Run){ .loop = g_main_loop_new(NULL, FALSE),
                .events = g_ptr_array_new_with_free_func(g_free),
                .console = g_string_new("") };
}

static void free_run(Run *r)
{
  if (r->child > 0) {
    kill(r->child, SIGKILL);
    g_spawn_close_pid(r->child);
  }
  g_clear_pointer(&r->body, json_node_unref);
  g_free(r->error);
  g_free(r->wanted);
  g_ptr_array_free(r->events, TRUE);
  g_string_free(r->console, TRUE);
  g_main_loop_unref(r->loop);
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

  Run r = new_run();
  const char *argv[] = { "gdb", "-q", "-nx", "-iex", "set debuginfod enabled off", "-i", "dap", NULL };
  GError *error = NULL;
  Dap *d = dap_start(argv, dir, NULL, NULL, "gdb", on_event, NULL, &r, &error);
  char *core = g_build_filename(dir, "demo.core", NULL);
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
  /* line 10 is a logpoint: it prints, the program goes on */
  char *where = g_strdup_printf("{\"source\":{\"path\":\"%s\"},\"breakpoints\":[{\"line\":4},"
                                "{\"line\":10,\"logMessage\":\"a is {a}\"}]}",
                                source);
  ask(&r, d, "setBreakpoints", where);
  g_assert_null(r.error);
  g_assert_cmpint(json_array_get_length(json_node_get_array(json_at(r.body, "breakpoints"))), ==, 2);
  ask(&r, d, "configurationDone", NULL);

  wait_event(&r, "stopped");
  g_assert_cmpint(stopped_line(&r, d), ==, 4);
  ask(&r, d, "stackTrace", "{\"threadId\":1,\"levels\":5}");
  g_assert_cmpstr(json_string_at(r.body, "stackFrames.0.name"), ==, "twice");
  g_assert_cmpstr(json_string_at(r.body, "stackFrames.0.source.path"), ==, source);
  g_assert_cmpstr(json_string_at(r.body, "stackFrames.1.name"), ==, "main");
  g_assert_cmpint(json_int_at(r.body, "stackFrames.1.line", -1), ==, 9);

  /* m's memory: where it is, then its four bytes once line 4 set it to 6 */
  gint64 frame = json_int_at(r.body, "stackFrames.0.id", 0);
  char *address =
      g_strdup_printf("{\"expression\":\"&m\",\"frameId\":%" G_GINT64_FORMAT ",\"context\":\"watch\"}", frame);
  ask(&r, d, "evaluate", address);
  g_assert_null(r.error);
  char *reference = g_strdup(json_string_at(r.body, "memoryReference"));
  if (!reference) /* the value is the address: "(int *) 0x7ffd..." */
    reference = g_strdup(strstr(json_string_at(r.body, "result"), "0x"));
  g_assert_nonnull(reference);
  g_assert_true(json_at(dap_capabilities(d), "supportsReadMemoryRequest") != NULL);

  ask(&r, d, "next", "{\"threadId\":1}"); /* step over: the next line */
  wait_event(&r, "stopped");
  g_assert_cmpint(stopped_line(&r, d), ==, 5);
  char *read = g_strdup_printf("{\"memoryReference\":\"%s\",\"count\":4}", reference);
  ask(&r, d, "readMemory", read);
  g_assert_null(r.error);
  gsize n = 0;
  guchar *bytes = g_base64_decode(json_string_at(r.body, "data"), &n);
  g_assert_cmpuint(n, ==, 4);
  g_assert_cmpint(bytes[0] | bytes[1] << 8 | bytes[2] << 16 | bytes[3] << 24, ==, 6); /* little-endian */
  g_free(bytes);

  /* a core dump of it as it is now, opened below */
  char *gcore = g_strdup_printf("{\"expression\":\"gcore %s\",\"context\":\"repl\"}", core);
  ask(&r, d, "evaluate", gcore);
  g_assert_null(r.error);
  g_assert_true(g_file_test(core, G_FILE_TEST_IS_REGULAR));

  ask(&r, d, "stepOut", "{\"threadId\":1}"); /* back in main, on the call's line */
  wait_event(&r, "stopped");
  g_assert_cmpint(stopped_line(&r, d), ==, 9);

  ask(&r, d, "continue", "{\"threadId\":1}"); /* to the end, past the logpoint */
  wait_event(&r, "terminated");
  g_assert_nonnull(strstr(r.console->str, "a is 6"));

  ask(&r, d, "disconnect", "{\"terminateDebuggee\":true}");
  dap_free(d);

  /* the core dump: stopped where it was, in twice, at line 5; nothing runs */
  d = dap_start(argv, dir, NULL, NULL, "gdb", on_event, NULL, &r, &error);
  g_assert_no_error(error);
  wait_event(&r, "initialized");
  const char *request = NULL;
  char *open = adapter_core(ADAPTER_GDB, exe, core, &request);
  g_assert_cmpstr(request, ==, "attach");
  dap_request(d, request, open, NULL, NULL);
  ask(&r, d, "configurationDone", NULL);
  wait_event(&r, "stopped");
  ask(&r, d, "stackTrace", "{\"threadId\":1,\"levels\":5}");
  g_assert_cmpstr(json_string_at(r.body, "stackFrames.0.name"), ==, "twice");
  g_assert_cmpint(json_int_at(r.body, "stackFrames.0.line", -1), ==, 5);
  ask(&r, d, "disconnect", "{\"terminateDebuggee\":true}");
  dap_free(d);
  g_free(open);

  g_free(gcore);
  g_free(read);
  g_free(reference);
  g_free(address);
  g_free(where);
  g_free(args);
out:
  free_run(&r);
  g_remove(core);
  g_free(core);
  g_remove(exe);
  g_remove(source);
  g_rmdir(dir);
  g_free(exe);
  g_free(source);
  g_free(dir);
}

/* Attaching to a running process: it stops where it is, and goes on once the debugger detaches.
 * Only where the system lets a debugger attach to a process it did not start (ptrace_scope 0). */
static void test_attach(void)
{
  char *scope = NULL;
  gboolean allowed = g_file_get_contents("/proc/sys/kernel/yama/ptrace_scope", &scope, NULL, NULL) && atoi(scope) == 0;

  g_free(scope);
  if (!have_tools() || !allowed) {
    g_test_skip("no gdb, or ptrace_scope does not allow attaching");
    return;
  }
  const char *sleeper[] = { "sleep", "30", NULL };
  GPid pid;
  g_assert_true(g_spawn_async(NULL, (char **)sleeper, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD, NULL, NULL,
                              &pid, NULL));
  Run r = new_run();
  const char *argv[] = { "gdb", "-q", "-nx", "-iex", "set debuginfod enabled off", "-i", "dap", NULL };
  GError *error = NULL;
  Dap *d = dap_start(argv, "/", NULL, NULL, "gdb", on_event, NULL, &r, &error);
  const char *request = NULL;
  char *attach = adapter_attach(ADAPTER_GDB, pid, NULL, &request);

  g_assert_no_error(error);
  wait_event(&r, "initialized");
  if (dap_capabilities(d)) {
    dap_request(d, request, attach, NULL, NULL);
    ask(&r, d, "configurationDone", NULL);
    wait_event(&r, "stopped");
    ask(&r, d, "disconnect", "{\"terminateDebuggee\":false}");
  }
  dap_free(d);
  g_assert_cmpint(kill(pid, 0), ==, 0); /* it goes on */
  kill(pid, SIGKILL);
  g_spawn_close_pid(pid);
  g_free(attach);
  free_run(&r);
}

/* A Go program under Delve, which dials in on a socket (dap.c) and runs the program in the foreground of
 * its terminal (a pty, as the Debug panel's): a breakpoint stops it, its output reaches the terminal.
 * Skipped without dlv and go. */
static void test_delve(void)
{
  char *missing = NULL, **argv = adapter_argv(ADAPTER_DELVE, NULL, NULL, &missing), *go = g_find_program_in_path("go");

  if (!argv || !go) {
    g_test_skip("no dlv or go");
    g_free(missing);
    g_strfreev(argv);
    g_free(go);
    return;
  }
  char *dir = g_dir_make_tmp("t-code-delve-XXXXXX", NULL), *source = g_build_filename(dir, "main.go", NULL);
  char *mod = g_build_filename(dir, "go.mod", NULL);
  g_file_set_contents(mod, "module demo\n\ngo 1.21\n", -1, NULL);
  g_file_set_contents(source,
                      "package main\n"         /* 1 */
                      "\n"                     /* 2 */
                      "import \"fmt\"\n"       /* 3 */
                      "\n"                     /* 4 */
                      "func main() {\n"        /* 5 */
                      "\tn := 21\n"            /* 6 */
                      "\tfmt.Println(n * 2)\n" /* 7 */
                      "}\n",                   /* 8 */
                      -1, NULL);
  Run r = new_run();
  GError *error = NULL;
  int master = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
  g_assert_cmpint(master, >=, 0);
  g_assert_cmpint(grantpt(master), ==, 0);
  g_assert_cmpint(unlockpt(master), ==, 0);
  char *pts = g_strdup(ptsname(master));
  Dap *d = dap_start((const char *const *)argv, dir, NULL, pts, adapter_id(ADAPTER_DELVE), on_event, NULL, &r, &error);
  AdapterTarget target = { .program = dir, .cwd = dir, .tty = pts };
  char *launch = adapter_launch(ADAPTER_DELVE, &target);
  char *where = g_strdup_printf("{\"source\":{\"path\":\"%s\"},\"breakpoints\":[{\"line\":7}]}", source);

  g_assert_no_error(error);
  dap_request(d, "launch", launch, on_reply, &r); /* Delve builds the program first */
  wait_for(&r);
  g_assert_null(r.error);
  wait_event(&r, "initialized");
  ask(&r, d, "setBreakpoints", where);
  g_assert_null(r.error);
  ask(&r, d, "configurationDone", NULL);
  wait_event(&r, "stopped");
  g_assert_cmpint(stopped_line(&r, d), ==, 7);
  ask(&r, d, "continue", "{\"threadId\":1}");
  wait_event(&r, "terminated");
  char printed[256] = { 0 };
  g_assert_cmpint(read(master, printed, sizeof printed - 1), >, 0);
  g_assert_nonnull(strstr(printed, "42"));
  ask(&r, d, "disconnect", "{\"terminateDebuggee\":true}");
  dap_free(d);
  g_free(where);
  g_free(launch);
  free_run(&r);
  g_strfreev(argv);
  g_free(go);
  char *bin = g_build_filename(dir, "__debug_bin", NULL);
  g_remove(bin);
  g_free(bin);
  close(master);
  g_free(pts);
  g_remove(source);
  g_remove(mod);
  g_rmdir(dir);
  g_free(mod);
  g_free(source);
  g_free(dir);
}

/* A Python file under debugpy, which has the program started by the client (runInTerminal): a
 * breakpoint stops it. Skipped when python3 has no debugpy. */
static void test_debugpy(void)
{
  char **argv = adapter_argv(ADAPTER_DEBUGPY, NULL, NULL, NULL);

  if (!argv) {
    g_test_skip("python3 has no debugpy");
    return;
  }
  char *dir = g_dir_make_tmp("t-code-debugpy-XXXXXX", NULL), *source = g_build_filename(dir, "demo.py", NULL);
  g_file_set_contents(source,
                      "def twice(n):\n"    /* 1 */
                      "    m = n * 2\n"    /* 2 */
                      "    return m\n"     /* 3 */
                      "\n"                 /* 4 */
                      "print(twice(3))\n", /* 5 */
                      -1, NULL);
  Run r = new_run();
  GError *error = NULL;
  Dap *d = dap_start((const char *const *)argv, dir, NULL, NULL, adapter_id(ADAPTER_DEBUGPY), on_event, on_request, &r,
                     &error);
  AdapterTarget target = { .program = source, .cwd = dir };
  char *launch = adapter_launch(ADAPTER_DEBUGPY, &target);
  char *where = g_strdup_printf("{\"source\":{\"path\":\"%s\"},\"breakpoints\":[{\"line\":3}]}", source);

  g_assert_no_error(error);
  dap_request(d, "launch", launch, NULL, NULL);
  wait_event(&r, "initialized");
  ask(&r, d, "setBreakpoints", where);
  g_assert_null(r.error);
  ask(&r, d, "configurationDone", NULL);
  wait_event(&r, "stopped");
  g_assert_cmpint(r.child, >, 0); /* started by the client */
  ask(&r, d, "threads", NULL);
  gint64 thread = json_int_at(r.body, "threads.0.id", 1);
  char *stack = g_strdup_printf("{\"threadId\":%" G_GINT64_FORMAT ",\"levels\":1}", thread);
  ask(&r, d, "stackTrace", stack);
  g_assert_cmpint(json_int_at(r.body, "stackFrames.0.line", -1), ==, 3);
  g_assert_cmpstr(json_string_at(r.body, "stackFrames.0.name"), ==, "twice");
  ask(&r, d, "disconnect", "{\"terminateDebuggee\":true}");
  dap_free(d);
  g_free(stack);
  g_free(where);
  g_free(launch);
  free_run(&r);
  g_strfreev(argv);
  g_remove(source);
  g_rmdir(dir);
  g_free(source);
  g_free(dir);
}

/* The same program under lldb-dap, which has the program started by the client (runInTerminal): a
 * breakpoint, a logpoint, the stack, a variable, its memory. Skipped without lldb-dap. */
static void test_lldb(void)
{
  char *missing = NULL, **argv = adapter_argv(ADAPTER_LLDB, NULL, NULL, &missing), *gcc = g_find_program_in_path("gcc");

  if (!argv || !gcc) {
    g_test_skip("no lldb-dap or gcc");
    g_strfreev(argv);
    g_free(missing);
    g_free(gcc);
    return;
  }
  char *dir = g_dir_make_tmp("t-code-lldb-XXXXXX", NULL);
  char *source = g_build_filename(dir, "demo.c", NULL), *exe = g_build_filename(dir, "demo", NULL);
  int status = 0;
  g_file_set_contents(source, program, -1, NULL);
  const char *cc[] = { "gcc", "-g", "-O0", "-o", exe, source, NULL };
  g_assert_true(g_spawn_sync(dir, (char **)cc, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, &status, NULL));
  g_assert_cmpint(status, ==, 0);

  Run r = new_run();
  GError *error = NULL;
  Dap *d =
      dap_start((const char *const *)argv, dir, NULL, NULL, adapter_id(ADAPTER_LLDB), on_event, on_request, &r, &error);
  AdapterTarget target = { .program = exe, .cwd = dir };
  char *launch = adapter_launch(ADAPTER_LLDB, &target);
  char *where = g_strdup_printf("{\"source\":{\"path\":\"%s\"},\"breakpoints\":[{\"line\":4},"
                                "{\"line\":10,\"logMessage\":\"a is {a}\"}]}",
                                source);
  g_assert_no_error(error);
  dap_request(d, "launch", launch, NULL, NULL);
  wait_event(&r, "initialized");
  ask(&r, d, "setBreakpoints", where);
  g_assert_null(r.error);
  ask(&r, d, "configurationDone", NULL);
  wait_event(&r, "stopped");
  g_assert_cmpint(r.child, >, 0); /* started by the client */

  ask(&r, d, "threads", NULL);
  gint64 thread = json_int_at(r.body, "threads.0.id", 1);
  char *stack = g_strdup_printf("{\"threadId\":%" G_GINT64_FORMAT ",\"levels\":5}", thread);
  ask(&r, d, "stackTrace", stack);
  g_assert_cmpint(json_int_at(r.body, "stackFrames.0.line", -1), ==, 4);
  g_assert_nonnull(strstr(json_string_at(r.body, "stackFrames.0.name"), "twice"));
  g_assert_cmpint(json_int_at(r.body, "stackFrames.1.line", -1), ==, 9);
  gint64 frame = json_int_at(r.body, "stackFrames.0.id", 0);

  /* n is 3; &m has a memory reference readMemory reads */
  char *value = g_strdup_printf("{\"expression\":\"n\",\"frameId\":%" G_GINT64_FORMAT ",\"context\":\"watch\"}", frame);
  ask(&r, d, "evaluate", value);
  g_assert_null(r.error);
  g_assert_cmpstr(json_string_at(r.body, "result"), ==, "3");
  char *address =
      g_strdup_printf("{\"expression\":\"&m\",\"frameId\":%" G_GINT64_FORMAT ",\"context\":\"watch\"}", frame);
  ask(&r, d, "evaluate", address);
  g_assert_null(r.error);
  char *reference = g_strdup(json_string_at(r.body, "memoryReference"));
  if (!reference)
    reference = g_strdup(strstr(json_string_at(r.body, "result"), "0x"));
  g_assert_nonnull(reference);
  char *next = g_strdup_printf("{\"threadId\":%" G_GINT64_FORMAT "}", thread);
  ask(&r, d, "next", next);
  wait_event(&r, "stopped");
  char *read = g_strdup_printf("{\"memoryReference\":\"%s\",\"count\":4}", reference);
  ask(&r, d, "readMemory", read);
  g_assert_null(r.error);
  gsize n = 0;
  guchar *bytes = g_base64_decode(json_string_at(r.body, "data"), &n);
  g_assert_cmpuint(n, ==, 4);
  g_assert_cmpint(bytes[0] | bytes[1] << 8 | bytes[2] << 16 | bytes[3] << 24, ==, 6);
  g_free(bytes);

  ask(&r, d, "continue", next); /* to the end, past the logpoint */
  wait_event(&r, "terminated");
  g_assert_nonnull(strstr(r.console->str, "a is 6"));
  ask(&r, d, "disconnect", "{\"terminateDebuggee\":true}");
  dap_free(d);

  g_free(read);
  g_free(next);
  g_free(reference);
  g_free(address);
  g_free(value);
  g_free(stack);
  g_free(where);
  g_free(launch);
  free_run(&r);
  g_strfreev(argv);
  g_free(gcc);
  g_remove(exe);
  g_remove(source);
  g_rmdir(dir);
  g_free(exe);
  g_free(source);
  g_free(dir);
}

/* The debuggers' own console chatter is left out; a logpoint's line is not. */
static void test_noise(void)
{
  g_assert_true(adapter_noise(ADAPTER_LLDB, "Attached to process 856849."));
  g_assert_true(adapter_noise(ADAPTER_LLDB, "Process 856849 exited with status = 0 (0x00000000)"));
  g_assert_true(adapter_noise(ADAPTER_LLDB, "warning: (x86_64) /lib64/libasan.so.8 mismatch addresses for section "
                                            "container when merging with /lib64/libasan.so.8, expected: 0x0, actual: "
                                            "0x20"));
  g_assert_true(adapter_noise(ADAPTER_LLDB, "Executable binary set to '/p/demo' (x86_64-unknown-linux-gnu)."));
  g_assert_true(adapter_noise(ADAPTER_DELVE, "Type 'dlv help' for list of commands."));
  g_assert_false(adapter_noise(ADAPTER_LLDB, "a is 6"));
  g_assert_false(adapter_noise(ADAPTER_LLDB, "Process count: 3"));
  g_assert_false(adapter_noise(ADAPTER_LLDB, "warning: low memory"));
  g_assert_false(adapter_noise(ADAPTER_GDB, "Attached to process 42."));
}

/* What each debugger is told to launch, attach to, or open. */
static void test_adapter_arguments(void)
{
  const char *const args[] = { "-v", "two words", NULL };
  AdapterTarget native = { .program = "/p/demo", .args = args, .cwd = "/p" };
  AdapterTarget python = {
    .program = "/p/app/cli.py", .cwd = "/p", .module = "app.cli", .python = "/p/.venv/bin/python"
  };
  const char *request = NULL;
  char *json;

  json = adapter_launch(ADAPTER_GDB, &native);
  g_assert_cmpstr(json, ==, "{\"program\":\"/p/demo\",\"args\":[\"-v\",\"two words\"],\"cwd\":\"/p\"}");
  g_free(json);
  json = adapter_launch(ADAPTER_LLDB, &native);
  g_assert_nonnull(strstr(json, "\"runInTerminal\":true"));
  g_free(json);
  json = adapter_launch(ADAPTER_DEBUGPY, &python);
  g_assert_nonnull(strstr(json, "\"module\":\"app.cli\""));
  g_assert_null(strstr(json, "\"program\""));
  g_assert_nonnull(strstr(json, "\"python\":\"/p/.venv/bin/python\""));
  g_assert_nonnull(strstr(json, "\"console\":\"integratedTerminal\""));
  g_free(json);
  native.tty = "/dev/pts/9";
  json = adapter_launch(ADAPTER_DELVE, &native);
  g_assert_true(g_str_has_prefix(json, "{\"mode\":\"debug\",\"program\":\"/p/demo\""));
  g_assert_nonnull(strstr(json, "\"stdinFrom\":\"/dev/pts/9\",\"stdoutTo\":\"/dev/pts/9\""));
  g_free(json);

  json = adapter_attach(ADAPTER_GDB, 42, "/p/demo", &request);
  g_assert_cmpstr(request, ==, "attach");
  g_assert_cmpstr(json, ==, "{\"pid\":42,\"program\":\"/p/demo\"}");
  g_free(json);
  json = adapter_attach(ADAPTER_DELVE, 42, NULL, &request);
  g_assert_cmpstr(json, ==, "{\"mode\":\"local\",\"processId\":42}");
  g_free(json);

  json = adapter_core(ADAPTER_GDB, "/p/demo", "/tmp/core", &request);
  g_assert_cmpstr(request, ==, "attach");
  g_assert_cmpstr(json, ==, "{\"program\":\"/p/demo\",\"coreFile\":\"/tmp/core\"}");
  g_free(json);
  json = adapter_core(ADAPTER_DELVE, "/p/demo", "/tmp/core", &request);
  g_assert_cmpstr(request, ==, "launch");
  g_assert_cmpstr(json, ==, "{\"mode\":\"core\",\"program\":\"/p/demo\",\"coreFilePath\":\"/tmp/core\"}");
  g_free(json);
  g_assert_null(adapter_core(ADAPTER_DEBUGPY, "/p/demo.py", "/tmp/core", &request));

  char *missing = NULL; /* an interpreter without debugpy: how to install it */
  g_assert_null(adapter_argv(ADAPTER_DEBUGPY, NULL, "/nonexistent/python", &missing));
  g_assert_nonnull(strstr(missing, "/nonexistent/python -m pip install debugpy"));
  g_free(missing);
}

/* systemd-coredump's list: the user's present ones, the newest first. */
static void test_core_dumps(void)
{
  const char *json =
      "[{\"time\":100,\"pid\":10,\"uid\":1000,\"sig\":11,\"corefile\":\"present\",\"exe\":\"/p/old\"},"
      "{\"time\":300,\"pid\":30,\"uid\":1000,\"sig\":6,\"corefile\":\"present\",\"exe\":\"/p/new\"},"
      "{\"time\":200,\"pid\":20,\"uid\":1000,\"sig\":11,\"corefile\":\"missing\",\"exe\":\"/p/gone\"},"
      "{\"time\":400,\"pid\":40,\"uid\":0,\"sig\":11,\"corefile\":\"present\",\"exe\":\"/usr/bin/root\"}]";
  GPtrArray *list = adapter_parse_core_dumps(json, 1000);

  g_assert_cmpuint(list->len, ==, 2);
  g_assert_cmpstr(((CoreDump *)g_ptr_array_index(list, 0))->exe, ==, "/p/new");
  g_assert_cmpint(((CoreDump *)g_ptr_array_index(list, 0))->signal, ==, 6);
  g_assert_cmpint(((CoreDump *)g_ptr_array_index(list, 1))->pid, ==, 10);
  g_ptr_array_unref(list);
  list = adapter_parse_core_dumps("not json", 1000);
  g_assert_cmpuint(list->len, ==, 0);
  g_ptr_array_unref(list);
}

/* The processes to attach to: the user's, without t-code's own (here: the test and its children). */
static void test_attachable(void)
{
  const char *sleeper[] = { "sleep", "30", NULL };
  GPid pid;
  gboolean found = FALSE, self = FALSE;

  g_assert_true(g_spawn_async(NULL, (char **)sleeper, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD, NULL, NULL,
                              &pid, NULL));
  GPtrArray *all = adapter_attachable(0), *others = adapter_attachable(getpid());
  for (guint i = 0; i < all->len; i++)
    if (((Attachable *)g_ptr_array_index(all, i))->pid == pid) {
      found = TRUE;
      g_assert_cmpstr(((Attachable *)g_ptr_array_index(all, i))->command, ==, "sleep 30");
      g_assert_cmpstr(((Attachable *)g_ptr_array_index(all, i))->name, ==, "sleep");
    }
  for (guint i = 0; i < others->len; i++)
    self |= ((Attachable *)g_ptr_array_index(others, i))->pid == pid ||
            ((Attachable *)g_ptr_array_index(others, i))->pid == getpid();
  g_assert_true(found);
  g_assert_false(self);
  g_ptr_array_unref(all);
  g_ptr_array_unref(others);
  kill(pid, SIGKILL);
  g_spawn_close_pid(pid);
}

/* A debugger that cannot start says so, and nothing is left behind. */
static void test_missing(void)
{
  const char *argv[] = { "t-code-no-such-debugger", NULL };
  GError *error = NULL;
  Dap *d = dap_start(argv, "/", NULL, NULL, "gdb", on_event, NULL, NULL, &error);

  g_assert_null(d);
  g_assert_nonnull(error);
  g_error_free(error);
}

int main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/dap/session", test_session);
  g_test_add_func("/dap/attach", test_attach);
  g_test_add_func("/dap/delve", test_delve);
  g_test_add_func("/dap/debugpy", test_debugpy);
  g_test_add_func("/dap/lldb", test_lldb);
  g_test_add_func("/dap/missing", test_missing);
  g_test_add_func("/adapters/arguments", test_adapter_arguments);
  g_test_add_func("/adapters/noise", test_noise);
  g_test_add_func("/adapters/core-dumps", test_core_dumps);
  g_test_add_func("/adapters/attachable", test_attachable);
  return g_test_run();
}
