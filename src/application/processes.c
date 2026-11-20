/*
 * The processes t-code runs (see processes.h): descendants through /proc/<pid>/task/<tid>/children,
 * their CPU time and threads from /proc/<pid>/stat, memory from smaps_rollup.
 */
#define _DEFAULT_SOURCE
#include "application/processes.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#define MAX_DEPTH 64

struct ProcSampler {
  GHashTable *ticks; /* pid -> Ticks *: CPU time at the last sample */
  gint64 at;         /* g_get_monotonic_time() of the last sample; 0 before the first */
};

typedef struct {
  guint64 start; /* the process the ticks belong to */
  guint64 used;  /* utime + stime */
} Ticks;

typedef struct {
  char *name;
  int ppid, threads;
  guint64 used, start, rss_pages;
} Stat;

static void owner_free(gpointer data)
{
  ProcOwner *o = data;

  g_free(o->label);
  g_free(o);
}

GHashTable *procs_owners_new(void)
{
  return g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, owner_free);
}

ProcSampler *procs_sampler_new(void)
{
  ProcSampler *s = g_new0(ProcSampler, 1);

  s->ticks = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
  return s;
}

void procs_sampler_free(ProcSampler *s)
{
  if (!s)
    return;
  g_hash_table_unref(s->ticks);
  g_free(s);
}

/* /proc/<pid>/stat; the name sits in parentheses and may hold both. */
static gboolean read_stat(int pid, Stat *out)
{
  char path[64], *text = NULL;
  gboolean ok = FALSE;

  snprintf(path, sizeof path, "/proc/%d/stat", pid);
  if (!g_file_get_contents(path, &text, NULL, NULL))
    return FALSE;
  char *open = strchr(text, '('), *close = strrchr(text, ')');
  if (open && close && close > open) {
    char state;
    unsigned long long utime, stime, start, rss;
    /* state ppid pgrp session tty_nr tpgid flags minflt cminflt majflt cmajflt utime stime cutime
     * cstime priority nice num_threads itrealvalue starttime vsize rss */
    ok = sscanf(close + 1, " %c %d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu %*d %*d %*d %*d %d %*d %llu %*u %llu",
                &state, &out->ppid, &utime, &stime, &out->threads, &start, &rss) == 7 &&
         state != 'Z';
    if (ok) {
      out->name = g_strndup(open + 1, (gsize)(close - open - 1));
      out->used = utime + stime;
      out->start = start;
      out->rss_pages = rss;
    }
  }
  g_free(text);
  return ok;
}

/* The Pss line of smaps_rollup, in bytes; 0 when it cannot be read. */
static guint64 read_pss(int pid)
{
  char path[64], *text = NULL;
  guint64 kb = 0;

  snprintf(path, sizeof path, "/proc/%d/smaps_rollup", pid);
  if (!g_file_get_contents(path, &text, NULL, NULL))
    return 0;
  char *line = strstr(text, "\nPss:");
  if (line)
    kb = g_ascii_strtoull(line + 5, NULL, 10);
  g_free(text);
  return kb * 1024;
}

/* argv joined by spaces, or NULL when there is none. */
static char *read_command(int pid)
{
  char path[64], *text = NULL;
  gsize length = 0;

  snprintf(path, sizeof path, "/proc/%d/cmdline", pid);
  if (!g_file_get_contents(path, &text, &length, NULL) || length == 0) {
    g_free(text);
    return NULL;
  }
  while (length > 0 && text[length - 1] == '\0')
    length--;
  for (gsize i = 0; i < length; i++)
    if (text[i] == '\0')
      text[i] = ' ';
  text[length] = '\0';
  char *valid = g_utf8_make_valid(text, -1);
  g_free(text);
  return valid;
}

/* pid's children from every thread's children file. */
static GArray *children_of(int pid)
{
  GArray *pids = g_array_new(FALSE, FALSE, sizeof(int));
  char path[64];
  const char *task;

  snprintf(path, sizeof path, "/proc/%d/task", pid);
  GDir *tasks = g_dir_open(path, 0, NULL);
  while (tasks && (task = g_dir_read_name(tasks))) {
    char *file = g_strdup_printf("/proc/%d/task/%s/children", pid, task), *text = NULL;
    if (g_file_get_contents(file, &text, NULL, NULL)) {
      char **words = g_strsplit_set(g_strstrip(text), " ", -1);
      for (char **w = words; *w; w++) {
        int child = atoi(*w);
        if (child > 0)
          g_array_append_val(pids, child);
      }
      g_strfreev(words);
    }
    g_free(text);
    g_free(file);
  }
  if (tasks)
    g_dir_close(tasks);
  return pids;
}

static void proc_free(gpointer data)
{
  Proc *p = data;

  g_free(p->name);
  g_free(p->command);
  g_free(p);
}

typedef struct {
  ProcSampler *s;
  GHashTable *owners;
  GHashTable *ticks; /* the new ticks table */
  GPtrArray *out;
  double seconds; /* since the last sample; 0 on the first */
  long hz, page;
} Walk;

static Proc *read_proc(Walk *w, int pid, int depth, const ProcOwner *inherited)
{
  Stat st;

  if (!read_stat(pid, &st))
    return NULL;
  Proc *p = g_new0(Proc, 1);
  p->pid = pid;
  p->ppid = st.ppid;
  p->depth = depth;
  p->start = st.start;
  p->name = st.name;
  p->command = read_command(pid);
  if (!p->command)
    p->command = g_strdup(p->name);
  p->threads = st.threads;
  p->rss = st.rss_pages * (guint64)w->page;
  p->memory = read_pss(pid);
  if (!p->memory)
    p->memory = p->rss;
  p->owner = g_hash_table_lookup(w->owners, GINT_TO_POINTER(pid));
  if (!p->owner)
    p->owner = inherited;

  Ticks *before = g_hash_table_lookup(w->s->ticks, GINT_TO_POINTER(pid));
  if (before && before->start == st.start && w->seconds > 0 && st.used >= before->used)
    p->cpu = 100.0 * (double)(st.used - before->used) / (double)w->hz / w->seconds;
  Ticks *now = g_new(Ticks, 1);
  now->start = st.start;
  now->used = st.used;
  g_hash_table_replace(w->ticks, GINT_TO_POINTER(pid), now);
  return p;
}

static int by_owner(gconstpointer a, gconstpointer b)
{
  const Proc *x = *(Proc *const *)a, *y = *(Proc *const *)b;

  if (x->owner->kind != y->owner->kind)
    return (int)x->owner->kind - (int)y->owner->kind;
  int c = g_utf8_collate(x->owner->label, y->owner->label);
  return c ? c : x->pid - y->pid;
}

static int by_pid(gconstpointer a, gconstpointer b)
{
  return (*(Proc *const *)a)->pid - (*(Proc *const *)b)->pid;
}

/* p (already read) goes out, then its descendants. */
static void walk(Walk *w, Proc *p)
{
  GArray *pids;
  GPtrArray *kids;

  g_ptr_array_add(w->out, p);
  if (p->depth >= MAX_DEPTH)
    return;
  pids = children_of(p->pid);
  kids = g_ptr_array_new();
  for (guint i = 0; i < pids->len; i++) {
    int child = g_array_index(pids, int, i);
    Proc *c;
    if (!g_hash_table_contains(w->ticks, GINT_TO_POINTER(child)) && (c = read_proc(w, child, p->depth + 1, p->owner)))
      g_ptr_array_add(kids, c);
  }
  g_array_free(pids, TRUE);
  g_ptr_array_sort(kids, p->depth == 0 ? by_owner : by_pid);
  for (guint i = 0; i < kids->len; i++)
    walk(w, g_ptr_array_index(kids, i));
  g_ptr_array_free(kids, TRUE);
}

GPtrArray *procs_sample(ProcSampler *s, int root, GHashTable *owners)
{
  static const ProcOwner tcode = { PROC_TCODE, (char *)"t-code" };
  gint64 now = g_get_monotonic_time();
  Walk w = {
    .s = s,
    .owners = owners,
    .ticks = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free),
    .out = g_ptr_array_new_with_free_func(proc_free),
    .seconds = s->at ? (double)(now - s->at) / G_USEC_PER_SEC : 0,
    .hz = sysconf(_SC_CLK_TCK),
    .page = sysconf(_SC_PAGESIZE),
  };
  Proc *p;

  if (w.hz <= 0)
    w.hz = 100;
  if (w.page <= 0)
    w.page = 4096;
  if ((p = read_proc(&w, root, 0, &tcode)))
    walk(&w, p);
  g_hash_table_unref(s->ticks);
  s->ticks = w.ticks;
  s->at = now;
  return w.out;
}

gboolean procs_signal(int pid, guint64 start, int sig)
{
  Stat st = { 0 };
  gboolean sent = FALSE;

  if (pid <= 0)
    return FALSE;
#if defined(SYS_pidfd_open) && defined(SYS_pidfd_send_signal)
  int fd = (int)syscall(SYS_pidfd_open, pid, 0);
  if (fd < 0)
    return FALSE;
  if (read_stat(pid, &st) && st.start == start)
    sent = syscall(SYS_pidfd_send_signal, fd, sig, NULL, 0) == 0;
  close(fd);
#else
  if (read_stat(pid, &st) && st.start == start)
    sent = kill(pid, sig) == 0;
#endif
  g_free(st.name);
  return sent;
}

char *procs_format_memory(guint64 bytes)
{
  if (bytes < 1024 * 1024)
    return g_strdup_printf("%" G_GUINT64_FORMAT " KB", bytes / 1024);
  if (bytes < (guint64)1024 * 1024 * 1024)
    return g_strdup_printf("%.1f MB", (double)bytes / (1024.0 * 1024.0));
  return g_strdup_printf("%.2f GB", (double)bytes / (1024.0 * 1024.0 * 1024.0));
}
