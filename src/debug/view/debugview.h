/*
 * The Run and Debug panel, while a program is paused:
 *   Variables   the paused function's arguments and local variables with their values; arrays,
 *               structs and pointers open like folders; a value that changed since the last pause is
 *               colored
 *   Watch       expressions evaluated at each pause ("a[i]", "sum / n"): "+" adds one
 *   Call Stack  how the program got there: the paused function, the one that called it, and so on
 *               up to main; a click shows that function's line and its variables
 *   Breakpoints every breakpoint with its condition; a click shows it, x takes it away
 */
#pragma once

#include "app.h"

#include <json-glib/json-glib.h>

/* The panel (window.c docks it, on the left by default). */
GtkWidget *debug_view(App *t);

/* The program paused: frames is the stackTrace answer's stackFrames, shown the index of the frame
 * the editor shows (its variables are listed). */
void debug_view_paused(App *t, JsonNode *frames, int shown);

/* It runs again: the variables are not known now. */
void debug_view_running(App *t);

/* Debugging ended. */
void debug_view_ended(App *t);

/* The breakpoints var_changed (debug_breakpoints_changed): the list follows. */
void debug_view_breakpoints_changed(App *t);

/* Adds expression to the Watch ("Debug: Add to Watch"). */
void debug_view_add_watch(App *t, const char *expression);

/* The watch expressions, for the session (free with g_strfreev), and putting them back. */
char **debug_view_watches(void);
void debug_view_set_watches(char **expressions);

/* The id of the frame whose variables show (gdb's frameId, for evaluating in it), or -1 when the
 * program is not paused. */
gint64 debug_view_frame_id(void);

/* What the lists show, a line each (for the tests): the variables and the watch as "name = value",
 * "*" after a changed one; the call stack's rows as "function file:line". */
char *debug_view_variables_text(void);
char *debug_view_watch_text(void);
char *debug_view_stack_text(void);
