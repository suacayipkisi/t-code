/*
 * The Disassembly View: the paused function's machine instructions with its source lines
 * between them, in a read-only editor tab beside the source ("Debug: Open Disassembly View", the Debug
 * panel's ASM button). A yellow arrow marks the instruction that runs next; while that tab is the
 * current one, F10 / F11 go one instruction at a time; a click left of an instruction puts a
 * breakpoint on it.
 */
#pragma once

#include "app.h"

/* Opens the view beside the source (the program must be paused). */
void disassembly_open(App *t);

/* The program paused (or another frame was chosen): the view, if open, shows the shown frame's
 * function. */
void disassembly_update(App *t);

/* Debugging ended: the instruction breakpoints are forgotten (another run has other addresses). */
void disassembly_ended(App *t);

/* Whether the current tab is the view (the steps go by instruction). */
gboolean disassembly_shown(App *t);

/* Whether tab is the view; the breakpoints' column (editor/gutter/breakpoints.c) asks. */
gboolean disassembly_is(App *t, Tab *tab);

/* Whether the instruction on line (0-based) of the view has a breakpoint, and putting one there or
 * taking it away. */
gboolean disassembly_breakpoint_at(App *t, int line);
void disassembly_toggle_breakpoint(App *t, int line);
