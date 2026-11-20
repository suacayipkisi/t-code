/*
 * Tests for ending the programs t-code started (src/application/children.c), GLib-only: a session
 * leader's helpers the way MATLAB has them (one in its own process group, one in a new session, one
 * orphaned) all end, while an unrelated process with the same name survives.
 */
#define _DEFAULT_SOURCE
#include "application/children.h"
#include "util.h"

#include <signal.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

static void sleep_forever(void)
{
  execlp("sleep", "sleep", "300", (char *)NULL);
  _exit(127);
}

/* The leader: a new session, then its helpers; it writes their pids to fd and becomes "sleep". */
static void leader_main(int fd)
{
  pid_t pids[3];

  setsid();
  if ((pids[0] = fork()) == 0) { /* its own process group, like MATLABWindow */
    setpgid(0, 0);
    sleep_forever();
  }
  if ((pids[1] = fork()) == 0) { /* a new session: found as a descendant */
    setsid();
    sleep_forever();
  }
  int pair[2];
  if (pipe(pair) != 0)
    _exit(1);
  if (fork() == 0) { /* exits at once: its child is orphaned, still in the session */
    pid_t orphan = fork();
    if (orphan == 0)
      sleep_forever();
    if (write(pair[1], &orphan, sizeof orphan) != sizeof orphan)
      _exit(1);
    _exit(0);
  }
  if (read(pair[0], &pids[2], sizeof pids[2]) != sizeof pids[2])
    _exit(1);
  wait(NULL);
  if (write(fd, pids, sizeof pids) != sizeof pids)
    _exit(1);
  close(fd);
  sleep_forever();
}

static gboolean running(pid_t pid)
{
  char path[64], *text = NULL;
  gboolean alive;

  snprintf(path, sizeof path, "/proc/%d/stat", pid);
  if (!g_file_get_contents(path, &text, NULL, NULL))
    return FALSE;
  alive = !strstr(text, ") Z ");
  g_free(text);
  return alive;
}

static void wait_count(Children *c, int want)
{
  for (int i = 0; i < 200 && children_signal(c, 0) != want; i++)
    g_usleep(20000);
  g_assert_cmpint(children_signal(c, 0), ==, want);
}

static void test_children_end(void)
{
  int pair[2];
  pid_t helpers[3], stranger, leader;

  g_assert_cmpint(pipe(pair), ==, 0);
  if ((stranger = fork()) == 0) {
    setsid();
    sleep_forever();
  }
  if ((leader = fork()) == 0)
    leader_main(pair[1]);
  close(pair[1]);
  g_assert_cmpint(read(pair[0], helpers, sizeof helpers), ==, sizeof helpers);
  close(pair[0]);

  Children *c = children_new(leader);
  g_assert_nonnull(c);
  wait_count(c, 4); /* the leader and its three helpers, not the stranger */

  /* the leader goes first: the helper that left the session is still known */
  kill(leader, SIGKILL);
  waitpid(leader, NULL, 0);
  wait_count(c, 3);

  children_signal(c, SIGTERM);
  for (int i = 0; i < 3; i++)
    for (int tries = 0; tries < 200 && running(helpers[i]); tries++)
      g_usleep(20000);
  for (int i = 0; i < 3; i++)
    g_assert_false(running(helpers[i]));
  g_assert_cmpint(children_signal(c, 0), ==, 0);
  g_assert_true(running(stranger));

  kill(stranger, SIGKILL);
  waitpid(stranger, NULL, 0);
  children_free(c);
}

/* Nothing to end: no process, or one that is not running. */
static void test_children_none(void)
{
  pid_t gone = fork();

  if (gone == 0)
    _exit(0);
  waitpid(gone, NULL, 0);
  g_assert_null(children_new(0));
  g_assert_null(children_new(gone));
  children_free(NULL);
}

int main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/children/end", test_children_end);
  g_test_add_func("/children/none", test_children_none);
  return g_test_run();
}
