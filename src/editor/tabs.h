/*
 * Editor tabs, in tab groups (each a GtkNotebook under its own tab strip). The groups of a window
 * form a tree of splits (groups.c): any number, side by side or one over another; the second
 * window has a tree of its own. A tab is an editor (editor.h) or a Markdown preview (preview.h). A
 * file is never open in two groups at once: two editors would overwrite each other's saves.
 */
#pragma once

#include "app.h"

/* Finding tabs ------------------------------------------------------------------------------- */

/* The tab a notebook page belongs to. */
Tab *tab_of(GtkWidget *page);

/* Tabs in all groups. */
int tab_count(App *t);

/* Tab 0 .. tab_count() - 1, the first group's first. */
Tab *tab_nth(App *t, int index);

/* The current tab of the active group (or else of the other one); NULL when none is open. */
Tab *current_tab(App *t);

/* The tab of that kind showing path, or NULL. */
Tab *find_tab(App *t, TabKind kind, const char *path);

/* The group a tab is in. */
GtkNotebook *tab_group(Tab *tab);

/* The window a tab is in: the main one, or the second window. Its dialogs go over it. */
GtkWindow *tab_window(Tab *tab);

/* Whether an editor has unsaved changes. */
gboolean any_unsaved(App *t);

/* Adding, moving, closing -------------------------------------------------------------------- */

/* Adds page as a tab of the active group, right after its current tab, and switches to it. The
 * Tab is freed with the page. */
Tab *add_tab(App *t, TabKind kind, GtkWidget *page, const char *path);

/* The group's temporary tab (a click in the Files panel opened it), or NULL. */
Tab *temporary_tab(App *t, GtkNotebook *group);

/* Keeps a temporary tab: the next click in the Files panel opens another tab instead of replacing
 * it. */
void keep_tab(Tab *tab);

/* Puts right (an old version) next to left in left's notebook page: one tab, both sides; it closes
 * with left. */
void join_tabs(Tab *left, Tab *right);

/* Takes a comparison apart: its right side closes, left is a tab of its own again. */
void part_tabs(Tab *left);

/* A tab's position in its group, and moving it to another one there. */
int tab_position(Tab *tab);
void set_tab_position(Tab *tab, int position);

/* Moves a tab (with its editor) to the end of another group. */
void move_tab(Tab *tab, GtkNotebook *group);

/* Takes the tab out of its group at once, unsaved changes or not; a comparison goes with both
 * sides. Closing an editor the normal way goes through
 * quit_editor(), which asks about unsaved changes. */
void remove_tab(Tab *tab);

/* Removes every tab without asking (a project switch, after asking); the second window goes too. */
void close_all_tabs(App *t);

/* The label: the file name, "●" when unsaved, "Preview" for previews, in italics while temporary;
 * the path as tooltip. */
void update_tab_label(Tab *tab);

/* Focus -------------------------------------------------------------------------------------- */

/* Makes tab the current one of its group, that group the active one, and gives it the keyboard. */
void select_tab(Tab *tab);

/* The next (step 1) or previous (step -1) tab in the current group, going round. */
void step_tab(App *t, int step);

/* Gives the keyboard to the current tab, or to the file manager when there is none. */
void focus_current_tab(App *t);

/* Makes a group the active one and focuses its tab (NULL: nothing happens). */
void focus_group(App *t, GtkNotebook *group);

/* A group --------------------------------------------------------------------------------------- */

/* A new group's notebook, under its tab strip (groups.c puts it into a tree). */
GtkNotebook *new_tab_group(App *t);

/* The group's widget in its tree: its tab strip over its notebook. */
GtkWidget *tab_group_widget(GtkNotebook *group);

/* The group of a tree node made by tab_group_widget(), or NULL for any other widget. */
GtkNotebook *group_of_widget(GtkWidget *widget);

/* Where to insert a tab dropped on the group's strip at x (in strip_widget's coordinates). */
int strip_drop_position(GtkNotebook *group, double x, double *line_x);

/* The scrolled window holding the group's strip. */
GtkWidget *strip_widget(GtkNotebook *group);

/* The group trees (groups.c) -------------------------------------------------------------------- */

typedef enum { SPLIT_LEFT, SPLIT_RIGHT, SPLIT_UP, SPLIT_DOWN } SplitSide;

/* The main window's tree with its first group, and t->editors.area / view: the hint while no tab is
 * open, the drop markers over it. */
void create_groups(App *t);

/* The second window's tree with its first group; returns what the window holds. */
GtkWidget *create_window_groups(App *t);

/* The second window is going: its groups are no longer counted. */
void forget_window_groups(App *t);

/* The groups, in order (the main window's first, each tree left to right and top to bottom). */
int group_count(App *t);
GtkNotebook *group_nth(App *t, int index);
int group_index(App *t, GtkNotebook *group);

/* The window a group is in: 0 the main one, 1 the second. */
int group_window(App *t, GtkNotebook *group);

/* A window's first group (NULL: no second window). */
GtkNotebook *window_first_group(App *t, int window);

/* A new empty group next to group (or, for split_window, along one edge of a whole window's tree,
 * taking a third of it); it becomes the active one. */
GtkNotebook *split_group(GtkNotebook *group, SplitSide side);
GtkNotebook *split_window(App *t, int window, SplitSide side);

/* Where "to the side" is: the next group in the active one's window, else the one before, else a new
 * group right of it. */
GtkNotebook *side_group(App *t);

/* Moves the group's tabs to its neighbour and takes it away (not a window's only group). */
void close_group(GtkNotebook *group);

/* Takes the group away once it has no tabs (its window's only one stays; the second window closes). */
void close_group_if_empty(GtkNotebook *group);

/* Puts every tab of the main window into its first group, which becomes the only one. */
void join_groups(App *t);

/* A window's tree as text, "h(500,g,v(333,g,g))": h / v a split side by side / one over another with
 * the first side's share in thousandths, g a group (in group order). NULL: no such window. */
char *groups_layout(App *t, int window);

/* Gives a window whose groups have no tabs the tree layout describes, with empty groups; FALSE (and
 * nothing changed) when layout is not one. */
gboolean set_groups_layout(App *t, int window, const char *layout);

/* Takes away the window's groups that have no tab (one always stays). */
void prune_empty_groups(App *t, int window);

/* Shows the tabs or the hint, whichever fits now. */
void update_editor_area(App *t);
