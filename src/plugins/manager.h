/*
 * The Plugins window ("Plugins: Manage Plugins", manager.c): every plugin found, and for the one
 * picked its name, description, on / off, Start / Stop, its commands, permissions and what it brings,
 * its facts (id, version, API, folder, registry) and the last lines it printed under two disclosures,
 * Open Folder and Uninstall; Browse lists the registry. And the Plugins panel (panel.c, the docks'
 * "plugins"): each plugin with its state and its switch, a click opens it in the window.
 */
#pragma once

#include "app.h"
#include "plugins/plugins.h"

/* Opens the window, or brings it to the front. */
void show_plugins_window(App *t);

/* The same, showing the plugin id (NULL: the one shown already) or, with browse, the registry. */
void show_plugins_window_at(App *t, const char *id, gboolean browse);

/* What a plugin is doing, in a word or two: "off", "on", "running", "stopped", "failed", "not started". */
const char *plugin_state_text(Plugin *p);

/* A "●" in p's state's color: green running, red failed, a dim "○" off. */
GtkWidget *plugin_state_dot(Plugin *p);

/* Turns p on or off from a switch over parent. An unreviewed plugin's first turn on asks first: FALSE
 * then (it stays off until the answer, and a yes turns it on). */
gboolean plugins_switch(App *t, Plugin *p, gboolean on, GtkWindow *parent);

/* panel.c: the Plugins panel, one per process. */
GtkWidget *plugins_panel(App *t);
