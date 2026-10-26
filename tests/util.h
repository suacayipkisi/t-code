/*
 * What the tests share: turning the main loop for a while or until something is true, and writing a
 * file with its folders. Each test is a program of its own: these are static inline.
 */
#pragma once

#include <glib.h>
#include <glib/gstdio.h>

/* Runs the main loop for ms milliseconds. */
static inline void run_for(int ms)
{
  gint64 end = g_get_monotonic_time() + ms * 1000;

  while (g_get_monotonic_time() < end)
    g_main_context_iteration(NULL, FALSE);
}

/* Runs the main loop until done(data) is true or 5 seconds pass. */
static inline void run_until(gboolean (*done)(gconstpointer), gconstpointer data)
{
  gint64 end = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;

  while (!done(data) && g_get_monotonic_time() < end)
    g_main_context_iteration(NULL, FALSE);
}

/* Writes text (len bytes; -1: up to its end) to dir/name, making the folders on the way; mode 0
 * keeps the default permissions. Returns the path (free it). */
static inline char *write_at(const char *dir, const char *name, const char *text, gssize len, int mode)
{
  char *path = g_build_filename(dir, name, NULL), *folder = g_path_get_dirname(path);

  g_mkdir_with_parents(folder, 0755);
  g_assert_true(g_file_set_contents(path, text, len, NULL));
  if (mode)
    g_chmod(path, mode);
  g_free(folder);
  return path;
}
