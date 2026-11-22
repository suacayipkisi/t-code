/*
 * What the GTK tests share (util.h stays GLib-only): finding a widget in a tree of them.
 */
#pragma once

#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* GTK's own frame clock: 4.24 warns about its frame timings now and then, and 4.25's frame throttling
 * asserts in its own timeout (no t-code code in either stack). */
static inline gboolean gtk_noise(const char *domain, const char *message)
{
  return g_strcmp0(domain, "Gdk") == 0 && message &&
         (strstr(message, "gdk_frame_timings_submitted") || strstr(message, "gdk_draw_context_frame_stop_throttling"));
}

/* GTK's noise is only printed; every other warning or critical still ends the test, as g_test_init
 * makes them. */
static void gtk_noise_check(GLogLevelFlags level, const char *domain, const char *message)
{
  if (gtk_noise(domain, message)) {
    g_printerr("# ignored: Gdk: %s\n", message);
    return;
  }
  g_printerr("%s%s%s: %s\n", domain ? domain : "", domain ? "-" : "",
             level & G_LOG_LEVEL_CRITICAL  ? "CRITICAL"
             : level & G_LOG_LEVEL_WARNING ? "WARNING"
                                           : "message",
             message ? message : "");
  if (level & (G_LOG_LEVEL_WARNING | G_LOG_LEVEL_CRITICAL)) {
    printf("Bail out! %s%s%s\n", domain ? domain : "", domain ? ": " : "", message ? message : "");
    fflush(stdout);
    abort();
  }
}

/* g_log() (GLib, GTK's g_return_if_fail): not on its way to the writer in a test. */
static void gtk_noise_handler(const char *domain, GLogLevelFlags level, const char *message, gpointer data)
{
  if (level & (G_LOG_LEVEL_WARNING | G_LOG_LEVEL_CRITICAL))
    gtk_noise_check(level, domain, message);
  else
    g_log_default_handler(domain, level, message, data);
}

/* Structured logging (g_log_structured, GDK's own warnings). */
static GLogWriterOutput gtk_noise_writer(GLogLevelFlags level, const GLogField *fields, gsize n, gpointer data)
{
  const char *domain = NULL, *message = NULL;

  if (!(level & (G_LOG_LEVEL_WARNING | G_LOG_LEVEL_CRITICAL)))
    return g_log_writer_default(level, fields, n, data);
  for (gsize i = 0; i < n; i++) {
    if (g_strcmp0(fields[i].key, "GLIB_DOMAIN") == 0)
      domain = fields[i].value;
    else if (g_strcmp0(fields[i].key, "MESSAGE") == 0)
      message = fields[i].value;
  }
  gtk_noise_check(level, domain, message);
  return G_LOG_WRITER_HANDLED;
}

/* Called once, after g_test_init and before GTK starts. GLib decides a message is fatal before any
 * handler sees it, and resets g_test_log_set_fatal_handler for each test case: so warnings and
 * criticals are not fatal to GLib, and gtk_noise_check ends the test for the others. */
static inline void ignore_gtk_noise(void)
{
  g_log_set_always_fatal(G_LOG_FATAL_MASK);
  g_log_set_default_handler(gtk_noise_handler, NULL);
  g_log_set_writer_func(gtk_noise_writer, NULL, NULL);
}
