/*
 * The end of what a program writes to stderr (GLib/GIO-only): read to its end in the background,
 * keeping only the last ERR_TAIL_BYTES, so that when a language server or a debugger stops on its own,
 * the log can say why ("rust-analyzer is not installed for the toolchain", a Node.js stack trace)
 * instead of nothing. Bounded memory however much the program writes (clangd logs a lot).
 */
#pragma once

#include <gio/gio.h>

#define ERR_TAIL_BYTES 4096

typedef struct ErrTail ErrTail;

/* text: the last lines, without surrounding white space ("" when nothing came). */
typedef void (*ErrTailDone)(const char *text, gpointer data);

/* Starts reading in (a program's stderr pipe; a reference is taken). */
ErrTail *err_tail_new(GInputStream *in);

/* What came so far, trimmed (free it). */
char *err_tail_text(ErrTail *t);

/* The program went: done gets the text once its stderr ends, or after wait_ms when something it
 * started keeps the pipe open. The caller's hold ends here: t frees itself after done. */
void err_tail_finish(ErrTail *t, guint wait_ms, ErrTailDone done, gpointer data);

/* Stops reading and drops the caller's hold, with no report (the program was stopped on purpose). */
void err_tail_free(ErrTail *t);

/* The last non-empty line of text, or text itself; for one-line messages (free it). */
char *err_tail_last_line(const char *text);
