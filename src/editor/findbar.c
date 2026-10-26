/*
 * The find bar (see findbar.h), on GtkSourceView's search: GtkSourceSearchSettings hold what is
 * searched, a GtkSourceSearchContext finds, counts and highlights it in the buffer.
 */
#include "editor/findbar.h"
#include "window/palette.h"

#include <string.h>

#define MAX_SEED 200 /* a longer selection is not taken as the text to find */

struct FindBar {
  Tab *tab;
  GtkWidget *bar, *replace_row, *expand;
  GtkEntry *find, *replace;
  GtkLabel *count;
  GtkSourceSearchSettings *settings;
  GtkSourceSearchContext *context;
  guint count_id;
};

static const char *const find_css =
    ".tcode-find { background: var(--tc-raised); border: 1px solid var(--tc-input); border-top: none;"
    "  border-radius: 0 0 6px 6px; padding: 4px 6px; box-shadow: 0 2px 8px rgba(0, 0, 0, .36); }"
    ".tcode-find entry { min-height: 24px; background: var(--tc-input); border: 1px solid var(--tc-border-strong); border-radius: 4px;"
    "  box-shadow: none; color: var(--tc-fg); }"
    ".tcode-find entry:focus-within { border-color: var(--tc-accent); outline: none; }"
    ".tcode-find entry.error { border-color: var(--tc-error); }"
    ".tcode-find button { min-height: 22px; min-width: 22px; padding: 0 4px; border-radius: 4px;"
    "  background: none; box-shadow: none; border: 1px solid transparent; color: var(--tc-fg); }"
    ".tcode-find button:hover { background: color-mix(in srgb, var(--tc-fg) 10%, transparent); }"
    ".tcode-find button:checked { background: color-mix(in srgb, var(--tc-accent) 51%, transparent); border-color: var(--tc-accent); color: var(--tc-fg-strong); }"
    ".tcode-find button.text { font-size: 0.85em; }"
    ".tcode-find label.count { color: var(--tc-fg); font-size: 0.85em; min-width: 72px; }"
    ".tcode-find label.count.none { color: var(--tc-error); }";

/* Matches ------------------------------------------------------------------------------------ */

/* "3 of 12", "No results", or "?" while the context still counts. */
static void update_count(FindBar *f)
{
  GtkTextBuffer *buffer = GTK_TEXT_BUFFER(f->tab->buffer);
  int total = gtk_source_search_context_get_occurrences_count(f->context);
  GtkTextIter start, end;
  char *text;

  gtk_widget_remove_css_class(GTK_WIDGET(f->count), "none");
  if (!*gtk_editable_get_text(GTK_EDITABLE(f->find))) {
    gtk_label_set_text(f->count, "");
    return;
  }
  if (total < 0) {
    gtk_label_set_text(f->count, "?");
    return;
  }
  if (total == 0) {
    gtk_label_set_text(f->count, "No results");
    gtk_widget_add_css_class(GTK_WIDGET(f->count), "none");
    return;
  }
  gtk_text_buffer_get_selection_bounds(buffer, &start, &end);
  int position = gtk_source_search_context_get_occurrence_position(f->context, &start, &end);
  text = position > 0 ? g_strdup_printf("%d of %d", position, total) : g_strdup_printf("? of %d", total);
  gtk_label_set_text(f->count, text);
  g_free(text);
}

/* Selects a match and scrolls to it, the cursor at its end. */
static void show_match(FindBar *f, GtkTextIter *start, GtkTextIter *end)
{
  GtkTextBuffer *buffer = GTK_TEXT_BUFFER(f->tab->buffer);

  gtk_text_buffer_select_range(buffer, end, start);
  editor_view_reveal_cursor(f->tab);
  update_count(f);
}

/* Searches from the selection: from its start (again: typing more of the word keeps the same
 * match), its end (next), or backwards from its start (previous). */
static void search(FindBar *f, gboolean again, gboolean backward)
{
  GtkTextBuffer *buffer = GTK_TEXT_BUFFER(f->tab->buffer);
  GtkTextIter from, start, end, match_start, match_end;
  gboolean found;

  if (!gtk_source_search_settings_get_search_text(f->settings))
    return;
  gtk_text_buffer_get_selection_bounds(buffer, &start, &end);
  from = backward || again ? start : end;
  found = backward ? gtk_source_search_context_backward(f->context, &from, &match_start, &match_end, NULL)
                   : gtk_source_search_context_forward(f->context, &from, &match_start, &match_end, NULL);
  if (found)
    show_match(f, &match_start, &match_end);
  else
    update_count(f);
}

/* The bar's widgets ------------------------------------------------------------------------- */

static void on_find_changed(GtkEditable *entry, FindBar *f)
{
  const char *text = gtk_editable_get_text(entry);

  gtk_source_search_settings_set_search_text(f->settings, *text ? text : NULL);
  search(f, TRUE, FALSE);
}

/* An invalid regular expression: the entry turns red. */
static void on_regex_error(GtkSourceSearchContext *context, GParamSpec *spec G_GNUC_UNUSED, FindBar *f)
{
  GError *error = gtk_source_search_context_get_regex_error(context);

  if (error)
    gtk_widget_add_css_class(GTK_WIDGET(f->find), "error");
  else
    gtk_widget_remove_css_class(GTK_WIDGET(f->find), "error");
  gtk_widget_set_tooltip_text(GTK_WIDGET(f->find), error ? error->message : NULL);
  g_clear_error(&error);
}

static gboolean update_count_now(gpointer data)
{
  FindBar *f = data;

  f->count_id = 0;
  update_count(f);
  return G_SOURCE_REMOVE;
}

/* Counted: "x of N" a moment later, once the context also knows where the selected match is. */
static void on_count_changed(GtkSourceSearchContext *context G_GNUC_UNUSED, GParamSpec *spec G_GNUC_UNUSED, FindBar *f)
{
  if (!f->count_id)
    f->count_id = g_idle_add(update_count_now, f);
}

/* The cursor moved (a click, the arrows): "3 of 12" follows. */
static void on_cursor_moved(GtkTextBuffer *buffer, const GtkTextIter *where G_GNUC_UNUSED, GtkTextMark *mark,
                            FindBar *f)
{
  if (mark == gtk_text_buffer_get_insert(buffer) && gtk_widget_get_visible(f->bar))
    update_count(f);
}

static void hide(FindBar *f)
{
  gtk_widget_set_visible(f->bar, FALSE);
  gtk_source_search_context_set_highlight(f->context, FALSE);
  gtk_widget_grab_focus(GTK_WIDGET(f->tab->source));
}

static void on_next(FindBar *f)
{
  search(f, FALSE, FALSE);
}

static void on_previous(FindBar *f)
{
  search(f, FALSE, TRUE);
}

/* Replaces the selected match (when the selection is one) and goes on to the next. */
static void on_replace(FindBar *f)
{
  GtkTextBuffer *buffer = GTK_TEXT_BUFFER(f->tab->buffer);
  GtkTextIter start, end;

  if (!gtk_text_view_get_editable(GTK_TEXT_VIEW(f->tab->source)))
    return;
  gtk_text_buffer_get_selection_bounds(buffer, &start, &end);
  if (gtk_source_search_context_get_occurrence_position(f->context, &start, &end) > 0)
    gtk_source_search_context_replace(f->context, &start, &end, gtk_editable_get_text(GTK_EDITABLE(f->replace)), -1,
                                      NULL);
  search(f, FALSE, FALSE);
}

static void on_replace_all(FindBar *f)
{
  if (gtk_text_view_get_editable(GTK_TEXT_VIEW(f->tab->source)))
    gtk_source_search_context_replace_all(f->context, gtk_editable_get_text(GTK_EDITABLE(f->replace)), -1, NULL);
}

static void on_expand(FindBar *f)
{
  gboolean show = !gtk_widget_get_visible(f->replace_row);

  gtk_widget_set_visible(f->replace_row, show);
  gtk_button_set_icon_name(GTK_BUTTON(f->expand), show ? "pan-down-symbolic" : "pan-end-symbolic");
}

/* Keys in the bar: Shift+Enter finds the previous match, Escape closes the bar. */
static gboolean on_bar_key(GtkEventControllerKey *keys G_GNUC_UNUSED, guint key, guint code G_GNUC_UNUSED,
                           GdkModifierType state, FindBar *f)
{
  if (key == GDK_KEY_Escape) {
    hide(f);
    return TRUE;
  }
  if ((key == GDK_KEY_Return || key == GDK_KEY_KP_Enter) && (state & GDK_SHIFT_MASK) &&
      gtk_widget_has_focus(GTK_WIDGET(f->find))) {
    on_previous(f);
    return TRUE;
  }
  return FALSE;
}

/* Escape in the text closes the bar too. */
static gboolean on_view_key(GtkEventControllerKey *keys G_GNUC_UNUSED, guint key, guint code G_GNUC_UNUSED,
                            GdkModifierType state G_GNUC_UNUSED, FindBar *f)
{
  if (key != GDK_KEY_Escape || !gtk_widget_get_visible(f->bar))
    return FALSE;
  hide(f);
  return TRUE;
}

/* A small flat button: an icon, or a label (Aa, ab, .*). */
static GtkWidget *bar_button(GtkWidget *box, gboolean toggle, const char *icon, const char *label, const char *tip)
{
  GtkWidget *button = toggle ? gtk_toggle_button_new() : gtk_button_new();

  if (icon) {
    gtk_button_set_icon_name(GTK_BUTTON(button), icon);
  } else {
    gtk_button_set_label(GTK_BUTTON(button), label);
    gtk_widget_add_css_class(button, "text");
  }
  gtk_widget_set_tooltip_text(button, tip);
  gtk_widget_set_focus_on_click(button, FALSE);
  gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
  gtk_box_append(GTK_BOX(box), button);
  return button;
}

/* Another option: the match is looked for again from the selection (Aa can make "FOO" no match). */
static void on_option(GtkToggleButton *button G_GNUC_UNUSED, FindBar *f)
{
  search(f, TRUE, FALSE);
}

/* A toggle that sets a boolean property of the search settings. */
static void option(FindBar *f, GtkWidget *box, const char *label, const char *tip, const char *property)
{
  GtkWidget *button = bar_button(box, TRUE, NULL, label, tip);

  g_object_bind_property(button, "active", f->settings, property, G_BINDING_DEFAULT);
  g_signal_connect_after(button, "toggled", G_CALLBACK(on_option), f); /* after the binding set it */
}

static FindBar *findbar_new(Tab *tab)
{
  FindBar *f = g_new0(FindBar, 1);
  GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2), *spacer, *button;
  GtkEventController *keys;

  palette_add_css(find_css);
  f->tab = tab;
  f->settings = gtk_source_search_settings_new();
  gtk_source_search_settings_set_wrap_around(f->settings, TRUE);
  f->context = gtk_source_search_context_new(tab->buffer, f->settings);
  g_signal_connect(f->context, "notify::occurrences-count", G_CALLBACK(on_count_changed), f);
  g_signal_connect(f->context, "notify::regex-error", G_CALLBACK(on_regex_error), f);
  g_signal_connect(tab->buffer, "mark-set", G_CALLBACK(on_cursor_moved), f);

  f->bar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_add_css_class(f->bar, "tcode-find");
  gtk_widget_set_halign(f->bar, GTK_ALIGN_END);
  gtk_widget_set_valign(f->bar, GTK_ALIGN_START);
  gtk_widget_set_margin_end(f->bar, 18); /* clear of the scrollbar */

  f->expand = bar_button(row, FALSE, "pan-end-symbolic", NULL, "Toggle Replace (Ctrl+H)");
  g_signal_connect_swapped(f->expand, "clicked", G_CALLBACK(on_expand), f);
  f->find = GTK_ENTRY(gtk_entry_new());
  gtk_entry_set_placeholder_text(f->find, "Find");
  gtk_editable_set_width_chars(GTK_EDITABLE(f->find), 26);
  g_signal_connect(f->find, "changed", G_CALLBACK(on_find_changed), f);
  g_signal_connect_swapped(f->find, "activate", G_CALLBACK(on_next), f);
  gtk_box_append(GTK_BOX(row), GTK_WIDGET(f->find));
  option(f, row, "Aa", "Match Case", "case-sensitive");
  option(f, row, "ab", "Match Whole Word", "at-word-boundaries");
  option(f, row, ".*", "Use Regular Expression", "regex-enabled");
  f->count = GTK_LABEL(gtk_label_new(NULL));
  gtk_widget_add_css_class(GTK_WIDGET(f->count), "count");
  gtk_box_append(GTK_BOX(row), GTK_WIDGET(f->count));
  button = bar_button(row, FALSE, "go-up-symbolic", NULL, "Previous Match (Shift+Enter, Shift+F3)");
  g_signal_connect_swapped(button, "clicked", G_CALLBACK(on_previous), f);
  button = bar_button(row, FALSE, "go-down-symbolic", NULL, "Next Match (Enter, F3)");
  g_signal_connect_swapped(button, "clicked", G_CALLBACK(on_next), f);
  button = bar_button(row, FALSE, "window-close-symbolic", NULL, "Close (Escape)");
  g_signal_connect_swapped(button, "clicked", G_CALLBACK(hide), f);
  gtk_box_append(GTK_BOX(f->bar), row);

  f->replace_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
  spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_size_request(spacer, 28, -1); /* under the expand button */
  gtk_box_append(GTK_BOX(f->replace_row), spacer);
  f->replace = GTK_ENTRY(gtk_entry_new());
  gtk_entry_set_placeholder_text(f->replace, "Replace");
  gtk_editable_set_width_chars(GTK_EDITABLE(f->replace), 26);
  g_signal_connect_swapped(f->replace, "activate", G_CALLBACK(on_replace), f);
  gtk_box_append(GTK_BOX(f->replace_row), GTK_WIDGET(f->replace));
  button = bar_button(f->replace_row, FALSE, NULL, "Replace", "Replace (Enter)");
  g_signal_connect_swapped(button, "clicked", G_CALLBACK(on_replace), f);
  button = bar_button(f->replace_row, FALSE, NULL, "All", "Replace All");
  g_signal_connect_swapped(button, "clicked", G_CALLBACK(on_replace_all), f);
  gtk_widget_set_visible(f->replace_row, FALSE);
  gtk_box_append(GTK_BOX(f->bar), f->replace_row);

  keys = gtk_event_controller_key_new();
  gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
  g_signal_connect(keys, "key-pressed", G_CALLBACK(on_bar_key), f);
  gtk_widget_add_controller(f->bar, keys);
  keys = gtk_event_controller_key_new();
  g_signal_connect(keys, "key-pressed", G_CALLBACK(on_view_key), f);
  gtk_widget_add_controller(GTK_WIDGET(tab->source), keys);

  gtk_widget_set_visible(f->bar, FALSE);
  gtk_overlay_add_overlay(GTK_OVERLAY(tab->view->overlay), f->bar);
  return f;
}

void findbar_show(Tab *tab, gboolean replace)
{
  FindBar *f = tab->view->find ? tab->view->find : (tab->view->find = findbar_new(tab));
  GtkTextBuffer *buffer = GTK_TEXT_BUFFER(tab->buffer);
  GtkTextIter start, end;

  if (gtk_text_buffer_get_selection_bounds(buffer, &start, &end) &&
      gtk_text_iter_get_line(&start) == gtk_text_iter_get_line(&end)) {
    char *selected = gtk_text_buffer_get_text(buffer, &start, &end, TRUE);
    /* The match is the selection itself: no jump to the next one. When it is what is looked for
     * already (a match the bar selected), it stays selected: "1 of 3", and Replace takes it. */
    if (strlen(selected) <= MAX_SEED && strcmp(selected, gtk_editable_get_text(GTK_EDITABLE(f->find))) != 0) {
      gtk_text_buffer_place_cursor(buffer, &start);
      gtk_editable_set_text(GTK_EDITABLE(f->find), selected);
    }
    g_free(selected);
  }
  if (gtk_widget_get_visible(f->replace_row) != replace)
    on_expand(f);
  gtk_widget_set_visible(f->bar, TRUE);
  gtk_source_search_context_set_highlight(f->context, TRUE);
  gtk_widget_grab_focus(GTK_WIDGET(replace && *gtk_editable_get_text(GTK_EDITABLE(f->find)) ? f->replace : f->find));
  gtk_editable_select_region(GTK_EDITABLE(f->find), 0, -1);
  update_count(f);
}

void findbar_next(Tab *tab, gboolean backward)
{
  FindBar *f = tab->view->find;

  if (!f || !gtk_source_search_settings_get_search_text(f->settings)) {
    findbar_show(tab, FALSE);
    return;
  }
  gtk_source_search_context_set_highlight(f->context, TRUE);
  gtk_widget_set_visible(f->bar, TRUE);
  search(f, FALSE, backward);
}

void findbar_free(FindBar *f)
{
  if (!f)
    return;
  g_signal_handlers_disconnect_by_data(f->tab->buffer, f);
  g_signal_handlers_disconnect_by_data(f->context, f);
  g_clear_handle_id(&f->count_id, g_source_remove);
  g_object_unref(f->context);
  g_object_unref(f->settings);
  g_free(f);
}
