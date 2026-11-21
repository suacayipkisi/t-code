/*
 * The tab groups' trees (see tabs.h). A window holds a root box with one child: a group's widget
 * (tab_group_widget) or a split, a GtkPaned whose two sides are again a group or a split. Splitting
 * a group puts a new split in its place; a group that goes leaves the other side of its split in the
 * split's place. The widgets are the tree: nothing else keeps it.
 */
#include "editor/tabs.h"

#include "editor/dropzone.h"

#include <stdlib.h>

/* The groups, in order --------------------------------------------------------------------------- */

/* Adds the groups under node to out, the start side of each split first. */
static void collect(GtkWidget *node, GPtrArray *out)
{
  GtkNotebook *group = group_of_widget(node);

  if (group) {
    g_ptr_array_add(out, group);
  } else if (GTK_IS_PANED(node)) {
    collect(gtk_paned_get_start_child(GTK_PANED(node)), out);
    collect(gtk_paned_get_end_child(GTK_PANED(node)), out);
  } else {
    for (GtkWidget *child = gtk_widget_get_first_child(node); child; child = gtk_widget_get_next_sibling(child))
      collect(child, out);
  }
}

/* Makes t->editors.groups follow the trees again (after a split or a group going). */
static void reorder(App *t)
{
  g_ptr_array_set_size(t->editors.groups, 0);
  for (int w = 0; w < 2; w++)
    if (t->editors.roots[w])
      collect(t->editors.roots[w], t->editors.groups);
  if (t->editors.groups->len && !g_ptr_array_find(t->editors.groups, t->editors.active, NULL))
    t->editors.active = g_ptr_array_index(t->editors.groups, 0);
}

int group_count(App *t)
{
  return t->editors.groups ? (int)t->editors.groups->len : 0;
}

GtkNotebook *group_nth(App *t, int index)
{
  return index >= 0 && index < group_count(t) ? g_ptr_array_index(t->editors.groups, index) : NULL;
}

int group_index(App *t, GtkNotebook *group)
{
  guint index;

  return group && t->editors.groups && g_ptr_array_find(t->editors.groups, group, &index) ? (int)index : -1;
}

int group_window(App *t, GtkNotebook *group)
{
  return t->editors.roots[1] && gtk_widget_is_ancestor(GTK_WIDGET(group), t->editors.roots[1]) ? 1 : 0;
}

/* The first group under node. */
static GtkNotebook *first_group(GtkWidget *node)
{
  GPtrArray *found = g_ptr_array_new();
  GtkNotebook *group;

  collect(node, found);
  group = found->len ? g_ptr_array_index(found, 0) : NULL;
  g_ptr_array_unref(found);
  return group;
}

GtkNotebook *window_first_group(App *t, int window)
{
  return t->editors.roots[window] ? first_group(t->editors.roots[window]) : NULL;
}

/* The groups of a window, in order. */
static GPtrArray *window_groups(App *t, int window)
{
  GPtrArray *groups = g_ptr_array_new();

  if (t->editors.roots[window])
    collect(t->editors.roots[window], groups);
  return groups;
}

/* Building the tree ------------------------------------------------------------------------------ */

/* The first side keeps its share of the split once the split has a size of its own. */
static void on_max_position(GtkPaned *paned, GParamSpec *pspec G_GNUC_UNUSED, gpointer data G_GNUC_UNUSED)
{
  int max = 0;
  double *share = g_object_get_data(G_OBJECT(paned), "t-code-share");

  g_object_get(paned, "max-position", &max, NULL);
  if (max <= 0 || max == G_MAXINT || !share)
    return;
  gtk_paned_set_position(paned, (int)(max * *share + 0.5));
  g_signal_handlers_disconnect_by_func(paned, on_max_position, NULL);
}

/* A split: start over end (vertical) or start left of end. share: the start side's part (0: half,
 * GtkPaned's own choice while the position was never set). */
static GtkPaned *new_split(gboolean vertical, double share)
{
  GtkPaned *paned = GTK_PANED(gtk_paned_new(vertical ? GTK_ORIENTATION_VERTICAL : GTK_ORIENTATION_HORIZONTAL));

  gtk_widget_set_hexpand(GTK_WIDGET(paned), TRUE);
  gtk_widget_set_vexpand(GTK_WIDGET(paned), TRUE);
  if (share > 0) {
    double *kept = g_new(double, 1);
    *kept = share;
    g_object_set_data_full(G_OBJECT(paned), "t-code-share", kept, g_free);
    g_signal_connect(paned, "notify::max-position", G_CALLBACK(on_max_position), NULL);
  }
  return paned;
}

/* A GtkPaned remembers which side had the focus last and warns when that side leaves it while the
 * window's focus is somewhere else: the window has no focus while a tree changes, and gets it back
 * after (if its widget is still there). */
static GtkWidget *hold_focus(GtkWidget *node)
{
  GtkRoot *root = gtk_widget_get_root(node);
  GtkWidget *focus = GTK_IS_WINDOW(root) ? gtk_window_get_focus(GTK_WINDOW(root)) : NULL;

  if (!focus)
    return NULL;
  g_object_ref(focus);
  gtk_window_set_focus(GTK_WINDOW(root), NULL);
  return focus;
}

static void give_back_focus(GtkWidget *focus)
{
  if (!focus)
    return;
  if (gtk_widget_get_root(focus))
    gtk_widget_grab_focus(focus);
  g_object_unref(focus);
}

/* Puts replacement where node is (the caller keeps node alive meanwhile if it still wants it). */
static void replace_node(GtkWidget *node, GtkWidget *replacement)
{
  GtkWidget *parent = gtk_widget_get_parent(node);

  if (GTK_IS_PANED(parent)) {
    if (gtk_paned_get_start_child(GTK_PANED(parent)) == node)
      gtk_paned_set_start_child(GTK_PANED(parent), replacement);
    else
      gtk_paned_set_end_child(GTK_PANED(parent), replacement);
  } else { /* the root box */
    gtk_box_remove(GTK_BOX(parent), node);
    gtk_box_append(GTK_BOX(parent), replacement);
  }
}

/* A new group beside node (a group or a whole tree), taking share of the space (0: half). */
static GtkNotebook *split_node(App *t, GtkWidget *node, SplitSide side, double share)
{
  GtkNotebook *group = new_tab_group(t);
  GtkWidget *box = tab_group_widget(group);
  gboolean before = side == SPLIT_LEFT || side == SPLIT_UP;
  GtkPaned *paned = new_split(side == SPLIT_UP || side == SPLIT_DOWN, share > 0 ? (before ? share : 1 - share) : 0);
  GtkWidget *focus = hold_focus(node);

  g_object_ref(node);
  replace_node(node, GTK_WIDGET(paned));
  gtk_paned_set_start_child(paned, before ? box : node);
  gtk_paned_set_end_child(paned, before ? node : box);
  g_object_unref(node);
  give_back_focus(focus);
  reorder(t);
  t->editors.active = group;
  update_editor_area(t);
  return group;
}

GtkNotebook *split_group(GtkNotebook *group, SplitSide side)
{
  App *t = g_object_get_data(G_OBJECT(group), "t-code-app");

  return split_node(t, tab_group_widget(group), side, 0);
}

GtkNotebook *split_window(App *t, int window, SplitSide side)
{
  GtkWidget *root = t->editors.roots[window];

  return root ? split_node(t, gtk_widget_get_first_child(root), side, 1.0 / 3) : NULL;
}

GtkNotebook *side_group(App *t)
{
  GtkNotebook *active = t->editors.active;
  GPtrArray *groups = window_groups(t, group_window(t, active));
  GtkNotebook *side = NULL;
  guint index;

  if (g_ptr_array_find(groups, active, &index)) {
    if (index + 1 < groups->len)
      side = g_ptr_array_index(groups, index + 1);
    else if (index > 0)
      side = g_ptr_array_index(groups, index - 1);
  }
  g_ptr_array_unref(groups);
  if (!side)
    side = split_group(active, SPLIT_RIGHT);
  t->editors.active = side;
  return side;
}

/* Taking groups away ----------------------------------------------------------------------------- */

/* Takes an empty group out of its tree: the other side of its split takes the split's place. FALSE:
 * it is its window's only group. */
static gboolean remove_group(GtkNotebook *group)
{
  App *t = g_object_get_data(G_OBJECT(group), "t-code-app");
  GtkWidget *box = tab_group_widget(group), *split = gtk_widget_get_parent(box), *other;
  gboolean was_active = t->editors.active == group;
  GtkWidget *focus;

  if (!GTK_IS_PANED(split))
    return FALSE;
  focus = hold_focus(split);
  other = gtk_paned_get_start_child(GTK_PANED(split)) == box ? gtk_paned_get_end_child(GTK_PANED(split))
                                                             : gtk_paned_get_start_child(GTK_PANED(split));
  g_object_ref(other);
  if (gtk_paned_get_start_child(GTK_PANED(split)) == other)
    gtk_paned_set_start_child(GTK_PANED(split), NULL);
  else
    gtk_paned_set_end_child(GTK_PANED(split), NULL);
  replace_node(split, other); /* the split goes, and the group with it */
  if (was_active)
    t->editors.active = first_group(other);
  g_object_unref(other);
  give_back_focus(focus);
  reorder(t);
  update_editor_area(t);
  return TRUE;
}

/* The group that takes over a closing group's tabs: the first one of the other side of its split. */
static GtkNotebook *neighbour(GtkNotebook *group)
{
  GtkWidget *box = tab_group_widget(group), *split = gtk_widget_get_parent(box);

  if (!GTK_IS_PANED(split))
    return NULL;
  return first_group(gtk_paned_get_start_child(GTK_PANED(split)) == box ? gtk_paned_get_end_child(GTK_PANED(split))
                                                                        : gtk_paned_get_start_child(GTK_PANED(split)));
}

void close_group(GtkNotebook *group)
{
  App *t = g_object_get_data(G_OBJECT(group), "t-code-app");
  GtkNotebook *to = neighbour(group);

  if (!to)
    return;
  while (gtk_notebook_get_n_pages(group) > 0)
    move_tab(tab_of(gtk_notebook_get_nth_page(group, 0)), to);
  remove_group(group);
  t->editors.active = to;
  focus_current_tab(t);
}

void close_group_if_empty(GtkNotebook *group)
{
  App *t = g_object_get_data(G_OBJECT(group), "t-code-app");

  if (t->closing || group_index(t, group) < 0 || gtk_notebook_get_n_pages(group) > 0)
    return;
  if (remove_group(group))
    return;
  if (group_window(t, group) == 1 && t->editors.second) /* the second window's last group */
    gtk_window_close(t->editors.second);
}

void join_groups(App *t)
{
  GtkWidget *root = t->editors.roots[0];
  GtkNotebook *first = window_first_group(t, 0);
  GtkWidget *box = tab_group_widget(first);
  GPtrArray *groups = window_groups(t, 0);

  for (guint g = 1; g < groups->len; g++) {
    GtkNotebook *group = g_ptr_array_index(groups, g);
    while (gtk_notebook_get_n_pages(group) > 0)
      move_tab(tab_of(gtk_notebook_get_nth_page(group, 0)), first);
  }
  g_ptr_array_unref(groups);
  if (gtk_widget_get_parent(box) != root) {
    GtkWidget *parent = gtk_widget_get_parent(box), *focus = hold_focus(box);
    g_object_ref(box);
    if (gtk_paned_get_start_child(GTK_PANED(parent)) == box)
      gtk_paned_set_start_child(GTK_PANED(parent), NULL);
    else
      gtk_paned_set_end_child(GTK_PANED(parent), NULL);
    gtk_box_remove(GTK_BOX(root), gtk_widget_get_first_child(root)); /* the other groups go with it */
    gtk_box_append(GTK_BOX(root), box);
    g_object_unref(box);
    give_back_focus(focus);
  }
  t->editors.active = first;
  reorder(t);
  update_editor_area(t);
}

void prune_empty_groups(App *t, int window)
{
  GPtrArray *groups = window_groups(t, window);

  for (guint g = 0; g < groups->len; g++)
    if (gtk_notebook_get_n_pages(g_ptr_array_index(groups, g)) == 0)
      remove_group(g_ptr_array_index(groups, g)); /* not the last one left */
  g_ptr_array_unref(groups);
}

/* The layout as text ----------------------------------------------------------------------------- */

static void write_node(GtkWidget *node, GString *out)
{
  if (group_of_widget(node)) {
    g_string_append_c(out, 'g');
    return;
  }
  if (!GTK_IS_PANED(node))
    return;
  GtkPaned *paned = GTK_PANED(node);
  gboolean vertical = gtk_orientable_get_orientation(GTK_ORIENTABLE(paned)) == GTK_ORIENTATION_VERTICAL;
  double *share = g_object_get_data(G_OBJECT(paned), "t-code-share");
  int max = 0, permille;

  g_object_get(paned, "max-position", &max, NULL);
  if (max > 0 && max != G_MAXINT)
    permille = (int)(1000.0 * gtk_paned_get_position(paned) / max + 0.5);
  else
    permille = share ? (int)(1000 * *share + 0.5) : 500; /* not laid out yet */
  g_string_append_printf(out, "%c(%d,", vertical ? 'v' : 'h', CLAMP(permille, 1, 999));
  write_node(gtk_paned_get_start_child(paned), out);
  g_string_append_c(out, ',');
  write_node(gtk_paned_get_end_child(paned), out);
  g_string_append_c(out, ')');
}

char *groups_layout(App *t, int window)
{
  GString *out;

  if (!t->editors.roots[window])
    return NULL;
  out = g_string_new(NULL);
  write_node(gtk_widget_get_first_child(t->editors.roots[window]), out);
  return g_string_free(out, FALSE);
}

/* Reads one node at *p into a new (floating) widget tree, or NULL. */
static GtkWidget *read_node(App *t, const char **p, int depth)
{
  if (depth > 32)
    return NULL;
  if (**p == 'g') {
    (*p)++;
    return tab_group_widget(new_tab_group(t));
  }
  if ((**p != 'h' && **p != 'v') || (*p)[1] != '(')
    return NULL;
  gboolean vertical = **p == 'v';
  char *end;
  long permille = strtol(*p + 2, &end, 10);
  if (end == *p + 2 || *end != ',' || permille < 1 || permille > 999)
    return NULL;
  *p = end + 1;
  GtkPaned *paned = new_split(vertical, permille / 1000.0);
  g_object_ref_sink(paned);
  GtkWidget *start = read_node(t, p, depth + 1);
  if (start)
    gtk_paned_set_start_child(paned, start);
  GtkWidget *finish = start && **p == ',' ? ((*p)++, read_node(t, p, depth + 1)) : NULL;
  if (finish)
    gtk_paned_set_end_child(paned, finish);
  if (!finish || **p != ')') {
    g_object_unref(paned); /* with what it holds */
    return NULL;
  }
  (*p)++;
  g_object_force_floating(G_OBJECT(paned));
  return GTK_WIDGET(paned);
}

gboolean set_groups_layout(App *t, int window, const char *layout)
{
  GtkWidget *root = t->editors.roots[window], *tree;
  const char *p = layout;

  if (!root || !layout)
    return FALSE;
  GPtrArray *groups = window_groups(t, window);
  gboolean empty = TRUE;
  for (guint g = 0; g < groups->len; g++)
    empty = empty && gtk_notebook_get_n_pages(g_ptr_array_index(groups, g)) == 0;
  g_ptr_array_unref(groups);
  if (!empty)
    return FALSE;
  tree = read_node(t, &p, 0);
  if (!tree)
    return FALSE;
  if (*p) { /* more after it */
    g_object_ref_sink(tree);
    g_object_unref(tree);
    return FALSE;
  }
  GtkWidget *focus = hold_focus(gtk_widget_get_first_child(root));
  gtk_box_remove(GTK_BOX(root), gtk_widget_get_first_child(root));
  gtk_box_append(GTK_BOX(root), tree);
  give_back_focus(focus);
  reorder(t);
  t->editors.active = first_group(tree);
  update_editor_area(t);
  return TRUE;
}

/* The trees -------------------------------------------------------------------------------------- */

/* A window's root box with its first group. */
static GtkWidget *new_root(App *t, int window)
{
  GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  gtk_widget_set_hexpand(root, TRUE);
  gtk_widget_set_vexpand(root, TRUE);
  gtk_box_append(GTK_BOX(root), tab_group_widget(new_tab_group(t)));
  t->editors.roots[window] = root;
  reorder(t);
  return root;
}

void update_editor_area(App *t)
{
  GPtrArray *groups = window_groups(t, 0);
  gboolean tabs = groups->len > 1 || (groups->len && gtk_notebook_get_n_pages(g_ptr_array_index(groups, 0)) > 0);

  g_ptr_array_unref(groups);
  gtk_stack_set_visible_child_name(t->editors.area, tabs ? "tabs" : "empty");
}

/* The editor area: a hint while nothing is open, otherwise the groups. */
void create_groups(App *t)
{
  GtkWidget *hint = gtk_label_new("Open a file from the file manager on the left, or press Ctrl+P");
  GtkWidget *root;

  t->editors.groups = g_ptr_array_new();
  root = new_root(t, 0);
  t->editors.active = window_first_group(t, 0);
  t->editors.area = GTK_STACK(gtk_stack_new());
  gtk_widget_add_css_class(hint, "dim-label");
  /* Wrapped, so a narrow editor area does not need the whole sentence's width. */
  gtk_label_set_wrap(GTK_LABEL(hint), TRUE);
  gtk_label_set_justify(GTK_LABEL(hint), GTK_JUSTIFY_CENTER);
  gtk_stack_add_named(t->editors.area, hint, "empty");
  gtk_stack_add_named(t->editors.area, root, "tabs");
  gtk_stack_set_visible_child_name(t->editors.area, "empty");
  t->editors.view = drop_view_new(t, GTK_WIDGET(t->editors.area), 0);
}

GtkWidget *create_window_groups(App *t)
{
  GtkWidget *root = new_root(t, 1);

  return drop_view_new(t, root, 1);
}

void forget_window_groups(App *t)
{
  t->editors.roots[1] = NULL;
  reorder(t);
}
