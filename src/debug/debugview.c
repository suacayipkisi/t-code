/*
 * The Run and Debug panel (see debugview.h). A pause asks gdb for the shown frame's scopes, then each
 * scope's variables (Arguments, then Locals, then Registers as one closed row); a variable with a
 * variablesReference has children, asked for when its row is shown, and at once when gdb gave no value
 * for it (an array, a struct): its value is then made from theirs, "{70, 85, 90}". Watch expressions
 * are evaluated in the shown frame at each pause (evaluate, context "watch") and become rows the same
 * way. Every answer carries the pause it was asked in (generation): answers from before a step are
 * dropped.
 */
#include "debug/debugview.h"

#include "debug/dap.h"
#include "debug/debugger.h"
#include "debug/disassembly.h"
#include "editor/breakpoints.h"
#include "editor/editor.h"
#include "lsp/json.h"
#include "popups/contextmenu.h"
#include "window/palette.h"

#include <string.h>

#define PREVIEW_ITEMS 8 /* children shown in a value made from them: "{1, 2, 3, …}" */

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

/* A variable ------------------------------------------------------------------------------------ */

struct _TcodeVar {
  GObject parent;
  char *name;
  char *value;
  char *key;        /* where it is, to compare with the last pause: "main/p/x" */
  gint64 reference; /* gdb's variablesReference: > 0 when it has children */
  gboolean changed; /* its value is not the last pause's */
  gboolean asked;   /* its children were asked for */
  gboolean preview; /* its value is made from its children's */
  gboolean missing; /* no value: a watch expression that cannot be evaluated now */
  gboolean waiting; /* Registers: its row is added once its children came (after the variables) */
  GListStore *children;
};

#define TCODE_TYPE_VAR (tcode_var_get_type())
G_DECLARE_FINAL_TYPE(TcodeVar, tcode_var, TCODE, VAR, GObject)
G_DEFINE_FINAL_TYPE(TcodeVar, tcode_var, G_TYPE_OBJECT)

static guint value_changed; /* the "value-changed" signal: a value made from the children came */

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

/* The panel ------------------------------------------------------------------------------------- */

#define NOT_PAUSED "not available: the program is not paused"

static struct {
  App *app;
  GtkWidget *note;      /* instead of the variables: not debugging, running */
  GtkWidget *variables; /* the variables' list (scrolled) */
  GListStore *root;     /* the shown frame's variables */
  GListStore *watch;    /* the watch expressions' values */
  GPtrArray *watches;   /* the watch expressions (char *) */
  GtkWidget *watch_entry;
  GtkListBox *stack;
  GtkListBox *breakpoints;
  JsonNode *frames;    /* the last pause's (a copy) */
  int shown;           /* the frame whose variables are listed */
  gboolean all_frames; /* the frames without source are listed one by one, not as one row */
  guint generation;    /* counts pauses and frame changes: older answers are dropped */
  guint watch_round;   /* counts the watch list's changes, the same way for its answers */
  GHashTable *last;    /* key -> value at the last pause */
  GHashTable *now;     /* the same for this one */
} view;

/* What an answer was asked for. */
typedef struct {
  guint generation;
  guint watch_round; /* the watch rows are made again after an expression is added or removed */
  TcodeVar *parent;  /* NULL: the frame's top level */
  char *prefix;      /* the keys' start */
} Asked;

static Asked *asked_new(TcodeVar *parent, const char *prefix)
{
  Asked *a = g_new0(Asked, 1);

  a->generation = view.generation;
  a->watch_round = view.watch_round;
  a->parent = parent ? g_object_ref(parent) : NULL;
  a->prefix = g_strdup(prefix);
  return a;
}

static void asked_free(gpointer data)
{
  Asked *a = data;

  if (a->parent)
    g_object_unref(a->parent);
  g_free(a->prefix);
  g_free(a);
}

/* Shows the variables, or a note instead (NULL: the variables). */
static void show_note(const char *note)
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
static gboolean changed(const char *key, const char *value)
{
  const char *before = g_hash_table_lookup(view.last, key);

  g_hash_table_replace(view.now, g_strdup(key), g_strdup(value));
  return before && strcmp(before, value) != 0;
}

static void ask_children(TcodeVar *var);

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
      var->changed = changed(var->key, var->value);
    else if (var->reference > 0) { /* an array or a struct: its value comes from its children */
      var->preview = TRUE;
      ask_children(var);
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
    a->parent->changed = changed(a->parent->key, a->parent->value);
    g_signal_emit(a->parent, value_changed, 0);
  }
}

static void ask_children(TcodeVar *var)
{
  Dap *dap = debug_dap(view.app);
  char *args;

  if (var->asked || var->reference <= 0 || !dap)
    return;
  var->asked = TRUE;
  args = g_strdup_printf("{\"variablesReference\":%" G_GINT64_FORMAT "}", var->reference);
  dap_request_full(dap, "variables", args, on_variables, asked_new(var, var->key), asked_free);
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
    show_note("No variables here (no debugging information for this function).");
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
        ask_children(registers); /* its answer comes after the variables': the row goes last */
        g_object_unref(registers);
        continue;
      }
      if (pass == 2 || g_strcmp0(hint, pass == 0 ? "arguments" : "locals") != 0)
        continue;
      char *args =
          g_strdup_printf("{\"variablesReference\":%" G_GINT64_FORMAT "}", json_int_at(scope, "variablesReference", 0));
      dap_request_full(dap, "variables", args, on_variables, asked_new(NULL, a->prefix), asked_free);
      g_free(args);
    }
}

static GtkListBoxRow *row_of_frame(int index);
static void evaluate_watches(void);

/* The frame at index (of view.frames) is listed: its variables are asked for. */
static void list_frame(int index)
{
  JsonNode *frame = view.frames ? json_array_get_element(json_node_get_array(view.frames), index) : NULL;
  Dap *dap = debug_dap(view.app);
  const char *function = json_string_at(frame, "name");
  char *args;

  view.generation++;
  view.shown = index;
  g_list_store_remove_all(view.root);
  evaluate_watches();
  if (!frame || !dap)
    return;
  args = g_strdup_printf("{\"frameId\":%" G_GINT64_FORMAT "}", json_int_at(frame, "id", 0));
  dap_request_full(dap, "scopes", args, on_scopes, asked_new(NULL, function ? function : "?"), asked_free);
  g_free(args);
  gtk_list_box_select_row(view.stack, row_of_frame(index));
}

/* Watch --------------------------------------------------------------------------------------- */

/* The shown frame's id, or -1 when the program is not paused. */
gint64 debug_view_frame_id(void)
{
  JsonNode *frame = view.frames ? json_array_get_element(json_node_get_array(view.frames), view.shown) : NULL;

  return frame && debug_dap(view.app) ? json_int_at(frame, "id", -1) : -1;
}

/* A watch row: expression = value (or why there is none). */
static TcodeVar *watch_var(const char *expression, const char *value, gint64 reference, gboolean missing)
{
  TcodeVar *var = g_object_new(TCODE_TYPE_VAR, NULL);

  var->name = g_strdup(expression);
  var->value = g_strdup(value ? value : "");
  var->key = g_strdup_printf("watch/%s", expression);
  var->reference = reference;
  var->missing = missing;
  if (reference > 0)
    var->children = g_list_store_new(TCODE_TYPE_VAR);
  return var;
}

/* An expression's value came (or why there is none: "No symbol "x" in current context."). */
static void on_watch(gpointer data, JsonNode *body, const char *error)
{
  Asked *a = data;
  TcodeVar *var;

  if (a->generation != view.generation || a->watch_round != view.watch_round)
    return;
  var = watch_var(a->prefix, error ? error : json_string_at(body, "result"), json_int_at(body, "variablesReference", 0),
                  error != NULL);
  if (!error && !*var->value && var->reference > 0) { /* an array or a struct */
    var->preview = TRUE;
    ask_children(var);
  } else if (!error) {
    var->changed = changed(var->key, var->value);
  }
  g_list_store_append(view.watch, var);
  g_object_unref(var);
}

/* The watch expressions are evaluated in the shown frame (each answer adds its row, in order). */
static void evaluate_watches(void)
{
  gint64 frame = debug_view_frame_id();
  Dap *dap = debug_dap(view.app);

  if (!view.watch)
    return;
  g_list_store_remove_all(view.watch);
  for (guint i = 0; i < view.watches->len; i++) {
    const char *expression = g_ptr_array_index(view.watches, i);
    if (frame < 0 || !dap) {
      TcodeVar *var = watch_var(expression, NOT_PAUSED, 0, TRUE);
      g_list_store_append(view.watch, var);
      g_object_unref(var);
      continue;
    }
    char *quoted = json_quote(expression);
    char *args =
        g_strdup_printf("{\"expression\":%s,\"frameId\":%" G_GINT64_FORMAT ",\"context\":\"watch\"}", quoted, frame);
    dap_request_full(dap, "evaluate", args, on_watch, asked_new(NULL, expression), asked_free);
    g_free(args);
    g_free(quoted);
  }
}

void debug_view_add_watch(App *t G_GNUC_UNUSED, const char *expression)
{
  char *clean = g_strstrip(g_strdup(expression ? expression : ""));

  if (!view.watches || !*clean) {
    g_free(clean);
    return;
  }
  for (guint i = 0; i < view.watches->len; i++)
    if (strcmp(g_ptr_array_index(view.watches, i), clean) == 0) {
      g_free(clean);
      return;
    }
  g_ptr_array_add(view.watches, clean);
  view.watch_round++; /* the rows are made again: answers still on the way would add theirs too */
  evaluate_watches();
}

static void remove_watch(const char *expression)
{
  for (guint i = 0; i < view.watches->len; i++)
    if (strcmp(g_ptr_array_index(view.watches, i), expression) == 0) {
      g_ptr_array_remove_index(view.watches, i);
      break;
    }
  view.watch_round++;
  evaluate_watches();
}

char **debug_view_watches(void)
{
  GStrvBuilder *builder = g_strv_builder_new();

  for (guint i = 0; view.watches && i < view.watches->len; i++)
    g_strv_builder_add(builder, g_ptr_array_index(view.watches, i));
  char **list = g_strv_builder_end(builder);
  g_strv_builder_unref(builder);
  return list;
}

void debug_view_set_watches(char **expressions)
{
  if (!view.watches)
    return;
  g_ptr_array_set_size(view.watches, 0);
  for (int i = 0; expressions && expressions[i]; i++)
    if (*expressions[i])
      g_ptr_array_add(view.watches, g_strdup(expressions[i]));
  evaluate_watches();
}

static void on_watch_entry(GtkEntry *entry, gpointer data G_GNUC_UNUSED)
{
  debug_view_add_watch(view.app, gtk_editable_get_text(GTK_EDITABLE(entry)));
  gtk_editable_set_text(GTK_EDITABLE(entry), "");
  gtk_widget_set_visible(GTK_WIDGET(entry), FALSE);
}

/* Escape: no expression after all. */
static gboolean on_watch_entry_key(GtkEventControllerKey *keys G_GNUC_UNUSED, guint key, guint code G_GNUC_UNUSED,
                                   GdkModifierType state G_GNUC_UNUSED, GtkWidget *entry)
{
  if (key != GDK_KEY_Escape)
    return FALSE;
  gtk_editable_set_text(GTK_EDITABLE(entry), "");
  gtk_widget_set_visible(entry, FALSE);
  return TRUE;
}

/* "+": an entry for the expression. */
static void on_add_watch(GtkButton *button G_GNUC_UNUSED, gpointer data G_GNUC_UNUSED)
{
  gtk_widget_set_visible(view.watch_entry, TRUE);
  gtk_widget_grab_focus(view.watch_entry);
}

/* Variables' rows ------------------------------------------------------------------------------- */

static GListModel *children_of(gpointer item, gpointer data G_GNUC_UNUSED)
{
  TcodeVar *var = item;

  if (!var->children)
    return NULL;
  ask_children(var); /* asked for when the row is shown, so its arrow knows */
  return G_LIST_MODEL(g_object_ref(var->children));
}

static void on_remove_watch(GtkButton *button, gpointer data G_GNUC_UNUSED)
{
  char *expression = g_strdup(g_object_get_data(G_OBJECT(button), "expression"));

  if (expression)
    remove_watch(expression);
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
static void remove_from_watch(gpointer data) { remove_watch(((VarMenu *)data)->name); }
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
static void setup_row(GtkSignalListItemFactory *factory G_GNUC_UNUSED, GtkListItem *item, gpointer data)
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

static void bind_row(GtkSignalListItemFactory *factory G_GNUC_UNUSED, GtkListItem *item, gpointer data G_GNUC_UNUSED)
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

static void unbind_row(GtkSignalListItemFactory *factory G_GNUC_UNUSED, GtkListItem *item, gpointer data G_GNUC_UNUSED)
{
  GtkWidget *box = gtk_tree_expander_get_child(GTK_TREE_EXPANDER(gtk_list_item_get_child(item)));
  TcodeVar *var = g_object_get_data(G_OBJECT(item), "var");

  if (var)
    g_signal_handlers_disconnect_by_data(var, box);
  g_object_set_data(G_OBJECT(gtk_list_item_get_child(item)), "var", NULL);
  g_object_set_data(G_OBJECT(item), "var", NULL);
}

/* Call Stack ------------------------------------------------------------------------------------ */

/* Whether frame has a file on disk to show. */
static gboolean has_source(JsonNode *frame)
{
  const char *path = json_string_at(frame, "source.path");

  return path && g_file_test(path, G_FILE_TEST_IS_REGULAR);
}

static GtkWidget *stack_row(const char *function, const char *where, int index)
{
  GtkWidget *row = gtk_list_box_row_new(), *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *name = gtk_label_new(function), *place = gtk_label_new(where);

  gtk_widget_add_css_class(name, "function");
  gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
  gtk_label_set_xalign(GTK_LABEL(name), 0);
  gtk_widget_set_hexpand(name, TRUE);
  gtk_widget_add_css_class(place, "where");
  gtk_box_append(GTK_BOX(box), name);
  gtk_box_append(GTK_BOX(box), place);
  gtk_widget_set_margin_start(box, 10);
  gtk_widget_set_margin_end(box, 10);
  gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
  g_object_set_data(G_OBJECT(row), "frame", GINT_TO_POINTER(index)); /* -1: the hidden frames' row */
  return row;
}

/* The frames, the program's own with their file and line. Frames without a file (the C library's,
 * ASan's) in a row go in one dim row, which opens them. */
static void fill_stack(void)
{
  guint n = view.frames ? json_array_get_length(json_node_get_array(view.frames)) : 0;

  gtk_list_box_remove_all(view.stack);
  for (guint i = 0; i < n; i++) {
    JsonNode *frame = json_array_get_element(json_node_get_array(view.frames), i);
    const char *function = json_string_at(frame, "name"), *path = json_string_at(frame, "source.path");
    guint run = 0;
    GtkWidget *row;

    while (!view.all_frames && i + run < n &&
           !has_source(json_array_get_element(json_node_get_array(view.frames), i + run)))
      run++;
    if (run > 1) {
      char *text = g_strdup_printf("%u frames in libraries", run);
      row = stack_row(text, "show", -1);
      gtk_widget_set_tooltip_text(row, "Functions of the C library or of ASan, without source: click to list them");
      gtk_widget_add_css_class(row, "no-source");
      gtk_list_box_append(view.stack, row);
      g_free(text);
      i += run - 1;
      continue;
    }
    if (has_source(frame)) {
      char *file = g_path_get_basename(path);
      char *where = g_strdup_printf("%s:%d", file, (int)json_int_at(frame, "line", 0));
      row = stack_row(function ? function : "?", where, (int)i);
      gtk_widget_set_tooltip_text(row, path);
      g_free(where);
      g_free(file);
    } else {
      row = stack_row(function ? function : "?", "", (int)i);
      gtk_widget_set_tooltip_text(row, "No source: a library without debugging information");
      gtk_widget_add_css_class(row, "no-source");
      gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
    }
    gtk_list_box_append(view.stack, row);
  }
}

/* The row of frame index, or NULL (hidden). */
static GtkListBoxRow *row_of_frame(int index)
{
  for (GtkListBoxRow *row = gtk_list_box_get_row_at_index(view.stack, 0); row;
       row = GTK_LIST_BOX_ROW(gtk_widget_get_next_sibling(GTK_WIDGET(row))))
    if (GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "frame")) == index)
      return row;
  return NULL;
}

/* A click on a frame: its line in the editor, its variables in the list; on the hidden frames' row:
 * they show. */
static void on_frame_activated(GtkListBox *box G_GNUC_UNUSED, GtkListBoxRow *row, gpointer data G_GNUC_UNUSED)
{
  int index = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "frame"));
  JsonNode *frame = view.frames && index >= 0 ? json_array_get_element(json_node_get_array(view.frames), index) : NULL;
  const char *path = json_string_at(frame, "source.path");

  if (index < 0) {
    view.all_frames = TRUE;
    fill_stack();
    gtk_list_box_select_row(view.stack, row_of_frame(view.shown));
    return;
  }
  if (!frame || !path)
    return;
  open_file_at(view.app, path, (int)json_int_at(frame, "line", 1), 1);
  if (index != view.shown) {
    list_frame(index);
    disassembly_update(view.app);
  }
}

/* Breakpoints ---------------------------------------------------------------------------------- */

typedef struct {
  const char *path;
  Breakpoint *b;
} Listed;

static int by_place(gconstpointer x, gconstpointer y)
{
  const Listed *a = x, *b = y;
  int paths = strcmp(a->path, b->path);

  return paths ? paths : a->b->line - b->b->line;
}

static void on_remove_breakpoint(GtkButton *button, gpointer data G_GNUC_UNUSED)
{
  char *path = g_strdup(g_object_get_data(G_OBJECT(button), "path"));

  breakpoint_remove(view.app, path, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "line")));
  g_free(path);
}

/* A click on a breakpoint: its line in the editor. */
static void on_breakpoint_activated(GtkListBox *box G_GNUC_UNUSED, GtkListBoxRow *row, gpointer data G_GNUC_UNUSED)
{
  const char *path = g_object_get_data(G_OBJECT(row), "path");

  if (path)
    open_file_at(view.app, path, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "line")), 1);
}

/* A breakpoint's right-click menu: its place, copied. */
typedef struct {
  char *path;
  int line;
} Place;

static void place_free(Place *p)
{
  g_free(p->path);
  g_free(p);
}

static void open_place(gpointer data)
{
  open_file_at(view.app, ((Place *)data)->path, ((Place *)data)->line, 1);
}

static void remove_place(gpointer data)
{
  breakpoint_remove(view.app, ((Place *)data)->path, ((Place *)data)->line);
}

static void remove_every(gpointer data G_GNUC_UNUSED)
{
  breakpoints_remove_all(view.app);
}

static void on_breakpoints_menu(GtkWidget *list, double x, double y, gpointer data G_GNUC_UNUSED)
{
  static const MenuItem items[] = {
    { "Open", open_place },
    { NULL, NULL },
    { "Remove Breakpoint", remove_place },
    { "Remove All Breakpoints", remove_every },
  };
  GtkListBoxRow *row = gtk_list_box_get_row_at_y(GTK_LIST_BOX(list), (int)y);
  const char *path = row ? g_object_get_data(G_OBJECT(row), "path") : NULL;

  if (!path)
    return;
  Place *p = g_new(Place, 1);
  p->path = g_strdup(path);
  p->line = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "line"));
  context_menu(list, x, y, items, G_N_ELEMENTS(items), p, (GDestroyNotify)place_free);
}

static void copy_stack(gpointer data G_GNUC_UNUSED)
{
  char *text = debug_view_stack_text();

  copy_to_clipboard(text);
  g_free(text);
}

static void on_stack_menu(GtkWidget *list, double x, double y, gpointer data G_GNUC_UNUSED)
{
  static const MenuItem items[] = { { "Copy Call Stack", copy_stack } };

  if (gtk_widget_get_first_child(list))
    context_menu(list, x, y, items, G_N_ELEMENTS(items), NULL, NULL);
}

void debug_view_breakpoints_changed(App *t)
{
  GArray *all = g_array_new(FALSE, FALSE, sizeof(Listed));
  GHashTableIter iter;
  gpointer path, list;

  if (!view.breakpoints) {
    g_array_free(all, TRUE);
    return;
  }
  gtk_list_box_remove_all(view.breakpoints);
  if (t->debug.breakpoints) {
    g_hash_table_iter_init(&iter, t->debug.breakpoints);
    while (g_hash_table_iter_next(&iter, &path, &list))
      for (guint i = 0; i < ((GArray *)list)->len; i++) {
        Listed l = { path, &g_array_index((GArray *)list, Breakpoint, i) };
        g_array_append_val(all, l);
      }
  }
  g_array_sort(all, by_place);
  for (guint i = 0; i < all->len; i++) {
    Listed *l = &g_array_index(all, Listed, i);
    char *name = g_path_get_basename(l->path), *where = g_strdup_printf("%s:%d", name, l->b->line);
    GtkWidget *row = gtk_list_box_row_new(), *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *place = gtk_label_new(where), *condition = gtk_label_new(NULL);
    GtkWidget *remove = gtk_button_new_from_icon_name("window-close-symbolic");

    if (l->b->condition) {
      char *text = g_strdup_printf("when %s", l->b->condition);
      gtk_label_set_text(GTK_LABEL(condition), text);
      g_free(text);
    }
    gtk_widget_add_css_class(condition, "condition");
    gtk_label_set_xalign(GTK_LABEL(condition), 0);
    gtk_label_set_ellipsize(GTK_LABEL(condition), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(condition, TRUE);
    gtk_widget_add_css_class(remove, "flat");
    gtk_widget_add_css_class(remove, "remove");
    gtk_widget_set_tooltip_text(remove, "Remove this breakpoint");
    g_object_set_data_full(G_OBJECT(remove), "path", g_strdup(l->path), g_free);
    g_object_set_data(G_OBJECT(remove), "line", GINT_TO_POINTER(l->b->line));
    g_signal_connect(remove, "clicked", G_CALLBACK(on_remove_breakpoint), NULL);
    gtk_box_append(GTK_BOX(box), place);
    gtk_box_append(GTK_BOX(box), condition);
    gtk_box_append(GTK_BOX(box), remove);
    gtk_widget_set_margin_start(box, 10);
    gtk_widget_set_margin_end(box, 4);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
    gtk_widget_set_tooltip_text(row, l->path);
    g_object_set_data_full(G_OBJECT(row), "path", g_strdup(l->path), g_free);
    g_object_set_data(G_OBJECT(row), "line", GINT_TO_POINTER(l->b->line));
    gtk_list_box_append(view.breakpoints, row);
    g_free(where);
    g_free(name);
  }
  if (all->len == 0) {
    GtkWidget *row = gtk_list_box_row_new(), *label = gtk_label_new("None: click left of a line number to add one.");
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_label_set_wrap(GTK_LABEL(label), TRUE);
    gtk_widget_add_css_class(label, "condition");
    gtk_widget_set_margin_start(label, 10);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), label);
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
    gtk_list_box_append(view.breakpoints, row);
  }
  g_array_free(all, TRUE);
}

static void on_remove_all(GtkButton *button G_GNUC_UNUSED, gpointer data G_GNUC_UNUSED)
{
  breakpoints_remove_all(view.app);
}

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
  fill_stack();
  show_note(NULL);
  list_frame(MAX(shown, 0));
}

void debug_view_running(App *t G_GNUC_UNUSED)
{
  if (!view.root)
    return;
  view.generation++;
  g_list_store_remove_all(view.root);
  gtk_list_box_remove_all(view.stack);
  g_clear_pointer(&view.frames, json_node_unref);
  evaluate_watches();
  show_note("Running… The variables show here when the program pauses: at a breakpoint, after a step, or with "
            "Pause (F6).");
}

void debug_view_ended(App *t G_GNUC_UNUSED)
{
  if (!view.root)
    return;
  view.generation++;
  g_list_store_remove_all(view.root);
  gtk_list_box_remove_all(view.stack);
  g_clear_pointer(&view.frames, json_node_unref);
  g_hash_table_remove_all(view.last);
  g_hash_table_remove_all(view.now);
  evaluate_watches();
  show_note("Not debugging.\n\nClick left of a line number (or F9) to put a breakpoint there, then F5: the program "
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
      gtk_tree_list_model_new(G_LIST_MODEL(g_object_ref(store)), FALSE, FALSE, children_of, NULL, NULL);

  g_signal_connect(factory, "setup", G_CALLBACK(setup_row), GINT_TO_POINTER(watch));
  g_signal_connect(factory, "bind", G_CALLBACK(bind_row), NULL);
  g_signal_connect(factory, "unbind", G_CALLBACK(unbind_row), NULL);
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
  g_signal_connect(view.watch_entry, "activate", G_CALLBACK(on_watch_entry), NULL);
  g_signal_connect(keys, "key-pressed", G_CALLBACK(on_watch_entry_key), view.watch_entry);
  gtk_widget_add_controller(view.watch_entry, keys);
  gtk_box_append(GTK_BOX(rest),
                 heading("WATCH", "list-add-symbolic", "Add an expression to watch", G_CALLBACK(on_add_watch)));
  gtk_box_append(GTK_BOX(rest), view.watch_entry);
  gtk_box_append(GTK_BOX(rest), var_list(view.watch, TRUE));

  view.stack = GTK_LIST_BOX(gtk_list_box_new());
  g_signal_connect(view.stack, "row-activated", G_CALLBACK(on_frame_activated), NULL);
  context_menu_attach(GTK_WIDGET(view.stack), on_stack_menu, NULL);
  gtk_box_append(GTK_BOX(rest), heading("CALL STACK", NULL, NULL, NULL));
  gtk_box_append(GTK_BOX(rest), GTK_WIDGET(view.stack));

  view.breakpoints = GTK_LIST_BOX(gtk_list_box_new());
  gtk_list_box_set_selection_mode(view.breakpoints, GTK_SELECTION_NONE);
  g_signal_connect(view.breakpoints, "row-activated", G_CALLBACK(on_breakpoint_activated), NULL);
  context_menu_attach(GTK_WIDGET(view.breakpoints), on_breakpoints_menu, NULL);
  gtk_box_append(GTK_BOX(rest), heading("BREAKPOINTS", "edit-clear-all-symbolic", "Remove all breakpoints",
                                        G_CALLBACK(on_remove_all)));
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
