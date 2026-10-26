/*
 * The Run and Debug panel's Watch (see state.h): expressions evaluated in the shown frame at each
 * pause, added from its entry or a variable's menu, kept per project by the session.
 */
#include "debug/view/state.h"

#include "debug/dap.h"
#include "debug/debugger.h"
#include "debug/view/debugview.h"

#include <string.h>

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
    var_ask_children(var);
  } else if (!error) {
    var->changed = var_changed(var->key, var->value);
  }
  g_list_store_append(view.watch, var);
  g_object_unref(var);
}

/* The watch expressions are evaluated in the shown frame (each answer adds its row, in order). */
void watch_evaluate(void)
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
    dap_request_full(dap, "evaluate", args, on_watch, var_asked_new(NULL, expression), var_asked_free);
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
  watch_evaluate();
}

void watch_remove(const char *expression)
{
  for (guint i = 0; i < view.watches->len; i++)
    if (strcmp(g_ptr_array_index(view.watches, i), expression) == 0) {
      g_ptr_array_remove_index(view.watches, i);
      break;
    }
  view.watch_round++;
  watch_evaluate();
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
  watch_evaluate();
}

void watch_on_entry(GtkEntry *entry, gpointer data G_GNUC_UNUSED)
{
  debug_view_add_watch(view.app, gtk_editable_get_text(GTK_EDITABLE(entry)));
  gtk_editable_set_text(GTK_EDITABLE(entry), "");
  gtk_widget_set_visible(GTK_WIDGET(entry), FALSE);
}

/* Escape: no expression after all. */
gboolean watch_on_entry_key(GtkEventControllerKey *keys G_GNUC_UNUSED, guint key, guint code G_GNUC_UNUSED,
                            GdkModifierType state G_GNUC_UNUSED, GtkWidget *entry)
{
  if (key != GDK_KEY_Escape)
    return FALSE;
  gtk_editable_set_text(GTK_EDITABLE(entry), "");
  gtk_widget_set_visible(entry, FALSE);
  return TRUE;
}

/* "+": an entry for the expression. */
void watch_on_add(GtkButton *button G_GNUC_UNUSED, gpointer data G_GNUC_UNUSED)
{
  gtk_widget_set_visible(view.watch_entry, TRUE);
  gtk_widget_grab_focus(view.watch_entry);
}
