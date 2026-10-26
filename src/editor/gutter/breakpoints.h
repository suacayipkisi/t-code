/*
 * Breakpoints in the editor: a red dot left of the line numbers, put and taken away by a click there
 * (a faint one shows under the pointer) or F9; a right click there asks for a condition ("i == 3":
 * it stops only when that is true). They move with the text as it is edited, and stay
 * for files that are not open (t->debug.breakpoints, kept in the session per project). While the
 * program is stopped, the line it stopped at has a yellow arrow and a yellow background.
 */
#pragma once

#include "app.h"

typedef struct Breakpoints Breakpoints;

/* One breakpoint. */
typedef struct {
  int line;        /* 1-based */
  char *condition; /* it stops only when this C expression is true ("i == 3"), or NULL: always */
} Breakpoint;

/* Puts the dots' column into tab's gutter (editorview.c). */
void breakpoints_attach(Tab *tab);

/* Frees it once the view is gone; NULL is fine. */
void breakpoints_free(Breakpoints *b);

/* The file was loaded (again): its breakpoints, and the stopped line, go into the text. */
void breakpoints_loaded(Tab *tab);

/* Puts a breakpoint on line (0-based), or takes it away. */
void breakpoint_toggle(Tab *tab, int line);

/* The same on the cursor's line (F9). */
void breakpoint_toggle_at_cursor(Tab *tab);

/* Sets the condition of the breakpoint on line (0-based), making one when there is none; NULL: it stops
 * always. A right click in the column asks for it. */
void breakpoint_set_condition(Tab *tab, int line, const char *condition);

/* The condition of the breakpoint on line (0-based), or NULL. */
const char *breakpoint_condition(Tab *tab, int line);

/* Takes the breakpoint on line (1-based) of path away, also when the file is not open. */
void breakpoint_remove(App *t, const char *path, int line);

/* The debugger put the breakpoint on line from (1-based) of path on line to (from has no code): the dot
 * moves there. */
void breakpoint_move(App *t, const char *path, int from, int to);

/* path's breakpoints (Breakpoint, ascending lines), or NULL when it has none. Owned by t. */
GArray *breakpoints_in(App *t, const char *path);

/* Takes every breakpoint away. */
void breakpoints_remove_all(App *t);

/* Every breakpoint as "line:path", for the session (free with g_strfreev); NULL when there are none.
 * *conditions gets their conditions in the same order ("" for none; free it too). */
char **breakpoints_save(App *t, char ***conditions);

/* Puts back what breakpoints_save() gave (another project's go); NULL: none. */
void breakpoints_restore(App *t, char **saved, char **conditions);

/* The program stopped at line (1-based) of path: the editors showing it mark it. path NULL: it runs
 * again (or ended), the mark goes. */
void breakpoints_show_stop(App *t, const char *path, int line);

/* The Disassembly View's instruction that runs next is on line (1-based; 0: none, it runs). */
void breakpoints_show_instruction(App *t, int line);

/* Draws the stopped line's background (guides.c, under the text). */
void breakpoints_draw(Tab *tab, GtkTextView *view, GtkSnapshot *snapshot);
