/*
 * t-code's log file, ~/.local/state/t-code/t-code.log: a line for each start ("=== <date> t-code
 * <version>, pid <n>") and one for each clean quit ("=== quit"), so a run without the second one
 * crashed or was killed; GLib's and GTK's warnings and worse (everything with G_MESSAGES_DEBUG) in
 * between, each with its time. For a t-code started from the desktop's menu, where no terminal shows
 * them, and for problems noticed only later. The last LOG_RUNS starts are kept, and fewer when they
 * hold over LOG_MAX_BYTES. GLib-only.
 */
#pragma once

#include <glib.h>

#define LOG_RUNS 50
#define LOG_MAX_BYTES (4 * 1024 * 1024)

/* ~/.local/state/t-code/t-code.log (free it). */
char *log_default_path(void);

/* Drops the runs over the limits, writes this run's line, and from now on messages also go to path
 * (the default one when NULL); they still go to stderr or the journal as before. */
void log_start(const char *path);

/* The clean quit's line; the file is closed. */
void log_stop(void);

/* The file messages go to, or NULL before log_start(). */
const char *log_path(void);
