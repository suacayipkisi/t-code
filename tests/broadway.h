/*
 * Tests that show a window run it on GTK's Broadway backend, so nothing appears on the screen:
 * start_broadway() starts a gtk4-broadwayd of their own (without one, the window opens on the
 * desktop for a moment), stop_broadway() ends it. Included by the tests that need it.
 */
#pragma once

#include <glib.h>
#include <glib/gstdio.h>
#include <signal.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

static GPid broadway;
static char *broadway_socket; /* its socket, removed with it: gtk4-broadwayd leaves it on SIGTERM */

/* gtk4-broadwayd ends with the test, also when an assertion aborts it. */
static void broadway_child_setup(gpointer data G_GNUC_UNUSED)
{
  prctl(PR_SET_PDEATHSIG, SIGTERM);
}

/* display n's socket path. */
static char *broadway_socket_path(int n)
{
  char *name = g_strdup_printf("broadway%d.socket", n + 1);
  char *path = g_build_filename(g_get_user_runtime_dir(), name, NULL);

  g_free(name);
  return path;
}

/* Whether display n is taken: a gtk4-broadwayd listens on its socket. Starting one there would take
 * that socket over and then fail on its port. A socket nobody listens on is left over (a test that
 * was killed): it goes, or the displays fill up with them and every test falls back to the desktop. */
static gboolean broadway_taken(int n)
{
  char *path = broadway_socket_path(n);
  struct sockaddr_un address = { .sun_family = AF_UNIX };
  gboolean taken = FALSE;

  if (g_file_test(path, G_FILE_TEST_EXISTS) && strlen(path) < sizeof address.sun_path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    strcpy(address.sun_path, path);
    taken = fd < 0 || connect(fd, (struct sockaddr *)&address, sizeof address) == 0;
    if (fd >= 0)
      close(fd);
    if (!taken)
      g_unlink(path);
  }
  g_free(path);
  return taken;
}

static void start_broadway(void)
{
  for (int tries = 0; tries < 20 && !broadway; tries++) {
    int n = 50 + g_random_int_range(0, 50), status;
    char *display = g_strdup_printf(":%d", n);
    char *argv[] = { "gtk4-broadwayd", display, NULL };
    GPid pid;

    if (!broadway_taken(n) && g_spawn_async(NULL, argv, NULL,
                                            G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD |
                                                G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                                            broadway_child_setup, NULL, &pid, NULL)) {
      g_usleep(300 * 1000);                      /* until it listens */
      if (waitpid(pid, &status, WNOHANG) == 0) { /* still running: it got the display */
        broadway = pid;
        broadway_socket = broadway_socket_path(n);
        g_setenv("GDK_BACKEND", "broadway", TRUE);
        g_setenv("BROADWAY_DISPLAY", display, TRUE);
      }
    }
    g_free(display);
  }
}

static void stop_broadway(void)
{
  if (broadway) {
    kill(broadway, SIGTERM);
    waitpid(broadway, NULL, 0);
    broadway = 0;
  }
  if (broadway_socket) {
    g_unlink(broadway_socket);
    g_clear_pointer(&broadway_socket, g_free);
  }
}
