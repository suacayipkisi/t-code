/*
 * The Run and Debug panel (see debugview.h). A pause asks gdb for the shown frame's scopes, then each
 * scope's variables (Arguments, then Locals, then Registers as one closed row); a variable with a
 * variablesReference has children, asked for when its row is shown, and at once when gdb gave no value
 * for it (an array, a struct): its value is then made from theirs, "{70, 85, 90}". Watch expressions
 * are evaluated in the shown frame at each pause (evaluate, context "watch") and become rows the same
 * way. Every answer carries the pause it was asked in (generation): answers from before a step are
 * dropped. variables.c, watch.c, stack.c and breakpoints.c have the sections (state.h).
 */
#include "debug/view/debugview.h"
#include "debug/view/state.h"

#include "popups/contextmenu.h"
#include "window/palette.h"

DebugView view;

static const char *const css =
    ".tcode-debug-view { background: var(--tc-panel); }"
    ".tcode-debug-view .section { font-size: 0.8em; font-weight: bold; color: var(--tc-fg); padding: 4px 10px; }"
    ".tcode-debug-view .note { color: var(--tc-fg-dim); margin: 10px; }"
    ".tcode-debug-view listview, .tcode-debug-view list { background: none; }"
    ".tcode-debug-view .name { color: var(--tc-variable); }"
    ".tcode-debug-view .value { color: var(--tc-fg); }"
    ".tcode-debug-view .value.changed { color: var(--tc-warning); font-weight: bold; }"
    ".tcode-debug-view .where { color: var(--tc-fg-dim); font-size: 0.9em; }"
    ".tcode-debug-view row.no-source .function { color: var(--tc-fg-faint); }"
    ".tcode-debug-view .value.missing, .tcode-debug-view .condition { color: var(--tc-fg-faint); }"
    ".tcode-debug-view .heading button { min-height: 20px; min-width: 20px; padding: 0 4px; margin-right: 6px; }"
    ".tcode-debug-view .remove { min-height: 18px; min-width: 18px; padding: 0; opacity: 0.6; }"
    ".tcode-debug-view .remove:hover { opacity: 1; }"
    ".tcode-debug-view entry { margin: 2px 10px 4px 10px; }";

/* From the debugger -------------------------------------------------------------------------- */

void debug_view_paused(App *t G_GNUC_UNUSED, JsonNode *frames, int shown)
{
  GHashTable *older = view.last;

  if (!view.root)
    return;
  view.last = view.now; /* changed values are those that differ from the last pause's */
  view.now = older;
  g_hash_table_remove_all(view.now);
  g_clear_pointer(&view.frames, json_node_unref);
  view.frames = frames && JSON_NODE_HOLDS_ARRAY(frames) ? json_node_copy(frames) : NULL;
  view.all_frames = FALSE;
  stack_fill();
  var_show_note(NULL);
  var_list_frame(MAX(shown, 0));
}

/* The last pause's frames and variables go (older answers are dropped); the watch rows follow. */
static void forget_pause(void)
{
  view.generation++;
  g_list_store_remove_all(view.root);
  context_menu_clear_list(view.stack);
  g_clear_pointer(&view.frames, json_node_unref);
  watch_evaluate();
}

void debug_view_running(App *t G_GNUC_UNUSED)
{
  if (!view.root)
    return;
  forget_pause();
  var_show_note("Running… The variables show here when the program pauses: at a breakpoint, after a step, or with "
                "Pause (F6).");
}

void debug_view_ended(App *t G_GNUC_UNUSED)
{
  if (!view.root)
    return;
  g_hash_table_remove_all(view.last); /* a value changed against the last run's is not shown changed */
  g_hash_table_remove_all(view.now);
  forget_pause();
  var_show_note("Not debugging.\n\nClick left of a line number (or F9) to put a breakpoint there, then F5: the program "
                "pauses at that line, and its variables show here.");
}

/* store's rows as "name = value" lines, "*" after a changed one. */
static char *text_of(GListStore *store)
{
  GString *text = g_string_new("");
  guint n = store ? g_list_model_get_n_items(G_LIST_MODEL(store)) : 0;

  for (guint i = 0; i < n; i++) {
    TcodeVar *var = g_list_model_get_item(G_LIST_MODEL(store), i);
    g_string_append_printf(text, "%s = %s%s\n", var->name, var->value, var->changed ? "*" : "");
    g_object_unref(var);
  }
  return g_string_free(text, FALSE);
}

char *debug_view_variables_text(void)
{
  return text_of(view.root);
}

char *debug_view_watch_text(void)
{
  return text_of(view.watch);
}

char *debug_view_stack_text(void)
{
  GString *text = g_string_new("");

  for (GtkWidget *row = view.stack ? gtk_widget_get_first_child(GTK_WIDGET(view.stack)) : NULL; row;
       row = gtk_widget_get_next_sibling(row)) {
    GtkWidget *box = gtk_list_box_row_get_child(GTK_LIST_BOX_ROW(row));
    g_string_append_printf(text, "%s %s\n", gtk_label_get_text(GTK_LABEL(gtk_widget_get_first_child(box))),
                           gtk_label_get_text(GTK_LABEL(gtk_widget_get_last_child(box))));
  }
  return g_string_free(text, FALSE);
}

/* A section's heading, with a button on its right (icon NULL: none). */
static GtkWidget *heading(const char *text, const char *icon, const char *tip, GCallback clicked)
{
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0), *label = gtk_label_new(text);

  gtk_label_set_xalign(GTK_LABEL(label), 0);
  gtk_widget_set_hexpand(label, TRUE);
  gtk_widget_add_css_class(label, "section");
  gtk_widget_add_css_class(box, "heading");
  gtk_box_append(GTK_BOX(box), label);
  if (icon) {
    GtkWidget *button = gtk_button_new_from_icon_name(icon);
    gtk_widget_add_css_class(button, "flat");
    gtk_widget_set_tooltip_text(button, tip);
    g_signal_connect(button, "clicked", clicked, NULL);
    gtk_box_append(GTK_BOX(box), button);
  }
  return box;
}

/* A list view of store's rows (and their children), made by the row functions above. */
static GtkWidget *var_list(GListStore *store, gboolean watch)
{
  GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
  GtkTreeListModel *tree =
      gtk_tree_list_model_new(G_LIST_MODEL(g_object_ref(store)), FALSE, FALSE, var_children_of, NULL, NULL);

  g_signal_connect(factory, "setup", G_CALLBACK(var_setup_row), GINT_TO_POINTER(watch));
  g_signal_connect(factory, "bind", G_CALLBACK(var_bind_row), NULL);
  g_signal_connect(factory, "unbind", G_CALLBACK(var_unbind_row), NULL);
  return gtk_list_view_new(GTK_SELECTION_MODEL(gtk_no_selection_new(G_LIST_MODEL(tree))), factory);
}

static void on_view_destroyed(GtkWidget *widget G_GNUC_UNUSED, gpointer data G_GNUC_UNUSED)
{
  view.note = view.variables = view.watch_entry = NULL;
  view.stack = view.breakpoints = NULL;
  g_clear_object(&view.root);
  g_clear_object(&view.watch);
  g_clear_pointer(&view.watches, g_ptr_array_unref);
  g_clear_pointer(&view.frames, json_node_unref);
  g_clear_pointer(&view.last, g_hash_table_destroy);
  g_clear_pointer(&view.now, g_hash_table_destroy);
}

GtkWidget *debug_view(App *t)
{
  GtkWidget *panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
  GtkWidget *top = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0), *rest = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *rest_scroll = gtk_scrolled_window_new();
  GtkEventController *keys = gtk_event_controller_key_new();

  palette_add_css(css);
  view.app = t;
  view.root = g_list_store_new(TCODE_TYPE_VAR);
  view.watch = g_list_store_new(TCODE_TYPE_VAR);
  view.watches = g_ptr_array_new_with_free_func(g_free);
  view.last = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  view.now = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  gtk_widget_add_css_class(panel, "tcode-debug-view");

  /* Variables, or a note while there are none */
  view.variables = gtk_scrolled_window_new();
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(view.variables), var_list(view.root, FALSE));
  gtk_widget_set_vexpand(view.variables, TRUE);
  view.note = gtk_label_new(NULL);
  gtk_label_set_wrap(GTK_LABEL(view.note), TRUE);
  gtk_label_set_xalign(GTK_LABEL(view.note), 0);
  gtk_widget_set_valign(view.note, GTK_ALIGN_START);
  gtk_widget_set_vexpand(view.note, TRUE);
  gtk_widget_add_css_class(view.note, "note");
  gtk_box_append(GTK_BOX(top), heading("VARIABLES", NULL, NULL, NULL));
  gtk_box_append(GTK_BOX(top), view.note);
  gtk_box_append(GTK_BOX(top), view.variables);

  /* Watch, Call Stack, Breakpoints */
  view.watch_entry = gtk_entry_new();
  gtk_entry_set_placeholder_text(GTK_ENTRY(view.watch_entry), "An expression: a[i], sum / n, *p");
  gtk_widget_set_visible(view.watch_entry, FALSE);
  g_signal_connect(view.watch_entry, "activate", G_CALLBACK(watch_on_entry), NULL);
  g_signal_connect(keys, "key-pressed", G_CALLBACK(watch_on_entry_key), view.watch_entry);
  gtk_widget_add_controller(view.watch_entry, keys);
  gtk_box_append(GTK_BOX(rest),
                 heading("WATCH", "list-add-symbolic", "Add an expression to watch", G_CALLBACK(watch_on_add)));
  gtk_box_append(GTK_BOX(rest), view.watch_entry);
  gtk_box_append(GTK_BOX(rest), var_list(view.watch, TRUE));

  view.stack = GTK_LIST_BOX(gtk_list_box_new());
  g_signal_connect(view.stack, "row-activated", G_CALLBACK(stack_on_frame_activated), NULL);
  context_menu_attach(GTK_WIDGET(view.stack), stack_on_menu, NULL);
  gtk_box_append(GTK_BOX(rest), heading("CALL STACK", NULL, NULL, NULL));
  gtk_box_append(GTK_BOX(rest), GTK_WIDGET(view.stack));

  view.breakpoints = GTK_LIST_BOX(gtk_list_box_new());
  gtk_list_box_set_selection_mode(view.breakpoints, GTK_SELECTION_NONE);
  g_signal_connect(view.breakpoints, "row-activated", G_CALLBACK(bplist_on_activated), NULL);
  context_menu_attach(GTK_WIDGET(view.breakpoints), bplist_on_menu, NULL);
  gtk_box_append(GTK_BOX(rest), heading("BREAKPOINTS", "edit-clear-all-symbolic", "Remove all breakpoints",
                                        G_CALLBACK(bplist_on_remove_all)));
  gtk_box_append(GTK_BOX(rest), GTK_WIDGET(view.breakpoints));
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(rest_scroll), rest);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(rest_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);

  gtk_paned_set_start_child(GTK_PANED(paned), top);
  gtk_paned_set_end_child(GTK_PANED(paned), rest_scroll);
  gtk_paned_set_position(GTK_PANED(paned), 300);
  gtk_widget_set_vexpand(paned, TRUE);
  gtk_box_append(GTK_BOX(panel), paned);
  g_signal_connect(panel, "destroy", G_CALLBACK(on_view_destroyed), NULL);
  debug_view_ended(t);
  debug_view_breakpoints_changed(t);
  return panel;
}
