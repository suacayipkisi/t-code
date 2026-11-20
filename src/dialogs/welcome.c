/*
 * The welcome window (see welcome.h): t-code's name and version, Open Folder, the home folder and a
 * few keys on the left; the recent folders on the right, or a line saying how they get there.
 */
#include "dialogs/welcome.h"

#include "application/identity.h"

#include <string.h>

#ifndef TCODE_VERSION /* set by the Makefile */
#define TCODE_VERSION "unknown"
#endif

#define WELCOME_WIDTH 820
#define WELCOME_HEIGHT 520

typedef struct {
  GtkWindow *window;
  WelcomeChosen chosen; /* NULL once a folder was picked */
  gpointer data;
  gboolean closed; /* the window is gone (a folder dialog may still answer) */
} Welcome;

/* A folder was picked: the caller opens it, then this window goes. */
static void pick(Welcome *w, const char *folder)
{
  WelcomeChosen chosen = w->chosen;

  if (!chosen || w->closed)
    return;
  w->chosen = NULL;
  chosen(folder, w->data);
  gtk_window_destroy(w->window);
}

static void on_destroy(GtkWidget *window G_GNUC_UNUSED, Welcome *w)
{
  w->closed = TRUE;
}

/* path with the home folder written as "~". */
static char *home_relative(const char *path)
{
  const char *home = g_get_home_dir();
  size_t n = strlen(home);

  if (strncmp(path, home, n) == 0 && (path[n] == '/' || path[n] == '\0'))
    return g_strconcat("~", path + n, NULL);
  return g_strdup(path);
}

/* Open Folder… --------------------------------------------------------------------------------- */

static void on_folder_chosen(GObject *source, GAsyncResult *result, gpointer data)
{
  GtkWindow *window = data; /* held until the dialog answers */
  Welcome *w = g_object_get_data(G_OBJECT(window), "t-code-welcome");
  GFile *folder = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(source), result, NULL);

  if (folder) {
    char *path = g_file_get_path(folder);
    if (path)
      pick(w, path);
    g_free(path);
    g_object_unref(folder);
  }
  g_object_unref(window);
}

static void on_open_folder(GtkButton *button G_GNUC_UNUSED, Welcome *w)
{
  GtkFileDialog *dialog = gtk_file_dialog_new();
  GFile *start = g_file_new_for_path(g_get_home_dir());

  gtk_file_dialog_set_title(dialog, "Open Folder");
  gtk_file_dialog_set_initial_folder(dialog, start);
  gtk_file_dialog_select_folder(dialog, w->window, NULL, on_folder_chosen, g_object_ref(w->window));
  g_object_unref(start);
  g_object_unref(dialog);
}

static void on_home(GtkButton *button G_GNUC_UNUSED, Welcome *w)
{
  pick(w, g_get_home_dir());
}

static void on_recent_activated(GtkListBox *box G_GNUC_UNUSED, GtkListBoxRow *row, Welcome *w)
{
  pick(w, g_object_get_data(G_OBJECT(row), "t-code-folder"));
}

/* Building it ---------------------------------------------------------------------------------- */

static GtkWidget *label_with(const char *text, const char *css_class, float xalign)
{
  GtkWidget *label = gtk_label_new(text);

  if (css_class)
    gtk_widget_add_css_class(label, css_class);
  gtk_label_set_xalign(GTK_LABEL(label), xalign);
  return label;
}

/* A row of the recent folders: the folder's name over its path. */
static GtkWidget *recent_row(const char *folder)
{
  GtkWidget *row = gtk_list_box_row_new(), *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  char *name = g_path_get_basename(folder), *where = home_relative(folder);
  GtkWidget *path = label_with(where, "dim-label", 0);

  gtk_label_set_ellipsize(GTK_LABEL(path), PANGO_ELLIPSIZE_MIDDLE);
  gtk_widget_set_margin_top(box, 8);
  gtk_widget_set_margin_bottom(box, 8);
  gtk_widget_set_margin_start(box, 12);
  gtk_widget_set_margin_end(box, 12);
  gtk_box_append(GTK_BOX(box), label_with(name, "heading", 0));
  gtk_box_append(GTK_BOX(box), path);
  gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
  gtk_widget_set_tooltip_text(row, folder);
  g_object_set_data_full(G_OBJECT(row), "t-code-folder", g_strdup(folder), g_free);
  g_free(name);
  g_free(where);
  return row;
}

/* The right side: the recent folders that still exist, or how they get there. */
static GtkWidget *recent_side(const char *const *recent, Welcome *w)
{
  GtkWidget *side = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  GtkWidget *list = gtk_list_box_new(), *scrolled, *frame;
  int shown = 0;

  gtk_box_append(GTK_BOX(side), label_with("Recent Folders", "title-4", 0));
  gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(list), TRUE);
  g_signal_connect(list, "row-activated", G_CALLBACK(on_recent_activated), w);
  for (int i = 0; recent && recent[i]; i++)
    if (g_file_test(recent[i], G_FILE_TEST_IS_DIR)) {
      gtk_list_box_append(GTK_LIST_BOX(list), recent_row(recent[i]));
      shown++;
    }
  if (!shown) {
    GtkWidget *none = label_with("No recent folders yet.\nOpen a folder to start: t-code remembers it and opens it "
                                 "again next time.",
                                 "dim-label", 0.5);
    gtk_label_set_wrap(GTK_LABEL(none), TRUE);
    gtk_label_set_justify(GTK_LABEL(none), GTK_JUSTIFY_CENTER);
    gtk_widget_set_vexpand(none, TRUE);
    gtk_widget_set_margin_start(none, 24);
    gtk_widget_set_margin_end(none, 24);
    gtk_box_append(GTK_BOX(side), none);
    g_object_unref(g_object_ref_sink(list));
    return side;
  }
  scrolled = gtk_scrolled_window_new();
  frame = gtk_frame_new(NULL);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), list);
  gtk_frame_set_child(GTK_FRAME(frame), scrolled);
  gtk_widget_set_vexpand(frame, TRUE);
  gtk_box_append(GTK_BOX(side), frame);
  return side;
}

/* The left side: the name, the two buttons, a few keys. */
static GtkWidget *start_side(Welcome *w, GtkWidget **open_button)
{
  /* clang-format off */
  static const char *const keys[][2] = {
    { "Ctrl+Shift+O", "Open another folder" },
    { "Ctrl+P",       "Go to a file" },
    { "Ctrl+Shift+P", "All commands" },
  };
  /* clang-format on */
  GtkWidget *side = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8), *icon = gtk_image_new_from_icon_name(TCODE_APP_ID);
  GtkWidget *open = gtk_button_new_with_mnemonic("_Open Folder…"),
            *home = gtk_button_new_with_mnemonic("Start in _Home Folder");
  GtkWidget *grid = gtk_grid_new();

  gtk_image_set_pixel_size(GTK_IMAGE(icon), 96);
  gtk_widget_set_halign(icon, GTK_ALIGN_START);
  gtk_box_append(GTK_BOX(side), icon);
  gtk_box_append(GTK_BOX(side), label_with(TCODE_DISPLAY_NAME, "title-1", 0));
  gtk_box_append(GTK_BOX(side), label_with("A lightweight C/C++ IDE", NULL, 0));
  gtk_box_append(GTK_BOX(side), label_with("Version " TCODE_VERSION, "dim-label", 0));

  gtk_widget_add_css_class(open, "suggested-action");
  gtk_widget_set_margin_top(open, 24);
  g_signal_connect(open, "clicked", G_CALLBACK(on_open_folder), w);
  gtk_box_append(GTK_BOX(side), open);
  gtk_widget_add_css_class(home, "flat");
  g_signal_connect(home, "clicked", G_CALLBACK(on_home), w);
  gtk_box_append(GTK_BOX(side), home);

  gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
  gtk_grid_set_column_spacing(GTK_GRID(grid), 16);
  gtk_widget_set_valign(grid, GTK_ALIGN_END);
  gtk_widget_set_vexpand(grid, TRUE);
  for (guint i = 0; i < G_N_ELEMENTS(keys); i++) {
    gtk_grid_attach(GTK_GRID(grid), label_with(keys[i][0], "monospace", 0), 0, (int)i, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), label_with(keys[i][1], "dim-label", 0), 1, (int)i, 1, 1);
  }
  gtk_box_append(GTK_BOX(side), grid);
  *open_button = open;
  return side;
}

GtkWindow *show_welcome(GtkApplication *app, const char *const *recent, WelcomeChosen chosen, gpointer data)
{
  Welcome *w = g_new0(Welcome, 1);
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 32), *open = NULL;
  GtkWidget *left, *right;

  w->window = GTK_WINDOW(gtk_window_new());
  w->chosen = chosen;
  w->data = data;
  if (app)
    gtk_window_set_application(w->window, app);
  g_object_set_data_full(G_OBJECT(w->window), "t-code-welcome", w, g_free);
  g_signal_connect(w->window, "destroy", G_CALLBACK(on_destroy), w);
  gtk_window_set_title(w->window, "Welcome to " TCODE_DISPLAY_NAME);
  gtk_window_set_icon_name(w->window, TCODE_APP_ID);
  gtk_window_set_default_size(w->window, WELCOME_WIDTH, WELCOME_HEIGHT);

  left = start_side(w, &open);
  right = recent_side(recent, w);
  gtk_widget_set_size_request(left, 280, -1);
  gtk_widget_set_hexpand(right, TRUE);
  gtk_box_append(GTK_BOX(box), left);
  gtk_box_append(GTK_BOX(box), right);
  gtk_widget_set_margin_top(box, 32);
  gtk_widget_set_margin_bottom(box, 32);
  gtk_widget_set_margin_start(box, 32);
  gtk_widget_set_margin_end(box, 32);
  gtk_window_set_child(w->window, box);
  gtk_window_set_default_widget(w->window, open);
  gtk_window_present(w->window);
  gtk_widget_grab_focus(open);
  return w->window;
}
