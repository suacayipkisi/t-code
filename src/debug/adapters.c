/*
 * The debuggers (see adapters.h). gdb gets the program's terminal as its inferior-tty, Delve as the
 * files its stdin, stdout and stderr go to; lldb-dap and debugpy ask t-code to start the program there
 * (runInTerminal, debugger.c).
 */
#define _DEFAULT_SOURCE /* getuid() */
#include "debug/adapters.h"

#include "lsp/json.h"

#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* clang-format off */
static const struct {
  const char *name, *id;
  const char *install;
} adapters[] = {
  [ADAPTER_GDB]     = { "gdb", "gdb",
                        "gdb is not installed: install the package \"gdb\" (dnf, apt, pacman, xbps-install) to debug." },
  [ADAPTER_LLDB]    = { "lldb-dap", "lldb-dap",
                        "lldb-dap is not installed: install the package \"lldb\" (dnf, pacman; apt: \"lldb\" or "
                        "\"lldb-<version>\") to debug with it." },
  [ADAPTER_DEBUGPY] = { "debugpy", "debugpy",
                        "debugpy is not installed for %s: run \"%s -m pip install debugpy\" (or install "
                        "\"python3-debugpy\") to debug Python." },
  [ADAPTER_DELVE]   = { "Delve", "go",
                        "Delve is not installed: run \"go install github.com/go-delve/delve/cmd/dlv@latest\" (or install "
                        "\"delve\") to debug Go." },
  [ADAPTER_PLUGIN]  = { "debugger", "plugin", "The plugin's debugger did not start." },
};
/* clang-format on */

const char *adapter_name(AdapterKind kind)
{
  return adapters[kind].name;
}

const char *adapter_id(AdapterKind kind)
{
  return adapters[kind].id;
}

gboolean adapter_owns_terminal(AdapterKind kind)
{
  return kind == ADAPTER_GDB || kind == ADAPTER_DELVE;
}

gboolean adapter_noise(AdapterKind kind, const char *line)
{
  const char *rest;

  switch (kind) {
    case ADAPTER_DELVE: /* its own command line, not here */ return g_str_has_prefix(line, "Type 'dlv help'");
    case ADAPTER_LLDB:
      if (g_str_has_prefix(line, "Attached to process ") || g_str_has_prefix(line, "Launching: ") ||
          g_str_has_prefix(line, "Executable binary set to ") ||
          g_str_has_prefix(line, "To get started with the debug console") ||
          g_str_has_prefix(line, "For more information visit https://lldb.llvm.org"))
        return TRUE; /* the Debug panel says it */
      if (g_str_has_prefix(line, "warning: (") && strstr(line, "mismatch addresses for section"))
        return TRUE; /* about a library's sections (libasan's, on Fedora): nothing the user can do */
      if (!g_str_has_prefix(line, "Process "))
        return FALSE;
      rest = line + strlen("Process ");
      if (!g_ascii_isdigit(*rest))
        return FALSE;
      while (g_ascii_isdigit(*rest))
        rest++;
      /* "Process 42 exited with status = 0 (0x00000000)": the Debug panel says how it ended */
      return g_str_has_prefix(rest, " exited with status") || g_str_has_prefix(rest, " launched") ||
             g_str_has_prefix(rest, " stopped") || g_str_has_prefix(rest, " resuming");
    default: return FALSE;
  }
}

/* lldb-dap under one of its names: lldb-dap, lldb-dap-19 (Debian's versioned ones), lldb-vscode (before
 * LLVM 18); the newest version first. NULL: none. */
static char *find_lldb(void)
{
  static const char *const names[] = { "lldb-dap", "lldb-vscode" };
  const char *path = g_getenv("PATH");
  char **dirs = g_strsplit(path ? path : "/usr/bin", ":", -1), *found = NULL;
  int best = -1;

  for (size_t n = 0; n < G_N_ELEMENTS(names) && !found; n++) {
    if ((found = g_find_program_in_path(names[n])))
      break;
    for (int i = 0; dirs[i]; i++) { /* lldb-dap-<version> */
      GDir *dir = *dirs[i] ? g_dir_open(dirs[i], 0, NULL) : NULL;
      const char *name;
      size_t length = strlen(names[n]);
      while (dir && (name = g_dir_read_name(dir))) {
        if (strncmp(name, names[n], length) != 0 || name[length] != '-' || !g_ascii_isdigit(name[length + 1]))
          continue; /* not lldb-dap-<version> */
        int version = atoi(name + length + 1);
        if (version > best) {
          char *candidate = g_build_filename(dirs[i], name, NULL);
          if (g_file_test(candidate, G_FILE_TEST_IS_EXECUTABLE)) {
            best = version;
            g_free(found);
            found = candidate;
          } else {
            g_free(candidate);
          }
        }
      }
      if (dir)
        g_dir_close(dir);
    }
  }
  g_strfreev(dirs);
  return found;
}

/* dlv in PATH, else where go install puts it ($GOBIN, $GOPATH/bin, ~/go/bin). */
static char *find_delve(void)
{
  char *found = g_find_program_in_path("dlv");
  const char *gobin = g_getenv("GOBIN"), *gopath = g_getenv("GOPATH");
  char *candidates[] = {
    gobin && *gobin ? g_build_filename(gobin, "dlv", NULL) : NULL,
    gopath && *gopath ? g_build_filename(gopath, "bin", "dlv", NULL) : NULL,
    g_build_filename(g_get_home_dir(), "go", "bin", "dlv", NULL),
  };

  for (size_t i = 0; i < G_N_ELEMENTS(candidates); i++) {
    if (!found && candidates[i] && g_file_test(candidates[i], G_FILE_TEST_IS_EXECUTABLE))
      found = g_steal_pointer(&candidates[i]);
    g_free(candidates[i]);
  }
  return found;
}

/* Whether python can import debugpy. */
static gboolean has_debugpy(const char *python)
{
  const char *argv[] = { python, "-c", "import debugpy.adapter", NULL };
  int status = 1;

  return g_spawn_sync(NULL, (char **)argv, NULL,
                      G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, NULL,
                      NULL, &status, NULL) &&
         g_spawn_check_wait_status(status, NULL);
}

AdapterKind adapter_native(const char *preferred)
{
  gboolean lldb = g_strcmp0(preferred, "lldb") == 0;
  char *gdb = g_find_program_in_path("gdb"), *other = find_lldb();
  AdapterKind kind = lldb ? (other || !gdb ? ADAPTER_LLDB : ADAPTER_GDB) : (gdb || !other ? ADAPTER_GDB : ADAPTER_LLDB);

  g_free(gdb);
  g_free(other);
  return kind;
}

char **adapter_argv(AdapterKind kind, const char *tty, const char *python, char **missing)
{
  GStrvBuilder *argv = g_strv_builder_new();
  char *program = NULL, **result;

  if (missing)
    *missing = NULL;
  switch (kind) {
    case ADAPTER_GDB:
      if ((program = g_find_program_in_path("gdb"))) {
        char *inferior = g_strdup_printf("set inferior-tty %s", tty ? tty : "");
        /* Intel's syntax ("mov eax, 1"), as most teaching material has it */
        const char *const args[] = { program, "-q",
                                     "-iex",  "set debuginfod enabled off",
                                     "-iex",  "set disassembly-flavor intel",
                                     "-iex",  inferior,
                                     "-i",    "dap",
                                     NULL };
        g_strv_builder_addv(argv, (const char **)args);
        g_free(inferior);
      }
      break;
    case ADAPTER_LLDB:
      if ((program = find_lldb()))
        g_strv_builder_add(argv, program);
      break;
    case ADAPTER_DEBUGPY:
      python = python ? python : "python3";
      if (has_debugpy(python)) {
        program = g_strdup(python);
        const char *const args[] = { python, "-m", "debugpy.adapter", NULL };
        g_strv_builder_addv(argv, (const char **)args);
      }
      break;
    case ADAPTER_DELVE:
      if ((program = find_delve())) {
        const char *const args[] = { program, "dap", "--client-addr", "{socket}", NULL };
        g_strv_builder_addv(argv, (const char **)args);
      }
      break;
    case ADAPTER_PLUGIN: break; /* the plugin gives it */
  }
  result = program ? g_strv_builder_end(argv) : NULL;
  if (!program && missing)
    *missing = kind == ADAPTER_DEBUGPY ? g_strdup_printf(adapters[kind].install, python, python)
                                       : g_strdup(adapters[kind].install);
  g_strv_builder_unref(argv);
  g_free(program);
  return result;
}

/* ,"name":<quoted value> when value is set. */
static void add_string(GString *json, const char *name, const char *value)
{
  char *quoted;

  if (!value)
    return;
  quoted = json_quote(value);
  g_string_append_printf(json, ",\"%s\":%s", name, quoted);
  g_free(quoted);
}

/* ,"args":[...] */
static void add_args(GString *json, const char *const *args)
{
  g_string_append(json, ",\"args\":[");
  for (int i = 0; args && args[i]; i++) {
    char *quoted = json_quote(args[i]);
    g_string_append_printf(json, "%s%s", i ? "," : "", quoted);
    g_free(quoted);
  }
  g_string_append_c(json, ']');
}

/* The object built with a comma before each member: the first one goes. */
static char *close_object(GString *json)
{
  if (json->len > 1 && json->str[1] == ',')
    g_string_erase(json, 1, 1);
  g_string_append_c(json, '}');
  return g_string_free(json, FALSE);
}

char *adapter_launch(AdapterKind kind, const AdapterTarget *target)
{
  GString *json = g_string_new("{");

  switch (kind) {
    case ADAPTER_GDB:
    case ADAPTER_LLDB:
      add_string(json, "program", target->program);
      add_args(json, target->args);
      add_string(json, "cwd", target->cwd);
      if (kind == ADAPTER_LLDB) /* runInTerminal: before LLVM 20; console: since */
        g_string_append(json, ",\"runInTerminal\":true,\"console\":\"integratedTerminal\"");
      break;
    case ADAPTER_DEBUGPY:
      if (target->module)
        add_string(json, "module", target->module);
      else
        add_string(json, "program", target->program);
      add_args(json, target->args);
      add_string(json, "cwd", target->cwd);
      add_string(json, "python", target->python ? target->python : "python3");
      if (target->pythonpath) {
        const char *now = g_getenv("PYTHONPATH");
        char *value = now && *now ? g_strconcat(target->pythonpath, ":", now, NULL) : g_strdup(target->pythonpath);
        char *quoted = json_quote(value);
        g_string_append_printf(json, ",\"env\":{\"PYTHONPATH\":%s}", quoted);
        g_free(quoted);
        g_free(value);
      }
      g_string_append(json, ",\"console\":\"integratedTerminal\",\"justMyCode\":true,\"subProcess\":false");
      break;
    case ADAPTER_DELVE:
      g_string_append(json, ",\"mode\":\"debug\"");
      add_string(json, "program", target->program);
      add_args(json, target->args);
      add_string(json, "cwd", target->cwd);
      add_string(json, "stdinFrom", target->tty); /* it has no runInTerminal: the terminal's device instead */
      add_string(json, "stdoutTo", target->tty);
      add_string(json, "stderrTo", target->tty);
      break;
    case ADAPTER_PLUGIN: break;
  }
  return close_object(json);
}

char *adapter_attach(AdapterKind kind, int pid, const char *program, const char **request)
{
  GString *json = g_string_new("{");

  *request = "attach";
  switch (kind) {
    case ADAPTER_GDB:
    case ADAPTER_LLDB:
      g_string_append_printf(json, ",\"pid\":%d", pid);
      add_string(json, "program", program);
      break;
    case ADAPTER_DEBUGPY: g_string_append_printf(json, ",\"processId\":%d,\"justMyCode\":true", pid); break;
    case ADAPTER_DELVE: g_string_append_printf(json, ",\"mode\":\"local\",\"processId\":%d", pid); break;
    case ADAPTER_PLUGIN: break;
  }
  return close_object(json);
}

char *adapter_core(AdapterKind kind, const char *program, const char *core, const char **request)
{
  GString *json;

  if (kind == ADAPTER_DEBUGPY)
    return NULL;
  *request = kind == ADAPTER_DELVE ? "launch" : "attach";
  json = g_string_new(kind == ADAPTER_DELVE ? "{,\"mode\":\"core\"" : "{");
  add_string(json, "program", program);
  add_string(json, kind == ADAPTER_DELVE ? "coreFilePath" : "coreFile", core);
  return close_object(json);
}

/* Processes ---------------------------------------------------------------------------------- */

static void attachable_free(gpointer data)
{
  Attachable *a = data;

  g_free(a->name);
  g_free(a->command);
  g_free(a->exe);
  g_free(a);
}

/* pid's parent and start time (clock ticks since boot) from /proc/<pid>/stat; FALSE when it is gone. */
static gboolean read_stat(int pid, int *parent, guint64 *start)
{
  char *path = g_strdup_printf("/proc/%d/stat", pid), *text = NULL, *close;
  gboolean ok = g_file_get_contents(path, &text, NULL, NULL) && (close = strrchr(text, ')'));

  if (ok) { /* after "pid (name)": state ppid pgrp session tty tpgid flags minflt cminflt majflt cmajflt utime
               stime cutime cstime priority nice threads itrealvalue starttime */
    char **fields = g_strsplit(close + 2, " ", 0);
    ok = g_strv_length(fields) > 19;
    if (ok) {
      *parent = atoi(fields[1]);
      *start = g_ascii_strtoull(fields[19], NULL, 10);
    }
    g_strfreev(fields);
  }
  g_free(text);
  g_free(path);
  return ok;
}

typedef struct {
  Attachable *a;
  int parent;
  guint64 start;
} Found;

static int by_start(gconstpointer a, gconstpointer b)
{
  guint64 x = ((const Found *)a)->start, y = ((const Found *)b)->start;

  return x < y ? 1 : x > y ? -1 : 0;
}

/* Whether pid is root or descends from it, by the parents in found (pid -> parent). */
static gboolean descends(GHashTable *parents, int pid, int root)
{
  for (int depth = 0; pid > 1 && depth < 64; depth++) {
    if (pid == root)
      return TRUE;
    pid = GPOINTER_TO_INT(g_hash_table_lookup(parents, GINT_TO_POINTER(pid)));
  }
  return FALSE;
}

GPtrArray *adapter_attachable(int self)
{
  GPtrArray *list = g_ptr_array_new_with_free_func(attachable_free);
  GArray *found = g_array_new(FALSE, FALSE, sizeof(Found));
  GHashTable *parents = g_hash_table_new(NULL, NULL);
  GDir *proc = g_dir_open("/proc", 0, NULL);
  const char *name;
  GStatBuf st;

  while (proc && (name = g_dir_read_name(proc))) {
    int pid = atoi(name), parent;
    guint64 start;
    char *dir = g_strdup_printf("/proc/%s", name), *cmdline = g_build_filename(dir, "cmdline", NULL), *text = NULL;
    gsize length = 0;
    if (pid > 0 && g_stat(dir, &st) == 0 && st.st_uid == getuid() && read_stat(pid, &parent, &start) &&
        g_file_get_contents(cmdline, &text, &length, NULL) && length > 0) { /* kernel threads have none */
      Attachable *a = g_new0(Attachable, 1);
      char *exe_link = g_build_filename(dir, "exe", NULL), *comm = g_build_filename(dir, "comm", NULL);
      for (gsize i = 0; i + 1 < length; i++)
        if (text[i] == '\0')
          text[i] = ' ';
      a->pid = pid;
      a->command = g_strchomp(g_strndup(text, length));
      a->exe = g_file_read_link(exe_link, NULL);
      if (!g_file_get_contents(comm, &a->name, NULL, NULL))
        a->name = g_strdup("?");
      g_strchomp(a->name);
      g_array_append_val(found, ((Found){ a, parent, start }));
      g_hash_table_insert(parents, GINT_TO_POINTER(pid), GINT_TO_POINTER(parent));
      g_free(comm);
      g_free(exe_link);
    }
    g_free(text);
    g_free(cmdline);
    g_free(dir);
  }
  if (proc)
    g_dir_close(proc);
  g_array_sort(found, by_start);
  for (guint i = 0; i < found->len; i++) {
    Found *f = &g_array_index(found, Found, i);
    if (self > 0 && descends(parents, f->a->pid, self))
      attachable_free(f->a);
    else
      g_ptr_array_add(list, f->a);
  }
  g_hash_table_destroy(parents);
  g_array_free(found, TRUE);
  return list;
}

AdapterKind adapter_for_process(int pid, const char *preferred)
{
  char *link = g_strdup_printf("/proc/%d/exe", pid), *exe = g_file_read_link(link, NULL);
  char *base = exe ? g_path_get_basename(exe) : NULL;
  gboolean python = base && g_str_has_prefix(base, "python");

  g_free(base);
  g_free(exe);
  g_free(link);
  return python ? ADAPTER_DEBUGPY : adapter_native(preferred);
}

/* Core dumps --------------------------------------------------------------------------------- */

static void core_free(gpointer data)
{
  CoreDump *c = data;

  g_free(c->exe);
  g_free(c);
}

static int by_time(gconstpointer a, gconstpointer b)
{
  gint64 x = (*(CoreDump *const *)a)->time, y = (*(CoreDump *const *)b)->time;

  return x < y ? 1 : x > y ? -1 : 0;
}

GPtrArray *adapter_parse_core_dumps(const char *json, int uid)
{
  GPtrArray *list = g_ptr_array_new_with_free_func(core_free);
  JsonParser *parser = json_parser_new();
  JsonNode *root;

  if (json && json_parser_load_from_data(parser, json, -1, NULL) && (root = json_parser_get_root(parser)) &&
      JSON_NODE_HOLDS_ARRAY(root)) {
    JsonArray *array = json_node_get_array(root);
    for (guint i = 0; i < json_array_get_length(array); i++) {
      JsonNode *entry = json_array_get_element(array, i);
      const char *exe = json_string_at(entry, "exe");
      if (!exe || json_int_at(entry, "uid", -1) != uid || g_strcmp0(json_string_at(entry, "corefile"), "present") != 0)
        continue;
      CoreDump *c = g_new0(CoreDump, 1);
      c->pid = (int)json_int_at(entry, "pid", 0);
      c->signal = (int)json_int_at(entry, "sig", 0);
      c->time = json_int_at(entry, "time", 0);
      c->exe = g_strdup(exe);
      g_ptr_array_add(list, c);
    }
  }
  g_object_unref(parser);
  g_ptr_array_sort(list, by_time);
  return list;
}

static void on_core_dumps_listed(GObject *source, GAsyncResult *result, gpointer data)
{
  GTask *task = data;
  char *out = NULL;

  g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result, &out, NULL, NULL);
  g_task_return_pointer(task, adapter_parse_core_dumps(out, (int)getuid()), (GDestroyNotify)g_ptr_array_unref);
  g_free(out);
  g_object_unref(task);
}

/* coredumpctl reads the journal, which may take seconds: never in the window's thread. */
void adapter_core_dumps_async(GCancellable *cancellable, GAsyncReadyCallback callback, gpointer data)
{
  const char *argv[] = { "coredumpctl", "list", "--json=short", "--no-pager", "--quiet", NULL };
  GTask *task = g_task_new(NULL, cancellable, callback, data);
  GSubprocess *process =
      g_subprocess_newv(argv, G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, NULL);

  if (!process) {
    g_task_return_pointer(task, adapter_parse_core_dumps(NULL, (int)getuid()), (GDestroyNotify)g_ptr_array_unref);
    g_object_unref(task);
    return;
  }
  g_subprocess_communicate_utf8_async(process, NULL, cancellable, on_core_dumps_listed, task);
  g_object_unref(process);
}

GPtrArray *adapter_core_dumps_finish(GAsyncResult *result, GError **error)
{
  return g_task_propagate_pointer(G_TASK(result), error);
}
