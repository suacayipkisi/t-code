/*
 * The debuggers t-code drives over the Debug Adapter Protocol, and how each one is started and told
 * what to debug: gdb and lldb-dap (C, C++, Rust), debugpy (Python), Delve (Go). GLib and json-glib only.
 */
#pragma once

#include <glib.h>

typedef enum {
  ADAPTER_GDB,
  ADAPTER_LLDB,
  ADAPTER_DEBUGPY,
  ADAPTER_DELVE,
} AdapterKind;

/* What a session debugs. Only program is needed; the rest may be NULL. */
typedef struct {
  const char *program;     /* the executable; a Python file; a Go package's folder */
  const char *const *args; /* its arguments (NULL-terminated) */
  const char *cwd;         /* where it runs */
  const char *module;      /* Python: run as "python -m module" instead of program */
  const char *python;      /* Python: the interpreter (NULL: python3) */
  const char *pythonpath;  /* Python: put first in PYTHONPATH */
  const char *tty;         /* Delve: the program's terminal (its stdin, stdout and stderr) */
} AdapterTarget;

/* The name the Debug panel shows ("gdb", "lldb-dap", "debugpy", "Delve"). */
const char *adapter_name(AdapterKind kind);

/* The adapterID "initialize" names. */
const char *adapter_id(AdapterKind kind);

/* Whether the program's terminal is given to the debugger itself (gdb's inferior-tty, Delve's
 * redirections); else the debugger asks t-code to start it there (runInTerminal). */
gboolean adapter_owns_terminal(AdapterKind kind);

/* The C / C++ / Rust debugger: preferred ("gdb", "lldb"; NULL or "": gdb) when it is installed, else the
 * other one when that is, else preferred's (which then says it is missing). */
AdapterKind adapter_native(const char *preferred);

/* The command that starts kind's debugger (free with g_strfreev): tty is the program's terminal (gdb),
 * python the interpreter (debugpy; NULL: python3). NULL when it is not installed: *missing (may be
 * NULL) then says how to install it (free it). Delve's has "{socket}" (dap.h). */
char **adapter_argv(AdapterKind kind, const char *tty, const char *python, char **missing);

/* The arguments of "launch" for target (a JSON object; free it). */
char *adapter_launch(AdapterKind kind, const AdapterTarget *target);

/* The request ("launch" or "attach") and its arguments that attach to the running process pid (its
 * executable program may be NULL). */
char *adapter_attach(AdapterKind kind, int pid, const char *program, const char **request);

/* The same for a core dump (a crashed program's memory, saved) of program; NULL when kind cannot read
 * one (debugpy). */
char *adapter_core(AdapterKind kind, const char *program, const char *core, const char **request);

/* A process the user may attach to. */
typedef struct {
  int pid;
  char *name;    /* its command's name ("python3") */
  char *command; /* its command line, spaces between the arguments */
  char *exe;     /* its executable, or NULL when it cannot be read */
} Attachable;

/* The user's processes but self and its descendants, the newest first (Attachable; free with
 * g_ptr_array_unref). */
GPtrArray *adapter_attachable(int self);

/* The debugger for the running process pid: debugpy for a Python one, else adapter_native(preferred). */
AdapterKind adapter_for_process(int pid, const char *preferred);

/* A crashed program's memory that systemd-coredump kept. */
typedef struct {
  int pid;
  int signal;
  gint64 time; /* microseconds since 1970 */
  char *exe;
} CoreDump;

/* The user's core dumps that systemd-coredump still holds, the newest first (CoreDump; free with
 * g_ptr_array_unref); empty without coredumpctl. */
GPtrArray *adapter_core_dumps(void);

/* The same from coredumpctl's "list --json=short" output (for the tests); uid: the user's. */
GPtrArray *adapter_parse_core_dumps(const char *json, int uid);
