/*
 * The popup list at the top of the window: a search entry and a list
 * of results. Go to File, the command palette and File History all use it; each gives a query
 * function that fills the list and a choose function for the picked row.
 */
#include "popups/picker.h"

#include "editor/tabs.h"
#include "window/palette.h"

#include <string.h>

#define MAX_ROWS 200

struct Picker {
  App *app;
  GtkWidget *box;
  GtkSearchEntry *entry;
  GtkListBox *list;
  GtkLabel *message;
  PickerQuery query;
  PickerChoose choose;
  PickerChoose preview; /* the row the arrows went to; NULL: none */
  PickerCancel cancel;  /* closed without a pick */
  guint generation;
  int rows;
};

static const char *const css =
    ".tcode-picker { background: var(--tc-raised); border: 1px solid var(--tc-border-strong); border-radius: 12px;"
    "  box-shadow: 0 4px 16px rgba(0,0,0,.6); padding: 6px; }"
    ".tcode-picker list { background: transparent; }"
    ".tcode-picker row { padding: 3px 8px; border-radius: 7px; }"
    ".tcode-picker row:selected { background: var(--tc-selection); }"
    ".tcode-picker .detail { color: var(--tc-fg-dim); font-size: smaller; }"
    ".tcode-picker .message { color: var(--tc-fg-dim); padding: 6px 8px; }";

/* Matching ----------------------------------------------------------------------------------- */

/* Higher is better; -1 when the query letters do not all appear in order. Matches at the start
 * of a word and runs of consecutive letters count more, and so do matches after the last '/'. */
int fuzzy_score(const char *query, const char *text)
{
  const char *base = strrchr(text, '/');
  const char *p = text;
  int total = 0, prev = -2;

  base = base ? base + 1 : text;
  for (const char *q = query; *q; q++) {
    char c = g_ascii_tolower(*q);
    if (c == ' ')
      continue;
    while (*p && g_ascii_tolower(*p) != c)
      p++;
    if (!*p)
      return -1;
    int at = p - text;
    total += 1;
    if (at == prev + 1)
      total += 5;
    if (at == 0 || strchr("/_-. :", text[at - 1]))
      total += 8;
    if (p >= base)
      total += 3;
    prev = at;
    p++;
  }
  return total * 16 - (int)strlen(text);
}

/* Rows --------------------------------------------------------------------------------------- */

void picker_add(App *t, guint generation, const char *label, const char *detail, const char *payload)
{
  Picker *p = t->picker;

  if (generation != p->generation || p->rows >= MAX_ROWS)
    return;

  GtkWidget *row = gtk_list_box_row_new();
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  GtkWidget *main_label = gtk_label_new(label);

  gtk_label_set_ellipsize(GTK_LABEL(main_label), PANGO_ELLIPSIZE_END);
  gtk_label_set_xalign(GTK_LABEL(main_label), 0);
  gtk_box_append(GTK_BOX(box), main_label);
  if (detail && *detail) {
    GtkWidget *detail_label = gtk_label_new(detail);
    gtk_widget_add_css_class(detail_label, "detail");
    gtk_label_set_ellipsize(GTK_LABEL(detail_label), PANGO_ELLIPSIZE_START);
    gtk_widget_set_hexpand(detail_label, TRUE);
    gtk_label_set_xalign(GTK_LABEL(detail_label), 1);
    gtk_box_append(GTK_BOX(box), detail_label);
  }
  gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
  g_object_set_data_full(G_OBJECT(row), "payload", g_strdup(payload), g_free);
  gtk_list_box_append(p->list, row);
  if (p->rows++ == 0)
    gtk_list_box_select_row(p->list, GTK_LIST_BOX_ROW(row));
  gtk_widget_set_visible(GTK_WIDGET(p->message), FALSE);
}

/* A line of text instead of results ("Searching…", "No results"); NULL hides it. */
void picker_message(App *t, guint generation, const char *text)
{
  Picker *p = t->picker;

  if (generation != p->generation)
    return;
  gtk_label_set_text(p->message, text ? text : "");
  gtk_widget_set_visible(GTK_WIDGET(p->message), text != NULL);
}

/* The text changed: a new generation (older results are dropped), an empty list, and the query
 * runs. */
static void refresh(Picker *p)
{
  p->generation++;
  p->rows = 0;
  gtk_list_box_remove_all(p->list);
  gtk_widget_set_visible(GTK_WIDGET(p->message), FALSE);
  p->query(p->app, gtk_editable_get_text(GTK_EDITABLE(p->entry)), p->generation);
}

/* New data for query(): fill the list again, if the picker still shows that query's results. */
void picker_requery(App *t, PickerQuery query)
{
  Picker *p = t->picker;

  if (gtk_widget_get_visible(p->box) && p->query == query)
    refresh(p);
}

/* Showing and choosing ----------------------------------------------------------------------- */

/* Closed without a pick (Escape, a click outside): the caller undoes what its previews did. */
static void cancelled(Picker *p)
{
  PickerCancel cancel = p->cancel;

  p->preview = NULL;
  p->cancel = NULL;
  if (cancel)
    cancel(p->app);
}

static void hide(Picker *p)
{
  p->generation++; /* late results go nowhere */
  gtk_widget_set_visible(p->box, FALSE);
  cancelled(p);
  focus_current_tab(p->app);
}

/* Focus went somewhere else (a click outside, a tab that opened): close without taking it back. */
static void on_focus_leave(GtkEventControllerFocus *focus G_GNUC_UNUSED, Picker *p)
{
  if (gtk_widget_get_visible(p->box)) {
    p->generation++;
    gtk_widget_set_visible(p->box, FALSE);
    cancelled(p);
  }
}

/* A row was picked: the picker closes, then the caller's choose function gets the row's payload. */
static void choose(Picker *p, GtkListBoxRow *row)
{
  if (!row)
    return;
  char *payload = g_strdup(g_object_get_data(G_OBJECT(row), "payload"));
  PickerChoose chosen = p->choose;

  p->preview = NULL;
  p->cancel = NULL; /* picked: nothing to undo */
  hide(p);
  chosen(p->app, payload);
  g_free(payload);
}

/* Enter in the entry picks the selected row. */
static void on_activate(GtkSearchEntry *entry G_GNUC_UNUSED, Picker *p)
{
  choose(p, gtk_list_box_get_selected_row(p->list));
}

/* A click (or Enter) on a row picks it. */
static void on_row_activated(GtkListBox *list G_GNUC_UNUSED, GtkListBoxRow *row, Picker *p)
{
  choose(p, row);
}

/* Up / Down / Page Up / Page Down move through the results while typing goes on in the entry. */
static gboolean on_key(GtkEventControllerKey *controller G_GNUC_UNUSED, guint key, guint code G_GNUC_UNUSED,
                       GdkModifierType state G_GNUC_UNUSED, Picker *p)
{
  int step = key == GDK_KEY_Down        ? 1
             : key == GDK_KEY_Up        ? -1
             : key == GDK_KEY_Page_Down ? 10
             : key == GDK_KEY_Page_Up   ? -10
                                        : 0;

  if (!step)
    return FALSE;
  if (p->rows == 0)
    return TRUE;
  GtkListBoxRow *row = gtk_list_box_get_selected_row(p->list);
  int index = CLAMP((row ? gtk_list_box_row_get_index(row) : 0) + step, 0, p->rows - 1);
  GtkListBoxRow *next = gtk_list_box_get_row_at_index(p->list, index);
  gtk_list_box_select_row(p->list, next);
  gtk_widget_grab_focus(GTK_WIDGET(p->entry));
  if (p->preview && next)
    p->preview(p->app, g_object_get_data(G_OBJECT(next), "payload"));
  return TRUE;
}

void picker_open(App *t, const char *placeholder, PickerQuery query, PickerChoose chosen)
{
  Picker *p = t->picker;

  if (gtk_widget_get_visible(p->box))
    cancelled(p); /* another picker over this one */
  p->query = query;
  p->choose = chosen;
  p->preview = NULL;
  p->cancel = NULL;
  gtk_search_entry_set_placeholder_text(p->entry, placeholder);
  g_signal_handlers_block_matched(p->entry, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, p);
  gtk_editable_set_text(GTK_EDITABLE(p->entry), "");
  g_signal_handlers_unblock_matched(p->entry, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, p);
  gtk_widget_set_visible(p->box, TRUE);
  gtk_widget_grab_focus(GTK_WIDGET(p->entry));
  refresh(p);
}

void picker_set_preview(App *t, PickerChoose preview, PickerCancel cancel)
{
  t->picker->preview = preview;
  t->picker->cancel = cancel;
}

void picker_init(App *t, GtkOverlay *overlay)
{
  Picker *p = g_new0(Picker, 1);
  GtkWidget *scroll = gtk_scrolled_window_new();
  GtkEventController *keys = gtk_event_controller_key_new();
  GtkEventController *focus = gtk_event_controller_focus_new();

  p->app = t;
  p->box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  p->entry = GTK_SEARCH_ENTRY(gtk_search_entry_new());
  p->list = GTK_LIST_BOX(gtk_list_box_new());
  p->message = GTK_LABEL(gtk_label_new(NULL));
  t->picker = p;

  palette_add_css(css);

  gtk_widget_add_css_class(p->box, "tcode-picker");
  gtk_widget_set_size_request(p->box, 640, -1);
  gtk_widget_set_halign(p->box, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(p->box, GTK_ALIGN_START);
  gtk_widget_set_margin_top(p->box, 8);
  gtk_widget_add_css_class(GTK_WIDGET(p->message), "message");
  gtk_label_set_xalign(p->message, 0);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), 440);
  gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), TRUE);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(p->list));
  gtk_box_append(GTK_BOX(p->box), GTK_WIDGET(p->entry));
  gtk_box_append(GTK_BOX(p->box), GTK_WIDGET(p->message));
  gtk_box_append(GTK_BOX(p->box), scroll);

  g_signal_connect_swapped(p->entry, "search-changed", G_CALLBACK(refresh), p);
  g_signal_connect(p->entry, "activate", G_CALLBACK(on_activate), p);
  g_signal_connect_swapped(p->entry, "stop-search", G_CALLBACK(hide), p);
  g_signal_connect(p->list, "row-activated", G_CALLBACK(on_row_activated), p);
  gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
  g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key), p);
  gtk_widget_add_controller(GTK_WIDGET(p->entry), keys);
  g_signal_connect(focus, "leave", G_CALLBACK(on_focus_leave), p);
  gtk_widget_add_controller(p->box, focus);

  gtk_widget_set_visible(p->box, FALSE);
  gtk_overlay_add_overlay(overlay, p->box);
}
