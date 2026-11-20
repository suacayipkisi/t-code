/*
 * Debugging (see debugger.h), with gdb by default; lldb-dap, debugpy and Delve go the same way over
 * the Debug Adapter Protocol (debug/adapters.c says how each one starts). A session goes:
 *   F5          the project is built in the shell (project/run.c); the build ends with
 *               "t-code --send '_debug <program>'", and the control socket calls debug_launch()
 *   launch      gdb starts ("gdb -i dap", debug/dap.c) with the program's terminal as its
 *               inferior-tty; it answers "initialize", says "initialized", gets every breakpoint
 *               and "configurationDone", then starts the program
 *   stopped     a breakpoint, a step's end, a crash (ASan aborts, so its report stops at its line
 *               too): the stack's innermost frame with a file is shown in the editor
 *   F10 ...     next / stepIn / stepOut / continue, with the stopped thread
 *   terminated  the program ended: gdb is stopped too (one gdb per run)
 * Shift+F5 asks gdb to end the program ("disconnect"), and kills it when it does not within END_MS.
 * The other debuggers have the program started in its terminal by t-code (runInTerminal). A session may
 * also attach to a running process, or open a core dump (a crashed program's memory, saved).
 */
#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 700 /* ptsname() under -std=c23 */
#include "debug/debugger.h"

#include "application/processes.h"
#include "debug/adapters.h"
#include "debug/dap.h"
#include "debug/memory.h"
#include "debug/view/debugview.h"
#include "debug/disassembly.h"
#include "editor/gutter/breakpoints.h"
#include "editor/editor.h"
#include "editor/tabs.h"
#include "lsp/json.h"
#include "plugins/plugins.h"
#include "popups/picker.h"
#include "project/run/run.h"
#include "project/session.h"
#include "window/terminal.h"
#include "window/window.h"

#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <sys/ioctl.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define END_MS 2000     /* gdb gets this long to end the program before it is killed */
#define LATER_MS 150    /* the program's last output reaches its terminal before "exited" */
#define STACK_LEVELS 50 /* frames asked for to find one with a file */
#define PROGRAM_DEPTH 4 /* folders deep a Makefile's programs are looked for */
#define MAX_PROGRAMS 200
#define EVALUATE_GDB "Evaluate while paused: a[i], sum / n, *p   (-exec runs a gdb command)"
#define EVALUATE "Evaluate while paused: a[i], sum / n, len(items)"

typedef enum {
  STARTING, /* gdb starts, the program is not running yet */
  RUNNING,
  STOPPED,
  ENDING, /* asked to end */
} State;

struct Debugger {
  App *app;
  Dap *dap;
  AdapterKind kind;
  char *name;         /* a plugin's debugger: the name it gave ("Java"); NULL: adapter_name's */
  GSubprocess *child; /* what the debugger had started in the program's terminal (runInTerminal) */
  gboolean examining; /* a core dump: nothing runs, the stack and variables are only looked at */
  State state;
  gboolean configured; /* the breakpoints went before the start: each change goes on its own now */
  gboolean attached;   /* to a process that ran before: Stop leaves it running */
  char *core;          /* a core dump taken out of systemd-coredump for this session, deleted at its end */
  int signo;           /* the signal a core dump's program got (systemd-coredump says), 0: unknown */
  gint64 thread;       /* the thread that stopped last, which the steps go on with */
  char *reason;        /* why it stopped last ("breakpoint", "step", "signal") */
  gboolean crashed;    /* it stopped on a crash: its end is that crash's */
  GHashTable *sent;    /* gdb's breakpoint id -> Sent: where it was asked for */
  gboolean restart;    /* build and start again once ended */
  DebugAgain again;    /* how: NULL for F5's program */
  guint end_id;        /* gdb is killed when it does not end the program in time */
};

/* The Debug panel, and what is between F5 and the start. One per window (one window per process). */
static struct {
  GtkWidget *status;
  GtkWidget *start, *pause, *over, *into, *out, *restart, *stop, *assembly, *memory;
  GtkWidget *evaluate;
  char *tty;     /* the terminal's device: the program's stdin, stdout and stderr */
  char *cwd;     /* where the program being built for debugging will run; NULL: none is */
  char *pick_in; /* the folder the program picker lists */
  GPtrArray *programs;
  char *core;        /* a core file whose program the program picker asks for, or NULL: F5's */
  GPtrArray *listed; /* the attach and core dump pickers' rows (Attachable, CoreDump) */
} ui;

/* The terminal ------------------------------------------------------------------------------- */

/* A line of t-code's own in the program's terminal, dimmed (or red: an error). */
static void say_text(App *t, gboolean error, const char *text)
{
  long column = 0;
  char *line;

  if (!t->debug.term)
    return;
  vte_terminal_get_cursor_position(t->debug.term, &column, NULL);
  line = g_strdup_printf("%s%s%s\x1b[0m\r\n", column > 0 ? "\r\n" : "", error ? "\x1b[31m" : "\x1b[2m", text);
  vte_terminal_feed(t->debug.term, line, -1);
  g_free(line);
}

/* The same, printf-like. */
G_GNUC_PRINTF(3, 4) static void say(App *t, gboolean error, const char *format, ...)
{
  va_list args;
  char *text;

  va_start(args, format);
  text = g_strdup_vprintf(format, args);
  va_end(args);
  say_text(t, error, text);
  g_free(text);
}

typedef struct {
  App *app;
  gboolean error;
  char *text;
} Line;

static gboolean say_now(gpointer data)
{
  Line *line = data;

  say_text(line->app, line->error, line->text);
  g_free(line->text);
  g_free(line);
  return G_SOURCE_REMOVE;
}

/* The same a moment later: gdb tells of the program's end before its terminal has read the program's
 * last lines. */
G_GNUC_PRINTF(3, 4) static void say_later(App *t, gboolean error, const char *format, ...)
{
  Line *line = g_new(Line, 1);
  va_list args;

  va_start(args, format);
  *line = (Line){ t, error, g_strdup_vprintf(format, args) };
  va_end(args);
  g_timeout_add(LATER_MS, say_now, line);
}

void debug_say(App *t, const char *text)
{
  say_text(t, TRUE, text);
}

/* Whether the debugger said it can do capability ("supportsReadMemoryRequest"). */
/* The debugger's name for the Debug panel ("gdb", "Java"). */
static const char *session_name(Debugger *s)
{
  return s->name ? s->name : adapter_name(s->kind);
}

static gboolean can(Debugger *s, const char *capability)
{
  JsonNode *yes = s ? json_at(dap_capabilities(s->dap), capability) : NULL;

  return yes && JSON_NODE_HOLDS_VALUE(yes) && json_node_get_boolean(yes);
}

/* The panel's buttons and status follow the session. */
static void update_ui(App *t, const char *status)
{
  Debugger *s = t->debug.session;
  gboolean stopped = s && s->state == STOPPED, moves = stopped && !s->examining;

  if (!ui.start)
    return;
  gtk_widget_set_sensitive(ui.start, !s || moves);
  gtk_widget_set_tooltip_text(ui.start, stopped ? "Continue (F5)" : "Start Debugging (F5)");
  gtk_widget_set_sensitive(ui.pause, s && s->state == RUNNING);
  gtk_widget_set_sensitive(ui.over, moves);
  gtk_widget_set_sensitive(ui.into, moves);
  gtk_widget_set_sensitive(ui.out, moves);
  gtk_widget_set_sensitive(ui.restart, s != NULL && !s->examining && !s->attached);
  gtk_widget_set_sensitive(ui.stop, s != NULL);
  gtk_widget_set_sensitive(ui.assembly, stopped && s->kind == ADAPTER_GDB); /* gdb's "disassemble /s" */
  gtk_widget_set_sensitive(ui.memory, stopped && can(s, "supportsReadMemoryRequest"));
  if (status)
    gtk_label_set_text(GTK_LABEL(ui.status), status);
}

/* The session's end ------------------------------------------------------------------------- */

/* gdb and the program are gone (or are killed now): everything is let go. */
static void finish(Debugger *s)
{
  App *t = s->app;
  gboolean again = s->restart;
  DebugAgain how = s->again;

  t->debug.session = NULL;
  g_clear_handle_id(&s->end_id, g_source_remove);
  dap_free(s->dap);
  if (s->child) { /* the program the debugger had started, with what it started in turn */
    const char *id = g_subprocess_get_identifier(s->child);
    if (id)
      kill(-atoi(id), SIGKILL);
    g_object_unref(s->child);
  }
  g_hash_table_destroy(s->sent);
  g_free(s->reason);
  g_free(s->name);
  if (s->core)
    g_unlink(s->core);
  g_free(s->core);
  g_free(s);
  if (t->closing) { /* the tabs are gone */
    g_clear_pointer(&t->debug.stop_path, g_free);
  } else {
    breakpoints_show_stop(t, NULL, 0);
    debug_view_ended(t);
    disassembly_ended(t);
    memory_ended(t);
  }
  update_ui(t, "Not debugging");
  if (again && !t->closing) {
    if (how)
      how(t);
    else
      debug_start_or_continue(t);
  }
}

/* gdb did not end the program in time. */
static gboolean on_end_timeout(gpointer data)
{
  Debugger *s = data;

  s->end_id = 0;
  finish(s);
  return G_SOURCE_REMOVE;
}

static void on_disconnected(gpointer data, JsonNode *body G_GNUC_UNUSED, const char *error G_GNUC_UNUSED)
{
  finish(data);
}

/* Asks gdb to end the program; it is killed with gdb when that takes too long. */
static void end_session(Debugger *s)
{
  if (s->state == ENDING) { /* asked twice: no waiting */
    finish(s);
    return;
  }
  gboolean attached = s->attached;
  s->state = ENDING;
  breakpoints_show_stop(s->app, NULL, 0);
  say(s->app, FALSE, attached ? "Detached: the process goes on." : "Stopped.");
  update_ui(s->app, "Stopping…");
  /* a process attached to goes on without the debugger */
  dap_request(s->dap, "disconnect", attached ? "{\"terminateDebuggee\":false}" : "{\"terminateDebuggee\":true}",
              on_disconnected, s);
  s->end_id = g_timeout_add(END_MS, on_end_timeout, s);
}

void debug_end(App *t)
{
  if (t->debug.session) {
    t->debug.session->restart = FALSE;
    finish(t->debug.session);
  }
  g_clear_pointer(&ui.cwd, g_free);
}

/* Breakpoints -------------------------------------------------------------------------------- */

/* A breakpoint as it was sent to gdb. */
typedef struct {
  char *path;
  int line;
} Sent;

static void sent_free(gpointer data)
{
  Sent *sent = data;

  g_free(sent->path);
  g_free(sent);
}

/* gdb says where breakpoint id is: when that is another line (the asked one has no code, like a blank
 * line or a function's head), the dot moves there. */
static void placed(App *t, gint64 id, JsonNode *breakpoint)
{
  Debugger *s = t->debug.session;
  Sent *sent = s ? g_hash_table_lookup(s->sent, &id) : NULL;
  JsonNode *verified = json_at(breakpoint, "verified");
  int line = (int)json_int_at(breakpoint, "line", 0);

  if (!sent || !verified || !json_node_get_boolean(verified) || line <= 0 || line == sent->line)
    return;
  char *path = g_strdup(sent->path);
  int from = sent->line;
  sent->line = line;
  breakpoint_move(t, path, from, line); /* sends the file's breakpoints again */
  g_free(path);
}

/* The lines asked for in one setBreakpoints: its answer lists the breakpoints in the same order. */
typedef struct {
  App *app;
  char *path;
  GArray *lines; /* int */
} Asked;

static void asked_free(gpointer data)
{
  Asked *a = data;

  g_free(a->path);
  g_array_free(a->lines, TRUE);
  g_free(a);
}

static void on_breakpoints_set(gpointer data, JsonNode *body, const char *error G_GNUC_UNUSED)
{
  Asked *a = data;
  Debugger *s = a->app->debug.session;
  JsonNode *list = json_at(body, "breakpoints");
  guint n = list && JSON_NODE_HOLDS_ARRAY(list) ? json_array_get_length(json_node_get_array(list)) : 0;

  for (guint i = 0; s && i < n && i < a->lines->len; i++) {
    JsonNode *breakpoint = json_array_get_element(json_node_get_array(list), i);
    gint64 id = json_int_at(breakpoint, "id", -1);
    const char *reason = json_string_at(breakpoint, "reason"), *message = json_string_at(breakpoint, "message");
    if (id >= 0) {
      Sent *sent = g_new(Sent, 1);
      gint64 *key = g_new(gint64, 1);
      *sent = (Sent){ g_strdup(a->path), g_array_index(a->lines, int, i) };
      *key = id;
      g_hash_table_replace(s->sent, key, sent);
    }
    if (g_strcmp0(reason, "failed") == 0 && message) { /* a condition gdb cannot read */
      char *name = g_path_get_basename(a->path);
      say(a->app, TRUE, "The breakpoint at %s:%d does not work: %s", name, g_array_index(a->lines, int, i), message);
      g_free(name);
    }
  }
  for (guint i = 0; s && i < n && i < a->lines->len; i++) { /* last: it may send again */
    JsonNode *breakpoint = json_array_get_element(json_node_get_array(list), i);
    placed(a->app, json_int_at(breakpoint, "id", -1), breakpoint);
  }
}

/* Tells gdb the breakpoints in path (none: it takes its own away). */
static void send_breakpoints(Debugger *s, const char *path)
{
  GArray *lines = breakpoints_in(s->app, path);
  GString *args = g_string_new("{\"source\":{\"path\":");
  char *quoted = json_quote(path);

  g_string_append_printf(args, "%s},\"breakpoints\":[", quoted);
  for (guint i = 0; lines && i < lines->len; i++) {
    Breakpoint *b = &g_array_index(lines, Breakpoint, i);
    char *condition = b->condition ? json_quote(b->condition) : NULL, *log = b->log ? json_quote(b->log) : NULL;
    g_string_append_printf(args, "%s{\"line\":%d", i ? "," : "", b->line);
    if (condition)
      g_string_append_printf(args, ",\"condition\":%s", condition);
    if (log)
      g_string_append_printf(args, ",\"logMessage\":%s", log);
    g_string_append(args, "}");
    g_free(condition);
    g_free(log);
  }
  g_string_append(args, "]}");
  Asked *a = g_new0(Asked, 1);
  a->app = s->app;
  a->path = g_strdup(path);
  a->lines = g_array_new(FALSE, FALSE, sizeof(int));
  for (guint i = 0; lines && i < lines->len; i++)
    g_array_append_val(a->lines, g_array_index(lines, Breakpoint, i).line);
  dap_request_full(s->dap, "setBreakpoints", args->str, on_breakpoints_set, a, asked_free);
  g_string_free(args, TRUE);
  g_free(quoted);
}

void debug_breakpoints_changed(App *t, const char *path)
{
  Debugger *s = t->debug.session;

  if (s && s->configured && s->state != ENDING)
    send_breakpoints(s, path);
  debug_view_breakpoints_changed(t);
}

/* Stopping ----------------------------------------------------------------------------------- */

/* Why the program stopped, for the status: "Paused on breakpoint", ... */
static const char *pause_reason(const char *reason)
{
  if (g_strcmp0(reason, "breakpoint") == 0)
    return "Paused on breakpoint";
  if (g_strcmp0(reason, "step") == 0)
    return "Paused after a step";
  if (g_strcmp0(reason, "pause") == 0)
    return "Paused";
  if (g_strcmp0(reason, "signal") == 0 || g_strcmp0(reason, "exception") == 0)
    return "Paused on a crash";
  return "Paused";
}

/* What a crash was, in plain words: an ASan report, or the signal (signo, 0: unknown). */
static const char *crash_meaning(gboolean sanitizer, int signo)
{
  if (sanitizer)
    return "a memory error: reading or writing outside an array, or memory already freed (the report above says "
           "which)";
  switch (signo) {
    case 11: return "a bad pointer (segmentation fault): NULL, freed, or pointing outside the program's memory";
    case 8: return "an arithmetic error, like a division by zero";
    case 6: return "abort(): a failed assert(), or the C library found its memory broken";
    case 7: return "a bad memory access (bus error)";
    case 4:
    case 5: return "a trap: the compiler's checks caught undefined behavior";
    default: return "a signal from the system";
  }
}

/* What the status calls a crash: an exception in Python (and a plugin's language: Java), a panic in Go. */
static const char *crash_word(Debugger *s)
{
  return s->kind == ADAPTER_DEBUGPY || s->kind == ADAPTER_PLUGIN ? "Exception"
         : s->kind == ADAPTER_DELVE                              ? "Panic"
                                                                 : "Crashed";
}

/* Whether the debugger offers the exception filter id ("uncaught": java-debug's, debugpy's). */
static gboolean has_filter(Debugger *s, const char *id)
{
  JsonNode *filters = json_at(dap_capabilities(s->dap), "exceptionBreakpointFilters");

  for (guint i = 0;
       filters && JSON_NODE_HOLDS_ARRAY(filters) && i < json_array_get_length(json_node_get_array(filters)); i++)
    if (g_strcmp0(json_string_at(json_array_get_element(json_node_get_array(filters), i), "filter"), id) == 0)
      return TRUE;
  return FALSE;
}

typedef struct {
  App *app;
  char *where; /* "average.c:8 (in average)" */
} Crash;

static void crash_free(gpointer data)
{
  Crash *c = data;

  g_free(c->where);
  g_free(c);
}

static void say_crash(App *t, const char *where, gboolean sanitizer, int signo)
{
  say_later(t, TRUE, "■ The program crashed at %s: %s.", where, crash_meaning(sanitizer, signo));
}

/* An exception's name and message came (exceptionInfo: debugpy, Delve). */
static void on_exception(gpointer data, JsonNode *body, const char *error G_GNUC_UNUSED)
{
  Crash *c = data;
  const char *id = json_string_at(body, "exceptionId"), *description = json_string_at(body, "description");

  say_later(c->app, TRUE, "■ %s%s%s at %s.", id ? id : "An exception", description ? ": " : "",
            description ? description : "", c->where);
}

/* The signal's number came ($_siginfo.si_signo). */
static void on_signal(gpointer data, JsonNode *body, const char *error G_GNUC_UNUSED)
{
  Crash *c = data;
  const char *number = json_string_at(body, "result");

  say_crash(c->app, c->where, FALSE, number ? atoi(number) : 0);
}

/* The stack of the thread that stopped: its innermost frame with a file on disk (not the C
 * library's, or ASan's) is shown, in the editor and in the Run and Debug panel. */
static void on_stack(gpointer data, JsonNode *body, const char *error)
{
  Debugger *s = data;
  App *t = s->app;
  JsonNode *frames = json_at(body, "stackFrames");
  guint n = frames && JSON_NODE_HOLDS_ARRAY(frames) ? json_array_get_length(json_node_get_array(frames)) : 0;
  const char *path = NULL, *top = NULL, *function = NULL;
  gboolean crash = g_strcmp0(s->reason, "signal") == 0 || g_strcmp0(s->reason, "exception") == 0 || s->examining;
  gboolean sanitizer = FALSE;
  int line = 0, shown = 0;
  char *where;

  if (s->state != STOPPED) /* went on meanwhile */
    return;
  for (guint i = 0; i < n; i++) {
    JsonNode *frame = json_array_get_element(json_node_get_array(frames), i);
    const char *file = json_string_at(frame, "source.path"), *name = json_string_at(frame, "name");
    if (i == 0)
      top = name;
    if (name && !path && (strstr(name, "__asan") || strstr(name, "__sanitizer") || strstr(name, "__ubsan")))
      sanitizer = TRUE;
    if (!path && file && g_file_test(file, G_FILE_TEST_IS_REGULAR)) {
      path = file;
      line = (int)json_int_at(frame, "line", 0);
      function = name;
      shown = (int)i;
    }
  }
  if (path && line > 0) {
    char *name = g_path_get_basename(path);
    char *status = g_strdup_printf("%s: %s:%d", crash ? crash_word(s) : pause_reason(s->reason), name, line);
    where = g_strdup_printf("%s:%d (in %s)", name, line, function ? function : "?");
    breakpoints_show_stop(t, path, line);
    if (!disassembly_shown(t)) /* stepping by instruction there: it stays in front */
      open_file_at(t, path, line, 1);
    update_ui(t, status);
    g_free(status);
    g_free(name);
  } else {
    where = g_strdup_printf("%s (no source)", top ? top : "?");
    char *status = g_strdup_printf("%s in %s", crash ? crash_word(s) : pause_reason(s->reason), where);
    if (error)
      say_text(t, TRUE, error);
    update_ui(t, status);
    g_free(status);
  }
  debug_view_paused(t, frames, shown);
  disassembly_update(t);
  memory_update(t);
  if (crash) {
    s->crashed = TRUE;
    if (s->kind == ADAPTER_DEBUGPY || s->kind == ADAPTER_DELVE ||
        (s->kind == ADAPTER_PLUGIN && can(s, "supportsExceptionInfoRequest"))) { /* its own words */
      Crash *c = g_new0(Crash, 1);
      char *args = g_strdup_printf("{\"threadId\":%" G_GINT64_FORMAT "}", s->thread);
      c->app = t;
      c->where = g_strdup(where);
      dap_request_full(s->dap, "exceptionInfo", args, on_exception, c, crash_free);
      g_free(args);
    } else if (sanitizer || s->signo > 0 || s->kind != ADAPTER_GDB) {
      say_crash(t, where, sanitizer, s->signo);
    } else {
      Crash *c = g_new0(Crash, 1);
      char *args = g_strdup_printf(
          "{\"expression\":\"$_siginfo.si_signo\",\"context\":\"watch\",\"frameId\":%" G_GINT64_FORMAT "}",
          json_int_at(frames, "0.id", 0));
      c->app = t;
      c->where = g_strdup(where);
      dap_request_full(s->dap, "evaluate", args, on_signal, c, crash_free);
      g_free(args);
    }
  }
  g_free(where);
}

static void stopped(Debugger *s, JsonNode *body)
{
  const char *reason = json_string_at(body, "reason");
  char *args;

  s->state = STOPPED;
  s->thread = json_int_at(body, "threadId", s->thread);
  update_ui(s->app, pause_reason(reason));
  g_set_str(&s->reason, reason);
  args = g_strdup_printf("{\"threadId\":%" G_GINT64_FORMAT ",\"levels\":%d}", s->thread, STACK_LEVELS);
  dap_request(s->dap, "stackTrace", args, on_stack, s);
  g_free(args);
}

/* Events ------------------------------------------------------------------------------------- */

static void on_event(gpointer owner, const char *event, JsonNode *body)
{
  Debugger *s = owner;
  App *t = s->app;

  if (!event) { /* gdb went */
    if (s->state != ENDING)
      say(t, TRUE, "%s ended unexpectedly", session_name(s));
    finish(s);
  } else if (strcmp(event, "initialized") == 0) {
    GHashTableIter iter;
    gpointer path;
    if (t->debug.breakpoints) {
      g_hash_table_iter_init(&iter, t->debug.breakpoints);
      while (g_hash_table_iter_next(&iter, &path, NULL))
        send_breakpoints(s, path);
    }
    /* an exception nothing catches stops it, as a crash does */
    if (s->kind == ADAPTER_DEBUGPY || (s->kind == ADAPTER_PLUGIN && has_filter(s, "uncaught")))
      dap_request(s->dap, "setExceptionBreakpoints", "{\"filters\":[\"uncaught\"]}", NULL, NULL);
    s->configured = TRUE;
    dap_request(s->dap, "configurationDone", NULL, NULL, NULL);
  } else if (strcmp(event, "stopped") == 0) {
    stopped(s, body);
  } else if (strcmp(event, "breakpoint") == 0) { /* verified once the program loaded */
    placed(t, json_int_at(body, "breakpoint.id", -1), json_at(body, "breakpoint"));
  } else if (strcmp(event, "continued") == 0 && s->state != ENDING) {
    s->state = RUNNING;
    breakpoints_show_stop(t, NULL, 0);
    debug_view_running(t);
    update_ui(t, "Running");
  } else if (strcmp(event, "process") == 0 && s->state == STARTING) {
    s->state = RUNNING;
    debug_view_running(t);
    update_ui(t, "Running");
  } else if (strcmp(event, "exited") == 0 && s->state != ENDING) { /* Stop says so itself */
    if (s->crashed)                                                /* gdb says 0 */
      say_later(t, FALSE, "The program ended: the %s killed it.",
                s->kind == ADAPTER_DEBUGPY || s->kind == ADAPTER_PLUGIN ? "exception"
                : s->kind == ADAPTER_DELVE                              ? "panic"
                                                                        : "crash");
    else
      say_later(t, FALSE, "The program exited with code %d.", (int)json_int_at(body, "exitCode", 0));
  } else if (strcmp(event, "terminated") == 0) {
    finish(s);
  } else if (strcmp(event, "output") == 0) {
    const char *category = json_string_at(body, "category"), *output = json_string_at(body, "output");
    gboolean program = g_strcmp0(category, "stdout") == 0 || g_strcmp0(category, "stderr") == 0;
    if (output && program && !adapter_owns_terminal(s->kind)) { /* the program's own output, sent over */
      char **lines = g_strsplit(output, "\n", -1), *text = g_strjoinv("\r\n", lines);
      if (t->debug.term)
        vte_terminal_feed(t->debug.term, text, -1);
      g_free(text);
      g_strfreev(lines);
    } else if (output && g_strcmp0(category, "console") == 0 && s->state != ENDING) { /* a logpoint's line, ... */
      char **lines = g_strsplit(output, "\n", -1);
      for (char **line = lines; *line; line++) {
        g_strchomp(*line);
        if (**line && !adapter_noise(s->kind, *line))
          say(t, FALSE, "%s%s", s->kind == ADAPTER_GDB ? "◆ " : "", *line);
      }
      g_strfreev(lines);
    } else if (output && (g_strcmp0(category, "important") == 0 || g_strcmp0(category, "stderr") == 0)) {
      char **lines = g_strsplit(output, "\n", -1), *kept;
      GString *text = g_string_new("");
      for (char **line = lines; *line; line++)
        if (!adapter_noise(s->kind, g_strchomp(*line)))
          g_string_append_printf(text, "%s%s", text->len ? "\n" : "", *line);
      if ((kept = g_strchomp(text->str)) && *kept)
        say_text(t, FALSE, kept);
      g_string_free(text, TRUE);
      g_strfreev(lines);
    }
  }
}

/* Starting ----------------------------------------------------------------------------------- */

/* ASan aborts at its first report (gdb stops there: the bad line shows), and its leak check is off:
 * it cannot run under a debugger. What the user set in ASAN_OPTIONS stays. */
static char **sanitizer_env(char **env)
{
  const char *now = g_environ_getenv(env, "ASAN_OPTIONS");
  GString *value = g_string_new(now ? now : "");

  if (!strstr(value->str, "detect_leaks"))
    g_string_append_printf(value, "%sdetect_leaks=0", value->len ? ":" : "");
  if (!strstr(value->str, "abort_on_error"))
    g_string_append_printf(value, "%sabort_on_error=1", value->len ? ":" : "");
  env = g_environ_setenv(env, "ASAN_OPTIONS", value->str, TRUE);
  g_string_free(value, TRUE);
  return env;
}

static void on_launched(gpointer data, JsonNode *body G_GNUC_UNUSED, const char *error)
{
  Debugger *s = data;

  if (!error)
    return;
  if (s->attached && g_file_test("/proc/sys/kernel/yama/ptrace_scope", G_FILE_TEST_EXISTS)) {
    char *scope = NULL;
    g_file_get_contents("/proc/sys/kernel/yama/ptrace_scope", &scope, NULL, NULL);
    say(s->app, TRUE, "Could not attach: %s", error);
    if (scope && atoi(scope) > 0) /* only a program's own children may be debugged */
      say(s->app, TRUE,
          "The system lets a debugger attach only to the programs it started itself (ptrace_scope %d). For this "
          "session: sudo sysctl kernel.yama.ptrace_scope=0",
          atoi(scope));
    g_free(scope);
  } else {
    say(s->app, TRUE, "Could not start the program: %s", error);
  }
  end_session(s);
}

/* Child setup of a program the debugger has started in the terminal: a session of its own, whose
 * controlling terminal the terminal is (Ctrl+C, the shell's job control in it). */
static void in_terminal(gpointer data G_GNUC_UNUSED)
{
  setsid();
  ioctl(0, TIOCSCTTY, 0);
}

/* The debugger asks t-code to start the program in its terminal (runInTerminal: lldb-dap, debugpy,
 * Delve): it runs there in cwd with env added, and its pid is the answer. */
static char *on_adapter_request(gpointer owner, const char *command, JsonNode *arguments, char **error)
{
  Debugger *s = owner;
  JsonNode *args = json_at(arguments, "args"), *env = json_at(arguments, "env");
  const char *cwd = json_string_at(arguments, "cwd");
  GSubprocessLauncher *launcher;
  GPtrArray *argv;
  GError *failed = NULL;
  char **environ;

  if (strcmp(command, "runInTerminal") != 0 || !args || !JSON_NODE_HOLDS_ARRAY(args) || !ui.tty) {
    *error = g_strdup("not supported");
    return NULL;
  }
  if (s->child) {
    *error = g_strdup("a program runs already");
    return NULL;
  }
  argv = g_ptr_array_new();
  for (guint i = 0; i < json_array_get_length(json_node_get_array(args)); i++)
    g_ptr_array_add(argv, (gpointer)json_array_get_string_element(json_node_get_array(args), i));
  g_ptr_array_add(argv, NULL);
  environ = sanitizer_env(g_get_environ());
  if (env && JSON_NODE_HOLDS_OBJECT(env)) {
    JsonObjectIter iter;
    const char *name;
    JsonNode *value;
    json_object_iter_init(&iter, json_node_get_object(env));
    while (json_object_iter_next(&iter, &name, &value))
      environ = JSON_NODE_HOLDS_VALUE(value) && json_node_get_value_type(value) == G_TYPE_STRING
                    ? g_environ_setenv(environ, name, json_node_get_string(value), TRUE)
                    : g_environ_unsetenv(environ, name);
  }
  launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_NONE);
  g_subprocess_launcher_set_environ(launcher, environ);
  if (cwd && *cwd)
    g_subprocess_launcher_set_cwd(launcher, cwd);
  g_subprocess_launcher_set_stdin_file_path(launcher, ui.tty);
  g_subprocess_launcher_set_stdout_file_path(launcher, ui.tty);
  g_subprocess_launcher_set_stderr_file_path(launcher, ui.tty);
  g_subprocess_launcher_set_child_setup(launcher, in_terminal, NULL, NULL);
  s->child = argv->len > 1 ? g_subprocess_launcher_spawnv(launcher, (const char *const *)argv->pdata, &failed) : NULL;
  g_object_unref(launcher);
  g_strfreev(environ);
  g_ptr_array_free(argv, TRUE);
  if (!s->child) {
    *error = g_strdup(failed ? failed->message : "no program");
    g_clear_error(&failed);
    return NULL;
  }
  return g_strdup_printf("{\"processId\":%s}", g_subprocess_get_identifier(s->child));
}

/* Where a session's debugger is: its command line (argv), or a debugger a plugin started that listens
 * (host:port). */
typedef struct {
  AdapterKind kind;
  const char *name; /* a plugin's: its name for the debugger; NULL: adapter_name's */
  const char *id;   /* the adapterID */
  char **argv;
  const char *host;
  int port;
} Where;

/* Starts the debugger where says in cwd and sends it request ("launch", "attach") with arguments; shown
 * goes in the terminal ("▶ demo"); again: how Restart starts it again. NULL when it cannot start (the
 * Debug panel says why). */
static Debugger *begin_session(App *t, const Where *where, const char *request, const char *arguments, const char *cwd,
                               const char *shown, DebugAgain again)
{
  AdapterKind kind = where->kind;
  DapHandle handle = adapter_owns_terminal(kind) ? NULL : on_adapter_request;
  char **env = sanitizer_env(g_get_environ());
  GError *error = NULL;
  Debugger *s = g_new0(Debugger, 1);

  s->app = t;
  s->kind = kind;
  s->name = g_strdup(where->name);
  s->again = again;
  s->state = STARTING;
  s->thread = 1;
  s->sent = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, sent_free);
  s->dap = where->host ? dap_connect(where->host, where->port, where->id, on_event, handle, s)
                       : dap_start((const char *const *)where->argv, cwd, env, kind == ADAPTER_DELVE ? ui.tty : NULL,
                                   where->id, on_event, handle, s, &error);
  if (t->debug.term) /* this run's output alone */
    vte_terminal_reset(t->debug.term, TRUE, TRUE);
  show_sidebar_page(t, "run");
  if (!s->dap) {
    say(t, TRUE, "Could not start %s: %s", session_name(s), error->message);
    g_hash_table_destroy(s->sent);
    g_free(s->name);
    g_clear_pointer(&s, g_free);
    update_ui(t, "Not debugging");
  } else {
    t->debug.session = s;
    if (ui.evaluate)
      gtk_entry_set_placeholder_text(GTK_ENTRY(ui.evaluate), kind == ADAPTER_GDB ? EVALUATE_GDB : EVALUATE);
    s->attached = strcmp(request, "attach") == 0 && strstr(arguments, "\"coreFile\"") == NULL;
    s->examining = strstr(arguments, "\"coreFile") != NULL;
    if (kind == ADAPTER_GDB)
      say_text(t, FALSE, shown);
    else
      say(t, FALSE, "%s   (%s)", shown, session_name(s));
    dap_request(s->dap, request, arguments, on_launched, s); /* answered once configurationDone went */
    update_ui(t, "Starting…");
  }
  g_clear_error(&error);
  g_strfreev(env);
  return s;
}

/* Starts kind's debugger (python: debugpy's interpreter) in cwd and sends it request ("launch",
 * "attach") with arguments; shown goes in the terminal ("▶ demo"); again: how Restart starts it again.
 * The Debug panel says why when the debugger is not installed. */
static Debugger *start_session(App *t, AdapterKind kind, const char *python, const char *request, const char *arguments,
                               const char *cwd, const char *shown, DebugAgain again)
{
  char *missing = NULL, **argv = adapter_argv(kind, ui.tty, python, &missing);
  Debugger *s;

  show_sidebar_page(t, "debug");
  if (!argv) {
    say_text(t, TRUE, missing);
    g_free(missing);
    update_ui(t, "Not debugging");
    return NULL;
  }
  s = begin_session(t, &(Where){ .kind = kind, .id = adapter_id(kind), .argv = argv }, request, arguments, cwd, shown,
                    again);
  g_strfreev(argv);
  return s;
}

gboolean debug_start_adapter(App *t, const DebugAdapter *adapter, char **error)
{
  Where where = { .kind = ADAPTER_PLUGIN,
                  .name = adapter->name && *adapter->name ? adapter->name : NULL,
                  .id = adapter->id && *adapter->id ? adapter->id : adapter_id(ADAPTER_PLUGIN),
                  .argv = adapter->argv };
  char *host = NULL, *shown;
  gint64 port = 0;

  if (t->debug.session) {
    *error = g_strdup("a debugging session runs already");
    return FALSE;
  }
  if (adapter->connect) { /* "host:port" */
    const char *colon = strrchr(adapter->connect, ':');
    char *end = NULL;
    port = colon ? g_ascii_strtoll(colon + 1, &end, 10) : 0;
    if (!colon || colon == adapter->connect || !end || *end || port <= 0 || port > 65535) {
      *error = g_strdup("\"connect\" is \"host:port\"");
      return FALSE;
    }
    host = g_strndup(adapter->connect, colon - adapter->connect);
    where.host = host;
    where.port = (int)port;
  } else if (!adapter->argv || !adapter->argv[0]) {
    *error = g_strdup("needs \"connect\" (\"host:port\") or \"argv\"");
    return FALSE;
  }
  if (!adapter->request || (strcmp(adapter->request, "launch") != 0 && strcmp(adapter->request, "attach") != 0)) {
    g_free(host);
    *error = g_strdup("\"request\" is \"launch\" or \"attach\"");
    return FALSE;
  }
  show_sidebar_page(t, "debug");
  shown = g_strdup_printf("▶ %s", adapter->shown && *adapter->shown ? adapter->shown : where.name ? where.name : "");
  Debugger *s = begin_session(t, &where, adapter->request, adapter->arguments ? adapter->arguments : "{}",
                              adapter->cwd ? adapter->cwd : t->project.root, shown, NULL);
  g_free(shown);
  g_free(host);
  if (!s)
    *error = g_strdup("the debugger did not start (the Debug panel says why)");
  return s != NULL;
}

/* The C / C++ / Rust debugger the settings ask for. */
static AdapterKind native(App *t)
{
  return adapter_native(t->settings.lldb ? "lldb" : "gdb");
}

/* "▶ path", the path from the project's root when it is inside (free it). */
static char *shown_program(App *t, const char *program)
{
  gboolean inside = g_str_has_prefix(program, t->project.root) && program[strlen(t->project.root)] == '/';

  return g_strdup_printf("▶ %s", inside ? program + strlen(t->project.root) + 1 : program);
}

/* Starts the C / C++ / Rust debugger on program with args (NULL: none), to run in cwd; again: how
 * Restart starts it again. */
static void start(App *t, const char *program, const char *const *args, const char *cwd, DebugAgain again)
{
  AdapterKind kind = native(t);
  AdapterTarget target = { .program = program, .args = args, .cwd = cwd };
  char *launch = adapter_launch(kind, &target), *shown = shown_program(t, program);

  start_session(t, kind, NULL, "launch", launch, cwd, shown, again);
  g_free(shown);
  g_free(launch);
}

/* Which program (Makefile projects) ---------------------------------------------------------- */

/* Whether path is a program: an executable ELF file. */
static gboolean is_program(const char *path)
{
  char magic[4] = { 0 };
  FILE *file;
  gboolean elf;

  if (!g_file_test(path, G_FILE_TEST_IS_REGULAR) || !g_file_test(path, G_FILE_TEST_IS_EXECUTABLE))
    return FALSE;
  if (!(file = fopen(path, "rb")))
    return FALSE;
  elf = fread(magic, 1, 4, file) == 4 && memcmp(magic,
                                                "\x7f"
                                                "ELF",
                                                4) == 0;
  fclose(file);
  return elf && !strstr(path, ".so");
}

/* The programs under dir, depth folders deep, into list. */
static void find_programs(const char *dir, int depth, GPtrArray *list)
{
  GDir *folder = g_dir_open(dir, 0, NULL);
  const char *name;

  while (folder && list->len < MAX_PROGRAMS && (name = g_dir_read_name(folder))) {
    char *path = g_build_filename(dir, name, NULL);
    if (name[0] == '.' || strcmp(name, "CMakeFiles") == 0 || strcmp(name, "node_modules") == 0)
      ;
    else if (g_file_test(path, G_FILE_TEST_IS_DIR) && !g_file_test(path, G_FILE_TEST_IS_SYMLINK)) {
      if (depth > 0)
        find_programs(path, depth - 1, list);
    } else if (is_program(path)) {
      g_ptr_array_add(list, path);
      path = NULL;
    }
    g_free(path);
  }
  if (folder)
    g_dir_close(folder);
}

static gint64 modified(const char *path)
{
  GStatBuf st;

  return g_stat(path, &st) == 0 ? (gint64)st.st_mtime : 0;
}

/* Newest first. */
static int by_age(gconstpointer a, gconstpointer b)
{
  gint64 x = modified(*(char *const *)a), y = modified(*(char *const *)b);

  return x < y ? 1 : x > y ? -1 : 0;
}

static void program_query(App *t, const char *text, guint generation)
{
  int shown = 0;

  for (guint i = 0; ui.programs && i < ui.programs->len; i++) {
    const char *path = g_ptr_array_index(ui.programs, i);
    const char *relative = g_str_has_prefix(path, ui.pick_in) ? path + strlen(ui.pick_in) + 1 : path;
    if (*text && fuzzy_score(text, relative) < 0)
      continue;
    picker_add(t, generation, relative, NULL, path);
    shown++;
  }
  if (!shown) {
    char *message = ui.programs && ui.programs->len
                        ? g_strdup("No matching programs")
                        : g_strdup_printf("No programs in %s: build it first (Ctrl+Shift+B)", ui.pick_in);
    picker_message(t, generation, message);
    g_free(message);
  }
}

static void open_core(App *t, const char *core, const char *program, gboolean taken, int signo);

static void program_choose(App *t, const char *path)
{
  if (ui.core) { /* the program of a core file */
    char *core = g_steal_pointer(&ui.core);
    open_core(t, core, path, FALSE, 0);
    g_free(core);
    return;
  }
  session_set_project_value(t, "debug-program", path);
  if (ui.cwd && !t->debug.session) { /* a build waits for it */
    char *cwd = g_steal_pointer(&ui.cwd);
    start(t, path, NULL, cwd, NULL);
    g_free(cwd);
  }
}

/* Lists the programs in dir in the picker. */
static void pick_program(App *t, const char *dir)
{
  const char *title = ui.core ? "Which program crashed into this core dump?" : "Which program does F5 debug?";

  g_set_str(&ui.pick_in, dir);
  if (ui.programs)
    g_ptr_array_free(ui.programs, TRUE);
  ui.programs = g_ptr_array_new_with_free_func(g_free);
  find_programs(dir, PROGRAM_DEPTH, ui.programs);
  g_ptr_array_sort(ui.programs, by_age);
  picker_open(t, title, program_query, program_choose);
}

void debug_select_program(App *t)
{
  g_clear_pointer(&ui.core, g_free);
  pick_program(t, t->project.root);
}

/* The program remembered for this project, if it is still there (free it). */
static char *remembered_program(App *t)
{
  char *path = session_project_value(t, "debug-program");

  if (path && !is_program(path))
    g_clear_pointer(&path, g_free);
  return path;
}

void debug_launch(App *t, const char *path)
{
  char *cwd, *program;

  if (t->debug.session) /* one at a time */
    return;
  if (!ui.cwd) /* not built by F5: run where it lies */
    ui.cwd = g_file_test(path, G_FILE_TEST_IS_DIR) ? g_strdup(path) : g_path_get_dirname(path);
  if (!g_file_test(path, G_FILE_TEST_IS_DIR)) {
    program = g_strdup(path);
  } else if (!(program = remembered_program(t))) {
    pick_program(t, path); /* starts once picked */
    return;
  }
  cwd = g_steal_pointer(&ui.cwd);
  start(t, program, NULL, cwd, NULL);
  g_free(cwd);
  g_free(program);
}

/* Says so when a session goes on (one at a time). */
static gboolean busy(App *t)
{
  if (!t->debug.session)
    return FALSE;
  show_sidebar_page(t, "debug");
  say(t, TRUE, "A debugging session goes on: stop it first (Shift+F5).");
  return TRUE;
}

void debug_launch_test(App *t, const char *program, const char *const *args, const char *cwd, DebugAgain again)
{
  if (busy(t))
    return;
  g_clear_pointer(&ui.cwd, g_free); /* an F5 build no longer waits */
  start(t, program, args, cwd, again);
}

/* Attaching to a process ----------------------------------------------------------------------- */

static void attach_query(App *t, const char *text, guint generation)
{
  int shown = 0;

  for (guint i = 0; ui.listed && i < ui.listed->len; i++) {
    Attachable *a = g_ptr_array_index(ui.listed, i);
    char *label = g_strdup_printf("%s  (%d)", a->name, a->pid), *payload = g_strdup_printf("%d", a->pid);
    if (!*text || fuzzy_score(text, a->command) >= 0 || fuzzy_score(text, label) >= 0) {
      picker_add(t, generation, label, a->command, payload);
      shown++;
    }
    g_free(payload);
    g_free(label);
  }
  if (!shown)
    picker_message(t, generation, "No matching processes");
}

static void attach_choose(App *t, const char *payload)
{
  int pid = atoi(payload);
  Attachable *chosen = NULL;
  const char *request;

  for (guint i = 0; ui.listed && i < ui.listed->len && !chosen; i++)
    if (((Attachable *)g_ptr_array_index(ui.listed, i))->pid == pid)
      chosen = g_ptr_array_index(ui.listed, i);
  if (!chosen || busy(t))
    return;
  AdapterKind kind = adapter_for_process(pid, t->settings.lldb ? "lldb" : "gdb");
  char *link = g_strdup_printf("/proc/%d/cwd", pid), *cwd = g_file_read_link(link, NULL);
  char *args = adapter_attach(kind, pid, kind == ADAPTER_DEBUGPY ? NULL : chosen->exe, &request);
  char *shown = g_strdup_printf("▶ attached to %s (%d)", chosen->name, pid);

  start_session(t, kind, NULL, request, args, cwd ? cwd : t->project.root, shown, NULL);
  g_free(shown);
  g_free(args);
  g_free(cwd);
  g_free(link);
}

void debug_attach(App *t)
{
  if (busy(t))
    return;
  if (ui.listed)
    g_ptr_array_unref(ui.listed);
  ui.listed = adapter_attachable(getpid());
  picker_open(t, "Attach the debugger to which running process?", attach_query, attach_choose);
}

/* Core dumps ------------------------------------------------------------------------------------ */

#define CORE_FILE_PAYLOAD "file:"

/* Opens core, the memory program had when it crashed; taken: a copy of t-code's, deleted at the end;
 * signo: the signal that killed it (0: unknown). */
static void open_core(App *t, const char *core, const char *program, gboolean taken, int signo)
{
  AdapterKind kind = native(t);
  const char *request;
  char *args = adapter_core(kind, program, core, &request), *cwd = g_path_get_dirname(program);
  char *name = g_path_get_basename(program), *shown = g_strdup_printf("▶ the core dump of %s", name);
  Debugger *s;

  if (busy(t) || !args) {
    if (taken)
      g_unlink(core);
  } else if ((s = start_session(t, kind, NULL, request, args, cwd, shown, NULL))) {
    say(t, FALSE, "Nothing runs: the stack and the variables are as they were when it crashed. Shift+F5 closes it.");
    if (taken)
      s->core = g_strdup(core);
    s->signo = signo;
  } else if (taken) {
    g_unlink(core);
  }
  g_free(shown);
  g_free(name);
  g_free(cwd);
  g_free(args);
}

/* A signal's name for the core dump list. */
static const char *signal_name(int signal)
{
  switch (signal) {
    case 4: return "illegal instruction";
    case 5: return "trap";
    case 6: return "abort";
    case 7: return "bus error";
    case 8: return "arithmetic error";
    case 11: return "segmentation fault";
    default: return "signal";
  }
}

static void core_query(App *t, const char *text, guint generation)
{
  picker_add(t, generation, "Open a core file…", "a file named core, core.<pid>, ...", CORE_FILE_PAYLOAD);
  for (guint i = 0; ui.listed && i < ui.listed->len; i++) {
    CoreDump *c = g_ptr_array_index(ui.listed, i);
    GDateTime *when = g_date_time_new_from_unix_local_usec(c->time);
    char *at = when ? g_date_time_format(when, "%Y-%m-%d %H:%M:%S") : g_strdup("?");
    char *name = g_path_get_basename(c->exe), *label = g_strdup_printf("%s: %s", name, signal_name(c->signal));
    char *detail = g_strdup_printf("%s, pid %d, %s", at, c->pid, c->exe), *payload = g_strdup_printf("%u", i);
    if (!*text || fuzzy_score(text, c->exe) >= 0)
      picker_add(t, generation, label, detail, payload);
    g_free(payload);
    g_free(detail);
    g_free(label);
    g_free(name);
    g_free(at);
    if (when)
      g_date_time_unref(when);
  }
}

typedef struct {
  App *app;
  char *core, *exe;
  int signo;
} Taken;

static void taken_free(Taken *k)
{
  g_free(k->core);
  g_free(k->exe);
  g_free(k);
}

/* coredumpctl has written the core dump out. */
static void on_core_taken(GObject *source, GAsyncResult *result, gpointer data)
{
  Taken *k = data;
  gboolean ok = g_subprocess_wait_check_finish(G_SUBPROCESS(source), result, NULL);

  if (ok && !k->app->closing)
    open_core(k->app, k->core, k->exe, TRUE, k->signo);
  else if (!k->app->closing) {
    show_sidebar_page(k->app, "debug");
    say(k->app, TRUE, "coredumpctl could not give the core dump of %s.", k->exe);
    g_unlink(k->core);
  }
  taken_free(k);
}

static void on_core_file(GObject *source, GAsyncResult *result, gpointer data)
{
  App *t = data;
  GFile *file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, NULL);

  if (!file)
    return;
  g_free(ui.core);
  ui.core = g_file_get_path(file);
  g_object_unref(file);
  pick_program(t, t->project.root); /* its program */
}

static void core_choose(App *t, const char *payload)
{
  if (strcmp(payload, CORE_FILE_PAYLOAD) == 0) {
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Open a Core Dump");
    gtk_file_dialog_open(dialog, GTK_WINDOW(t->window), NULL, on_core_file, t);
    g_object_unref(dialog);
    return;
  }
  guint i = (guint)atoi(payload);
  if (!ui.listed || i >= ui.listed->len || busy(t))
    return;
  CoreDump *c = g_ptr_array_index(ui.listed, i);
  char *pid = g_strdup_printf("%d", c->pid), *name = g_strdup_printf("t-code-core-%d", c->pid);
  char *core = g_build_filename(g_get_user_runtime_dir(), name, NULL), *output = g_strconcat("--output=", core, NULL);
  const char *argv[] = { "coredumpctl", "dump", "--no-pager", "--quiet", output, pid, NULL };
  GError *error = NULL;
  GSubprocess *process =
      g_subprocess_newv(argv, G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, &error);

  if (process) {
    Taken *k = g_new0(Taken, 1);
    *k = (Taken){ t, g_strdup(core), g_strdup(c->exe), c->signal };
    show_sidebar_page(t, "debug");
    update_ui(t, "Reading the core dump…");
    g_subprocess_wait_check_async(process, NULL, on_core_taken, k);
    g_object_unref(process);
  } else {
    say(t, TRUE, "Could not run coredumpctl: %s", error->message);
    g_error_free(error);
  }
  g_free(output);
  g_free(core);
  g_free(name);
  g_free(pid);
}

void debug_open_core(App *t)
{
  if (busy(t))
    return;
  if (ui.listed)
    g_ptr_array_unref(ui.listed);
  ui.listed = adapter_core_dumps();
  picker_open(t, "Open which core dump? (the crashes systemd-coredump kept)", core_query, core_choose);
}

/* The commands ------------------------------------------------------------------------------- */

gboolean debug_active(App *t)
{
  return t->debug.session != NULL;
}

Dap *debug_dap(App *t)
{
  return t->debug.session && t->debug.session->state != ENDING ? t->debug.session->dap : NULL;
}

/* A request that sets the stopped program going (a step, continue). */
static void go(App *t, const char *command)
{
  Debugger *s = t->debug.session;
  /* In front of the Disassembly View, a step is one instruction (stepOut and continue have no steps) */
  gboolean instruction = disassembly_shown(t) && (strcmp(command, "next") == 0 || strcmp(command, "stepIn") == 0);
  char *args;

  if (!s || s->state != STOPPED || s->examining)
    return;
  s->state = RUNNING;
  breakpoints_show_stop(t, NULL, 0);
  debug_view_running(t);
  update_ui(t, "Running");
  args = g_strdup_printf("{\"threadId\":%" G_GINT64_FORMAT "%s}", s->thread,
                         instruction ? ",\"granularity\":\"instruction\"" : "");
  dap_request(s->dap, command, args, NULL, NULL);
  g_free(args);
}

/* F5 on a Python file: debugpy runs it (as a module when it is in its project's packages). */
static void start_python(App *t, const DebugPlan *plan)
{
  AdapterTarget target = {
    .program = plan->program,
    .cwd = plan->cwd,
    .module = plan->module,
    .python = plan->python,
    .pythonpath = plan->pythonpath,
  };
  char *launch = adapter_launch(ADAPTER_DEBUGPY, &target), *shown = shown_program(t, plan->program);

  start_session(t, ADAPTER_DEBUGPY, plan->python, "launch", launch, plan->cwd, shown, NULL);
  g_free(shown);
  g_free(launch);
}

/* F5 on a Go file: Delve builds its package (or the file alone) for debugging and runs it. */
static void start_go(App *t, const DebugPlan *plan)
{
  AdapterTarget target = { .program = plan->program, .cwd = plan->cwd, .tty = ui.tty };
  char *launch = adapter_launch(ADAPTER_DELVE, &target), *shown = shown_program(t, plan->program);

  start_session(t, ADAPTER_DELVE, NULL, "launch", launch, plan->cwd, shown, NULL);
  g_free(shown);
  g_free(launch);
}

/* Saved: the Python or Go program starts. */
static void start_saved(App *t, gpointer data G_GNUC_UNUSED)
{
  DebugPlan plan = project_debug_plan(t);

  if (!t->closing && !t->debug.session) {
    if (plan.way == DEBUG_PYTHON)
      start_python(t, &plan);
    else if (plan.way == DEBUG_GO)
      start_go(t, &plan);
  }
  debug_plan_clear(&plan);
}

/* The editors are saved: the plugin command ("<plugin id>\n<command id>") that starts debugging runs. */
static void debug_plugin_saved(App *t, gpointer data)
{
  char **ids = g_strsplit(data, "\n", 2);

  if (!t->closing && !t->debug.session)
    plugin_run_command(t, ids[0], ids[1]);
  g_strfreev(ids);
  g_free(data);
}

/* The plugin command F5 hands the current file to ("debug" in plugin.json), or NULL. */
static PluginCommand *plugin_debugs(App *t)
{
  Tab *tab = current_tab(t);

  return tab && tab->kind == TAB_EDITOR ? plugins_debug_command_for(t, tab->path) : NULL;
}

void debug_start_or_continue(App *t)
{
  DebugPlan plan;
  PluginCommand *command;

  if (t->debug.session) {
    go(t, "continue");
    return;
  }
  if ((command = plugin_debugs(t))) { /* the plugin starts its debugger, then asks for it (debug-start) */
    show_sidebar_page(t, "debug");
    if (t->debug.term)
      vte_terminal_reset(t->debug.term, TRUE, TRUE);
    say(t, FALSE, "%s prepares the program for debugging…", command->plugin->name);
    save_all(t, debug_plugin_saved, g_strconcat(command->plugin->id, "\n", command->id, NULL));
    return;
  }
  plan = project_debug_plan(t);
  if (plan.way == DEBUG_CANNOT) {
    show_sidebar_page(t, "debug");
    say(t, TRUE, "F5 debugs C, C++, Rust, Python and Go: Ctrl+F5 runs this file.");
  } else if (plan.way == DEBUG_PYTHON || plan.way == DEBUG_GO) {
    g_clear_pointer(&ui.cwd, g_free);
    save_all(t, start_saved, NULL);
  } else {
    char *missing = NULL, **argv = adapter_argv(native(t), NULL, NULL, &missing);
    if (!argv) {
      show_sidebar_page(t, "debug");
      say_text(t, TRUE, missing);
    } else {
      g_free(ui.cwd);
      ui.cwd = build_for_debug(t);
      update_ui(t, "Building… (Terminal)");
    }
    g_strfreev(argv);
    g_free(missing);
  }
  debug_plan_clear(&plan);
}

/* clang-format off */
void debug_step_over(App *t) { go(t, "next"); }
void debug_step_into(App *t) { go(t, "stepIn"); }
void debug_step_out(App *t) { go(t, "stepOut"); }
/* clang-format on */

void debug_pause(App *t)
{
  Debugger *s = t->debug.session;
  char *args;

  if (!s || s->state != RUNNING)
    return;
  args = g_strdup_printf("{\"threadId\":%" G_GINT64_FORMAT "}", s->thread);
  dap_request(s->dap, "pause", args, NULL, NULL);
  g_free(args);
}

void debug_stop(App *t)
{
  if (t->debug.session)
    end_session(t->debug.session);
}

void debug_restart(App *t)
{
  if (t->debug.session) {
    t->debug.session->restart = TRUE;
    end_session(t->debug.session);
  } else {
    debug_start_or_continue(t);
  }
}

/* The build for debugging failed (the control socket's "debug-failed"): its errors are in the
 * Terminal. */
void debug_build_failed(App *t)
{
  if (!ui.cwd || t->debug.session)
    return;
  g_clear_pointer(&ui.cwd, g_free);
  update_ui(t, "The build failed: its errors are in the Terminal");
}

/* The evaluate line ---------------------------------------------------------------------------- */

static void on_evaluated(gpointer data, JsonNode *body, const char *error)
{
  App *t = data;
  const char *result = json_string_at(body, "result");

  if (error)
    say_text(t, TRUE, error);
  else if (result)
    say(t, FALSE, "= %s", result);
}

/* Enter in the line under the terminal: the expression is evaluated in the paused frame ("-exec ..."
 * runs a gdb command). */
static void on_evaluate(GtkEntry *entry, App *t)
{
  const char *text = gtk_editable_get_text(GTK_EDITABLE(entry));
  gint64 frame = debug_view_frame_id();
  gboolean command = g_str_has_prefix(text, "-exec ");
  char *quoted, *args;

  if (!*text)
    return;
  say(t, FALSE, "› %s", text);
  if (!debug_dap(t) || (frame < 0 && !command)) {
    say(t, TRUE, "Pause the program first (a breakpoint, or Pause): an expression is evaluated where it is paused.");
    return;
  }
  quoted = json_quote(command ? text + 6 : text);
  if (frame >= 0)
    args = g_strdup_printf("{\"expression\":%s,\"frameId\":%" G_GINT64_FORMAT ",\"context\":\"%s\"}", quoted, frame,
                           command ? "repl" : "watch");
  else
    args = g_strdup_printf("{\"expression\":%s,\"context\":\"repl\"}", quoted);
  dap_request(debug_dap(t), "evaluate", args, on_evaluated, t);
  gtk_editable_set_text(GTK_EDITABLE(entry), "");
  g_free(args);
  g_free(quoted);
}

/* The panel ---------------------------------------------------------------------------------- */

static GtkWidget *tool_button(App *t, GtkWidget *bar, const char *icon, const char *tip, void (*run)(App *))
{
  GtkWidget *button = gtk_button_new_from_icon_name(icon);

  gtk_widget_set_tooltip_text(button, tip);
  gtk_widget_add_css_class(button, "flat");
  gtk_widget_set_focus_on_click(button, FALSE);
  g_signal_connect_swapped(button, "clicked", G_CALLBACK(run), t);
  gtk_box_append(GTK_BOX(bar), button);
  return button;
}

/* The window is going: nothing touches the panel any more. */
static void on_panel_destroyed(GtkWidget *box G_GNUC_UNUSED, App *t)
{
  ui.status = ui.start = ui.pause = ui.over = ui.into = ui.out = ui.restart = ui.stop = ui.assembly = ui.memory = NULL;
  ui.evaluate = NULL;
  t->debug.panel = NULL;
  t->debug.term = NULL;
}

/* The program's terminal: a pty of its own, whose other end gdb gives the programs. That end is
 * held open here, so the terminal stays between programs. */
static void open_pty(App *t)
{
  GError *error = NULL;
  VtePty *pty = vte_pty_new_sync(VTE_PTY_DEFAULT, NULL, &error);
  const char *name = pty ? ptsname(vte_pty_get_fd(pty)) : NULL;

  if (!name) {
    g_warning("t-code: no terminal for the debugger: %s", error ? error->message : "no pty name");
    g_clear_error(&error);
    g_clear_object(&pty);
    return;
  }
  ui.tty = g_strdup(name);
  if (open(ui.tty, O_RDWR | O_NOCTTY | O_CLOEXEC) < 0) /* held till t-code ends */
    g_warning("t-code: cannot open %s", ui.tty);
  vte_terminal_set_pty(t->debug.term, pty);
  g_object_unref(pty);
}

GtkWidget *debug_panel(App *t)
{
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);

  gtk_widget_add_css_class(bar, "tcode-debug-bar");
  ui.start = tool_button(t, bar, "tcode-run-symbolic", "Start Debugging (F5)", debug_start_or_continue);
  ui.pause = tool_button(t, bar, "tcode-pause-symbolic", "Pause (F6)", debug_pause);
  ui.over = tool_button(t, bar, "tcode-step-over-symbolic", "Step Over: the next line (F10)", debug_step_over);
  ui.into = tool_button(t, bar, "tcode-step-into-symbolic", "Step Into: into the function called here (F11)",
                        debug_step_into);
  ui.out = tool_button(t, bar, "tcode-step-out-symbolic", "Step Out: back to the caller (Shift+F11)", debug_step_out);
  ui.restart = tool_button(t, bar, "tcode-refresh-symbolic", "Restart (Ctrl+Shift+F5)", debug_restart);
  ui.stop = tool_button(t, bar, "tcode-stop-symbolic", "Stop (Shift+F5)", debug_stop);
  ui.assembly = gtk_button_new_with_label("ASM");
  gtk_widget_set_tooltip_text(ui.assembly, "Open Disassembly View: the paused function's machine instructions; while "
                                           "it is in front, F10 / F11 step one instruction");
  gtk_widget_add_css_class(ui.assembly, "flat");
  gtk_widget_set_focus_on_click(ui.assembly, FALSE);
  g_signal_connect_swapped(ui.assembly, "clicked", G_CALLBACK(disassembly_open), t);
  gtk_box_append(GTK_BOX(bar), ui.assembly);
  ui.memory = gtk_button_new_with_label("MEM");
  gtk_widget_set_tooltip_text(ui.memory, "Open Memory View: the bytes a pointer, an array or a variable points to, "
                                         "read again at each pause");
  gtk_widget_add_css_class(ui.memory, "flat");
  gtk_widget_set_focus_on_click(ui.memory, FALSE);
  g_signal_connect_swapped(ui.memory, "clicked", G_CALLBACK(memory_open), t);
  gtk_box_append(GTK_BOX(bar), ui.memory);
  ui.status = gtk_label_new("Not debugging");
  gtk_label_set_xalign(GTK_LABEL(ui.status), 0);
  gtk_label_set_ellipsize(GTK_LABEL(ui.status), PANGO_ELLIPSIZE_END);
  gtk_widget_set_hexpand(ui.status, TRUE);
  gtk_widget_set_margin_start(ui.status, 8);
  gtk_widget_add_css_class(ui.status, "dim-label");
  gtk_box_append(GTK_BOX(bar), ui.status);
  gtk_box_append(GTK_BOX(box), bar);

  t->debug.term = new_terminal(t);
  gtk_widget_add_css_class(GTK_WIDGET(t->debug.term), "tcode-shell");
  vte_terminal_set_clear_background(t->debug.term, FALSE);
  open_pty(t);
  gtk_box_append(GTK_BOX(box), GTK_WIDGET(t->debug.term));
  ui.evaluate = gtk_entry_new();
  gtk_entry_set_placeholder_text(GTK_ENTRY(ui.evaluate), EVALUATE_GDB);
  gtk_widget_add_css_class(ui.evaluate, "tcode-debug-evaluate");
  g_signal_connect(ui.evaluate, "activate", G_CALLBACK(on_evaluate), t);
  gtk_box_append(GTK_BOX(box), ui.evaluate);
  say(t, FALSE,
      "F5 builds the project and runs it here under the debugger (C, C++, Rust; Python with debugpy, Go with "
      "Delve); click left of a line number (or F9) to stop at that line, right click there for a condition or a "
      "logpoint.");
  t->debug.panel = box;
  g_signal_connect(box, "destroy", G_CALLBACK(on_panel_destroyed), t);
  update_ui(t, NULL);
  return box;
}

void debug_own_processes(App *t, GHashTable *owners)
{
  if (t->debug.session) {
    char *label = g_strdup_printf("Debugger: %s", session_name(t->debug.session));
    procs_own(owners, dap_pid(t->debug.session->dap), PROC_DEBUGGER, label);
    if (t->debug.session->child && g_subprocess_get_identifier(t->debug.session->child))
      procs_own(owners, atoi(g_subprocess_get_identifier(t->debug.session->child)), PROC_DEBUGGER, label);
    g_free(label);
  }
}
