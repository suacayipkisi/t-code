/*
 * The "No LSP" window (see lspinstall.h).
 */
#include "dialogs/lspinstall.h"

#include "lsp/client.h"
#include "lsp/install.h"

#define INSTALL_WIDTH 560

static GtkWidget *window; /* the one open, or NULL */

static void on_copy(GtkButton *button, gpointer data)
{
  gdk_clipboard_set_text(gtk_widget_get_clipboard(GTK_WIDGET(button)), data);
  gtk_button_set_label(button, "Copied");
}

static void on_try_again(App *t)
{
  gtk_window_close(GTK_WINDOW(window));
  lsp_open_installed(t);
}

static GtkWidget *wrapped_label(const char *text)
{
  GtkWidget *label = gtk_label_new(text);

  gtk_label_set_wrap(GTK_LABEL(label), TRUE);
  gtk_label_set_xalign(GTK_LABEL(label), 0);
  return label;
}

/* One command, selectable, with a Copy button beside it. */
static GtkWidget *command_row(const char *command)
{
  GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *label = wrapped_label(command), *copy = gtk_button_new_with_label("Copy");
  char *text = g_strdup(command);

  gtk_label_set_selectable(GTK_LABEL(label), TRUE);
  gtk_label_set_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
  gtk_widget_add_css_class(label, "monospace");
  gtk_widget_set_hexpand(label, TRUE);
  gtk_widget_set_valign(copy, GTK_ALIGN_CENTER);
  g_signal_connect_data(copy, "clicked", G_CALLBACK(on_copy), text, (GClosureNotify)(void (*)(void))g_free, 0);
  gtk_box_append(GTK_BOX(row), label);
  gtk_box_append(GTK_BOX(row), copy);
  return row;
}

void show_lsp_install(App *t, const char *server)
{
  const char *title = install_title(server), *note = install_note(server);
  char *os_release = install_os_release();
  GStrv commands = install_commands(server, install_distro(os_release));
  GtkWidget *box, *buttons, *close, *again, *heading;
  GtkEventController *keys;
  char *text;

  if (!title || !commands) {
    g_strfreev(commands);
    g_free(os_release);
    return;
  }
  if (window)
    gtk_window_destroy(GTK_WINDOW(window));
  window = gtk_window_new();
  g_object_add_weak_pointer(G_OBJECT(window), (gpointer *)&window);
  text = g_strdup_printf("Install %s", server);
  gtk_window_set_title(GTK_WINDOW(window), text);
  g_free(text);
  gtk_window_set_transient_for(GTK_WINDOW(window), t->window);
  gtk_window_set_modal(GTK_WINDOW(window), TRUE);
  gtk_window_set_default_size(GTK_WINDOW(window), INSTALL_WIDTH, -1);
  keys = GTK_EVENT_CONTROLLER(gtk_shortcut_controller_new());
  gtk_shortcut_controller_add_shortcut(
      GTK_SHORTCUT_CONTROLLER(keys),
      gtk_shortcut_new(gtk_keyval_trigger_new(GDK_KEY_Escape, 0), gtk_named_action_new("window.close")));
  gtk_widget_add_controller(window, keys);

  box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
  gtk_widget_set_margin_top(box, 18);
  gtk_widget_set_margin_bottom(box, 18);
  gtk_widget_set_margin_start(box, 18);
  gtk_widget_set_margin_end(box, 18);
  text = g_strdup_printf("%c%s is not installed", g_ascii_toupper(title[0]), title + 1);
  heading = wrapped_label(text);
  g_free(text);
  gtk_widget_add_css_class(heading, "heading");
  gtk_box_append(GTK_BOX(box), heading);
  gtk_box_append(GTK_BOX(box),
                 wrapped_label("Without it this file has no completion, errors and warnings or go to definition. "
                               "Install it with one of these commands in a terminal, then click Try Again."));
  for (int i = 0; commands[i]; i++)
    gtk_box_append(GTK_BOX(box), command_row(commands[i]));
  if (note) {
    GtkWidget *label = wrapped_label(note);
    gtk_widget_add_css_class(label, "dim-label");
    gtk_box_append(GTK_BOX(box), label);
  }

  buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign(buttons, GTK_ALIGN_END);
  gtk_widget_set_margin_top(buttons, 8);
  close = gtk_button_new_with_label("Close");
  again = gtk_button_new_with_label("Try Again");
  gtk_widget_add_css_class(again, "suggested-action");
  g_signal_connect_swapped(close, "clicked", G_CALLBACK(gtk_window_close), window);
  g_signal_connect_swapped(again, "clicked", G_CALLBACK(on_try_again), t);
  gtk_box_append(GTK_BOX(buttons), close);
  gtk_box_append(GTK_BOX(buttons), again);
  gtk_box_append(GTK_BOX(box), buttons);

  gtk_window_set_child(GTK_WINDOW(window), box);
  gtk_window_present(GTK_WINDOW(window));
  g_strfreev(commands);
  g_free(os_release);
}
