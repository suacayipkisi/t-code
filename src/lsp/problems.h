/*
 * The Problems panel: lists diagnostics (errors, warnings, notes) across open files,
 * with severity badges, message, and clickable file:line:col navigation.
 */
#pragma once

#include "app.h"

/* Creates the Problems panel widget (dock page "problems"). */
GtkWidget *problems_panel(App *t);

/* Refreshes the Problems panel with current diagnostics across all open editor tabs, once the main
 * loop is idle (a burst of calls makes one rebuild). */
void problems_update(App *t);

/* Focuses the problems list. */
void problems_focus(App *t);
