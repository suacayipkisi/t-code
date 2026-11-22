/*
 * Trees of splits (see splits.h).
 */
#include "window/splits.h"

#include <stdlib.h>

GtkWidget *split_root_new(void)
{
  GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  gtk_widget_set_hexpand(root, TRUE);
  gtk_widget_set_vexpand(root, TRUE);
  return root;
}

GtkWidget *split_root_child(GtkWidget *root)
{
  return gtk_widget_get_first_child(root);
}

GtkWidget *split_hold_focus(GtkWidget *node)
{
  GtkRoot *root = gtk_widget_get_root(node);
  GtkWidget *focus = GTK_IS_WINDOW(root) ? gtk_window_get_focus(GTK_WINDOW(root)) : NULL;

  if (!focus)
    return NULL;
  g_object_ref(focus);
  gtk_window_set_focus(GTK_WINDOW(root), NULL);
  return focus;
}

void split_give_back_focus(GtkWidget *focus)
{
  if (!focus)
    return;
  if (gtk_widget_get_root(focus))
    gtk_widget_grab_focus(focus);
  g_object_unref(focus);
}

/* The first side keeps its share once the split has a size of its own. */
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

/* A split: start over end (vertical) or left of it; share: the start side's part (0: half, GtkPaned's
 * own choice while its position was never set). */
static GtkPaned *new_split(gboolean vertical, double share)
{
  GtkPaned *paned = GTK_PANED(gtk_paned_new(vertical ? GTK_ORIENTATION_VERTICAL : GTK_ORIENTATION_HORIZONTAL));

  gtk_widget_set_hexpand(GTK_WIDGET(paned), TRUE);
  gtk_widget_set_vexpand(GTK_WIDGET(paned), TRUE);
  g_object_set_data(G_OBJECT(paned), "t-code-split", GINT_TO_POINTER(1));
  if (share > 0) {
    double *kept = g_new(double, 1);
    *kept = share;
    g_object_set_data_full(G_OBJECT(paned), "t-code-share", kept, g_free);
    g_signal_connect(paned, "notify::max-position", G_CALLBACK(on_max_position), NULL);
  }
  return paned;
}

static gboolean is_split(GtkWidget *widget)
{
  return GTK_IS_PANED(widget) && g_object_get_data(G_OBJECT(widget), "t-code-split");
}

/* Puts replacement where node is (the caller keeps node alive meanwhile if it still wants it). */
static void replace_node(GtkWidget *node, GtkWidget *replacement)
{
  GtkWidget *parent = gtk_widget_get_parent(node);

  if (is_split(parent)) {
    if (gtk_paned_get_start_child(GTK_PANED(parent)) == node)
      gtk_paned_set_start_child(GTK_PANED(parent), replacement);
    else
      gtk_paned_set_end_child(GTK_PANED(parent), replacement);
  } else { /* the root box */
    gtk_box_remove(GTK_BOX(parent), node);
    if (replacement)
      gtk_box_append(GTK_BOX(parent), replacement);
  }
}

void split_beside(GtkWidget *root, GtkWidget *node, GtkWidget *leaf, SplitSide side, double share)
{
  gboolean before = SPLIT_BEFORE(side);
  GtkPaned *paned;
  GtkWidget *focus;

  if (!node) {
    gtk_box_append(GTK_BOX(root), leaf);
    return;
  }
  focus = split_hold_focus(node);
  paned = new_split(SPLIT_VERTICAL(side), share > 0 ? (before ? share : 1 - share) : 0);
  g_object_ref(node);
  replace_node(node, GTK_WIDGET(paned));
  gtk_paned_set_start_child(paned, before ? leaf : node);
  gtk_paned_set_end_child(paned, before ? node : leaf);
  g_object_unref(node);
  split_give_back_focus(focus);
}

GtkPaned *split_parent(GtkWidget *leaf)
{
  GtkWidget *parent = gtk_widget_get_parent(leaf);

  return is_split(parent) ? GTK_PANED(parent) : NULL;
}

GtkWidget *split_other(GtkWidget *leaf)
{
  GtkPaned *split = split_parent(leaf);

  if (!split)
    return NULL;
  return gtk_paned_get_start_child(split) == leaf ? gtk_paned_get_end_child(split) : gtk_paned_get_start_child(split);
}

void split_remove(GtkWidget *leaf)
{
  GtkPaned *split = split_parent(leaf);
  GtkWidget *other, *focus = split_hold_focus(leaf);

  if (!split) {
    replace_node(leaf, NULL);
    split_give_back_focus(focus);
    return;
  }
  other = split_other(leaf);
  g_object_ref(other);
  if (gtk_paned_get_start_child(split) == other)
    gtk_paned_set_start_child(split, NULL);
  else
    gtk_paned_set_end_child(split, NULL);
  replace_node(GTK_WIDGET(split), other); /* the split goes, and the leaf with it */
  g_object_unref(other);
  split_give_back_focus(focus);
}

void split_collect(GtkWidget *node, gboolean (*is_leaf)(GtkWidget *widget), GPtrArray *out)
{
  if (!node)
    return;
  if (is_leaf(node)) {
    g_ptr_array_add(out, node);
  } else if (is_split(node)) {
    split_collect(gtk_paned_get_start_child(GTK_PANED(node)), is_leaf, out);
    split_collect(gtk_paned_get_end_child(GTK_PANED(node)), is_leaf, out);
  } else {
    for (GtkWidget *child = gtk_widget_get_first_child(node); child; child = gtk_widget_get_next_sibling(child))
      split_collect(child, is_leaf, out);
  }
}

static void write_node(GtkWidget *node, gboolean (*is_leaf)(GtkWidget *widget), char letter, GString *out)
{
  if (is_leaf(node)) {
    g_string_append_c(out, letter);
    return;
  }
  if (!is_split(node))
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
  write_node(gtk_paned_get_start_child(paned), is_leaf, letter, out);
  g_string_append_c(out, ',');
  write_node(gtk_paned_get_end_child(paned), is_leaf, letter, out);
  g_string_append_c(out, ')');
}

char *split_write(GtkWidget *root, gboolean (*is_leaf)(GtkWidget *widget), char letter)
{
  GtkWidget *node = split_root_child(root);
  GString *out;

  if (!node)
    return NULL;
  out = g_string_new(NULL);
  write_node(node, is_leaf, letter, out);
  return g_string_free(out, FALSE);
}

/* Reads one node at *p into a new (floating) widget tree, or NULL. */
static GtkWidget *read_node(const char **p, int depth, char letter, GtkWidget *(*new_leaf)(gpointer data),
                            gpointer data)
{
  if (depth > 32)
    return NULL;
  if (**p == letter) {
    (*p)++;
    return new_leaf(data);
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
  GtkWidget *start = read_node(p, depth + 1, letter, new_leaf, data);
  if (start)
    gtk_paned_set_start_child(paned, start);
  GtkWidget *finish = start && **p == ',' ? ((*p)++, read_node(p, depth + 1, letter, new_leaf, data)) : NULL;
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

gboolean split_read(GtkWidget *root, const char *text, char letter, GtkWidget *(*new_leaf)(gpointer data),
                    gpointer data)
{
  const char *p = text;
  GtkWidget *tree = text ? read_node(&p, 0, letter, new_leaf, data) : NULL, *old, *focus;

  if (!tree)
    return FALSE;
  if (*p) { /* more after it */
    g_object_ref_sink(tree);
    g_object_unref(tree);
    return FALSE;
  }
  old = split_root_child(root);
  focus = old ? split_hold_focus(old) : NULL;
  if (old)
    gtk_box_remove(GTK_BOX(root), old);
  gtk_box_append(GTK_BOX(root), tree);
  split_give_back_focus(focus);
  return TRUE;
}
