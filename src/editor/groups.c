/*
 * The tab groups' trees (see tabs.h): each window's groups are a tree of splits (window/splits.h)
 * whose leaves are the groups' widgets (tab_group_widget). Splitting a group puts a new split in its
 * place; a group that goes leaves the other side of its split in the split's place.
 */
#include "editor/tabs.h"

#include "editor/dropzone.h"

/* The groups, in order --------------------------------------------------------------------------- */

static gboolean is_group(GtkWidget *widget)
{
  return group_of_widget(widget) != NULL;
}

/* The groups under node, in order. */
static GPtrArray *groups_under(GtkWidget *node)
{
  GPtrArray *widgets = g_ptr_array_new(), *groups = g_ptr_array_new();

  split_collect(node, is_group, widgets);
  for (guint i = 0; i < widgets->len; i++)
    g_ptr_array_add(groups, group_of_widget(g_ptr_array_index(widgets, i)));
  g_ptr_array_unref(widgets);
  return groups;
}

/* Makes t->editors.groups follow the trees again (after a split or a group going). */
static void reorder(App *t)
{
  g_ptr_array_set_size(t->editors.groups, 0);
  for (int w = 0; w < 2; w++)
    if (t->editors.roots[w])
      g_ptr_array_extend_and_steal(t->editors.groups, groups_under(t->editors.roots[w]));
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
  GPtrArray *groups = groups_under(node);
  GtkNotebook *group = groups->len ? g_ptr_array_index(groups, 0) : NULL;

  g_ptr_array_unref(groups);
  return group;
}

GtkNotebook *window_first_group(App *t, int window)
{
  return t->editors.roots[window] ? first_group(t->editors.roots[window]) : NULL;
}

/* The groups of a window, in order. */
static GPtrArray *window_groups(App *t, int window)
{
  return t->editors.roots[window] ? groups_under(t->editors.roots[window]) : g_ptr_array_new();
}

/* Splitting -------------------------------------------------------------------------------------- */

/* A new group beside node (a group or a whole tree), taking share of the space (0: half). */
static GtkNotebook *split_node(App *t, GtkWidget *root, GtkWidget *node, SplitSide side, double share)
{
  GtkNotebook *group = new_tab_group(t);

  split_beside(root, node, tab_group_widget(group), side, share);
  reorder(t);
  t->editors.active = group;
  update_editor_area(t);
  return group;
}

GtkNotebook *split_group(GtkNotebook *group, SplitSide side)
{
  App *t = g_object_get_data(G_OBJECT(group), "t-code-app");

  return split_node(t, t->editors.roots[group_window(t, group)], tab_group_widget(group), side, 0);
}

GtkNotebook *split_window(App *t, int window, SplitSide side)
{
  GtkWidget *root = t->editors.roots[window];

  return root ? split_node(t, root, split_root_child(root), side, 1.0 / 3) : NULL;
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

/* Takes an empty group out of its tree. FALSE: it is its window's only group. */
static gboolean remove_group(GtkNotebook *group)
{
  App *t = g_object_get_data(G_OBJECT(group), "t-code-app");
  GtkWidget *other = split_other(tab_group_widget(group));
  gboolean was_active = t->editors.active == group;

  if (!other)
    return FALSE;
  g_object_ref(other);
  split_remove(tab_group_widget(group)); /* the group goes with its split */
  if (was_active)
    t->editors.active = first_group(other);
  g_object_unref(other);
  reorder(t);
  update_editor_area(t);
  return TRUE;
}

void close_group(GtkNotebook *group)
{
  App *t = g_object_get_data(G_OBJECT(group), "t-code-app");
  GtkWidget *other = split_other(tab_group_widget(group));
  GtkNotebook *to = other ? first_group(other) : NULL; /* the first one of the other side of its split */

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
  if (split_root_child(root) != box) {
    GtkWidget *focus = split_hold_focus(root);
    g_object_ref(box);
    split_remove(box);
    gtk_box_remove(GTK_BOX(root), split_root_child(root)); /* the other groups go with it */
    gtk_box_append(GTK_BOX(root), box);
    g_object_unref(box);
    split_give_back_focus(focus);
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

char *groups_layout(App *t, int window)
{
  return t->editors.roots[window] ? split_write(t->editors.roots[window], is_group, 'g') : NULL;
}

static GtkWidget *new_group_widget(gpointer data)
{
  return tab_group_widget(new_tab_group(data));
}

gboolean set_groups_layout(App *t, int window, const char *layout)
{
  GtkWidget *root = t->editors.roots[window];
  GPtrArray *groups;
  gboolean empty = TRUE;

  if (!root || !layout)
    return FALSE;
  groups = window_groups(t, window);
  for (guint g = 0; g < groups->len; g++)
    empty = empty && gtk_notebook_get_n_pages(g_ptr_array_index(groups, g)) == 0;
  g_ptr_array_unref(groups);
  if (!empty || !split_read(root, layout, 'g', new_group_widget, t))
    return FALSE;
  reorder(t);
  t->editors.active = window_first_group(t, window);
  update_editor_area(t);
  return TRUE;
}

/* The trees -------------------------------------------------------------------------------------- */

/* A window's root box with its first group. */
static GtkWidget *new_root(App *t, int window)
{
  GtkWidget *root = split_root_new();

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
