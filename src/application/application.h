/*
 * t-code's start and end: the GTK application (application.c) and its command line (cli.c). The window
 * itself is built by build_window() (window/window.c); see app.h for how the source folders divide
 * the work.
 */
#pragma once

#include <gio/gio.h>

/* Runs t-code: parses the command line, opens the window, runs the main loop until it closes, and
 * returns the exit status (main() returns it). */
int application_run(int argc, char **argv);

/* cli.c: adds t-code [dir] [file...]'s options (--version, --new-window, --send) to app. --version
 * and --send are answered before GTK starts, so they work without a display. */
void cli_add_options(GApplication *app);

/* Whether --new-window was given (the desktop file's "New Window" action, e.g. a middle click on the
 * dock): an empty window that asks for its folder, instead of the last project, which may be open in
 * another one. */
gboolean cli_new_window(void);
