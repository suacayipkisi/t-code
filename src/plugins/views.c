/*
 * Plugins' views (see views.h): a page (a vertical box) holding the text (a GtkTextView in a scrolled
 * window, or a Markdown text's preview), and for a view with an input its prompts, busy line and input
 * box under it. The page is the panel of a dock (window.h's add_panel) or of a TAB_VIEW tab. Each is
 * kept by its name, "view:<plugin>/<id>", until its page goes.
 */
#include "plugins/views.h"

#include "editor/preview/preview.h"
#include "editor/tabs.h"
#include "ipc/socket.h"
#include "window/palette.h"
#include "window/window.h"

#include <string.h>

typedef struct {
  App *t;
  char *owner, *id, *name, *title;
  GtkWidget *page;     /* the panel or the tab's page */
  GtkTextView *text;   /* a plain view's text, or NULL */
  Preview *markdown;   /* a Markdown view's, or NULL */
  GtkWidget *prompts;  /* the prompts' box above the input, or NULL without an input */
  GtkWidget *busy;     /* the busy line, hidden when not busy */
  GtkLabel *busy_text; /* its text */
  GtkTextView *input;  /* the input box, or NULL */
  GtkWidget *hint;     /* the input's placeholder, shown while it is empty */
  GtkDropDown *modes;  /* the modes beside the input, or NULL */
  gboolean in_dock;
} View;

static GHashTable *views; /* name → View */

static const char *const css =
    ".tcode-view-input { border: 1px solid var(--tc-border); border-radius: 8px; background: var(--tc-frame);"
    "  padding: 4px 4px 4px 8px; margin: 6px; }"
    ".tcode-view-input textview, .tcode-view-input textview text { background: transparent; }"
    ".tcode-view-input button { min-width: 24px; min-height: 24px; padding: 0 4px; border-radius: 6px; }"
    ".tcode-view-hint { color: var(--tc-fg-faint); }"
    ".tcode-view-prompt { border: 1px solid var(--tc-border); border-radius: 8px; padding: 6px 8px;"
    "  margin: 6px 6px 0; background: var(--tc-raised); }"
    ".tcode-view-busy { margin: 6px 8px 0; color: var(--tc-fg-dim); }";

static char *view_name(const char *owner, const char *id)
{
  return g_strdup_printf("view:%s/%s", owner, id);
}

static View *find_view(const char *owner, const char *id)
{
  char *name = view_name(owner, id);
  View *view = views ? g_hash_table_lookup(views, name) : NULL;

  g_free(name);
  return view;
}

gboolean view_place(const char *word, ViewPlace *place)
{
  static const char *const words[] = { "left", "bottom", "right", "editor", "side" };

  for (size_t i = 0; i < G_N_ELEMENTS(words); i++)
    if (g_strcmp0(word, words[i]) == 0) {
      *place = (ViewPlace)i;
      return TRUE;
    }
  return FALSE;
}

/* The view's page went (its panel taken away, its tab closed, the window gone). */
static void on_view_destroy(GtkWidget *widget G_GNUC_UNUSED, View *view)
{
  if (!view->in_dock && !view->t->closing && g_hash_table_lookup(views, view->name) == view)
    socket_view_closed(view->t, view->owner, view->id);
  g_hash_table_remove(views, view->name);
}

static void view_free(View *view)
{
  g_free(view->owner);
  g_free(view->id);
  g_free(view->name);
  g_free(view->title);
  g_free(view);
}

/* Plain text ---------------------------------------------------------------------------------- */

/* A line was activated: the plugin hears which (from 1) and its text. */
static void activate_line(View *view, GtkTextIter *at)
{
  GtkTextBuffer *buffer = gtk_text_view_get_buffer(view->text);
  GtkTextIter start = *at, end;
  char *text;

  gtk_text_iter_set_line_offset(&start, 0);
  end = start;
  if (!gtk_text_iter_ends_line(&end))
    gtk_text_iter_forward_to_line_end(&end);
  text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
  socket_view_activated(view->t, view->owner, view->id, gtk_text_iter_get_line(&start) + 1, text);
  g_free(text);
}

static void on_double_click(GtkGestureClick *gesture, int n_press, double x, double y, View *view)
{
  GtkTextIter at;
  int bx, by;

  if (n_press != 2)
    return;
  gtk_text_view_window_to_buffer_coords(view->text, GTK_TEXT_WINDOW_WIDGET, (int)x, (int)y, &bx, &by);
  gtk_text_view_get_iter_at_location(view->text, &at, bx, by);
  activate_line(view, &at);
  gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
}

static gboolean on_key(GtkEventControllerKey *keys G_GNUC_UNUSED, guint keyval, guint code G_GNUC_UNUSED,
                       GdkModifierType state G_GNUC_UNUSED, View *view)
{
  GtkTextBuffer *buffer = gtk_text_view_get_buffer(view->text);
  GtkTextIter at;

  if (keyval != GDK_KEY_Return && keyval != GDK_KEY_KP_Enter)
    return FALSE;
  gtk_text_buffer_get_iter_at_mark(buffer, &at, gtk_text_buffer_get_insert(buffer));
  activate_line(view, &at);
  return TRUE;
}

/* The plain text in a scrolled window. */
static GtkWidget *plain_text(View *view)
{
  GtkGesture *click = gtk_gesture_click_new();
  GtkEventController *keys = gtk_event_controller_key_new();
  GtkWidget *scrolled = gtk_scrolled_window_new();

  view->text = GTK_TEXT_VIEW(gtk_text_view_new());
  gtk_text_view_set_editable(view->text, FALSE);
  gtk_text_view_set_monospace(view->text, TRUE);
  gtk_text_view_set_left_margin(view->text, 8);
  gtk_text_view_set_top_margin(view->text, 6);
  gtk_widget_add_css_class(GTK_WIDGET(view->text), "tcode-view");
  g_signal_connect(click, "pressed", G_CALLBACK(on_double_click), view);
  gtk_widget_add_controller(GTK_WIDGET(view->text), GTK_EVENT_CONTROLLER(click));
  g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key), view);
  gtk_widget_add_controller(GTK_WIDGET(view->text), keys);
  gtk_widget_set_vexpand(scrolled, TRUE);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), GTK_WIDGET(view->text));
  return scrolled;
}

/* The input ----------------------------------------------------------------------------------- */

/* The mode picked beside the input, or NULL without modes. */
static const char *picked_mode(View *view)
{
  GtkStringObject *item = view->modes ? gtk_drop_down_get_selected_item(view->modes) : NULL;

  return item ? gtk_string_object_get_string(item) : NULL;
}

/* The input's text goes to the plugin (view_input) and the box empties; a blank one goes nowhere. */
static void send_input(View *view)
{
  GtkTextBuffer *buffer = gtk_text_view_get_buffer(view->input);
  GtkTextIter start, end;
  char *text;

  gtk_text_buffer_get_bounds(buffer, &start, &end);
  text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
  g_strstrip(text);
  if (*text) {
    socket_view_input(view->t, view->owner, view->id, text, picked_mode(view));
    gtk_text_buffer_set_text(buffer, "", 0);
  }
  g_free(text);
}

/* Enter sends; Shift+Enter starts a new line. */
static gboolean on_input_key(GtkEventControllerKey *keys G_GNUC_UNUSED, guint keyval, guint code G_GNUC_UNUSED,
                             GdkModifierType state, View *view)
{
  if ((keyval != GDK_KEY_Return && keyval != GDK_KEY_KP_Enter) || (state & GDK_SHIFT_MASK))
    return FALSE;
  send_input(view);
  return TRUE;
}

static void on_input_changed(GtkTextBuffer *buffer, View *view)
{
  gtk_widget_set_visible(view->hint, gtk_text_buffer_get_char_count(buffer) == 0);
}

static void on_stop(GtkButton *button G_GNUC_UNUSED, View *view)
{
  socket_view_stop(view->t, view->owner, view->id);
}

/* The prompts' box, the busy line and the input box with its modes and Send, appended to the page. */
static void add_input(View *view, const ViewOptions *options)
{
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4), *overlay = gtk_overlay_new();
  GtkWidget *scrolled = gtk_scrolled_window_new(), *send, *stop;
  GtkEventController *keys = gtk_event_controller_key_new();

  view->prompts = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append(GTK_BOX(view->page), view->prompts);

  view->busy = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_add_css_class(view->busy, "tcode-view-busy");
  view->busy_text = GTK_LABEL(gtk_label_new(""));
  gtk_label_set_xalign(view->busy_text, 0);
  gtk_label_set_ellipsize(view->busy_text, PANGO_ELLIPSIZE_END);
  gtk_widget_set_hexpand(GTK_WIDGET(view->busy_text), TRUE);
  stop = gtk_button_new_from_icon_name("tcode-stop-symbolic");
  gtk_widget_set_tooltip_text(stop, "Stop");
  gtk_button_set_has_frame(GTK_BUTTON(stop), FALSE);
  g_signal_connect(stop, "clicked", G_CALLBACK(on_stop), view);
  gtk_box_append(GTK_BOX(view->busy), GTK_WIDGET(view->busy_text));
  gtk_box_append(GTK_BOX(view->busy), stop);
  gtk_widget_set_visible(view->busy, FALSE);
  gtk_box_append(GTK_BOX(view->page), view->busy);

  view->input = GTK_TEXT_VIEW(gtk_text_view_new());
  gtk_text_view_set_wrap_mode(view->input, GTK_WRAP_WORD_CHAR);
  gtk_text_view_set_accepts_tab(view->input, FALSE);
  gtk_text_view_set_top_margin(view->input, 4);
  gtk_text_view_set_bottom_margin(view->input, 4);
  g_signal_connect(keys, "key-pressed", G_CALLBACK(on_input_key), view);
  gtk_widget_add_controller(GTK_WIDGET(view->input), keys);
  g_signal_connect(gtk_text_view_get_buffer(view->input), "changed", G_CALLBACK(on_input_changed), view);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scrolled), TRUE);
  gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scrolled), 160);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), GTK_WIDGET(view->input));
  gtk_overlay_set_child(GTK_OVERLAY(overlay), scrolled);
  view->hint = gtk_label_new(options->input);
  gtk_label_set_xalign(GTK_LABEL(view->hint), 0);
  gtk_label_set_ellipsize(GTK_LABEL(view->hint), PANGO_ELLIPSIZE_END);
  gtk_widget_set_halign(view->hint, GTK_ALIGN_FILL);
  gtk_widget_set_valign(view->hint, GTK_ALIGN_START);
  gtk_widget_set_margin_top(view->hint, 4);
  gtk_widget_set_can_target(view->hint, FALSE);
  gtk_widget_add_css_class(view->hint, "tcode-view-hint");
  gtk_overlay_add_overlay(GTK_OVERLAY(overlay), view->hint);
  gtk_widget_set_hexpand(overlay, TRUE);
  gtk_widget_set_valign(overlay, GTK_ALIGN_CENTER);
  gtk_box_append(GTK_BOX(box), overlay);

  if (options->modes && options->modes[0]) {
    view->modes = GTK_DROP_DOWN(gtk_drop_down_new_from_strings((const char *const *)options->modes));
    for (guint i = 0; options->mode && options->modes[i]; i++)
      if (strcmp(options->modes[i], options->mode) == 0)
        gtk_drop_down_set_selected(view->modes, i);
    gtk_widget_set_tooltip_text(GTK_WIDGET(view->modes), "Mode");
    gtk_widget_set_valign(GTK_WIDGET(view->modes), GTK_ALIGN_END);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(view->modes));
  }
  send = gtk_button_new_from_icon_name("tcode-arrow-up-symbolic");
  gtk_widget_set_tooltip_text(send, "Send (Enter; Shift+Enter: a new line)");
  gtk_widget_set_valign(send, GTK_ALIGN_END);
  g_signal_connect_swapped(send, "clicked", G_CALLBACK(send_input), view);
  gtk_box_append(GTK_BOX(box), send);
  gtk_widget_add_css_class(box, "tcode-view-input");
  gtk_box_append(GTK_BOX(view->page), box);
}

/* Making and showing views ------------------------------------------------------------------ */

static View *new_view(App *t, const char *owner, const char *id, const char *title, const ViewOptions *options)
{
  static gboolean styled;
  View *view = g_new0(View, 1);
  GtkWidget *content;

  if (!styled)
    palette_add_css(css);
  styled = TRUE;
  view->t = t;
  view->owner = g_strdup(owner);
  view->id = g_strdup(id);
  view->name = view_name(owner, id);
  view->title = g_strdup(title);
  view->page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_hexpand(view->page, TRUE);
  gtk_widget_set_vexpand(view->page, TRUE);
  if (options && options->markdown) {
    view->markdown = preview_text_new(t, t->project.root, &content);
    gtk_widget_set_vexpand(content, TRUE);
  } else {
    content = plain_text(view);
  }
  gtk_box_append(GTK_BOX(view->page), content);
  if (options && options->input)
    add_input(view, options);
  g_object_set_data_full(G_OBJECT(view->page), "t-code-view-title", g_strdup(title), g_free);
  g_signal_connect(view->page, "destroy", G_CALLBACK(on_view_destroy), view);
  if (!views)
    views = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, (GDestroyNotify)view_free);
  g_hash_table_insert(views, view->name, view);
  return view;
}

/* The tab showing a view, or NULL (it is in a dock). */
static Tab *view_tab(View *view)
{
  return find_tab(view->t, TAB_VIEW, view->name);
}

void view_open(App *t, const char *owner, const char *id, const char *title, ViewPlace place, const char *icon,
               const char *text, const ViewOptions *options)
{
  View *view = find_view(owner, id);

  if (!title || !*title)
    title = id;
  if (!view) {
    view = new_view(t, owner, id, title, options);
    if (place <= VIEW_RIGHT) {
      view->in_dock = TRUE;
      add_panel(t, view->name, title, icon && *icon ? icon : "tcode-plugins-symbolic", view->page, (int)place);
    } else {
      if (place == VIEW_SIDE)
        side_group(t);
      Tab *tab = add_tab(t, TAB_VIEW, view->page, view->name);
      keep_tab(tab);
    }
  } else if (g_strcmp0(view->title, title) != 0) {
    g_set_str(&view->title, title);
    g_object_set_data_full(G_OBJECT(view->page), "t-code-view-title", g_strdup(title), g_free);
    if (view->in_dock) {
      GtkStack *stack = panel_stack(t, view->name);
      if (stack)
        gtk_stack_page_set_title(gtk_stack_get_page(stack, view->page), title);
    } else if (view_tab(view)) {
      update_tab_label(view_tab(view));
    }
  }
  if (text)
    view_set_text(t, owner, id, text, VIEW_TEXT_SET);
  view_show(t, owner, id);
}

gboolean view_set_text(App *t G_GNUC_UNUSED, const char *owner, const char *id, const char *text, ViewTextChange change)
{
  View *view = find_view(owner, id);
  GtkTextBuffer *buffer;
  GtkTextIter end;

  if (!view)
    return FALSE;
  if (!text)
    text = "";
  if (view->markdown) {
    gboolean line = change == VIEW_TEXT_APPEND && *preview_text(view->markdown);
    char *added = line ? g_strconcat("\n", text, NULL) : g_strdup(text);
    preview_text_set(view->markdown, added, change != VIEW_TEXT_SET);
    g_free(added);
    return TRUE;
  }
  buffer = gtk_text_view_get_buffer(view->text);
  if (change == VIEW_TEXT_SET) {
    gtk_text_buffer_set_text(buffer, text, -1);
    return TRUE;
  }
  gtk_text_buffer_get_end_iter(buffer, &end);
  if (change == VIEW_TEXT_APPEND && gtk_text_buffer_get_char_count(buffer) > 0)
    gtk_text_buffer_insert(buffer, &end, "\n", 1);
  gtk_text_buffer_insert(buffer, &end, text, -1);
  gtk_text_buffer_get_end_iter(buffer, &end); /* a log: its end in view */
  gtk_text_view_scroll_to_iter(view->text, &end, 0, FALSE, 0, 1);
  return TRUE;
}

gboolean view_show(App *t, const char *owner, const char *id)
{
  View *view = find_view(owner, id);

  if (!view)
    return FALSE;
  if (view->in_dock) {
    show_sidebar_page(t, view->name);
  } else if (view_tab(view)) {
    Tab *tab = view_tab(view);
    gtk_notebook_set_current_page(tab_group(tab), tab_position(tab));
  }
  if (view->input)
    gtk_widget_grab_focus(GTK_WIDGET(view->input));
  return TRUE;
}

gboolean view_close(App *t, const char *owner, const char *id)
{
  View *view = find_view(owner, id);

  if (!view)
    return FALSE;
  g_signal_handlers_disconnect_by_func(view->page, on_view_destroy, view); /* no view_closed: the plugin did */
  if (view->in_dock)
    remove_panel(t, view->name);
  else if (view_tab(view))
    remove_tab(view_tab(view));
  g_hash_table_remove(views, view->name);
  return TRUE;
}

/* Prompts, busy, input ------------------------------------------------------------------------- */

/* The prompt named prompt, or NULL. */
static GtkWidget *find_prompt(View *view, const char *prompt)
{
  for (GtkWidget *card = gtk_widget_get_first_child(view->prompts); card; card = gtk_widget_get_next_sibling(card))
    if (g_strcmp0(g_object_get_data(G_OBJECT(card), "t-code-prompt"), prompt) == 0)
      return card;
  return NULL;
}

static void on_choice(GtkButton *button, View *view)
{
  GtkWidget *card = g_object_get_data(G_OBJECT(button), "t-code-card");
  char *prompt = g_strdup(g_object_get_data(G_OBJECT(card), "t-code-prompt"));

  socket_view_choice(view->t, view->owner, view->id, prompt, gtk_button_get_label(button));
  gtk_box_remove(GTK_BOX(view->prompts), card);
  if (view->input)
    gtk_widget_grab_focus(GTK_WIDGET(view->input));
  g_free(prompt);
}

gboolean view_prompt(App *t G_GNUC_UNUSED, const char *owner, const char *id, const char *prompt, const char *text,
                     char *const *choices)
{
  View *view = find_view(owner, id);
  GtkWidget *card, *message, *buttons;

  if (!view || !view->input)
    return FALSE;
  if ((card = find_prompt(view, prompt)))
    gtk_box_remove(GTK_BOX(view->prompts), card);
  if (!choices || !choices[0])
    return TRUE;
  card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_add_css_class(card, "tcode-view-prompt");
  g_object_set_data_full(G_OBJECT(card), "t-code-prompt", g_strdup(prompt), g_free);
  message = gtk_label_new(text);
  gtk_label_set_xalign(GTK_LABEL(message), 0);
  gtk_label_set_wrap(GTK_LABEL(message), TRUE);
  gtk_label_set_wrap_mode(GTK_LABEL(message), PANGO_WRAP_WORD_CHAR);
  gtk_label_set_selectable(GTK_LABEL(message), TRUE);
  gtk_box_append(GTK_BOX(card), message);
  buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_halign(buttons, GTK_ALIGN_END);
  for (int i = 0; choices[i]; i++) {
    GtkWidget *button = gtk_button_new_with_label(choices[i]);
    if (i == 0)
      gtk_widget_add_css_class(button, "suggested-action");
    g_object_set_data(G_OBJECT(button), "t-code-card", card);
    g_signal_connect(button, "clicked", G_CALLBACK(on_choice), view);
    gtk_box_append(GTK_BOX(buttons), button);
  }
  gtk_box_append(GTK_BOX(card), buttons);
  gtk_box_append(GTK_BOX(view->prompts), card);
  return TRUE;
}

gboolean view_busy(App *t G_GNUC_UNUSED, const char *owner, const char *id, const char *text)
{
  View *view = find_view(owner, id);

  if (!view || !view->input)
    return FALSE;
  gtk_label_set_text(view->busy_text, text ? text : "");
  gtk_widget_set_visible(view->busy, text != NULL);
  return TRUE;
}

gboolean view_input(App *t, const char *owner, const char *id, const char *text)
{
  View *view = find_view(owner, id);
  GtkTextBuffer *buffer;
  GtkTextIter end;

  if (!view || !view->input)
    return FALSE;
  buffer = gtk_text_view_get_buffer(view->input);
  gtk_text_buffer_set_text(buffer, text ? text : "", -1);
  if (text && *text && !g_ascii_isspace(text[strlen(text) - 1])) { /* typing goes on after it */
    gtk_text_buffer_get_end_iter(buffer, &end);
    gtk_text_buffer_insert(buffer, &end, " ", 1);
  }
  gtk_text_buffer_get_end_iter(buffer, &end);
  gtk_text_buffer_place_cursor(buffer, &end);
  view_show(t, owner, id);
  return TRUE;
}

const char *view_title(Tab *tab)
{
  return tab->kind == TAB_VIEW ? g_object_get_data(G_OBJECT(tab->page), "t-code-view-title") : NULL;
}
