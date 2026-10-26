/*
 * What the GTK tests share (util.h stays GLib-only): finding a widget in a tree of them.
 */
#pragma once

#include <gtk/gtk.h>

/* The first widget at or under root (in order) of type (any for G_TYPE_NONE), with the tooltip and
 * the CSS class when they are given (not NULL); NULL when there is none. */
static inline GtkWidget *find_widget(GtkWidget *root, GType type, const char *tooltip, const char *css_class)
{
  if (!root)
    return NULL;
  if ((type == G_TYPE_NONE || G_TYPE_CHECK_INSTANCE_TYPE(root, type)) &&
      (!tooltip || g_strcmp0(gtk_widget_get_tooltip_text(root), tooltip) == 0) &&
      (!css_class || gtk_widget_has_css_class(root, css_class)))
    return root;
  for (GtkWidget *child = gtk_widget_get_first_child(root); child; child = gtk_widget_get_next_sibling(child)) {
    GtkWidget *found = find_widget(child, type, tooltip, css_class);
    if (found)
      return found;
  }
  return NULL;
}
