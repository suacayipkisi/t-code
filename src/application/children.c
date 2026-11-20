/*
 * Ending the programs t-code started (see children.h): the leader's session and its descendants,
 * read from /proc, each checked by its start time before the signal.
 */
#define _DEFAULT_SOURCE
#include "application/children.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

struct Children {
  int leader;
  guint64 leader_start; /* processes started before the leader are never ours */
  GHashTable *seen;     /* pid -> start time (guint64 *): every process found so far */
};

typedef struct {
  int ppid, session;
  char state;
  guint64 start; /* clock ticks after boot */
} Stat;

/* /proc/<pid>/stat: the fields after the command name (which may hold spaces and parentheses). */
static gboolean read_stat(int pid, Stat *out)
{
  char path[64], *text = NULL;
  gboolean ok = FALSE;

  snprintf(path, sizeof path, "/proc/%d/stat", pid);
  if (!g_file_get_contents(path, &text, NULL, NULL))
    return FALSE;
  char *p = strrchr(text, ')');
  if (p) {
    int pgrp;
    unsigned long long start;
    /* state ppid pgrp session tty_nr tpgid flags minflt cminflt majflt cmajflt utime stime cutime
     * cstime priority nice num_threads itrealvalue starttime */
    ok = sscanf(p + 1, " %c %d %d %d %*d %*d %*u %*u %*u %*u %*u %*u %*u %*d %*d %*d %*d %*d %*d %llu", &out->state,
                &out->ppid, &pgrp, &out->session, &start) == 5;
    out->start = start;
  }
  g_free(text);
  return ok;
}

/* The signal goes through a pidfd opened before the start time is checked: the pid cannot stand for
 * another process by the time it arrives. */
static void send_signal(int pid, guint64 start, int sig)
{
#if defined(SYS_pidfd_open) && defined(SYS_pidfd_send_signal)
  int fd = (int)syscall(SYS_pidfd_open, pid, 0);
  Stat st;

  if (fd < 0)
    return;
  if (read_stat(pid, &st) && st.start == start)
    syscall(SYS_pidfd_send_signal, fd, sig, NULL, 0);
  close(fd);
#else
  Stat st;

  if (read_stat(pid, &st) && st.start == start)
    kill(pid, sig);
#endif
}

Children *children_new(int leader)
{
  Stat st;

  if (leader <= 0 || !read_stat(leader, &st) || st.state == 'Z')
    return NULL;
  Children *c = g_new0(Children, 1);
  c->leader = leader;
  c->leader_start = st.start;
  c->seen = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
  return c;
}

static void remember(GHashTable *found, int pid, guint64 start)
{
  guint64 *value = g_new(guint64, 1);

  *value = start;
  g_hash_table_replace(found, GINT_TO_POINTER(pid), value);
}

/* pid's children (every thread's), and theirs, into found. */
static void add_descendants(Children *c, GHashTable *found, int pid, int depth)
{
  char path[64];
  GDir *tasks;
  const char *task;

  snprintf(path, sizeof path, "/proc/%d/task", pid);
  if (depth > 64 || !(tasks = g_dir_open(path, 0, NULL)))
    return;
  while ((task = g_dir_read_name(tasks))) {
    char *file = g_strdup_printf("/proc/%d/task/%s/children", pid, task), *text = NULL;
    if (g_file_get_contents(file, &text, NULL, NULL)) {
      char **pids = g_strsplit_set(g_strstrip(text), " ", -1);
      for (char **p = pids; *p; p++) {
        int child = atoi(*p);
        Stat st;
        if (child > 0 && !g_hash_table_contains(found, GINT_TO_POINTER(child)) && read_stat(child, &st) &&
            st.start >= c->leader_start) {
          remember(found, child, st.start);
          add_descendants(c, found, child, depth + 1);
        }
      }
      g_strfreev(pids);
    }
    g_free(text);
    g_free(file);
  }
  g_dir_close(tasks);
}

int children_signal(Children *c, int sig)
{
  GHashTable *found = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
  GHashTableIter iter;
  gpointer key, value;
  GDir *proc = g_dir_open("/proc", 0, NULL);
  const char *name;
  int count = 0;

  /* the session: the kernel keeps its id from being reused while any process is in it */
  while (proc && (name = g_dir_read_name(proc))) {
    int pid = atoi(name);
    Stat st;
    if (pid > 0 && read_stat(pid, &st) && st.session == c->leader && st.start >= c->leader_start)
      remember(found, pid, st.start);
  }
  if (proc)
    g_dir_close(proc);
  /* the ones seen before, orphaned since (their start time tells them apart from a reused pid) */
  g_hash_table_iter_init(&iter, c->seen);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    Stat st;
    if (read_stat(GPOINTER_TO_INT(key), &st) && st.start == *(guint64 *)value)
      remember(found, GPOINTER_TO_INT(key), st.start);
  }
  /* and what left the session but still descends from it */
  GList *roots = g_hash_table_get_keys(found);
  for (GList *l = roots; l; l = l->next)
    add_descendants(c, found, GPOINTER_TO_INT(l->data), 0);
  g_list_free(roots);

  g_hash_table_iter_init(&iter, found);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    int pid = GPOINTER_TO_INT(key);
    Stat st;
    if (!read_stat(pid, &st) || st.start != *(guint64 *)value || st.state == 'Z')
      continue;
    count++;
    if (sig)
      send_signal(pid, st.start, sig);
    if (!g_hash_table_contains(c->seen, key))
      remember(c->seen, pid, st.start);
  }
  g_hash_table_unref(found);
  return count;
}

void children_free(Children *c)
{
  if (!c)
    return;
  g_hash_table_unref(c->seen);
  g_free(c);
}
