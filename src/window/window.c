/*
 * The main window (see window.h for the layout): building it, its text sizes, and closing it. Its
 * title bar is in titlebar.c, the docks around the editors in docks/.
 */
#include "window/window.h"

#include "application/application.h"
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
#include "project/trust.h"
#include "project/userfiles.h"
#include "lsp/inline/inlay.h"
#include "update/update.h"
#include "application/shellenv.h"
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
  apply_fonts(t); /* the terminals, the editors and the Files panel at the new size */
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

static void destroy_windows(App *t)
{
  if (t->editors.second) /* its tabs go with it, as the main window's do */
    gtk_window_destroy(t->editors.second);
  gtk_window_destroy(t->window);
}

/* The window's close button, once nothing unsaved is left (or the user said to drop it): the
 * session is saved, the plugins' programs in terminal tabs end, then the window goes; its tabs stop
 * their editors on the way. */
static void close_window(App *t, const char *arg G_GNUC_UNUSED)
{
  session_save(t);
  t->closing = TRUE;
  end_programs(t, destroy_windows);
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

/* A side dragged smaller than this (left / right: its width, bottom: its height) closes, the way a
 * side would rather go than show its panels cut off; dragged back out while the button is still down,
 * it opens again. Sizes it opens at when nothing was remembered. */
static const int side_snap[3] = { 170, 100, 170 };
static const int side_default[3] = { 280, 240, 280 };

static GtkPaned *side_paned(App *t, int side)
{
  return side == 0 ? t->layout.hpaned : side == 1 ? t->layout.vpaned : t->layout.rpaned;
}

/* The paned's largest position: where the side after the handle (right, bottom) is gone. */
static int paned_max(GtkPaned *paned, int side)
{
  int max = 0;

  g_object_get(paned, "max-position", &max, NULL);
  if (max <= 0)
    max = side == 1 ? gtk_widget_get_height(GTK_WIDGET(paned)) : gtk_widget_get_width(GTK_WIDGET(paned));
  return max;
}

static int side_size(App *t, int side)
{
  GtkPaned *paned = side_paned(t, side);
  int pos = gtk_paned_get_position(paned);

  return side == 0 ? pos : paned_max(paned, side) - pos;
}

static void set_side_size(App *t, int side, int size)
{
  GtkPaned *paned = side_paned(t, side);

  gtk_paned_set_position(paned, side == 0 ? size : MAX(paned_max(paned, side) - size, 0));
}

/* The size a side opens at again: its last one that was big enough. */
static int side_remembered(App *t, int side)
{
  return t->layout.sizes[side] >= side_snap[side] ? t->layout.sizes[side] : side_default[side];
}

/* The bottom with every dock collapsed is small on purpose (only their tab bars). */
static gboolean side_can_snap(App *t, int side)
{
  return side != 1 || side_has_expanded(t, 1);
}

/* A main paned moved. While the mouse drags it, a side smaller than side_snap squeezes to nothing
 * (and hides on release); otherwise a side pushed to (almost) nothing hides at once. */
static void on_paned_position(GtkPaned *paned, GParamSpec *spec G_GNUC_UNUSED, App *t)
{
  int side = paned == t->layout.hpaned ? 0 : paned == t->layout.vpaned ? 1 : 2;
  GtkWidget *pane = side_widget(t, side);
  int size = side_size(t, side);

  if (!pane || !gtk_widget_get_visible(pane) || !side_can_snap(t, side) || !(t->layout.placed & (1u << side)))
    return;
  if (t->layout.pressed) {
    if (size >= side_snap[side]) {
      t->layout.sizes[side] = size;
      t->layout.snapped &= ~(1u << side);
    } else {
      t->layout.snapped |= 1u << side;
      if (size > 0)
        set_side_size(t, side, 0);
    }
  } else if (size > 0 && size < 24 && paned_max(paned, side) > 320) {
    set_side_size(t, side, side_remembered(t, side));
    set_pane_visible(t, pane, FALSE);
  }
}

/* The bottom and the right side keep their own size (layout.sizes, from the session), not a
 * position from the top or left, which squeezed them to nothing in a window smaller than the one
 * it was saved in. It is given once the paned knows its size; the editors keep at least 200 px. */
static void on_paned_first_size(GtkPaned *paned, GParamSpec *spec G_GNUC_UNUSED, App *t)
{
  int side = paned == t->layout.vpaned ? 1 : 2;
  int max = 0;

  g_object_get(paned, "max-position", &max, NULL);
  if (max <= 200 || (t->layout.placed & (1u << side)))
    return;
  int size = side_remembered(t, side);
  if (max - size < 200)
    size = max / 3;
  if (!side_can_snap(t, side))
    size = 34; /* every bottom dock collapsed: only their tab bars */
  t->layout.placed |= 1u << side;
  set_side_size(t, side, size);
  g_signal_handlers_disconnect_by_func(paned, on_paned_first_size, t);
}

void end_pane_drag(App *t)
{
  t->layout.pressed = FALSE;
  for (int side = 0; side < 3; side++)
    if (t->layout.snapped & (1u << side)) {
      t->layout.snapped &= ~(1u << side);
      set_side_size(t, side, side_remembered(t, side));
      set_pane_visible(t, side_widget(t, side), FALSE);
    }
  update_edges(t);
}

/* Mouse buttons in the whole window, seen before any widget. */
static gboolean on_window_event(GtkEventControllerLegacy *legacy G_GNUC_UNUSED, GdkEvent *event, App *t)
{
  GdkEventType type = gdk_event_get_event_type(event);

  if (type == GDK_BUTTON_PRESS)
    t->layout.pressed = TRUE;
  else if (type == GDK_BUTTON_RELEASE && t->layout.pressed)
    end_pane_drag(t);
  else if (type == GDK_MOTION_NOTIFY && t->layout.pressed && /* its release went to a popover or a dialog */
           !(gdk_event_get_modifier_state(event) &
             (GDK_BUTTON1_MASK | GDK_BUTTON2_MASK | GDK_BUTTON3_MASK | GDK_BUTTON4_MASK | GDK_BUTTON5_MASK)))
    end_pane_drag(t);
  return GDK_EVENT_PROPAGATE;
}

void update_edges(App *t)
{
  if (t->layout.pressed) /* hiding a strip would end the drag that is opening its side */
    return;
  for (int side = 0; side < 3; side++)
    if (t->layout.edges[side])
      gtk_widget_set_visible(t->layout.edges[side],
                             !gtk_widget_get_visible(side_widget(t, side)) && (side != 2 || side_has_pages(t, side)));
}

/* Dragging a hidden side's edge strip inwards shows the side, as wide as the pointer has come. */
static void on_edge_drag(GtkGestureDrag *drag, double dx, double dy, App *t)
{
  int side = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(drag), "tcode-side"));
  int inward = (int)(side == 0 ? dx : side == 1 ? -dy : -dx);
  GtkWidget *pane = side_widget(t, side);

  if (!gtk_widget_get_visible(pane)) {
    if (inward < 4)
      return;
    set_pane_visible(t, pane, TRUE);
  }
  set_side_size(t, side, MAX(inward, 0));
}

static GtkWidget *edge_strip(App *t, int side)
{
  GtkWidget *strip = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  GtkGesture *drag = gtk_gesture_drag_new();

  if (side == 1) {
    gtk_widget_set_size_request(strip, -1, 4);
    gtk_widget_set_valign(strip, GTK_ALIGN_END);
  } else {
    gtk_widget_set_size_request(strip, 4, -1);
    gtk_widget_set_halign(strip, side == 0 ? GTK_ALIGN_START : GTK_ALIGN_END);
  }
  gtk_widget_set_cursor_from_name(strip, side == 1 ? "row-resize" : "col-resize");
  gtk_widget_set_visible(strip, FALSE);
  g_object_set_data(G_OBJECT(drag), "tcode-side", GINT_TO_POINTER(side));
  g_signal_connect(drag, "drag-update", G_CALLBACK(on_edge_drag), t);
  gtk_widget_add_controller(strip, GTK_EVENT_CONTROLLER(drag));
  t->layout.edges[side] = strip;
  return strip;
}

/* Wraps a paned in an overlay holding the edge strips of the given sides. */
static GtkWidget *with_edges(App *t, GtkPaned *paned, int first, int last)
{
  GtkWidget *overlay = gtk_overlay_new();

  gtk_overlay_set_child(GTK_OVERLAY(overlay), GTK_WIDGET(paned));
  for (int side = first; side <= last; side += 2)
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), edge_strip(t, side));
  return overlay;
}

/* The window --------------------------------------------------------------------------------- */

void apply_interface(App *t)
{
  apply_panels(t);
  title_bar_apply(t);
  status_bar_apply(t);
}

void build_window(App *t, GtkApplication *app)
{
  GtkWidget *overlay = gtk_overlay_new();                        /* the picker (Ctrl+P) floats over the content */
  GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0); /* the panes, the status bar */

  /* Before the window: the theme, where the helpers are, and the sockets they connect to. */
  load_theme(t);
  watch_user_files(t); /* settings.ini, theme.css and the editor colors, edited by hand */
  gtk_source_init();
  find_files(t);
  start_socket(t);
  plugins_init(t); /* before snippets_init and the first editor: their snippets and language specs */
  snippets_init(t);
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
  make_card(t->editors.view);
  /* Held without asking for its smallest size (a tab bar's height, a hint's width), like the docks:
   * the bottom or a side dragged far squeezes it instead of measuring it below its minimum. */
  GtkWidget *editors_clip = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(editors_clip), GTK_POLICY_EXTERNAL, GTK_POLICY_EXTERNAL);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(editors_clip), t->editors.view);
  gtk_viewport_set_scroll_to_focus(GTK_VIEWPORT(gtk_scrolled_window_get_child(GTK_SCROLLED_WINDOW(editors_clip))),
                                   FALSE);
  t->layout.vpaned = GTK_PANED(gtk_paned_new(GTK_ORIENTATION_VERTICAL));
  gtk_paned_set_start_child(t->layout.vpaned, editors_clip);
  gtk_paned_set_end_child(t->layout.vpaned, t->layout.bottom);
  gtk_paned_set_resize_end_child(t->layout.vpaned, FALSE);
  gtk_paned_set_shrink_start_child(t->layout.vpaned, TRUE);
  gtk_paned_set_shrink_end_child(t->layout.vpaned, TRUE);
  g_signal_connect(t->layout.vpaned, "notify::position", G_CALLBACK(on_paned_position), t);
  g_signal_connect(t->layout.vpaned, "notify::max-position", G_CALLBACK(on_paned_first_size), t);

  /* The right sidebar right of the center area. */
  t->layout.rpaned = GTK_PANED(gtk_paned_new(GTK_ORIENTATION_HORIZONTAL));
  gtk_paned_set_start_child(t->layout.rpaned, with_edges(t, t->layout.vpaned, 1, 1));
  gtk_paned_set_end_child(t->layout.rpaned, t->layout.right);
  gtk_paned_set_resize_end_child(t->layout.rpaned, FALSE);
  gtk_paned_set_shrink_start_child(t->layout.rpaned, TRUE);
  gtk_paned_set_shrink_end_child(t->layout.rpaned, TRUE);
  g_signal_connect(t->layout.rpaned, "notify::position", G_CALLBACK(on_paned_position), t);
  g_signal_connect(t->layout.rpaned, "notify::max-position", G_CALLBACK(on_paned_first_size), t);

  /* The left sidebar left of them. */
  t->layout.hpaned = GTK_PANED(gtk_paned_new(GTK_ORIENTATION_HORIZONTAL));
  gtk_paned_set_start_child(t->layout.hpaned, t->layout.left);
  gtk_paned_set_end_child(t->layout.hpaned, GTK_WIDGET(t->layout.rpaned));
  gtk_paned_set_resize_start_child(t->layout.hpaned, FALSE);
  gtk_paned_set_shrink_start_child(t->layout.hpaned, TRUE);
  gtk_paned_set_shrink_end_child(t->layout.hpaned, TRUE);
  g_signal_connect(t->layout.hpaned, "notify::position", G_CALLBACK(on_paned_position), t);
  gtk_paned_set_position(t->layout.hpaned, 280);
  t->layout.placed = 1u; /* the left side's position is its size */
  gtk_widget_set_vexpand(GTK_WIDGET(t->layout.hpaned), TRUE);
  gtk_widget_add_css_class(GTK_WIDGET(t->layout.hpaned), "tcode-workspace");
  g_signal_connect(t->layout.hpaned, "notify::position", G_CALLBACK(on_layout_changed), t);
  g_signal_connect(t->layout.vpaned, "notify::position", G_CALLBACK(on_layout_changed), t);
  g_signal_connect(t->layout.rpaned, "notify::position", G_CALLBACK(on_layout_changed), t);

  gtk_box_append(GTK_BOX(content), trust_bar_new(t)); /* "Trust this folder?" (project/trust.h) */
  gtk_box_append(GTK_BOX(content), with_edges(t, t->layout.hpaned, 0, 2));
  GtkEventController *legacy = gtk_event_controller_legacy_new();
  gtk_event_controller_set_propagation_phase(legacy, GTK_PHASE_CAPTURE);
  g_signal_connect(legacy, "event", G_CALLBACK(on_window_event), t);
  gtk_widget_add_controller(GTK_WIDGET(t->window), legacy);
  gtk_box_append(GTK_BOX(content), status_bar(t));
  gtk_overlay_set_child(GTK_OVERLAY(overlay), docks_drop_view(t, content));
  picker_init(t, GTK_OVERLAY(overlay));
  gtk_window_set_child(t->window, overlay);

  /* The last session's sizes, then start the programs and show it. */
  add_shortcuts(t);
  session_apply_window(t);
  apply_interface(t); /* after the docks are where the session left them */
  session_load_project(t);
  if (strcmp(t->project.root, g_get_home_dir()) != 0) /* home means no folder (see reopen_last_project) */
    session_add_recent(t);
  rebuild_recent_menu(t);
  start_shell_pane(t);
  start_status_updates(t);
  if (!shell_env_loading()) /* else once the login shell's environment is there */
    plugins_start_project(t);
  gtk_window_present(t->window);
  files_focus(t);
  updates_start(t);         /* the release check (Settings' "Check for updates") starts from here */
  application_trim_later(); /* what start-up freed (the renderer's set-up, the first frames) */
}
