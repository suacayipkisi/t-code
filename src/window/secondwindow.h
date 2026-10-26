/*
 * The second window ("Open in New Window"): another window of the same t-code, holding a third
 * tab group (t->editors.groups[WINDOW_GROUP]). It shares everything else with the main window: the
 * project, language servers and the session. A file is still open in one tab only: opening it here moves
 * its tab. Closing the window moves its tabs back to the main window; it also closes when its last
 * tab goes.
 */
#pragma once

#include "app.h"

/* Opens the second window (empty) unless it is open. */
void show_second_window(App *t);

/* Opens path in the second window (made first when there is none), or moves its tab there. */
void open_in_new_window(App *t, const char *path);

/* Moves the current tab into the second window. */
void move_to_new_window(App *t);
