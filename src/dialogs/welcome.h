/*
 * The welcome window: shown at start instead of the main window when there is no folder to open (the
 * first start, or the last project is gone, or reopening it is turned off in Settings). It offers
 * Open Folder, the recent folders that still exist, and the home folder; the main window is built
 * only once a folder is chosen.
 */
#pragma once

#include <gtk/gtk.h>

/* A folder was chosen in the welcome window; the window closes right after this returns. */
typedef void (*WelcomeChosen)(const char *folder, gpointer data);

/* Shows the welcome window (as app's window, when app is not NULL). recent: the recent folders, most
 * recent first (NULL or empty: none); missing folders are left out. chosen(folder, data) runs once,
 * when the user picks one; closing the window instead picks nothing. Returns the window. */
GtkWindow *show_welcome(GtkApplication *app, const char *const *recent, WelcomeChosen chosen, gpointer data);
