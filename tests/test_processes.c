/*
 * Tests for reading the processes t-code runs (src/application/processes.c), GLib-only: a child and
 * its own child are found under this process with their owner, the grandchild through its parent;
 * CPU is measured from the second sample on; the signal goes only to the process started then.
 */
#define _DEFAULT_SOURCE
#include "application/processes.h"
#include "util.h"

#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static const Proc *find(GPtrArray *procs, int pid)
{
  for (guint i = 0; i < procs->len; i++)
    if (((Proc *)g_ptr_array_index(procs, i))->pid == pid)
      return g_ptr_array_index(procs, i);
  return NULL;
}

/* pid's first child, once it has one ("sh -c 'sleep 30; :'": the ": " keeps sh from exec'ing sleep). */
static int grandchild_of(int pid)
{
  char *path = g_strdup_printf("/proc/%d/task/%d/children", pid, pid), *text = NULL;
  int found = 0;

  for (int i = 0; i < 200 && !found; i++) {
    g_free(text);
    text = NULL;
    if (g_file_get_contents(path, &text, NULL, NULL))
      found = atoi(text);
    if (!found)
      g_usleep(10000);
  }
  g_free(text);
  g_free(path);
  return found;
}

static void test_processes_sample(void)
{
  const char *argv[] = { "sh", "-c", "while :; do :; done; :", NULL };
  GPid child;
  GError *error = NULL;

  g_assert_true(g_spawn_async(NULL, (char **)argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD, NULL, NULL,
                              &child, &error));
  g_assert_no_error(error);
  GHashTable *owners = procs_owners_new();
  procs_own(owners, child, PROC_PLUGIN, "Plugin: Busy");
  procs_own(owners, child, PROC_SERVER, "ignored: the first owner stays");
  ProcSampler *s = procs_sampler_new();

  GPtrArray *procs = procs_sample(s, getpid(), owners);
  const Proc *self = g_ptr_array_index(procs, 0), *p = find(procs, child);
  g_assert_cmpint(self->pid, ==, getpid());
  g_assert_cmpint(self->depth, ==, 0);
  g_assert_cmpint(self->owner->kind, ==, PROC_TCODE);
  g_assert_cmpstr(self->owner->label, ==, "t-code");
  g_assert_cmpuint(self->memory, >, 0);
  g_assert_cmpuint(self->rss, >, 0); /* PSS splits shared pages: never more than RSS by much */
  g_assert_cmpuint(self->memory, <=, self->rss + self->rss / 10);
  g_assert_cmpint(self->threads, >=, 1);
  g_assert_nonnull(p);
  g_assert_cmpint(p->depth, ==, 1);
  g_assert_cmpint(p->ppid, ==, getpid());
  g_assert_cmpstr(p->owner->label, ==, "Plugin: Busy");
  g_assert_cmpstr(p->name, ==, "sh");
  g_assert_true(g_str_has_prefix(p->command, "sh -c while"));
  g_assert_cmpfloat(p->cpu, ==, 0); /* no sample before */
  g_ptr_array_unref(procs);

  g_usleep(300000);
  procs = procs_sample(s, getpid(), owners);
  p = find(procs, child);
  g_assert_nonnull(p);
  g_assert_cmpfloat(p->cpu, >, 20); /* a busy loop: near 100% of a core */
  guint64 start = p->start;
  g_ptr_array_unref(procs);

  g_assert_false(procs_signal(child, start + 1, SIGTERM)); /* another process's start time */
  g_assert_true(procs_signal(child, start, SIGTERM));
  waitpid(child, NULL, 0);
  g_spawn_close_pid(child);
  g_assert_false(procs_signal(child, start, SIGTERM)); /* gone */

  procs = procs_sample(s, getpid(), owners);
  g_assert_null(find(procs, child));
  g_ptr_array_unref(procs);
  procs_sampler_free(s);
  g_hash_table_unref(owners);
}

/* A grandchild without an owner of its own belongs to its parent's; children of the root come by
 * owner kind, each followed by its descendants. */
static void test_processes_tree(void)
{
  const char *server_argv[] = { "sleep", "30", NULL };
  const char *plugin_argv[] = { "sh", "-c", "sleep 30; :", NULL };
  GPid server, plugin;

  g_assert_true(g_spawn_async(NULL, (char **)server_argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD, NULL,
                              NULL, &server, NULL));
  g_assert_true(g_spawn_async(NULL, (char **)plugin_argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD, NULL,
                              NULL, &plugin, NULL));
  int helper = grandchild_of(plugin);
  g_assert_cmpint(helper, >, 0);
  GHashTable *owners = procs_owners_new();
  procs_own(owners, server, PROC_SERVER, "Language server: sleep");
  procs_own(owners, plugin, PROC_PLUGIN, "Plugin: Shell");
  ProcSampler *s = procs_sampler_new();
  GPtrArray *procs = procs_sample(s, getpid(), owners);

  const Proc *h = find(procs, helper);
  g_assert_nonnull(h);
  g_assert_cmpint(h->depth, ==, 2);
  g_assert_cmpstr(h->owner->label, ==, "Plugin: Shell");
  int at[3] = { -1, -1, -1 };
  for (guint i = 0; i < procs->len; i++) {
    int pid = ((Proc *)g_ptr_array_index(procs, i))->pid;
    if (pid == plugin)
      at[0] = (int)i;
    else if (pid == helper)
      at[1] = (int)i;
    else if (pid == server)
      at[2] = (int)i;
  }
  g_assert_cmpint(at[0], >, 0);
  g_assert_cmpint(at[1], ==, at[0] + 1); /* right under its parent */
  g_assert_cmpint(at[2], >, at[1]);      /* plugins before language servers */
  g_ptr_array_unref(procs);
  procs_sampler_free(s);
  g_hash_table_unref(owners);

  kill(helper, SIGKILL);
  kill(plugin, SIGKILL);
  kill(server, SIGKILL);
  waitpid(plugin, NULL, 0);
  waitpid(server, NULL, 0);
  g_spawn_close_pid(plugin);
  g_spawn_close_pid(server);
}

static void test_processes_format(void)
{
  char *text;

  text = procs_format_memory(512 * 1024);
  g_assert_cmpstr(text, ==, "512 KB");
  g_free(text);
  text = procs_format_memory(12 * 1024 * 1024 + 512 * 1024);
  g_assert_cmpstr(text, ==, "12.5 MB");
  g_free(text);
  text = procs_format_memory((guint64)3 * 1024 * 1024 * 1024);
  g_assert_cmpstr(text, ==, "3.00 GB");
  g_free(text);
}

int main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/processes/sample", test_processes_sample);
  g_test_add_func("/processes/tree", test_processes_tree);
  g_test_add_func("/processes/format", test_processes_format);
  return g_test_run();
}
