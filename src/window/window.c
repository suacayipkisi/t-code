/*
 * The main window (see window.h for the layout): building it, its text sizes, and closing it. Its
 * title bar is in titlebar.c, the docks around the editors in docks/.
 */
#include "window/window.h"

#include "application/identity.h"
#include "editor/editorview.h"
#include "editor/preview/preview.h"
#include "editor/editing/snippets.h"
#include "editor/tabs.h"
#include "files/files.h"
#include "ipc/socket.h"
#include "popups/picker.h"
#include "project/project.h"
#include "project/session.h"
#include "project/userfiles.h"
#include "lsp/inline/inlay.h"
#include "update/update.h"
#include "window/commands.h"
#include "window/docks/docks.h"
#include "window/paths.h"
#include "window/statusbar.h"
#include "window/terminal.h"
#include "window/theme.h"
#include "plugins/plugins.h"

#include <string.h>

void set_font_scale(App *t, double scale)
{
  t->settings.font_scale = CLAMP(scale, 0.5, 3.0);
  if (fabs(t->settings.font_scale - 1.0) < 0.005)
    t->settings.font_scale = 1.0;
  vte_terminal_set_font_scale(t->shell.term, t->settings.font_scale);
  if (t->debug.term)
    vte_terminal_set_font_scale(t->debug.term, t->settings.font_scale);
  editor_view_set_font(t);
  files_apply_font(t);
  status_show_font_scale(t);
}

void set_preview_zoom_scale(App *t, double scale)
{
  int n = tab_count(t);

  t->settings.preview_scale = CLAMP(scale, 0.5, 3.0);
  for (int i = 0; i < n; i++)
    if (tab_nth(t, i)->kind == TAB_PREVIEW)
      preview_set_zoom(tab_nth(t, i), t->settings.preview_scale);
}

/* Closing ------------------------------------------------------------------------------------ */

/* The window goes: the menu's actions with it. */
static void on_window_destroy(GtkWidget *window G_GNUC_UNUSED, App *t)
{
  g_clear_handle_id(&t->dock.checks_id, g_source_remove);
  g_clear_handle_id(&t->status.say_id, g_source_remove); /* its label goes with the window */
  g_clear_handle_id(&t->editors.peek_id, g_source_remove);
  g_clear_handle_id(&t->status.progress_id, g_source_remove);
  plugins_stop(t);
}

/* The window's close button, once nothing unsaved is left (or the user said to drop it): the
 * session is saved, then the window goes; its tabs stop their editors on the way. */
static void close_window(App *t, const char *arg G_GNUC_UNUSED)
{
  session_save(t);
  t->closing = TRUE;
  if (t->editors.second) /* its tabs go with it, as the main window's do */
    gtk_window_destroy(t->editors.second);
  gtk_window_destroy(t->window);
}

/* The window manager asks to close: TRUE keeps the window until unless_unsaved() has asked about
 * unsaved changes. (Autosave leaves little: a second's worth of typing.) */
static gboolean on_close_request(GtkWindow *window G_GNUC_UNUSED, App *t)
{
  if (t->closing)
    return FALSE;
  unless_unsaved(t, "Close t-code?", "Close", close_window, NULL);
  return TRUE;
}

/* Panes ------------------------------------------------------------------------------------ */

/* When the user drags a paned handle all the way to the edge (< 24px), collapse that pane cleanly. */
static void on_hpaned_position(GtkPaned *paned, GParamSpec *spec G_GNUC_UNUSED, App *t)
{
  int pos = gtk_paned_get_position(paned);

  if (pos > 0 && pos < 24 && gtk_widget_get_visible(t->layout.left)) {
    gtk_paned_set_position(paned, 280);
    set_pane_visible(t, t->layout.left, FALSE);
  }
}

static void on_rpaned_position(GtkPaned *paned, GParamSpec *spec G_GNUC_UNUSED, App *t)
{
  int w = gtk_widget_get_width(GTK_WIDGET(paned));
  int pos = gtk_paned_get_position(paned);

  if (w > 320 && pos > w - 24 && gtk_widget_get_visible(t->layout.right)) {
    gtk_paned_set_position(paned, w - 280);
    set_pane_visible(t, t->layout.right, FALSE);
  }
}

static void on_vpaned_position(GtkPaned *paned, GParamSpec *spec G_GNUC_UNUSED, App *t)
{
  int h = gtk_widget_get_height(GTK_WIDGET(paned));
  int pos = gtk_paned_get_position(paned);

  if (h > 200 && pos > h - 24 && gtk_widget_get_visible(t->layout.bottom)) {
    gtk_paned_set_position(paned, h - 240);
    set_pane_visible(t, t->layout.bottom, FALSE);
  }
}

/* The window --------------------------------------------------------------------------------- */

void build_window(App *t, GtkApplication *app)
{
  GtkWidget *overlay = gtk_overlay_new();                        /* the picker (Ctrl+P) floats over the content */
  GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0); /* the panes, the status bar */

  /* Before the window: the theme, where the helpers are, and the sockets they connect to. */
  load_theme(t);
  watch_user_files(t); /* settings.ini, theme.css and the editor colors, edited by hand */
  gtk_source_init();
  find_files(t);
  snippets_init(t);
  start_socket(t);
  plugins_init(t);
  gtk_icon_theme_add_search_path(gtk_icon_theme_get_for_display(gdk_display_get_default()), t->paths.icons);
  gtk_window_set_default_icon_name(TCODE_APP_ID);

  t->window = GTK_WINDOW(gtk_application_window_new(app));
  inlay_peek_keys(t, t->window);
  gtk_window_set_default_size(t->window, 1400, 900);
  add_project_actions(t);
  gtk_window_set_titlebar(t->window, title_bar(t));
  g_signal_connect(t->window, "close-request", G_CALLBACK(on_close_request), t);
  g_signal_connect(t->window, "destroy", G_CALLBACK(on_window_destroy), t);
  GSimpleAction *reset = g_simple_action_new("reset-panels", NULL);
  g_signal_connect_swapped(reset, "activate", G_CALLBACK(reset_panel_layout), t);
  g_action_map_add_action(G_ACTION_MAP(t->window), G_ACTION(reset));
  g_object_unref(reset);
  update_title(t);

  create_shell_pane(t);
  create_groups(t);
  docks_create(t);

  /* The editors above the bottom dock bar. */
  make_card(GTK_WIDGET(t->editors.area));
  t->layout.vpaned = GTK_PANED(gtk_paned_new(GTK_ORIENTATION_VERTICAL));
  gtk_paned_set_start_child(t->layout.vpaned, GTK_WIDGET(t->editors.area));
  gtk_paned_set_end_child(t->layout.vpaned, t->layout.bottom);
  gtk_paned_set_resize_end_child(t->layout.vpaned, FALSE);
  gtk_paned_set_shrink_start_child(t->layout.vpaned, TRUE);
  gtk_paned_set_shrink_end_child(t->layout.vpaned, TRUE);
  g_signal_connect(t->layout.vpaned, "notify::position", G_CALLBACK(on_vpaned_position), t);
  gtk_paned_set_position(t->layout.vpaned, 620);

  /* The right sidebar right of the center area. */
  t->layout.rpaned = GTK_PANED(gtk_paned_new(GTK_ORIENTATION_HORIZONTAL));
  gtk_paned_set_start_child(t->layout.rpaned, GTK_WIDGET(t->layout.vpaned));
  gtk_paned_set_end_child(t->layout.rpaned, t->layout.right);
  gtk_paned_set_resize_end_child(t->layout.rpaned, FALSE);
  gtk_paned_set_shrink_start_child(t->layout.rpaned, TRUE);
  gtk_paned_set_shrink_end_child(t->layout.rpaned, TRUE);
  g_signal_connect(t->layout.rpaned, "notify::position", G_CALLBACK(on_rpaned_position), t);
  gtk_paned_set_position(t->layout.rpaned, 840);

  /* The left sidebar left of them. */
  t->layout.hpaned = GTK_PANED(gtk_paned_new(GTK_ORIENTATION_HORIZONTAL));
  gtk_paned_set_start_child(t->layout.hpaned, t->layout.left);
  gtk_paned_set_end_child(t->layout.hpaned, GTK_WIDGET(t->layout.rpaned));
  gtk_paned_set_resize_start_child(t->layout.hpaned, FALSE);
  gtk_paned_set_shrink_start_child(t->layout.hpaned, TRUE);
  gtk_paned_set_shrink_end_child(t->layout.hpaned, TRUE);
  g_signal_connect(t->layout.hpaned, "notify::position", G_CALLBACK(on_hpaned_position), t);
  gtk_paned_set_position(t->layout.hpaned, 280);
  gtk_widget_set_vexpand(GTK_WIDGET(t->layout.hpaned), TRUE);
  gtk_widget_add_css_class(GTK_WIDGET(t->layout.hpaned), "tcode-workspace");
  g_signal_connect(t->layout.hpaned, "notify::position", G_CALLBACK(on_layout_changed), t);
  g_signal_connect(t->layout.vpaned, "notify::position", G_CALLBACK(on_layout_changed), t);
  g_signal_connect(t->layout.rpaned, "notify::position", G_CALLBACK(on_layout_changed), t);

  gtk_box_append(GTK_BOX(content), GTK_WIDGET(t->layout.hpaned));
  gtk_box_append(GTK_BOX(content), status_bar(t));
  gtk_overlay_set_child(GTK_OVERLAY(overlay), content);
  picker_init(t, GTK_OVERLAY(overlay));
  gtk_window_set_child(t->window, overlay);

  /* The last session's sizes, then start the programs and show it. */
  add_shortcuts(t);
  session_apply_window(t);
  session_load_project(t);
  if (strcmp(t->project.root, g_get_home_dir()) != 0) /* home means no folder (see reopen_last_project) */
    session_add_recent(t);
  rebuild_recent_menu(t);
  start_shell_pane(t);
  start_status_updates(t);
  gtk_window_present(t->window);
  files_focus(t);
  updates_start(t); /* the release check (Settings' "Check for updates") starts from here */
}
