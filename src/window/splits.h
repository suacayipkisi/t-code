/*
 * Trees of splits, for the editor groups (editor/groups.c) and the docks (window/docks/): a root box
 * holds one node, a leaf (a group, a dock) or a split, a GtkPaned whose two sides are again leaves or
 * splits. The widgets are the tree: nothing else keeps it. As text a tree reads "h(500,x,v(333,x,x))":
 * h / v a split side by side / one over another with its first side's share in thousandths, and a
 * letter for each leaf.
 */
#pragma once

#include <gtk/gtk.h>

typedef enum { SPLIT_LEFT, SPLIT_RIGHT, SPLIT_UP, SPLIT_DOWN } SplitSide;

/* Whether a leaf goes before (left of, over) what it splits from. */
#define SPLIT_BEFORE(side) ((side) == SPLIT_LEFT || (side) == SPLIT_UP)
#define SPLIT_VERTICAL(side) ((side) == SPLIT_UP || (side) == SPLIT_DOWN)

/* A root box for a tree (empty). */
GtkWidget *split_root_new(void);

/* The node a root holds, or NULL. */
GtkWidget *split_root_child(GtkWidget *root);

/* Puts leaf beside node (a leaf or a split, the root's child or deeper) in a new split; leaf takes
 * share of it (0: half). Into an empty root when node is NULL. */
void split_beside(GtkWidget *root, GtkWidget *node, GtkWidget *leaf, SplitSide side, double share);

/* Takes leaf out of its tree: the other side of its split takes the split's place (leaf goes with the
 * split unless the caller holds a reference); a root's only node leaves the root empty. */
void split_remove(GtkWidget *leaf);

/* The split leaf is a side of (NULL: the root's), and the other side of it. */
GtkPaned *split_parent(GtkWidget *leaf);
GtkWidget *split_other(GtkWidget *leaf);

/* Adds the leaves under node to out in order (the first side of each split first); is_leaf tells
 * them. */
void split_collect(GtkWidget *node, gboolean (*is_leaf)(GtkWidget *widget), GPtrArray *out);

/* The tree under root as text (NULL when the root is empty). */
char *split_write(GtkWidget *root, gboolean (*is_leaf)(GtkWidget *widget), char letter);

/* Gives root the tree text describes, a leaf made by new_leaf for each letter (in order); FALSE (and
 * the root as it was) when text is not one. What the root held goes. */
gboolean split_read(GtkWidget *root, const char *text, char letter, GtkWidget *(*new_leaf)(gpointer data),
                    gpointer data);

/* A GtkPaned remembers which side had the focus last and warns when that side leaves it while the
 * window's focus is elsewhere: the window has no focus while a tree changes (hold) and gets it back
 * after (if its widget is still in a window). */
GtkWidget *split_hold_focus(GtkWidget *node);
void split_give_back_focus(GtkWidget *focus);
