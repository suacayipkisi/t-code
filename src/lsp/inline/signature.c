/*
 * The parameter hint (see signature.h): a box over the text (in the page's overlay, placed just
 * above the cursor), filled from textDocument/signatureHelp. While it shows, every change and cursor
 * move asks again (a moment later, once for a burst); an empty answer closes it.
 */
#include "lsp/inline/signature.h"

#include "editor/editorview.h"
#include "lsp/client/client.h"
#include "window/palette.h"

#include <string.h>

#define AGAIN_MS 80 /* typing within this asks once */

typedef struct Asking Asking;

struct SignatureHelp {
  Tab *tab;
  GtkWidget *box; /* the hint, in the page's overlay */
  GtkLabel *label;
  Asking *asking; /* the request whose answer counts */
  GCancellable *cancel;
  guint again_id;
  guint trigger_id; /* a "(" or "," typed: asked once the typing is done */
  char trigger;
  gboolean retrigger; /* it shows: the next request says so */
  /* The view's adjustments and controllers, which call back with h: they may outlive the editor
   * (a comparison's other side scrolls them), so signature_free() disconnects them. */
  GObject *senders[4];
};

/* One request. The answer to one that is not help->asking any more (a newer one went) is dropped;
 * help is NULL once the editor went. */
struct Asking {
  SignatureHelp *help;
};

static const char *const signature_css =
    ".tcode-signature { background: var(--tc-raised); border: 1px solid var(--tc-border-strong); border-radius: 4px;"
    "  padding: 4px 8px; box-shadow: 0 2px 8px rgba(0, 0, 0, .36); }"
    ".tcode-signature label { font-family: 'Cascadia Code', monospace; font-size: 0.95em; color: var(--tc-fg); }";

static void hide_hint(SignatureHelp *h)
{
  gtk_widget_set_visible(h->box, FALSE);
  h->retrigger = FALSE;
}

/* The hint's text ---------------------------------------------------------------------------- */

/* Byte offset in text after units UTF-16 code units (the spec's parameter offsets). */
static int utf16_offset(const char *text, gint64 units)
{
  const char *p = text;

  for (gint64 at = 0; *p && at < units; p = g_utf8_next_char(p))
    at += g_utf8_get_char(p) >= 0x10000 ? 2 : 1;
  return (int)(p - text);
}

/* The active parameter's place in the signature's label, in bytes: from its [start, end] offsets,
 * or else where its own label is found after the "(". FALSE when there is none. */
static gboolean parameter_span(const char *label, JsonNode *parameter, int *start, int *end)
{
  JsonNode *span = json_at(parameter, "label");
  const char *name, *found, *open;

  if (span && JSON_NODE_HOLDS_ARRAY(span)) {
    *start = utf16_offset(label, json_int_at(span, "0", 0));
    *end = utf16_offset(label, json_int_at(span, "1", 0));
    return *end > *start;
  }
  name = json_string_at(parameter, "label");
  open = strchr(label, '(');
  found = name && *name ? strstr(open ? open : label, name) : NULL;
  if (!found)
    return FALSE;
  *start = (int)(found - label);
  *end = *start + (int)strlen(name);
  return TRUE;
}

/* A documentation field: a MarkupContent's value, or a plain string. */
static const char *documentation(JsonNode *node)
{
  const char *value = json_string_at(node, "documentation.value");

  return value ? value : json_string_at(node, "documentation");
}

/* Appends text, escaped, on a line of its own in grey (small: the function's documentation). */
static void append_note(GString *markup, const char *text, gboolean small)
{
  char *escaped;

  if (!text || !*text)
    return;
  escaped = g_markup_escape_text(text, -1);
  g_string_append_printf(markup, "\n<span foreground=\"%s\"%s>%s</span>", palette_hex(PAL_FG_DIM),
                         small ? " size=\"small\"" : "", escaped);
  g_free(escaped);
}

/* "1/3  " (with overloads), the signature with the active parameter bold, then the parameter's and
 * the function's documentation. NULL when there is no signature. */
static char *hint_markup(JsonNode *result)
{
  JsonNode *signatures = json_at(result, "signatures"), *signature, *parameter;
  guint count =
      signatures && JSON_NODE_HOLDS_ARRAY(signatures) ? json_array_get_length(json_node_get_array(signatures)) : 0;
  gint64 active = json_int_at(result, "activeSignature", 0), index;
  char *path;
  const char *label;
  GString *markup;
  int start, end;

  if (count == 0)
    return NULL;
  active = CLAMP(active, 0, (gint64)count - 1);
  signature = json_array_get_element(json_node_get_array(signatures), (guint)active);
  label = json_string_at(signature, "label");
  if (!label)
    return NULL;
  markup = g_string_new(NULL);
  if (count > 1)
    g_string_append_printf(markup, "<span foreground=\"%s\">%" G_GINT64_FORMAT "/%u</span>  ", palette_hex(PAL_FG_DIM),
                           active + 1, count);
  index = json_int_at(signature, "activeParameter", json_int_at(result, "activeParameter", 0));
  path = g_strdup_printf("parameters.%" G_GINT64_FORMAT, index);
  parameter = json_at(signature, path);
  g_free(path);
  if (parameter && parameter_span(label, parameter, &start, &end)) {
    char *before = g_markup_escape_text(label, start), *inside = g_markup_escape_text(label + start, end - start);
    char *after = g_markup_escape_text(label + end, -1);
    g_string_append_printf(markup, "%s<b><span foreground=\"%s\">%s</span></b>%s", before, palette_hex(PAL_INFO),
                           inside, after);
    g_free(after);
    g_free(inside);
    g_free(before);
  } else {
    char *escaped = g_markup_escape_text(label, -1);
    g_string_append(markup, escaped);
    g_free(escaped);
  }
  append_note(markup, documentation(parameter), FALSE);
  append_note(markup, documentation(signature), TRUE);
  return g_string_free(markup, FALSE);
}

/* Placing it --------------------------------------------------------------------------------- */

/* The overlay places the hint: just above the cursor, or below it at the top of the view; never
 * past the right edge. */
static gboolean on_place(GtkOverlay *overlay, GtkWidget *child, GdkRectangle *place, SignatureHelp *h)
{
  GtkTextView *view = GTK_TEXT_VIEW(h->tab->source);
  GtkTextBuffer *buffer = GTK_TEXT_BUFFER(h->tab->buffer);
  int width, height, overlay_width = gtk_widget_get_width(GTK_WIDGET(overlay));
  GdkRectangle cursor;
  GtkTextIter iter;
  graphene_point_t at;

  if (child != h->box)
    return FALSE;
  gtk_text_buffer_get_iter_at_mark(buffer, &iter, gtk_text_buffer_get_insert(buffer));
  gtk_text_view_get_iter_location(view, &iter, &cursor);
  gtk_text_view_buffer_to_window_coords(view, GTK_TEXT_WINDOW_WIDGET, cursor.x, cursor.y, &cursor.x, &cursor.y);
  if (!gtk_widget_compute_point(GTK_WIDGET(view), GTK_WIDGET(overlay), &GRAPHENE_POINT_INIT(cursor.x, cursor.y), &at))
    return FALSE;
  gtk_widget_measure(child, GTK_ORIENTATION_HORIZONTAL, -1, NULL, &width, NULL, NULL);
  width = MIN(width, MAX(overlay_width - 8, 1));
  gtk_widget_measure(child, GTK_ORIENTATION_VERTICAL, width, NULL, &height, NULL, NULL);
  place->width = width;
  place->height = height;
  place->x = (int)CLAMP(at.x - 8, 0, MAX(overlay_width - width, 0));
  place->y = at.y - height - 2 >= 0 ? (int)(at.y - height - 2) : (int)(at.y + cursor.height + 2);
  return TRUE;
}

/* Scrolled: the hint goes with the cursor. */
static void on_scrolled(GtkAdjustment *adjustment G_GNUC_UNUSED, SignatureHelp *h)
{
  if (gtk_widget_get_visible(h->box))
    gtk_widget_queue_allocate(h->tab->view->overlay);
}

/* Asking ------------------------------------------------------------------------------------- */

static void on_hint(JsonNode *result, gpointer data)
{
  Asking *asking = data;
  SignatureHelp *h = asking->help;
  char *markup = h && result ? hint_markup(result) : NULL;

  if (h && h->asking == asking) {
    h->asking = NULL;
    if (!markup) { /* outside the call (or no server) */
      hide_hint(h);
    } else if (gtk_widget_has_focus(GTK_WIDGET(h->tab->source))) {
      gtk_label_set_markup(h->label, markup);
      gtk_widget_set_visible(h->box, TRUE);
      gtk_widget_queue_allocate(h->tab->view->overlay);
      h->retrigger = TRUE;
    }
  }
  g_free(markup);
  g_free(asking);
}

/* Asks for the hint at the cursor; trigger is the character typed (0: asked, or the cursor moved). */
static void ask(SignatureHelp *h, char trigger)
{
  GtkTextBuffer *buffer = GTK_TEXT_BUFFER(h->tab->buffer);
  GtkTextIter cursor;
  char *position, *params, trigger_json[48] = "";

  if (!lsp_has(h->tab))
    return;
  if (h->asking)
    h->asking->help = NULL;
  if (h->cancel) {
    g_cancellable_cancel(h->cancel);
    g_object_unref(h->cancel);
  }
  h->cancel = g_cancellable_new();
  h->asking = g_new0(Asking, 1);
  h->asking->help = h;
  gtk_text_buffer_get_iter_at_mark(buffer, &cursor, gtk_text_buffer_get_insert(buffer));
  position = lsp_position_params(h->tab, &cursor);
  if (trigger)
    g_snprintf(trigger_json, sizeof trigger_json, ",\"triggerCharacter\":\"%c\"", trigger);
  params = g_strdup_printf("{%s,\"context\":{\"triggerKind\":%d%s,\"isRetrigger\":%s}}", position,
                           trigger ? 2 : (h->retrigger ? 3 : 1), trigger_json, h->retrigger ? "true" : "false");
  lsp_request(h->tab, "textDocument/signatureHelp", params, h->cancel, on_hint, h->asking);
  g_free(params);
  g_free(position);
}

static gboolean ask_again(gpointer data)
{
  SignatureHelp *h = data;

  h->again_id = 0;
  ask(h, 0);
  return G_SOURCE_REMOVE;
}

/* While it shows, the hint follows the cursor and the typing. */
static void follow(SignatureHelp *h)
{
  if (gtk_widget_get_visible(h->box) && !h->again_id)
    h->again_id = g_timeout_add(AGAIN_MS, ask_again, h);
}

static gboolean ask_for_trigger(gpointer data)
{
  SignatureHelp *h = data;

  h->trigger_id = 0;
  ask(h, h->trigger);
  return G_SOURCE_REMOVE;
}

/* After the buffer took the text: "(" and "," (also "()" typed with its closer) open the hint. Not
 * at once: "()" goes in before the cursor is put between the two, and asked from after the ")" the
 * server would see the cursor outside the call. */
static void on_inserted(GtkTextBuffer *buffer G_GNUC_UNUSED, const GtkTextIter *where G_GNUC_UNUSED, const char *text,
                        int len, SignatureHelp *h)
{
  if (h->tab->view->loading)
    return;
  if (len >= 1 && len <= 2 && (text[0] == '(' || text[0] == ',')) {
    h->trigger = text[0];
    if (!h->trigger_id)
      h->trigger_id = g_idle_add(ask_for_trigger, h);
  } else {
    follow(h);
  }
}

static void on_deleted(GtkTextBuffer *buffer G_GNUC_UNUSED, const GtkTextIter *start G_GNUC_UNUSED,
                       const GtkTextIter *end G_GNUC_UNUSED, SignatureHelp *h)
{
  follow(h);
}

static void on_cursor(GtkTextBuffer *buffer, const GtkTextIter *where G_GNUC_UNUSED, GtkTextMark *mark,
                      SignatureHelp *h)
{
  if (mark == gtk_text_buffer_get_insert(buffer))
    follow(h);
}

/* Escape closes it (and goes on: it may also close the completion list or the find bar). */
static gboolean on_hint_key(GtkEventControllerKey *keys G_GNUC_UNUSED, guint key, guint code G_GNUC_UNUSED,
                            GdkModifierType state G_GNUC_UNUSED, SignatureHelp *h)
{
  if (key == GDK_KEY_Escape && gtk_widget_get_visible(h->box))
    hide_hint(h);
  return FALSE;
}

static void on_focus_out(GtkEventControllerFocus *focus G_GNUC_UNUSED, SignatureHelp *h)
{
  hide_hint(h);
}

void signature_attach(Tab *tab)
{
  SignatureHelp *h = g_new0(SignatureHelp, 1);
  GtkEventController *keys = gtk_event_controller_key_new(), *focus = gtk_event_controller_focus_new();

  palette_add_css(signature_css);
  h->tab = tab;
  h->label = GTK_LABEL(gtk_label_new(NULL));
  gtk_label_set_wrap(h->label, TRUE);
  gtk_label_set_max_width_chars(h->label, 90);
  gtk_label_set_xalign(h->label, 0);
  h->box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class(h->box, "tcode-signature");
  gtk_box_append(GTK_BOX(h->box), GTK_WIDGET(h->label));
  gtk_widget_set_can_target(h->box, FALSE); /* clicks go to the text under it */
  gtk_widget_set_visible(h->box, FALSE);
  gtk_overlay_add_overlay(GTK_OVERLAY(tab->view->overlay), h->box);
  g_signal_connect(tab->view->overlay, "get-child-position", G_CALLBACK(on_place), h);
  h->senders[0] = g_object_ref(G_OBJECT(gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(tab->source))));
  h->senders[1] = g_object_ref(G_OBJECT(gtk_scrollable_get_hadjustment(GTK_SCROLLABLE(tab->source))));
  h->senders[2] = g_object_ref(G_OBJECT(keys));
  h->senders[3] = g_object_ref(G_OBJECT(focus));
  g_signal_connect(h->senders[0], "value-changed", G_CALLBACK(on_scrolled), h);
  g_signal_connect(h->senders[1], "value-changed", G_CALLBACK(on_scrolled), h);

  g_signal_connect_after(tab->buffer, "insert-text", G_CALLBACK(on_inserted), h);
  g_signal_connect_after(tab->buffer, "delete-range", G_CALLBACK(on_deleted), h);
  g_signal_connect(tab->buffer, "mark-set", G_CALLBACK(on_cursor), h);
  gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
  g_signal_connect(keys, "key-pressed", G_CALLBACK(on_hint_key), h);
  gtk_widget_add_controller(GTK_WIDGET(tab->source), keys);
  g_signal_connect(focus, "leave", G_CALLBACK(on_focus_out), h);
  gtk_widget_add_controller(GTK_WIDGET(tab->source), focus);
  tab->view->signature = h;
}

void signature_show(Tab *tab)
{
  if (tab->view->signature)
    ask(tab->view->signature, 0);
}

void signature_free(SignatureHelp *h)
{
  if (!h)
    return;
  g_signal_handlers_disconnect_by_data(h->tab->buffer, h);
  for (gsize i = 0; i < G_N_ELEMENTS(h->senders); i++) {
    g_signal_handlers_disconnect_by_data(h->senders[i], h);
    g_object_unref(h->senders[i]);
  }
  g_clear_handle_id(&h->again_id, g_source_remove);
  g_clear_handle_id(&h->trigger_id, g_source_remove);
  if (h->asking)
    h->asking->help = NULL; /* its answer is dropped */
  if (h->cancel) {
    g_cancellable_cancel(h->cancel);
    g_object_unref(h->cancel);
  }
  g_free(h);
}
