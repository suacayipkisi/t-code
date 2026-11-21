/*
 * The drop markers over a window's tab groups. While a tab is dragged over the groups, five squares
 * show over the group under the pointer (the middle one: into that group; the others: a new group on
 * that side of it) and, once the window has more than one group, four along the edges of the whole
 * tree (a new group along that edge). The square under the pointer shades where the tab would go;
 * over a tab strip a line shows where among the tabs it lands. Dropped elsewhere, nothing happens.
 */
#pragma once

#include "app.h"

typedef enum {
  DROP_NONE,
  DROP_CENTER,
  DROP_LEFT,
  DROP_RIGHT,
  DROP_TOP,
  DROP_BOTTOM,
  DROP_EDGE_LEFT,
  DROP_EDGE_RIGHT,
  DROP_EDGE_TOP,
  DROP_EDGE_BOTTOM,
  DROP_STRIP, /* among the tabs of a strip */
} DropZone;

#define DROP_MARKERS (DROP_EDGE_BOTTOM + 1)

/* Where the squares are (rect[zone], for the zones DROP_CENTER .. DROP_EDGE_BOTTOM). */
typedef struct {
  graphene_rect_t rect[DROP_MARKERS];
  gboolean shown[DROP_MARKERS];
} DropMarkers;

/* Places the squares: the cross in the middle of group (its side squares only with sides, and only
 * when the group is big enough for them), the edge squares along area with edges. */
void drop_markers_place(DropMarkers *markers, const graphene_rect_t *group, const graphene_rect_t *area, gboolean sides,
                        gboolean edges);

/* The shown square at (x, y), or DROP_NONE. */
DropZone drop_marker_at(const DropMarkers *markers, double x, double y);

/* The space a tab dropped on zone's square would take. */
void drop_zone_preview(DropZone zone, const graphene_rect_t *group, const graphene_rect_t *area, graphene_rect_t *out);

/* child (holding a window's group tree; window: 0 the main one, 1 the second) under the markers. */
GtkWidget *drop_view_new(App *t, GtkWidget *child, int window);

/* What a drag of tab (NULL: not known yet) at (x, y) in view's coordinates would do; shows it. */
DropZone drop_view_hover(GtkWidget *view, Tab *tab, double x, double y);

/* Drops tab at (x, y): FALSE when nothing happens there. */
gboolean drop_view_drop(GtkWidget *view, Tab *tab, double x, double y);

/* The drag left: the markers go. */
void drop_view_leave(GtkWidget *view);

/* The middle of zone's square as the last hover placed it; FALSE when it is not shown. */
gboolean drop_view_marker(GtkWidget *view, DropZone zone, double *x, double *y);
