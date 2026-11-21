/*
 * The drop markers (see dropzone.h): a drawing area over the window's groups, drawn only while a tab
 * is dragged; it never takes the pointer, so what is under it is found by picking through it.
 */
#include "editor/dropzone.h"

#include "editor/tabs.h"

#define SQUARE 36 /* a marker's side */
#define GAP 6     /* between the cross's squares */
#define MARGIN 10 /* between an edge square and its edge */

typedef struct {
  App *t;
  int window;
  GtkWidget *view, *hint;
  DropMarkers markers;
  GtkNotebook *group;   /* under the pointer */
  graphene_rect_t room; /* the group's, in the view */
  graphene_rect_t area; /* the whole tree's */
  graphene_rect_t preview;
  DropZone zone;
  int position; /* DROP_STRIP: where among the tabs */
} Drop;

/* Placing the squares ---------------------------------------------------------------------------- */

static void square(DropMarkers *markers, DropZone zone, double x, double y)
{
  graphene_rect_init(&markers->rect[zone], x, y, SQUARE, SQUARE);
  markers->shown[zone] = TRUE;
}

void drop_markers_place(DropMarkers *markers, const graphene_rect_t *group, const graphene_rect_t *area, gboolean sides,
                        gboolean edges)
{
  double cx = group->origin.x + group->size.width / 2 - SQUARE / 2.0;
  double cy = group->origin.y + group->size.height / 2 - SQUARE / 2.0;
  double step = SQUARE + GAP, room = 3 * SQUARE + 2 * GAP + 2 * MARGIN;
  double ax = area->origin.x, ay = area->origin.y, aw = area->size.width, ah = area->size.height;

  *markers = (DropMarkers){ 0 };
  square(markers, DROP_CENTER, cx, cy);
  if (sides && group->size.width >= room && group->size.height >= room) {
    square(markers, DROP_LEFT, cx - step, cy);
    square(markers, DROP_RIGHT, cx + step, cy);
    square(markers, DROP_TOP, cx, cy - step);
    square(markers, DROP_BOTTOM, cx, cy + step);
  }
  if (edges && aw >= 2 * room && ah >= 2 * room) {
    square(markers, DROP_EDGE_LEFT, ax + MARGIN, ay + ah / 2 - SQUARE / 2.0);
    square(markers, DROP_EDGE_RIGHT, ax + aw - MARGIN - SQUARE, ay + ah / 2 - SQUARE / 2.0);
    square(markers, DROP_EDGE_TOP, ax + aw / 2 - SQUARE / 2.0, ay + MARGIN);
    square(markers, DROP_EDGE_BOTTOM, ax + aw / 2 - SQUARE / 2.0, ay + ah - MARGIN - SQUARE);
  }
}

DropZone drop_marker_at(const DropMarkers *markers, double x, double y)
{
  for (int zone = DROP_CENTER; zone < DROP_MARKERS; zone++) /* the cross first: it may cover an edge square */
    if (markers->shown[zone] && graphene_rect_contains_point(&markers->rect[zone], &GRAPHENE_POINT_INIT(x, y)))
      return zone;
  return DROP_NONE;
}

void drop_zone_preview(DropZone zone, const graphene_rect_t *group, const graphene_rect_t *area, graphene_rect_t *out)
{
  const graphene_rect_t *r = zone >= DROP_EDGE_LEFT ? area : group;
  double part = zone >= DROP_EDGE_LEFT ? 1.0 / 3 : 0.5; /* split_window() takes a third, split_group() half */
  double x = r->origin.x, y = r->origin.y, w = r->size.width, h = r->size.height;

  switch (zone) {
    case DROP_LEFT:
    case DROP_EDGE_LEFT: graphene_rect_init(out, x, y, w * part, h); break;
    case DROP_RIGHT:
    case DROP_EDGE_RIGHT: graphene_rect_init(out, x + w * (1 - part), y, w * part, h); break;
    case DROP_TOP:
    case DROP_EDGE_TOP: graphene_rect_init(out, x, y, w, h * part); break;
    case DROP_BOTTOM:
    case DROP_EDGE_BOTTOM: graphene_rect_init(out, x, y + h * (1 - part), w, h * part); break;
    default: graphene_rect_init(out, x, y, w, h);
  }
}

/* Drawing ---------------------------------------------------------------------------------------- */

static void rounded(cairo_t *cr, const graphene_rect_t *r, double radius)
{
  double x = r->origin.x, y = r->origin.y, w = r->size.width, h = r->size.height;

  cairo_new_sub_path(cr);
  cairo_arc(cr, x + w - radius, y + radius, radius, -G_PI / 2, 0);
  cairo_arc(cr, x + w - radius, y + h - radius, radius, 0, G_PI / 2);
  cairo_arc(cr, x + radius, y + h - radius, radius, G_PI / 2, G_PI);
  cairo_arc(cr, x + radius, y + radius, radius, G_PI, 3 * G_PI / 2);
  cairo_close_path(cr);
}

static void set_color(cairo_t *cr, const GdkRGBA *color, double alpha)
{
  cairo_set_source_rgba(cr, color->red, color->green, color->blue, color->alpha * alpha);
}

/* One square: a small window with the part the tab would take shaded. */
static void draw_marker(cairo_t *cr, const GdkRGBA *accent, DropZone zone, const graphene_rect_t *r, gboolean hot)
{
  graphene_rect_t inner, part;

  rounded(cr, r, 4);
  cairo_set_source_rgba(cr, 0.08, 0.08, 0.08, 0.78);
  cairo_fill_preserve(cr);
  set_color(cr, accent, hot ? 1 : 0.7);
  cairo_set_line_width(cr, 1);
  cairo_stroke(cr);
  graphene_rect_inset_r(r, 6, 6, &inner);
  drop_zone_preview(zone, &inner, &inner, &part);
  cairo_rectangle(cr, inner.origin.x + 0.5, inner.origin.y + 0.5, inner.size.width - 1, inner.size.height - 1);
  cairo_set_source_rgba(cr, 1, 1, 1, 0.45);
  cairo_stroke(cr);
  cairo_rectangle(cr, part.origin.x, part.origin.y, part.size.width, part.size.height);
  set_color(cr, accent, hot ? 1 : 0.6);
  cairo_fill(cr);
}

static void draw(GtkDrawingArea *area, cairo_t *cr, int width G_GNUC_UNUSED, int height G_GNUC_UNUSED, gpointer data)
{
  Drop *d = data;
  GdkRGBA accent;

  gtk_widget_get_color(GTK_WIDGET(area), &accent);
  if (d->zone != DROP_NONE) {
    cairo_rectangle(cr, d->preview.origin.x, d->preview.origin.y, d->preview.size.width, d->preview.size.height);
    set_color(cr, &accent, d->zone == DROP_STRIP ? 1 : 0.22);
    cairo_fill_preserve(cr);
    set_color(cr, &accent, 0.9);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);
  }
  for (int zone = DROP_CENTER; zone < DROP_MARKERS; zone++)
    if (d->markers.shown[zone])
      draw_marker(cr, &accent, zone, &d->markers.rect[zone], zone == (int)d->zone);
}

/* Following the drag ----------------------------------------------------------------------------- */

static Drop *drop_of(GtkWidget *view)
{
  return g_object_get_data(G_OBJECT(view), "t-code-drop");
}

/* The group under (x, y), and whether that is over its tab strip. */
static GtkNotebook *group_at(Drop *d, double x, double y, gboolean *on_strip)
{
  GtkWidget *w = gtk_widget_pick(d->view, x, y, GTK_PICK_DEFAULT);

  *on_strip = FALSE;
  for (; w && w != d->view; w = gtk_widget_get_parent(w)) {
    GtkNotebook *group = group_of_widget(w);
    if (group)
      return group;
    if (gtk_widget_has_css_class(w, "tcode-tabs"))
      *on_strip = TRUE;
  }
  *on_strip = FALSE;
  return NULL;
}

/* Only a tab that is still open. */
static Tab *open_tab(App *t, Tab *tab)
{
  int n = tab_count(t);

  for (int i = 0; i < n && tab; i++)
    if (tab_nth(t, i) == tab)
      return tab;
  return NULL;
}

DropZone drop_view_hover(GtkWidget *view, Tab *tab, double x, double y)
{
  Drop *d = drop_of(view);
  App *t = d->t;
  GtkWidget *root = t->editors.roots[d->window];
  gboolean on_strip = FALSE, alone;
  GtkNotebook *group;
  int in_window = 0;

  d->zone = DROP_NONE;
  if (!root)
    return DROP_NONE;
  for (int i = 0; i < group_count(t); i++)
    in_window += group_window(t, group_nth(t, i)) == d->window;
  if (!gtk_widget_get_mapped(root) || !gtk_widget_compute_bounds(root, view, &d->area))
    graphene_rect_init(&d->area, 0, 0, gtk_widget_get_width(view), gtk_widget_get_height(view)); /* the hint */
  group = gtk_widget_get_mapped(root) ? group_at(d, x, y, &on_strip) : NULL;
  if (!group || !gtk_widget_compute_bounds(tab_group_widget(group), view, &d->room)) {
    group = window_first_group(t, d->window);
    on_strip = FALSE;
    d->room = d->area;
  }
  d->group = group;
  /* Splitting off a group's only tab, or splitting an empty group, would leave an empty group behind. */
  alone =
      (tab && tab_group(tab) == group && gtk_notebook_get_n_pages(group) == 1) || gtk_notebook_get_n_pages(group) == 0;
  drop_markers_place(&d->markers, &d->room, &d->area, !alone, in_window > 1);
  d->zone = drop_marker_at(&d->markers, x, y);
  if (d->zone == DROP_NONE && on_strip) {
    GtkWidget *strip = strip_widget(group);
    graphene_point_t at;
    graphene_rect_t bounds;
    double line = 0;
    if (gtk_widget_compute_point(view, strip, &GRAPHENE_POINT_INIT(x, y), &at) &&
        gtk_widget_compute_bounds(strip, view, &bounds)) {
      d->position = strip_drop_position(group, at.x, &line);
      graphene_rect_init(&d->preview, bounds.origin.x + line - 1, bounds.origin.y, 3, bounds.size.height);
      d->zone = DROP_STRIP;
    }
  } else if (d->zone != DROP_NONE) {
    drop_zone_preview(d->zone, &d->room, &d->area, &d->preview);
  }
  gtk_widget_set_visible(d->hint, TRUE);
  gtk_widget_queue_draw(d->hint);
  return d->zone;
}

void drop_view_leave(GtkWidget *view)
{
  Drop *d = drop_of(view);

  d->zone = DROP_NONE;
  d->markers = (DropMarkers){ 0 };
  gtk_widget_set_visible(d->hint, FALSE);
}

static SplitSide side_of(DropZone zone)
{
  switch (zone) {
    case DROP_LEFT:
    case DROP_EDGE_LEFT: return SPLIT_LEFT;
    case DROP_TOP:
    case DROP_EDGE_TOP: return SPLIT_UP;
    case DROP_BOTTOM:
    case DROP_EDGE_BOTTOM: return SPLIT_DOWN;
    default: return SPLIT_RIGHT;
  }
}

gboolean drop_view_drop(GtkWidget *view, Tab *tab, double x, double y)
{
  Drop *d = drop_of(view);
  App *t = d->t;
  DropZone zone;
  GtkNotebook *to;
  int position;

  tab = open_tab(t, tab);
  zone = tab ? drop_view_hover(view, tab, x, y) : DROP_NONE;
  position = d->position;
  drop_view_leave(view);
  switch (zone) {
    case DROP_NONE: return FALSE;
    case DROP_STRIP:
      if (tab_group(tab) != d->group)
        move_tab(tab, d->group);
      else if (tab_position(tab) < position)
        position--; /* it leaves its place first */
      set_tab_position(tab, position);
      select_tab(tab);
      return TRUE;
    case DROP_CENTER: to = d->group; break;
    case DROP_LEFT:
    case DROP_RIGHT:
    case DROP_TOP:
    case DROP_BOTTOM: to = split_group(d->group, side_of(zone)); break;
    default: to = split_window(t, d->window, side_of(zone));
  }
  move_tab(tab, to);
  select_tab(tab);
  return TRUE;
}

gboolean drop_view_marker(GtkWidget *view, DropZone zone, double *x, double *y)
{
  Drop *d = drop_of(view);

  if (zone < DROP_CENTER || zone >= DROP_MARKERS || !d->markers.shown[zone])
    return FALSE;
  *x = d->markers.rect[zone].origin.x + SQUARE / 2.0;
  *y = d->markers.rect[zone].origin.y + SQUARE / 2.0;
  return TRUE;
}

/* The drop target -------------------------------------------------------------------------------- */

/* The tab being dragged, once its value has come (NULL before). */
static Tab *dragged(GtkDropTarget *target, Drop *d)
{
  const GValue *value = gtk_drop_target_get_value(target);

  return value && G_VALUE_HOLDS_POINTER(value) ? open_tab(d->t, g_value_get_pointer(value)) : NULL;
}

static GdkDragAction on_motion(GtkDropTarget *target, double x, double y, Drop *d)
{
  return drop_view_hover(d->view, dragged(target, d), x, y) != DROP_NONE ? GDK_ACTION_MOVE : 0;
}

static void on_leave(GtkDropTarget *target G_GNUC_UNUSED, Drop *d)
{
  drop_view_leave(d->view);
}

static gboolean on_drop(GtkDropTarget *target G_GNUC_UNUSED, const GValue *value, double x, double y, Drop *d)
{
  return drop_view_drop(d->view, g_value_get_pointer(value), x, y);
}

GtkWidget *drop_view_new(App *t, GtkWidget *child, int window)
{
  GtkWidget *view = gtk_overlay_new(), *hint = gtk_drawing_area_new();
  GtkDropTarget *target = gtk_drop_target_new(G_TYPE_POINTER, GDK_ACTION_MOVE); /* a tab (tabs.c's drag) */
  Drop *d = g_new0(Drop, 1);

  d->t = t;
  d->window = window;
  d->view = view;
  d->hint = hint;
  g_object_set_data_full(G_OBJECT(view), "t-code-drop", d, g_free);
  gtk_widget_set_hexpand(view, TRUE);
  gtk_widget_set_vexpand(view, TRUE);
  gtk_overlay_set_child(GTK_OVERLAY(view), child);
  gtk_widget_add_css_class(hint, "tcode-drop-hint");
  gtk_widget_set_can_target(hint, FALSE);
  gtk_widget_set_visible(hint, FALSE);
  gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(hint), draw, d, NULL);
  gtk_overlay_add_overlay(GTK_OVERLAY(view), hint);

  gtk_drop_target_set_preload(target, TRUE); /* the tab is known while it moves: its own group's sides */
  gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(target), GTK_PHASE_CAPTURE);
  g_signal_connect(target, "enter", G_CALLBACK(on_motion), d);
  g_signal_connect(target, "motion", G_CALLBACK(on_motion), d);
  g_signal_connect(target, "leave", G_CALLBACK(on_leave), d);
  g_signal_connect(target, "drop", G_CALLBACK(on_drop), d);
  gtk_widget_add_controller(view, GTK_EVENT_CONTROLLER(target));
  return view;
}
