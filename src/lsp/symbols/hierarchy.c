/*
 * The Hierarchy panel (see hierarchy.h).
 */
#include "lsp/symbols/hierarchy.h"

#include "editor/editor.h"
#include "editor/tabs.h"
#include "lsp/client/client.h"
#include "lsp/json.h"
#include "lsp/symbols/symbols.h"
#include "window/window.h"
#include "window/palette.h"

/* A symbol in the tree ------------------------------------------------------------------------ */

struct _TcodeCall {
  GObject parent;
  char *name, *where; /* "draw", "shape.c:12" */
  int kind;
  char *path;
  int line, column; /* of its name, 0-based */
  JsonNode *item;   /* the server's CallHierarchyItem / TypeHierarchyItem, sent back for the children */
  GListStore *children;
  gboolean asked;
};

#define TCODE_TYPE_CALL (tcode_call_get_type())
G_DECLARE_FINAL_TYPE(TcodeCall, tcode_call, TCODE, CALL, GObject)
G_DEFINE_FINAL_TYPE(TcodeCall, tcode_call, G_TYPE_OBJECT)

static void tcode_call_finalize(GObject *object)
{
  TcodeCall *self = TCODE_CALL(object);

  g_free(self->name);
  g_free(self->where);
  g_free(self->path);
  if (self->item)
    json_node_unref(self->item);
  g_clear_object(&self->children);
  G_OBJECT_CLASS(tcode_call_parent_class)->finalize(object);
}

static void tcode_call_class_init(TcodeCallClass *klass)
{
  G_OBJECT_CLASS(klass)->finalize = tcode_call_finalize;
}

static void tcode_call_init(TcodeCall *self G_GNUC_UNUSED)
{
}

static struct {
  App *app;
  GtkLabel *title, *note;
  GtkToggleButton *up, *down;
  GListStore *roots;
  GtkTreeListModel *tree;
  GtkListView *list;
  char *origin;    /* the editor asked from: its server answers about the children too */
  JsonNode *items; /* what the prepare request found */
  gboolean types;  /* types: supertypes / subtypes; else callers / callees */
  gboolean going_up;
  guint generation; /* a new tree: older answers are dropped */
} hier;

static TcodeCall *hier_call_new(JsonNode *item)
{
  TcodeCall *call = g_object_new(TCODE_TYPE_CALL, NULL);
  const char *uri = json_string_at(item, "uri");
  char *base;

  call->item = json_node_copy(item);
  call->name = g_strdup(json_string_at(item, "name") ?: "");
  call->kind = (int)json_int_at(item, "kind", 0);
  call->path = uri ? lsp_uri_to_path(uri) : NULL;
  call->line = (int)json_int_at(item, "selectionRange.start.line", 0);
  call->column = (int)json_int_at(item, "selectionRange.start.character", 0);
  base = call->path ? g_path_get_basename(call->path) : g_strdup("");
  call->where = g_strdup_printf("%s:%d", base, call->line + 1);
  g_free(base);
  call->children = g_list_store_new(TCODE_TYPE_CALL);
  return call;
}

/* Asking for children ------------------------------------------------------------------------- */

typedef struct {
  TcodeCall *call; /* a reference */
  guint generation;
} HierAsked;

static void hier_on_children(JsonNode *result, gpointer data)
{
  HierAsked *a = data;
  JsonArray *array = result && JSON_NODE_HOLDS_ARRAY(result) ? json_node_get_array(result) : NULL;

  for (guint i = 0; a->generation == hier.generation && array && i < json_array_get_length(array); i++) {
    JsonNode *entry = json_array_get_element(array, i);
    /* incomingCalls: {from, fromRanges}; outgoingCalls: {to, fromRanges}; type hierarchy: items. */
    JsonNode *item = json_at(entry, "from") ? json_at(entry, "from")
                     : json_at(entry, "to") ? json_at(entry, "to")
                                            : entry;
    TcodeCall *child = hier_call_new(item);
    g_list_store_append(a->call->children, child);
    g_object_unref(child);
  }
  g_object_unref(a->call);
  g_free(a);
}

static void hier_ask_children(TcodeCall *call)
{
  Tab *tab = hier.origin && hier.app ? find_tab(hier.app, TAB_EDITOR, hier.origin) : NULL;
  const char *method = hier.types ? (hier.going_up ? "typeHierarchy/supertypes" : "typeHierarchy/subtypes")
                                  : (hier.going_up ? "callHierarchy/incomingCalls" : "callHierarchy/outgoingCalls");
  char *item, *params;
  HierAsked *a;

  if (call->asked || !tab)
    return;
  call->asked = TRUE;
  a = g_new0(HierAsked, 1);
  a->call = g_object_ref(call);
  a->generation = hier.generation;
  item = json_to_string(call->item, FALSE);
  params = g_strdup_printf("{\"item\":%s}", item);
  lsp_request(tab, method, params, NULL, hier_on_children, a);
  g_free(params);
  g_free(item);
}

static GListModel *hier_children_of(gpointer item, gpointer data G_GNUC_UNUSED)
{
  TcodeCall *call = item;

  hier_ask_children(call); /* when the row is shown, so its arrow knows */
  return G_LIST_MODEL(g_object_ref(call->children));
}

/* The tree ------------------------------------------------------------------------------------ */

static void hier_note(const char *text)
{
  gtk_label_set_text(hier.note, text ? text : "");
  gtk_widget_set_visible(GTK_WIDGET(hier.note), text != NULL);
}

/* The tree from the prepared items again, in the direction chosen. */
static void hier_restart(void)
{
  JsonArray *items = hier.items && JSON_NODE_HOLDS_ARRAY(hier.items) ? json_node_get_array(hier.items) : NULL;
  const char *name =
      items && json_array_get_length(items) ? json_string_at(json_array_get_element(items, 0), "name") : NULL;
  char *title;

  hier.generation++;
  g_list_store_remove_all(hier.roots);
  for (guint i = 0; items && i < json_array_get_length(items); i++) {
    TcodeCall *call = hier_call_new(json_array_get_element(items, i));
    g_list_store_append(hier.roots, call);
    g_object_unref(call);
  }
  gtk_button_set_label(GTK_BUTTON(hier.up), hier.types ? "Supertypes" : "Callers");
  gtk_button_set_label(GTK_BUTTON(hier.down), hier.types ? "Subtypes" : "Callees");
  title = name ? g_strdup_printf(hier.types ? (hier.going_up ? "Supertypes of %s" : "Subtypes of %s")
                                            : (hier.going_up ? "Callers of %s" : "Calls from %s"),
                                 name)
               : NULL;
  gtk_label_set_text(hier.title, title ? title : "");
  g_free(title);
  /* The roots open at once: their children are what was asked for. */
  for (guint i = 0; i < g_list_model_get_n_items(G_LIST_MODEL(hier.tree)); i++) {
    GtkTreeListRow *row = gtk_tree_list_model_get_row(hier.tree, i);
    if (row && gtk_tree_list_row_get_depth(row) == 0)
      gtk_tree_list_row_set_expanded(row, TRUE);
    g_clear_object(&row);
  }
}

void hierarchy_set_up(gboolean up)
{
  hier.going_up = up;
  if (hier.up) {
    gtk_toggle_button_set_active(hier.up, up);
    gtk_toggle_button_set_active(hier.down, !up);
  }
  if (hier.roots)
    hier_restart();
}

typedef struct {
  guint generation;
  gboolean types;
} HierPrepared;

static void hier_on_prepared(JsonNode *result, gpointer data)
{
  HierPrepared *p = data;

  if (p->generation == hier.generation && hier.roots) {
    if (hier.items)
      json_node_unref(hier.items);
    hier.items = result ? json_node_copy(result) : NULL;
    hier.types = p->types;
    if (!result || !JSON_NODE_HOLDS_ARRAY(result) || json_array_get_length(json_node_get_array(result)) == 0)
      hier_note(p->types ? "No type at the cursor." : "No function at the cursor.");
    else
      hier_note(NULL);
    hier_restart();
  }
  g_free(p);
}

void hierarchy_show(Tab *tab, gboolean types)
{
  GtkTextIter cursor;
  char *position, *params;
  HierPrepared *p;

  if (!hier.roots)
    return;
  show_sidebar_page(tab->app, "hierarchy");
  if (!lsp_has(tab) || !lsp_capability(tab, types ? "typeHierarchyProvider" : "callHierarchyProvider")) {
    hier.generation++;
    g_list_store_remove_all(hier.roots);
    gtk_label_set_text(hier.title, "");
    hier_note(types ? "The language server has no type hierarchy for this file."
                    : "The language server has no call hierarchy for this file.");
    return;
  }
  g_set_str(&hier.origin, tab->path);
  hier.generation++;
  hier_note(types ? "Looking for the type…" : "Looking for the function…");
  gtk_text_buffer_get_iter_at_mark(GTK_TEXT_BUFFER(tab->buffer), &cursor,
                                   gtk_text_buffer_get_insert(GTK_TEXT_BUFFER(tab->buffer)));
  position = lsp_position_params(tab, &cursor);
  params = g_strdup_printf("{%s}", position);
  p = g_new0(HierPrepared, 1);
  p->generation = hier.generation;
  p->types = types;
  lsp_request(tab, types ? "textDocument/prepareTypeHierarchy" : "textDocument/prepareCallHierarchy", params, NULL,
              hier_on_prepared, p);
  g_free(params);
  g_free(position);
}

/* Rows ---------------------------------------------------------------------------------------- */

static void hier_setup_row(GtkSignalListItemFactory *factory G_GNUC_UNUSED, GtkListItem *item,
                           gpointer data G_GNUC_UNUSED)
{
  GtkWidget *expander = gtk_tree_expander_new(), *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  GtkWidget *kind = gtk_label_new(NULL), *name = gtk_label_new(NULL), *where = gtk_label_new(NULL);

  gtk_widget_add_css_class(kind, "kind");
  gtk_widget_add_css_class(where, "where");
  gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
  gtk_label_set_ellipsize(GTK_LABEL(where), PANGO_ELLIPSIZE_START);
  gtk_box_append(GTK_BOX(box), kind);
  gtk_box_append(GTK_BOX(box), name);
  gtk_box_append(GTK_BOX(box), where);
  gtk_tree_expander_set_child(GTK_TREE_EXPANDER(expander), box);
  gtk_list_item_set_child(item, expander);
}

static void hier_bind_row(GtkSignalListItemFactory *factory G_GNUC_UNUSED, GtkListItem *item,
                          gpointer data G_GNUC_UNUSED)
{
  GtkTreeListRow *row = gtk_list_item_get_item(item);
  GtkWidget *expander = gtk_list_item_get_child(item);
  GtkWidget *kind = gtk_widget_get_first_child(gtk_tree_expander_get_child(GTK_TREE_EXPANDER(expander)));
  GtkWidget *name = gtk_widget_get_next_sibling(kind), *where = gtk_widget_get_next_sibling(name);
  TcodeCall *call = gtk_tree_list_row_get_item(row);

  gtk_tree_expander_set_list_row(GTK_TREE_EXPANDER(expander), row);
  gtk_label_set_text(GTK_LABEL(kind), symbols_kind_mark(call->kind));
  gtk_label_set_text(GTK_LABEL(name), call->name);
  gtk_label_set_text(GTK_LABEL(where), call->where);
  gtk_widget_set_tooltip_text(expander, call->path);
  g_object_unref(call);
}

/* A row activated: its symbol's file opens at its name. */
static void hier_on_activate(GtkListView *list G_GNUC_UNUSED, guint position, gpointer data G_GNUC_UNUSED)
{
  GtkTreeListRow *row = gtk_tree_list_model_get_row(hier.tree, position);
  TcodeCall *call = row ? gtk_tree_list_row_get_item(row) : NULL;

  if (call && call->path)
    open_file_at(hier.app, call->path, call->line + 1, call->column + 1);
  g_clear_object(&call);
  g_clear_object(&row);
}

static void hier_add_text(GString *out, GListModel *level, int depth)
{
  for (guint i = 0; i < g_list_model_get_n_items(level); i++) {
    TcodeCall *call = g_list_model_get_item(level, i);
    g_string_append_printf(out, "%*s%s %s\n", depth * 2, "", call->name, call->where);
    hier_add_text(out, G_LIST_MODEL(call->children), depth + 1);
    g_object_unref(call);
  }
}

char *hierarchy_text(void)
{
  GString *out = g_string_new(NULL);

  g_string_append_printf(out, "[%s]\n", hier.title ? gtk_label_get_text(hier.title) : "");
  if (hier.note && gtk_widget_get_visible(GTK_WIDGET(hier.note)))
    g_string_append_printf(out, "(%s)\n", gtk_label_get_text(hier.note));
  if (hier.roots)
    hier_add_text(out, G_LIST_MODEL(hier.roots), 0);
  return g_string_free(out, FALSE);
}

static void hier_on_direction(GtkToggleButton *button, gpointer data G_GNUC_UNUSED)
{
  if (!gtk_toggle_button_get_active(button))
    return;
  gboolean up = button == hier.up;
  if (up != hier.going_up)
    hierarchy_set_up(up);
}

static const char *const hierarchy_css = ".tcode-hierarchy { background: var(--tc-panel); }"
                                         ".tcode-hierarchy listview { background: transparent; }"
                                         ".tcode-hierarchy .title { color: var(--tc-fg-strong); margin: 8px 10px 2px; }"
                                         ".tcode-hierarchy .note { color: var(--tc-fg-dim); margin: 8px 10px; }"
                                         ".tcode-hierarchy .kind { color: var(--tc-accent); min-width: 16px; }"
                                         ".tcode-hierarchy .where { color: var(--tc-fg-faint); font-size: 0.9em; }"
                                         ".tcode-hierarchy .directions { margin: 2px 10px 4px; }"
                                         ".tcode-hierarchy .directions button { min-height: 0; padding: 1px 8px; }";

GtkWidget *hierarchy_panel(App *t)
{
  GtkWidget *panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0), *scroll = gtk_scrolled_window_new();
  GtkWidget *directions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  GtkListItemFactory *factory = gtk_signal_list_item_factory_new();

  palette_add_css(hierarchy_css);
  hier.app = t;
  hier.going_up = TRUE;
  gtk_widget_add_css_class(panel, "tcode-hierarchy");

  hier.title = GTK_LABEL(gtk_label_new(NULL));
  gtk_widget_add_css_class(GTK_WIDGET(hier.title), "title");
  gtk_label_set_xalign(hier.title, 0);
  gtk_label_set_ellipsize(hier.title, PANGO_ELLIPSIZE_END);
  gtk_box_append(GTK_BOX(panel), GTK_WIDGET(hier.title));

  gtk_widget_add_css_class(directions, "directions");
  gtk_widget_add_css_class(directions, "linked");
  hier.up = GTK_TOGGLE_BUTTON(gtk_toggle_button_new_with_label("Callers"));
  hier.down = GTK_TOGGLE_BUTTON(gtk_toggle_button_new_with_label("Callees"));
  gtk_toggle_button_set_group(hier.down, hier.up);
  gtk_toggle_button_set_active(hier.up, TRUE);
  g_signal_connect(hier.up, "toggled", G_CALLBACK(hier_on_direction), NULL);
  g_signal_connect(hier.down, "toggled", G_CALLBACK(hier_on_direction), NULL);
  gtk_box_append(GTK_BOX(directions), GTK_WIDGET(hier.up));
  gtk_box_append(GTK_BOX(directions), GTK_WIDGET(hier.down));
  gtk_box_append(GTK_BOX(panel), directions);

  hier.note = GTK_LABEL(gtk_label_new(NULL));
  gtk_widget_add_css_class(GTK_WIDGET(hier.note), "note");
  gtk_label_set_wrap(hier.note, TRUE);
  gtk_label_set_xalign(hier.note, 0);
  gtk_box_append(GTK_BOX(panel), GTK_WIDGET(hier.note));
  hier_note("Shift+Alt+H on a function shows its callers and callees; \"Show Type Hierarchy\" a type's "
            "supertypes and subtypes.");

  hier.roots = g_list_store_new(TCODE_TYPE_CALL);
  hier.tree =
      gtk_tree_list_model_new(G_LIST_MODEL(g_object_ref(hier.roots)), FALSE, FALSE, hier_children_of, NULL, NULL);
  g_signal_connect(factory, "setup", G_CALLBACK(hier_setup_row), NULL);
  g_signal_connect(factory, "bind", G_CALLBACK(hier_bind_row), NULL);
  hier.list = GTK_LIST_VIEW(
      gtk_list_view_new(GTK_SELECTION_MODEL(gtk_single_selection_new(G_LIST_MODEL(g_object_ref(hier.tree)))), factory));
  gtk_list_view_set_single_click_activate(hier.list, TRUE);
  g_signal_connect(hier.list, "activate", G_CALLBACK(hier_on_activate), NULL);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(hier.list));
  gtk_widget_set_vexpand(scroll, TRUE);
  gtk_box_append(GTK_BOX(panel), scroll);
  return panel;
}
