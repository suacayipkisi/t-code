/*
 * Every popover menu as it shows (right-click menus, the menu bar's, GTK's own text menus): the often used
 * items get t-code's icons by their label, and the menu asks for its whole height so it scrolls only when
 * it does not fit the screen (GTK shrank it to the room under the pointer and scrolled even with room
 * above it).
 */
#pragma once

#include <gtk/gtk.h>

/* Watches every GtkPopoverMenu shown from now on. Once, before the first window. */
void menus_init(void);

/* The icon name for a menu item's label ("Copy Path": "tcode-copy-symbolic"), or NULL. */
const char *menu_icon_for(const char *label);
