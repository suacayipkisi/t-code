/*
 * The Keyboard Shortcuts window: every shortcut of the window, the tabs, the file manager and the
 * editor in one place.
 */
#pragma once

#include "app.h"

/* Opens the window, or brings it to the front. */
void show_keybinds(App *t);

/* Settings > Keyboard Shortcuts: every command with its keys, a filter over them. Clicking a
 * command's keys waits for new ones (Escape: as they were, Backspace: none); the arrow beside gives
 * the default back. */
GtkWidget *keybinds_editor(App *t);
