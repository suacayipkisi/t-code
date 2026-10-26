/*
 * The Run and Debug panel's variables (see state.h): TcodeVar rows from the shown frame's scopes,
 * children asked for when a row is shown, values made from children ("{70, 85, 90}"), and the
 * rows' widgets and menu.
 */
#include "debug/view/state.h"

#include "debug/dap.h"
#include "debug/debugger.h"
#include "debug/view/debugview.h"
#include "popups/contextmenu.h"

#include <string.h>

/* A variable ------------------------------------------------------------------------------------ */

G_DEFINE_FINAL_TYPE(TcodeVar, tcode_var, G_TYPE_OBJECT)

guint value_changed; /* the "value-changed" signal: a value made from the children came */

static void tcode_var_finalize(GObject *object)
{
  TcodeVar *self = TCODE_VAR(object);

  g_free(self->name);
  g_free(self->value);
  g_free(self->key);
  g_clear_object(&self->children);
  G_OBJECT_CLASS(tcode_var_parent_class)->finalize(object);
}

static void tcode_var_class_init(TcodeVarClass *klass)
{
  G_OBJECT_CLASS(klass)->finalize = tcode_var_finalize;
  value_changed =
      g_signal_new("value-changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void tcode_var_init(TcodeVar *self G_GNUC_UNUSED)
{
}

Asked *var_asked_new(TcodeVar *parent, const char *prefix)
{
  Asked *a = g_new0(Asked, 1);

  a->generation = view.generation;
  a->watch_round = view.watch_round;
  a->parent = parent ? g_object_ref(parent) : NULL;
  a->prefix = g_strdup(prefix);
  return a;
}

void var_asked_free(gpointer data)
{
  Asked *a = data;

  if (a->parent)
    g_object_unref(a->parent);
  g_free(a->prefix);
  g_free(a);
}

/* Shows the variables, or a note instead (NULL: the variables). */
void var_show_note(const char *note)
{
  if (!view.note)
    return;
  gtk_widget_set_visible(view.note, note != NULL);
  gtk_widget_set_visible(view.variables, note == NULL);
  if (note)
    gtk_label_set_text(GTK_LABEL(view.note), note);
}

/* A value made from the children's: "{70, 85, 90}" (nested ones "{…}"). */
static char *preview_of(GListStore *children)
{
  guint n = g_list_model_get_n_items(G_LIST_MODEL(children));
  GString *text = g_string_new("{");

  for (guint i = 0; i < n && i < PREVIEW_ITEMS; i++) {
    TcodeVar *child = g_list_model_get_item(G_LIST_MODEL(children), i);
    g_string_append_printf(text, "%s%s", i ? ", " : "", *child->value ? child->value : "{…}");
    g_object_unref(child);
  }
  if (n > PREVIEW_ITEMS)
    g_string_append(text, ", …");
  g_string_append(text, "}");
  return g_string_free(text, FALSE);
}

/* Whether a value differs from the last pause's at key (and remembers it for the next one). */
gboolean var_changed(const char *key, const char *value)
{
  const char *before = g_hash_table_lookup(view.last, key);

  g_hash_table_replace(view.now, g_strdup(key), g_strdup(value));
  return before && strcmp(before, value) != 0;
}

/* gdb's variables for a scope or a variable: rows. */
static void on_variables(gpointer data, JsonNode *body, const char *error)
{
  Asked *a = data;
  JsonNode *list = json_at(body, "variables");
  GListStore *into = a->parent ? a->parent->children : view.root;
  guint n = list && JSON_NODE_HOLDS_ARRAY(list) ? json_array_get_length(json_node_get_array(list)) : 0;

  if (a->generation != view.generation)
    return;
  if (error && a->parent) { /* a NULL pointer's target: say why it has none */
    TcodeVar *note = g_object_new(TCODE_TYPE_VAR, NULL);
    note->name = g_strdup("");
    note->value = g_strdup(error);
    note->key = g_strdup("");
    g_list_store_append(into, note);
    g_object_unref(note);
  }
  for (guint i = 0; i < n; i++) {
    JsonNode *item = json_array_get_element(json_node_get_array(list), i);
    const char *name = json_string_at(item, "name"), *value = json_string_at(item, "value");
    TcodeVar *var = g_object_new(TCODE_TYPE_VAR, NULL);
    gboolean index = name && g_ascii_isdigit(name[0]); /* an array's element */
    var->name = index ? g_strdup_printf("[%s]", name) : g_strdup(name ? name : "?");
    var->value = g_strdup(value ? value : "");
    var->reference = json_int_at(item, "variablesReference", 0);
    var->key = g_strdup_printf("%s/%s", a->prefix, var->name);
    if (var->reference > 0)
      var->children = g_list_store_new(TCODE_TYPE_VAR);
    if (*var->value)
      var->changed = var_changed(var->key, var->value);
    else if (var->reference > 0) { /* an array or a struct: its value comes from its children */
      var->preview = TRUE;
      var_ask_children(var);
    }
    g_list_store_append(into, var);
    g_object_unref(var);
  }
  if (a->parent && a->parent->waiting) {
    a->parent->waiting = FALSE;
    g_list_store_append(view.root, a->parent);
  }
  if (a->parent && a->parent->preview) {
    g_free(a->parent->value);
    a->parent->value = preview_of(a->parent->children);
    a->parent->changed = var_changed(a->parent->key, a->parent->value);
    g_signal_emit(a->parent, value_changed, 0);
  }
}

void var_ask_children(TcodeVar *var)
{
  Dap *dap = debug_dap(view.app);
  char *args;

  if (var->asked || var->reference <= 0 || !dap)
    return;
  var->asked = TRUE;
  args = g_strdup_printf("{\"variablesReference\":%" G_GINT64_FORMAT "}", var->reference);
  dap_request_full(dap, "variables", args, on_variables, var_asked_new(var, var->key), var_asked_free);
  g_free(args);
}

/* The frame's scopes: Arguments and Locals are listed, then Registers as one closed row. */
static void on_scopes(gpointer data, JsonNode *body, const char *error)
{
  Asked *a = data;
  JsonNode *scopes = json_at(body, "scopes");
  guint n = scopes && JSON_NODE_HOLDS_ARRAY(scopes) ? json_array_get_length(json_node_get_array(scopes)) : 0;
  Dap *dap = debug_dap(view.app);

  if (a->generation != view.generation || !dap)
    return;
  if (error || n == 0)
    var_show_note("No variables here (no debugging information for this function).");
  for (int pass = 0; pass < 3; pass++) /* arguments, locals, registers */
    for (guint i = 0; i < n; i++) {
      JsonNode *scope = json_array_get_element(json_node_get_array(scopes), i);
      const char *hint = json_string_at(scope, "presentationHint");
      if (pass == 2 && g_strcmp0(hint, "registers") == 0) { /* last and closed: for the Disassembly View */
        TcodeVar *registers = g_object_new(TCODE_TYPE_VAR, NULL);
        registers->name = g_strdup("Registers");
        registers->value = g_strdup("the CPU's: rip, rsp, rax, …");
        registers->missing = TRUE;
        registers->key = g_strdup_printf("%s/Registers", a->prefix);
        registers->reference = json_int_at(scope, "variablesReference", 0);
        registers->children = g_list_store_new(TCODE_TYPE_VAR);
        registers->waiting = TRUE;
        var_ask_children(registers); /* its answer comes after the variables': the row goes last */
        g_object_unref(registers);
        continue;
      }
      if (pass == 2 || g_strcmp0(hint, pass == 0 ? "arguments" : "locals") != 0)
        continue;
      char *args =
          g_strdup_printf("{\"variablesReference\":%" G_GINT64_FORMAT "}", json_int_at(scope, "variablesReference", 0));
      dap_request_full(dap, "variables", args, on_variables, var_asked_new(NULL, a->prefix), var_asked_free);
      g_free(args);
    }
}

/* The frame at index (of view.frames) is listed: its variables are asked for. */
void var_list_frame(int index)
{
  JsonNode *frame = view.frames ? json_array_get_element(json_node_get_array(view.frames), index) : NULL;
  Dap *dap = debug_dap(view.app);
  const char *function = json_string_at(frame, "name");
  char *args;

  view.generation++;
  view.shown = index;
  g_list_store_remove_all(view.root);
  watch_evaluate();
  if (!frame || !dap)
    return;
  args = g_strdup_printf("{\"frameId\":%" G_GINT64_FORMAT "}", json_int_at(frame, "id", 0));
  dap_request_full(dap, "scopes", args, on_scopes, var_asked_new(NULL, function ? function : "?"), var_asked_free);
  g_free(args);
  gtk_list_box_select_row(view.stack, stack_row_of_frame(index));
}

/* Variables' rows ------------------------------------------------------------------------------- */

GListModel *var_children_of(gpointer item, gpointer data G_GNUC_UNUSED)
{
  TcodeVar *var = item;

  if (!var->children)
    return NULL;
  var_ask_children(var); /* asked for when the row is shown, so its arrow knows */
  return G_LIST_MODEL(g_object_ref(var->children));
}

static void on_remove_watch(GtkButton *button, gpointer data G_GNUC_UNUSED)
{
  char *expression = g_strdup(g_object_get_data(G_OBJECT(button), "expression"));

  if (expression)
    watch_remove(expression);
  g_free(expression);
}

/* A variable's right-click menu: what it showed (the row may show another one when an item runs). */
typedef struct {
  char *name, *value;
  gboolean watch, top; /* a Watch row; an expression or variable itself, not a member of one */
} VarMenu;

static void var_menu_free(VarMenu *m)
{
  g_free(m->name);
  g_free(m->value);
  g_free(m);
}

/* clang-format off */
static void copy_value(gpointer data) { copy_to_clipboard(((VarMenu *)data)->value); }
static void copy_name(gpointer data) { copy_to_clipboard(((VarMenu *)data)->name); }
static void add_to_watch(gpointer data) { debug_view_add_watch(view.app, ((VarMenu *)data)->name); }
static void remove_from_watch(gpointer data) { watch_remove(((VarMenu *)data)->name); }
/* clang-format on */

static void copy_both(gpointer data)
{
  VarMenu *m = data;
  char *text = g_strdup_printf("%s = %s", m->name, m->value);

  copy_to_clipboard(text);
  g_free(text);
}

static void on_var_menu(GtkWidget *expander, double x, double y, gpointer watch)
{
  TcodeVar *var = g_object_get_data(G_OBJECT(expander), "var");
  GtkTreeListRow *row = gtk_tree_expander_get_list_row(GTK_TREE_EXPANDER(expander));
  MenuItem items[8];
  int n = 0;

  if (!var || !row)
    return;
  VarMenu *m = g_new0(VarMenu, 1);
  m->name = g_strdup(var->name);
  m->value = g_strdup(var->value);
  m->watch = watch != NULL;
  m->top = gtk_tree_list_row_get_depth(row) == 0;
  items[n++] = (MenuItem){ "Copy Value", copy_value };
  items[n++] = (MenuItem){ "Copy Name", copy_name };
  items[n++] = (MenuItem){ "Copy Name = Value", copy_both };
  if (m->top && *m->name) {
    items[n++] = (MenuItem){ NULL, NULL };
    items[n++] =
        m->watch ? (MenuItem){ "Remove Expression", remove_from_watch } : (MenuItem){ "Add to Watch", add_to_watch };
  }
  context_menu(expander, x, y, items, n, m, (GDestroyNotify)var_menu_free);
}

/* A row: an arrow, name = value; the Watch's (data non-NULL) also a button that takes it away. */
void var_setup_row(GtkSignalListItemFactory *factory G_GNUC_UNUSED, GtkListItem *item, gpointer data)
{
  GtkWidget *expander = gtk_tree_expander_new();
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *name = gtk_label_new(NULL), *equals = gtk_label_new(" = "), *value = gtk_label_new(NULL);

  gtk_widget_add_css_class(name, "name");
  gtk_widget_add_css_class(value, "value");
  gtk_label_set_xalign(GTK_LABEL(value), 0);
  gtk_label_set_ellipsize(GTK_LABEL(value), PANGO_ELLIPSIZE_END);
  gtk_label_set_selectable(GTK_LABEL(value), FALSE);
  gtk_widget_set_hexpand(value, TRUE);
  gtk_box_append(GTK_BOX(box), name);
  gtk_box_append(GTK_BOX(box), equals);
  gtk_box_append(GTK_BOX(box), value);
  if (data) {
    GtkWidget *remove = gtk_button_new_from_icon_name("window-close-symbolic");
    gtk_widget_add_css_class(remove, "flat");
    gtk_widget_add_css_class(remove, "remove");
    gtk_widget_set_tooltip_text(remove, "Remove this expression");
    g_signal_connect(remove, "clicked", G_CALLBACK(on_remove_watch), NULL);
    gtk_box_append(GTK_BOX(box), remove);
  }
  gtk_tree_expander_set_child(GTK_TREE_EXPANDER(expander), box);
  gtk_list_item_set_child(item, expander);
  context_menu_attach(expander, on_var_menu, data);
}

/* The row shows var's value now. */
static void show_value(TcodeVar *var, GtkWidget *box)
{
  GtkWidget *name = gtk_widget_get_first_child(box);
  GtkWidget *equals = gtk_widget_get_next_sibling(name), *value = gtk_widget_get_next_sibling(equals);

  gtk_label_set_text(GTK_LABEL(name), var->name);
  gtk_widget_set_visible(equals, *var->name != '\0');
  gtk_label_set_text(GTK_LABEL(value), var->value);
  gtk_widget_set_tooltip_text(value, *var->value ? var->value : NULL);
  if (var->changed)
    gtk_widget_add_css_class(value, "changed");
  else
    gtk_widget_remove_css_class(value, "changed");
  if (var->missing)
    gtk_widget_add_css_class(value, "missing");
  else
    gtk_widget_remove_css_class(value, "missing");
}

void var_bind_row(GtkSignalListItemFactory *factory G_GNUC_UNUSED, GtkListItem *item, gpointer data G_GNUC_UNUSED)
{
  GtkTreeListRow *row = gtk_list_item_get_item(item);
  GtkWidget *expander = gtk_list_item_get_child(item);
  GtkWidget *box = gtk_tree_expander_get_child(GTK_TREE_EXPANDER(expander));
  TcodeVar *var = gtk_tree_list_row_get_item(row);

  GtkWidget *remove = gtk_widget_get_last_child(box);

  gtk_tree_expander_set_list_row(GTK_TREE_EXPANDER(expander), row);
  show_value(var, box);
  if (GTK_IS_BUTTON(remove)) { /* the Watch's: on its expressions, not on their children */
    gtk_widget_set_visible(remove, gtk_tree_list_row_get_depth(row) == 0);
    g_object_set_data_full(G_OBJECT(remove), "expression", g_strdup(var->name), g_free);
  }
  g_signal_connect_object(var, "value-changed", G_CALLBACK(show_value), box, G_CONNECT_DEFAULT);
  g_object_set_data_full(G_OBJECT(item), "var", var, g_object_unref);
  g_object_set_data(G_OBJECT(expander), "var", var); /* for its menu; the item holds the reference */
}

void var_unbind_row(GtkSignalListItemFactory *factory G_GNUC_UNUSED, GtkListItem *item, gpointer data G_GNUC_UNUSED)
{
  GtkWidget *box = gtk_tree_expander_get_child(GTK_TREE_EXPANDER(gtk_list_item_get_child(item)));
  TcodeVar *var = g_object_get_data(G_OBJECT(item), "var");

  if (var)
    g_signal_handlers_disconnect_by_data(var, box);
  g_object_set_data(G_OBJECT(gtk_list_item_get_child(item)), "var", NULL);
  g_object_set_data(G_OBJECT(item), "var", NULL);
}
