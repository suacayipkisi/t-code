/*
 * t-code's log file: GLib's and GTK's warnings and worse (everything with G_MESSAGES_DEBUG) are also
 * written, with the time, to ~/.local/state/t-code/t-code.log, for a t-code started from the
 * desktop's menu, where no terminal shows them. The file is made at the first message; one over
 * LOG_MAX_BYTES at start-up becomes t-code.log.1 (the one before goes). GLib-only.
 */
#pragma once

#include <glib.h>

#define LOG_MAX_BYTES (1024 * 1024)

/* ~/.local/state/t-code/t-code.log (free it). */
char *log_default_path(void);

/* From now on, messages also go to path (the default one when NULL); they still go to stderr or the
 * journal as before. The first one of this run is preceded by a line naming the version and pid. */
void log_start(const char *path);

/* The file messages go to, or NULL before log_start(). */
const char *log_path(void);
