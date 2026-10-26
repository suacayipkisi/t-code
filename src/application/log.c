/*
 * The log file (see log.h).
 */
#define _DEFAULT_SOURCE /* getpid() under -std=c23 with musl */

#include "application/log.h"

#include <glib/gstdio.h>
#include <stdio.h>
#include <unistd.h>

#ifndef TCODE_VERSION /* set by the Makefile */
#define TCODE_VERSION "unknown"
#endif

static char *file_path;
static FILE *file;  /* opened at the first message */
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

/* Opens the file, with a line for this run; FALSE when it cannot be written. */
static gboolean open_file(void)
{
  char *folder = g_path_get_dirname(file_path);

  g_mkdir_with_parents(folder, 0700);
  g_free(folder);
  if (!(file = g_fopen(file_path, "a")))
    return FALSE;
  GDateTime *now = g_date_time_new_now_local();
  char *date = g_date_time_format(now, "%Y-%m-%d %H:%M:%S");
  fprintf(file, "=== %s t-code %s, pid %d\n", date, TCODE_VERSION, (int)getpid());
  g_free(date);
  g_date_time_unref(now);
  return TRUE;
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
    if (file || open_file()) {
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
  GStatBuf st;

  file_path = path ? g_strdup(path) : log_default_path();
  if (g_stat(file_path, &st) == 0 && st.st_size > LOG_MAX_BYTES) {
    char *old = g_strconcat(file_path, ".1", NULL);
    g_rename(file_path, old);
    g_free(old);
  }
  g_log_set_writer_func(write_log, NULL, NULL);
}
