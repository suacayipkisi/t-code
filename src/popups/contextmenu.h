/*
 * Right-click menus: a list of items shown as a popover menu where the pointer is. The chosen
 * item runs once the menu has closed, so it may close what the menu was opened on (a tab, a row).
 */
#pragma once

#include "app.h"

typedef struct {
  const char *label; /* NULL: a line between groups of items */
  void (*run)(gpointer data);
} MenuItem;

/* Shows items (n of them) at x, y of widget; the chosen one gets data. free_data (may be NULL)
 * frees data when the menu has gone, chosen or not. */
void context_menu(GtkWidget *widget, double x, double y, const MenuItem *items, int n, gpointer data,
                  GDestroyNotify free_data);

/* A right click on widget calls show (which calls context_menu) with where it was. */
typedef void (*ContextMenuShow)(GtkWidget *widget, double x, double y, gpointer data);
void context_menu_attach(GtkWidget *widget, ContextMenuShow show, gpointer data);

/* Puts text on the clipboard. */
void copy_to_clipboard(const char *text);
