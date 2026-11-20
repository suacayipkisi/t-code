/*
 * The Plugins window ("Plugins: Manage Plugins"): every plugin found, and for the one picked its
 * version, API, where it was found, on / off, Start / Stop, its commands, permissions and what it
 * brings, the last lines it printed, Open Folder and Uninstall; Processes opens the Process Monitor.
 */
#pragma once

#include "app.h"

/* Opens the window, or brings it to the front. */
void show_plugins_window(App *t);
