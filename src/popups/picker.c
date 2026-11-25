/*
 * The popup list at the top of the window: a search entry and a list
 * of results. Go to File, the command palette and File History all use it; each gives a query
 * function that fills the list and a choose function for the picked row. With the title bar's search
 * bar shown, its entry is that bar and the list drops down from it; otherwise both are a box over the
 * top of the window's content.
 */
#include "popups/picker.h"

#include "editor/tabs.h"
#include "popups/quickopen.h"
#include "window/palette.h"

#include <string.h>

#define MAX_ROWS 200

struct Picker {
  App *app;
  GtkWidget *box;      /* the entry and the results over the window's content, without the search bar */
  GtkWidget *results;  /* the message and the list: in box, or in popover under the search bar */
  GtkPopover *popover; /* the search bar's dropdown (made the first time) */
  gboolean in_bar;     /* the entry is the search bar, the results in popover */
  gboolean shown;
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
    ".tcode-picker-list list { background: transparent; }"
    ".tcode-picker-list row { padding: 3px 8px; border-radius: 7px; }"
    ".tcode-picker-list row:selected { background: var(--tc-selection); }"
    ".tcode-picker-list .detail { color: var(--tc-fg-dim); font-size: smaller; }"
    ".tcode-picker-list .message { color: var(--tc-fg-dim); padding: 6px 8px; }"
    /* The search bar: one frame with its dropdown, the accent around both while it is open. The corners
     * come from window/theme.c (the corner radius setting). */
    ".tcode-search-bar { background: var(--tc-input); border: 1px solid var(--tc-border-strong); }"
    ".tcode-search-bar:hover { border-color: var(--tc-fg-faint); }"
    ".tcode-search-bar entry, .tcode-search-bar entry:focus-within { background: transparent; border: none;"
    "  box-shadow: none; outline: none; min-height: 24px; color: var(--tc-fg); }"
    ".tcode-search-bar.open { background: var(--tc-raised); border-color: var(--tc-accent);"
    "  border-bottom-color: var(--tc-border); }"
    ".tcode-picker-pop { padding: 0; margin: 0; }"
    ".tcode-picker-pop > contents { background: var(--tc-raised); border: 1px solid var(--tc-accent);"
    "  border-top: none; box-shadow: 0 8px 12px -4px rgba(0,0,0,.5); padding: 4px; margin: 0; }"; /* nothing above: the bar shows */

#define IDLE_PLACEHOLDER "Search files, # for commands"

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
  return MAX(total * 16 - (int)strlen(text), 0); /* a long text that matches still matches */
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

/* The entry's text changed: the results again. Typed into the idle search bar (it kept the focus):
 * Go to File with that text. */
static void on_search_changed(Picker *p)
{
  const char *text = gtk_editable_get_text(GTK_EDITABLE(p->entry));

  if (p->shown)
    refresh(p);
  else if (p->in_bar && *text)
    quick_open_show_text(p->app, text);
}

/* New data for query(): fill the list again, if the picker still shows that query's results. */
void picker_requery(App *t, PickerQuery query)
{
  Picker *p = t->picker;

  if (p->shown && p->query == query)
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

/* Hides the box, or the search bar's dropdown (the bar then empty again). */
static void hide_box(Picker *p)
{
  p->shown = FALSE;
  if (!p->in_bar) {
    gtk_widget_set_visible(p->box, FALSE);
    return;
  }
  gtk_popover_popdown(p->popover);
  gtk_widget_remove_css_class(p->app->layout.search_bar, "open");
  g_signal_handlers_block_matched(p->entry, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, p);
  gtk_editable_set_text(GTK_EDITABLE(p->entry), "");
  g_signal_handlers_unblock_matched(p->entry, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, p);
  gtk_search_entry_set_placeholder_text(p->entry, IDLE_PLACEHOLDER);
}

static void hide(Picker *p)
{
  p->generation++; /* late results go nowhere */
  hide_box(p);
  cancelled(p);
  focus_current_tab(p->app);
}

static void dismiss(Picker *p);

/* Focus went somewhere else (a click outside, a tab that opened): close without taking it back. Over
 * the content that is focus leaving the box; with the search bar, leaving the bar's entry (the
 * dropdown never takes it). */
static void on_focus_leave(GtkEventControllerFocus *focus, Picker *p)
{
  gboolean entry = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(focus)) == GTK_WIDGET(p->entry);

  if (entry == p->in_bar)
    dismiss(p);
}

/* Closes without a pick, the focus left where it is. */
static void dismiss(Picker *p)
{
  if (p->shown) {
    p->generation++;
    hide_box(p);
    cancelled(p);
  }
}

/* A click anywhere in the window but the picker (the dropdown is a window of its own: its clicks never
 * come here): it closes, and the click goes on to do what it does. */
static void on_window_pressed(GtkGestureClick *gesture, int n G_GNUC_UNUSED, double x, double y, Picker *p)
{
  GtkWidget *window = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
  GtkWidget *picked = gtk_widget_pick(window, x, y, GTK_PICK_DEFAULT);
  GtkWidget *inside = p->in_bar ? p->app->layout.search_bar : p->box;

  if (p->shown && !(picked && (picked == inside || gtk_widget_is_ancestor(picked, inside))))
    dismiss(p);
}

/* The window lost the focus to another one (another app): the dropdown closes. */
static void on_window_active(GtkWindow *window, GParamSpec *pspec G_GNUC_UNUSED, Picker *p)
{
  if (p->in_bar && !gtk_window_is_active(window))
    dismiss(p);
}

/* A click on the search bar while nothing shows: Go to File. */
static void on_bar_pressed(GtkGestureClick *gesture G_GNUC_UNUSED, int n G_GNUC_UNUSED, double x G_GNUC_UNUSED,
                           double y G_GNUC_UNUSED, Picker *p)
{
  if (p->in_bar && !p->shown)
    quick_open_show(p->app);
}

/* The search bar goes (the window closes), its children with it: the entry and the dropdown (p->popover
 * is a weak pointer). */
static void on_bar_destroy(GtkWidget *bar G_GNUC_UNUSED, Picker *p)
{
  p->in_bar = FALSE;
}

void picker_place(App *t)
{
  Picker *p = t->picker;
  GtkWidget *bar = t->layout.search_bar;
  gboolean in_bar = bar && t->settings.search_bar;

  if (!p || in_bar == p->in_bar)
    return;
  if (p->shown)
    hide(p);
  if (in_bar && !p->popover) {
    p->popover = GTK_POPOVER(gtk_popover_new());
    gtk_widget_add_css_class(GTK_WIDGET(p->popover), "tcode-picker-pop");
    gtk_popover_set_has_arrow(p->popover, FALSE);
    gtk_popover_set_autohide(p->popover, FALSE); /* the bar's entry keeps the keys */
    gtk_popover_set_position(p->popover, GTK_POS_BOTTOM);
    gtk_widget_set_parent(GTK_WIDGET(p->popover), bar);
    g_object_add_weak_pointer(G_OBJECT(p->popover), (gpointer *)&p->popover);
    g_signal_connect(bar, "destroy", G_CALLBACK(on_bar_destroy), p);
    GtkGesture *click = gtk_gesture_click_new();
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(click), GTK_PHASE_CAPTURE);
    g_signal_connect(click, "pressed", G_CALLBACK(on_bar_pressed), p);
    gtk_widget_add_controller(bar, GTK_EVENT_CONTROLLER(click));
  }
  g_object_ref(p->entry);
  g_object_ref(p->results);
  gtk_widget_unparent(GTK_WIDGET(p->entry));
  if (p->in_bar)
    gtk_popover_set_child(p->popover, NULL);
  else
    gtk_box_remove(GTK_BOX(p->box), p->results);
  if (in_bar) {
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(p->entry));
    gtk_popover_set_child(p->popover, p->results);
    gtk_search_entry_set_placeholder_text(p->entry, IDLE_PLACEHOLDER);
  } else {
    gtk_box_append(GTK_BOX(p->box), GTK_WIDGET(p->entry));
    gtk_box_append(GTK_BOX(p->box), p->results);
  }
  g_object_unref(p->entry);
  g_object_unref(p->results);
  p->in_bar = in_bar;
}

/* Shows the box, or the dropdown under the search bar as wide as the bar. */
static void show_box(Picker *p)
{
  GtkWidget *bar = p->app->layout.search_bar;

  p->shown = TRUE;
  if (!p->in_bar) {
    gtk_widget_set_visible(p->box, TRUE);
    return;
  }
  int width = gtk_widget_get_width(bar), height = gtk_widget_get_height(bar);
  gtk_widget_add_css_class(bar, "open");
  /* as wide as the bar, the dropdown's 4px padding on each side aside; a bar shown a moment ago may have
   * no width yet: then the list's own */
  gtk_widget_set_size_request(p->results, width > 8 ? width - 8 : -1, -1);
  gtk_popover_set_pointing_to(p->popover, &(const GdkRectangle){ 0, 0, width, height });
  gtk_popover_popup(p->popover);
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
  if (!p->shown)
    return;
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

  if (!step || !p->shown)
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
  picker_open_text(t, placeholder, "", query, chosen);
}

void picker_open_text(App *t, const char *placeholder, const char *text, PickerQuery query, PickerChoose chosen)
{
  Picker *p = t->picker;

  if (p->shown)
    cancelled(p); /* another picker over this one */
  p->query = query;
  p->choose = chosen;
  p->preview = NULL;
  p->cancel = NULL;
  gtk_search_entry_set_placeholder_text(p->entry, placeholder);
  g_signal_handlers_block_matched(p->entry, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, p);
  gtk_editable_set_text(GTK_EDITABLE(p->entry), text);
  g_signal_handlers_unblock_matched(p->entry, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, p);
  show_box(p);
  gtk_widget_grab_focus(GTK_WIDGET(p->entry));
  gtk_editable_set_position(GTK_EDITABLE(p->entry), -1); /* typing goes after the text, not over it */
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
  GtkEventController *entry_focus = gtk_event_controller_focus_new();

  p->app = t;
  p->box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  p->entry = GTK_SEARCH_ENTRY(gtk_search_entry_new());
  p->list = GTK_LIST_BOX(gtk_list_box_new());
  p->message = GTK_LABEL(gtk_label_new(NULL));
  p->results = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  t->picker = p;

  palette_add_css(css);

  gtk_widget_add_css_class(p->box, "tcode-picker");
  gtk_widget_add_css_class(p->results, "tcode-picker-list");
  gtk_widget_set_can_focus(p->results, FALSE);             /* a click on a row leaves the focus in the entry */
  gtk_editable_set_width_chars(GTK_EDITABLE(p->entry), 8); /* the search bar's width is its own */
  gtk_widget_set_hexpand(GTK_WIDGET(p->entry), TRUE);      /* the whole bar (which does not expand itself) */
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
  gtk_box_append(GTK_BOX(p->results), GTK_WIDGET(p->message));
  gtk_box_append(GTK_BOX(p->results), scroll);
  gtk_box_append(GTK_BOX(p->box), GTK_WIDGET(p->entry));
  gtk_box_append(GTK_BOX(p->box), p->results);

  g_signal_connect_swapped(p->entry, "search-changed", G_CALLBACK(on_search_changed), p);
  g_signal_connect(p->entry, "activate", G_CALLBACK(on_activate), p);
  g_signal_connect_swapped(p->entry, "stop-search", G_CALLBACK(hide), p);
  g_signal_connect(p->list, "row-activated", G_CALLBACK(on_row_activated), p);
  gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
  g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key), p);
  gtk_widget_add_controller(GTK_WIDGET(p->entry), keys);
  g_signal_connect(focus, "leave", G_CALLBACK(on_focus_leave), p);
  gtk_widget_add_controller(p->box, focus);
  g_signal_connect(entry_focus, "leave", G_CALLBACK(on_focus_leave), p);
  gtk_widget_add_controller(GTK_WIDGET(p->entry), entry_focus);

  gtk_widget_set_visible(p->box, FALSE);
  gtk_overlay_add_overlay(overlay, p->box);
  GtkGesture *click = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 0); /* any button */
  gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(click), GTK_PHASE_CAPTURE);
  g_signal_connect(click, "pressed", G_CALLBACK(on_window_pressed), p);
  gtk_widget_add_controller(GTK_WIDGET(t->window), GTK_EVENT_CONTROLLER(click));
  g_signal_connect(t->window, "notify::is-active", G_CALLBACK(on_window_active), p);
}
