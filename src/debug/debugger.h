/*
 * Debugging with gdb: F5 builds the project in the shell and starts the program under
 * gdb, which stops it at the breakpoints (editor/gutter/breakpoints.c) or where it crashes; the editor
 * then shows that line, and F10 / F11 / Shift+F11 go on one step at a time, F5 to the next stop.
 * The Debug panel holds the buttons for all that over a terminal the program reads and writes.
 */
#pragma once

#include "app.h"

/* The Debug panel: its buttons over the program's terminal (window.c docks it). */
GtkWidget *debug_panel(App *t);

/* Whether a program is being debugged (the step keys work only then). */
gboolean debug_active(App *t);

/* A line of t-code's own in the program's terminal, in red: why something cannot be done. */
void debug_say(App *t, const char *text);

/* gdb, for the Run and Debug panel's requests; NULL when not debugging. */
struct Dap *debug_dap(App *t);

/* F5: builds the project and starts debugging it; while the program is stopped, it goes on. */
void debug_start_or_continue(App *t);

/* F6: stops the running program where it is. */
void debug_pause(App *t);

/* F10, F11, Shift+F11: one line on, into the function called on the line, out of the function. */
void debug_step_over(App *t);
void debug_step_into(App *t);
void debug_step_out(App *t);

/* Shift+F5: ends the program and the debugging. */
void debug_stop(App *t);

/* Ctrl+Shift+F5: ends it, then builds and starts again. */
void debug_restart(App *t);

/* Asks which program F5 debugs in a project built by a Makefile (it is remembered). */
void debug_select_program(App *t);

/* The build for debugging is done (the control socket's "debug <path>", project/run.c): path is
 * the program, or the folder of a Makefile, whose program is asked for once. */
void debug_launch(App *t, const char *path);

/* The build for debugging failed (the control socket's "debug-failed <dir>"). */
void debug_build_failed(App *t);

/* The breakpoints in path changed (editor/gutter/breakpoints.c): gdb hears of them. */
void debug_breakpoints_changed(App *t, const char *path);

/* Ends debugging at once, without asking gdb (closing t-code, another project). */
void debug_end(App *t);
