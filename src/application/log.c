/*
 * The log file (see log.h).
 */
#define _DEFAULT_SOURCE /* getpid() under -std=c23 with musl */

#include "application/log.h"

#include <glib/gstdio.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#ifndef TCODE_VERSION /* set by the Makefile */
#define TCODE_VERSION "unknown"
#endif

static char *file_path;
static FILE *file;  /* from log_start() to log_stop() */
static GMutex lock; /* messages come from any thread */

char *log_default_path(void)
{
  return g_build_filename(g_get_user_state_dir(), "t-code", "t-code.log", NULL);
}

const char *log_path(void)
{
  return file_path;
}

static const char *level_name(GLogLevelFlags level)
{
  if (level & G_LOG_LEVEL_ERROR)
    return "ERROR";
  if (level & G_LOG_LEVEL_CRITICAL)
    return "CRITICAL";
  if (level & G_LOG_LEVEL_WARNING)
    return "WARNING";
  if (level & G_LOG_LEVEL_MESSAGE)
    return "Message";
  if (level & G_LOG_LEVEL_INFO)
    return "INFO";
  return "DEBUG";
}

/* "=== <time-format>": a line of the file's own. */
static void write_mark(const char *format)
{
  GDateTime *now = g_date_time_new_now_local();
  char *text = g_date_time_format(now, format);

  fprintf(file, "=== %s\n", text);
  fflush(file);
  g_free(text);
  g_date_time_unref(now);
}

/* Keeps the last LOG_RUNS - 1 runs (this one comes after them), fewer when they hold over
 * LOG_MAX_BYTES; a run starts at a line "=== <date> t-code ...". */
static void drop_old_runs(void)
{
  char *text = NULL;
  gsize size = 0;
  GArray *starts = g_array_new(FALSE, FALSE, sizeof(gsize));

  if (!g_file_get_contents(file_path, &text, &size, NULL)) {
    g_array_free(starts, TRUE);
    return;
  }
  for (gsize at = 0; at < size;) {
    if (g_str_has_prefix(text + at, "=== ") && !g_str_has_prefix(text + at, "=== quit"))
      g_array_append_val(starts, at);
    const char *end = memchr(text + at, '\n', size - at);
    at = end ? (gsize)(end - text) + 1 : size;
  }
  guint first = starts->len >= LOG_RUNS ? starts->len - (LOG_RUNS - 1) : 0;
  while (first < starts->len && size - g_array_index(starts, gsize, first) > LOG_MAX_BYTES)
    first++;
  gsize keep_from = first < starts->len ? g_array_index(starts, gsize, first) : size;
  if (keep_from > 0)
    g_file_set_contents(file_path, text + keep_from, (gssize)(size - keep_from), NULL);
  g_array_free(starts, TRUE);
  g_free(text);
}

static GLogWriterOutput write_log(GLogLevelFlags level, const GLogField *fields, gsize n, gpointer data G_GNUC_UNUSED)
{
  const char *domain = NULL, *message = NULL;

  for (gsize i = 0; i < n; i++)
    if (g_strcmp0(fields[i].key, "GLIB_DOMAIN") == 0)
      domain = fields[i].value;
    else if (g_strcmp0(fields[i].key, "MESSAGE") == 0)
      message = fields[i].value;
  if (message && ((level & (G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_WARNING)) ||
                  !g_log_writer_default_would_drop(level, domain))) {
    g_mutex_lock(&lock);
    if (file) {
      GDateTime *now = g_date_time_new_now_local();
      char *time = g_date_time_format(now, "%H:%M:%S");
      fprintf(file, "%s.%03d %s%s%s: %s\n", time, g_date_time_get_microsecond(now) / 1000, domain ? domain : "",
              domain ? "-" : "", level_name(level), message);
      fflush(file);
      g_free(time);
      g_date_time_unref(now);
    }
    g_mutex_unlock(&lock);
  }
  return g_log_writer_default(level, fields, n, NULL); /* stderr or the journal, and fatal levels */
}

void log_start(const char *path)
{
  file_path = path ? g_strdup(path) : log_default_path();
  char *folder = g_path_get_dirname(file_path);
  g_mkdir_with_parents(folder, 0700);
  g_free(folder);
  drop_old_runs();
  if ((file = g_fopen(file_path, "a"))) {
    char *format = g_strdup_printf("%%Y-%%m-%%d %%H:%%M:%%S t-code %s, pid %d", TCODE_VERSION, (int)getpid());
    write_mark(format);
    g_free(format);
  }
  g_log_set_writer_func(write_log, NULL, NULL);
}

void log_stop(void)
{
  g_mutex_lock(&lock);
  if (file) {
    write_mark("quit %H:%M:%S");
    fclose(file);
    file = NULL;
  }
  g_mutex_unlock(&lock);
}
