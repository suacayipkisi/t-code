/*
 * Markdown preview tabs (see preview.h): the file becomes text with tags in one read-only
 * GtkTextView (markdown.c, view.c), no browser engine, so a preview costs about what its text does.
 * Formulas are drawn by math.c. Links open (anchors scroll to their heading, Markdown files open as previews, other files in an
 * editor, the rest in the browser). The preview follows the file as it is saved, keeping its
 * scroll position.
 */
#include "editor/preview/state.h"

#include "editor/editor.h"
#include "editor/tabs.h"
#include "window/palette.h"
#include "window/terminal.h"

#include <string.h>

static const char *const css =
    ".tcode-preview, .tcode-preview text { background: var(--tc-panel); color: var(--tc-fg); }"
    ".tcode-preview-table { border: 1px solid var(--tc-border-strong); }"
    ".tcode-preview-table > label, .tcode-preview-table > box { padding: 6px 10px; color: var(--tc-fg); }"
    ".tcode-preview-table > box label { color: var(--tc-fg); }"
    ".tcode-preview-table > .head { background: var(--tc-border); color: var(--tc-fg-strong); }"
    ".tcode-preview-table > box.head label { color: var(--tc-fg-strong); font-weight: bold; }"
    ".tcode-preview-table > .odd { background: var(--tc-hover); }"
    ".tcode-preview-table label link { color: var(--tc-link); }";

gboolean is_markdown(const char *path)
{
  static const char *const extensions[] = { ".md", ".markdown", ".mdown", ".mkd", NULL };
  const char *ext = strrchr(path, '.');

  for (int i = 0; ext && extensions[i]; i++)
    if (g_ascii_strcasecmp(ext, extensions[i]) == 0)
      return TRUE;
  return FALSE;
}

/* Whether path's extension is one of extensions (lower case, with their dots). */
static gboolean has_extension(const char *path, const char *const *extensions)
{
  const char *ext = strrchr(path, '.');

  for (int i = 0; ext && extensions[i]; i++)
    if (g_ascii_strcasecmp(ext, extensions[i]) == 0)
      return TRUE;
  return FALSE;
}

gboolean is_image(const char *path)
{
  static const char *const extensions[] = { ".png", ".jpg", ".jpeg", ".gif",  ".webp", ".bmp",
                                            ".ico", ".tif", ".tiff", ".avif", ".jxl",  NULL };

  return has_extension(path, extensions);
}

gboolean has_preview(const char *path)
{
  static const char *const svg[] = { ".svg", NULL };

  return is_markdown(path) || has_extension(path, svg);
}

/* Rendering ---------------------------------------------------------------------------------- */

static void clear_links(Preview *p)
{
  for (guint i = 0; i < p->links->len; i++) {
    g_free(g_array_index(p->links, Link, i).url);
    g_free(g_array_index(p->links, Link, i).title);
  }
  g_array_set_size(p->links, 0);
}

/* Shows the file again from the top line it showed before. */
static void reload(Preview *p)
{
  GdkRectangle visible;
  GtkTextIter iter;
  int top_line = -1;

  if (gtk_text_buffer_get_char_count(p->buffer) > 0) {
    gtk_text_view_get_visible_rect(p->view, &visible);
    gtk_text_view_get_line_at_y(p->view, &iter, visible.y, NULL);
    top_line = gtk_text_iter_get_line(&iter);
  }
  g_ptr_array_set_size(p->tables, 0);
  g_ptr_array_set_size(p->pictures, 0);
  g_array_set_size(p->code_spans, 0);
  g_array_set_size(p->blocks, 0);
  g_array_set_size(p->details, 0);
  clear_links(p);
  p->hovered = -1;
  g_hash_table_remove_all(p->anchors);
  gtk_text_buffer_set_text(p->buffer, "", 0);
  preview_render(p);
  gtk_text_buffer_get_start_iter(p->buffer, &iter);
  gtk_text_buffer_place_cursor(p->buffer, &iter);
  preview_update_layout(p, p->width); /* 0: the view's width, once it has one */
  if (top_line > 0) {
    gtk_text_buffer_get_iter_at_line(p->buffer, &iter, top_line);
    GtkTextMark *mark = gtk_text_buffer_get_mark(p->buffer, "t-code-top");
    if (mark)
      gtk_text_buffer_move_mark(p->buffer, mark, &iter);
    else
      mark = gtk_text_buffer_create_mark(p->buffer, "t-code-top", &iter, TRUE);
    gtk_text_view_scroll_to_mark(p->view, mark, 0, TRUE, 0, 0);
  }
}

/* on_file_changed()'s timeout. */
static gboolean reload_now(gpointer data)
{
  Preview *p = data;

  p->reload_id = 0;
  reload(p);
  return G_SOURCE_REMOVE;
}

void preview_reload_later(Preview *p)
{
  g_clear_handle_id(&p->reload_id, g_source_remove);
  p->reload_id = g_timeout_add(150, reload_now, p);
}

/* The file was saved: show it again shortly, once for a burst of changes. */
static void on_file_changed(GFileMonitor *monitor G_GNUC_UNUSED, GFile *file G_GNUC_UNUSED, GFile *other G_GNUC_UNUSED,
                            GFileMonitorEvent event, Preview *p)
{
  if (event == G_FILE_MONITOR_EVENT_DELETED || event == G_FILE_MONITOR_EVENT_ATTRIBUTE_CHANGED)
    return;
  preview_reload_later(p);
}

/* Details -------------------------------------------------------------------------------------- */

void preview_show_details(Preview *p, Details *details)
{
  GtkTextIter from, to;

  if (details->summary < 0 || details->content < 0 || details->end < details->content)
    return; /* no summary to click: it stays open */
  gtk_text_buffer_get_iter_at_offset(p->buffer, &from, details->summary);
  to = from;
  gtk_text_iter_forward_char(&to);
  gtk_text_buffer_delete(p->buffer, &from, &to);
  gtk_text_buffer_insert(p->buffer, &from, details->open ? "▼" : "▶", -1);
  gtk_text_buffer_get_iter_at_offset(p->buffer, &from, details->summary);
  to = from;
  gtk_text_iter_forward_char(&to);
  gtk_text_buffer_apply_tag_by_name(p->buffer, "bold", &from, &to);
  gtk_text_buffer_apply_tag_by_name(p->buffer, "base", &from, &to);
  gtk_text_buffer_get_iter_at_offset(p->buffer, &from, details->content);
  gtk_text_buffer_get_iter_at_offset(p->buffer, &to, details->end);
  if (details->open)
    gtk_text_buffer_remove_tag_by_name(p->buffer, "hidden", &from, &to);
  else
    gtk_text_buffer_apply_tag_by_name(p->buffer, "hidden", &from, &to);
}

/* The <details> whose summary is at offset, or NULL. */
static Details *details_at(Preview *p, int offset)
{
  for (guint i = 0; i < p->details->len; i++) {
    Details *details = &g_array_index(p->details, Details, i);
    if (details->summary >= 0 && offset >= details->summary && offset < details->content)
      return details;
  }
  return NULL;
}

/* Links -------------------------------------------------------------------------------------- */

/* The link at a point of the view, or NULL. */
static const Link *link_at(Preview *p, double x, double y)
{
  GtkTextIter iter;
  int bx, by;

  gtk_text_view_window_to_buffer_coords(p->view, GTK_TEXT_WINDOW_WIDGET, (int)x, (int)y, &bx, &by);
  if (!gtk_text_view_get_iter_at_location(p->view, &iter, bx, by) ||
      !gtk_text_iter_has_tag(&iter, gtk_text_tag_table_lookup(gtk_text_buffer_get_tag_table(p->buffer), "link")))
    return NULL;
  int offset = gtk_text_iter_get_offset(&iter);
  for (guint i = 0; i < p->links->len; i++) {
    const Link *link = &g_array_index(p->links, Link, i);
    if (offset >= link->start && offset < link->end)
      return link;
  }
  return NULL;
}

/* Anchors scroll to their heading, Markdown files open as previews, other local files in an
 * editor, and everything else in the default app (the browser, for web links). */
void preview_follow(Preview *p, const char *url)
{
  char *dir = g_path_get_dirname(p->tab->path), *path = preview_local_path(dir, url);

  if (url[0] == '#') {
    char *slug = g_uri_unescape_string(url + 1, NULL);
    gpointer offset;
    if (slug && g_hash_table_lookup_extended(p->anchors, slug, NULL, &offset)) {
      GtkTextIter iter;
      gtk_text_buffer_get_iter_at_offset(p->buffer, &iter, GPOINTER_TO_INT(offset));
      gtk_text_view_scroll_to_iter(p->view, &iter, 0, TRUE, 0, 0);
    }
    g_free(slug);
  } else if (path && is_markdown(path) && g_file_test(path, G_FILE_TEST_IS_REGULAR)) {
    open_preview(p->tab->app, path);
  } else if (path && g_file_test(path, G_FILE_TEST_IS_REGULAR)) {
    open_file(p->tab->app, path);
  } else if (!path) {
    GtkUriLauncher *launcher = gtk_uri_launcher_new(url);
    gtk_uri_launcher_launch(launcher, p->tab->app->window, NULL, NULL, NULL);
    g_object_unref(launcher);
  }
  g_free(path);
  g_free(dir);
}

/* The offset of the text at a point of the view, or -1. */
static int offset_at(Preview *p, double x, double y)
{
  GtkTextIter iter;
  int bx, by;

  gtk_text_view_window_to_buffer_coords(p->view, GTK_TEXT_WINDOW_WIDGET, (int)x, (int)y, &bx, &by);
  return gtk_text_view_get_iter_at_location(p->view, &iter, bx, by) ? gtk_text_iter_get_offset(&iter) : -1;
}

static void on_click(GtkGestureClick *gesture G_GNUC_UNUSED, int n_press, double x, double y, Preview *p)
{
  gboolean plain = n_press == 1 && !gtk_text_buffer_get_has_selection(p->buffer);
  const Link *link = plain ? link_at(p, x, y) : NULL;
  Details *details = plain && !link ? details_at(p, offset_at(p, x, y)) : NULL;

  if (details) { /* a summary: its details open or close */
    details->open = !details->open;
    preview_show_details(p, details);
  } else if (link) {
    char *url = g_strdup(link->url); /* following it may show the file again */
    preview_follow(p, url);
    g_free(url);
  }
}

/* Underlines the link at index (-1: none) and only it: the one under the pointer. */
static void hover_link(Preview *p, int index)
{
  GtkTextIter from, to;

  if (index == p->hovered)
    return;
  if (p->hovered >= 0 && (guint)p->hovered < p->links->len) {
    const Link *old = &g_array_index(p->links, Link, p->hovered);
    gtk_text_buffer_get_iter_at_offset(p->buffer, &from, old->start);
    gtk_text_buffer_get_iter_at_offset(p->buffer, &to, old->end);
    gtk_text_buffer_remove_tag_by_name(p->buffer, "link-hover", &from, &to);
  }
  p->hovered = index;
  if (index >= 0) {
    const Link *link = &g_array_index(p->links, Link, index);
    gtk_text_buffer_get_iter_at_offset(p->buffer, &from, link->start);
    gtk_text_buffer_get_iter_at_offset(p->buffer, &to, link->end);
    gtk_text_buffer_apply_tag_by_name(p->buffer, "link-hover", &from, &to);
  }
}

/* The hand over links, and the link underlined. */
static void on_motion(GtkEventControllerMotion *motion G_GNUC_UNUSED, double x, double y, Preview *p)
{
  const Link *link = link_at(p, x, y);
  gboolean over = link != NULL;

  hover_link(p, link ? (int)(link - &g_array_index(p->links, Link, 0)) : -1);
  if (over != p->over_link) {
    p->over_link = over;
    gtk_widget_set_cursor_from_name(GTK_WIDGET(p->view), over ? "pointer" : "text");
  }
}

static void on_leave(GtkEventControllerMotion *motion G_GNUC_UNUSED, Preview *p)
{
  hover_link(p, -1);
}

/* Over a link: its title, else where it goes. */
static gboolean on_query_tooltip(GtkWidget *view G_GNUC_UNUSED, int x, int y, gboolean keyboard, GtkTooltip *tooltip,
                                 Preview *p)
{
  const Link *link = keyboard ? NULL : link_at(p, x, y);

  if (!link)
    return FALSE;
  gtk_tooltip_set_text(tooltip, link->title && *link->title ? link->title : link->url);
  return TRUE;
}

/* Tabs --------------------------------------------------------------------------------------- */

void preview_free(Preview *p)
{
  g_cancellable_cancel(p->cancel); /* the background fetches: their answers find no preview */
  g_object_unref(p->cancel);
  g_clear_handle_id(&p->reload_id, g_source_remove);
  if (p->monitor) {
    g_file_monitor_cancel(p->monitor);
    g_object_unref(p->monitor);
  }
  clear_links(p);
  g_array_free(p->links, TRUE);
  g_array_free(p->code_spans, TRUE);
  g_array_free(p->blocks, TRUE);
  g_hash_table_destroy(p->anchors);
  g_hash_table_destroy(p->colors);
  g_ptr_array_free(p->tables, TRUE);
  g_ptr_array_free(p->pictures, TRUE);
  g_hash_table_destroy(p->rises);
  g_hash_table_destroy(p->remote);
  g_array_free(p->details, TRUE);
  g_free(p);
}

void preview_restyle(Tab *tab)
{
  if (!tab->preview)
    return;
  preview_color_tags(tab->preview->buffer);
  reload(tab->preview); /* code blocks colored with the new scheme */
}

void preview_set_zoom(Tab *tab, double scale)
{
  if (tab->preview) {
    tab->preview->scale = scale;
    reload(tab->preview); /* the tables' cells and columns are sized at the text size */
  }
}

void open_preview(App *t, const char *path)
{
  Tab *open = find_tab(t, TAB_PREVIEW, path);

  if (open) {
    select_tab(open);
    return;
  }
  palette_add_css(css);

  Preview *p = g_new0(Preview, 1);
  GtkWidget *scrolled = gtk_scrolled_window_new();
  GtkGesture *click = gtk_gesture_click_new();
  GtkEventController *motion = gtk_event_controller_motion_new();
  GFile *file = g_file_new_for_path(path);

  p->scale = t->settings.preview_scale;
  p->image = !is_markdown(path); /* an image, or an SVG drawn */
  p->links = g_array_new(FALSE, FALSE, sizeof(Link));
  p->code_spans = g_array_new(FALSE, FALSE, sizeof(CodeSpan));
  p->blocks = g_array_new(FALSE, FALSE, sizeof(Block));
  p->hovered = -1;
  p->anchors = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  p->colors = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  p->tables = g_ptr_array_new_with_free_func(preview_table_free);
  p->pictures = g_ptr_array_new();
  p->rises = g_hash_table_new(NULL, NULL);
  p->remote = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, preview_remote_free);
  p->cancel = g_cancellable_new();
  p->details = g_array_new(FALSE, FALSE, sizeof(Details));
  p->buffer = gtk_text_buffer_new(NULL);
  PangoFontDescription *font = pango_font_description_from_string(t->settings.font ? t->settings.font : DEFAULT_FONT);
  const char *family = pango_font_description_get_family(font);
  char *families = g_strconcat(family ? family : "monospace", ",monospace", NULL); /* when it is not installed */
  preview_create_tags(p->buffer, families);
  g_free(families);
  pango_font_description_free(font);
  p->view = preview_view_new(p);
  g_object_unref(p->buffer); /* the view holds it */
  gtk_text_view_set_editable(p->view, FALSE);
  gtk_text_view_set_cursor_visible(p->view, FALSE);
  gtk_text_view_set_wrap_mode(p->view, GTK_WRAP_WORD_CHAR);
  gtk_text_view_set_pixels_inside_wrap(p->view, 5);
  gtk_text_view_set_left_margin(p->view, MARGIN);
  gtk_text_view_set_right_margin(p->view, MARGIN);
  gtk_text_view_set_top_margin(p->view, 24);
  gtk_text_view_set_bottom_margin(p->view, 48);
  gtk_widget_add_css_class(GTK_WIDGET(p->view), "tcode-preview");
  /* Nothing is wider than the view (code blocks wrap, images and tables are sized to it), but
   * GtkTextView measures lines with a hanging indent (list items) a few pixels too wide: no
   * horizontal scrollbar for those. EXTERNAL, not NEVER, which would make the view's width the
   * scrolled window's minimum. */
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), p->image ? GTK_POLICY_AUTOMATIC : GTK_POLICY_EXTERNAL,
                                 GTK_POLICY_AUTOMATIC); /* an image at its size may be wider than the view */
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), GTK_WIDGET(p->view));
  gtk_widget_set_hexpand(scrolled, TRUE);
  gtk_widget_set_vexpand(scrolled, TRUE);
  g_signal_connect(click, "released", G_CALLBACK(on_click), p);
  gtk_widget_add_controller(GTK_WIDGET(p->view), GTK_EVENT_CONTROLLER(click));
  g_signal_connect(motion, "motion", G_CALLBACK(on_motion), p);
  g_signal_connect(motion, "leave", G_CALLBACK(on_leave), p);
  gtk_widget_add_controller(GTK_WIDGET(p->view), motion);
  gtk_widget_set_has_tooltip(GTK_WIDGET(p->view), TRUE);
  g_signal_connect(p->view, "query-tooltip", G_CALLBACK(on_query_tooltip), p);

  p->tab = add_tab(t, TAB_PREVIEW, scrolled, path);
  p->tab->preview = p;
  p->monitor = g_file_monitor_file(file, G_FILE_MONITOR_WATCH_MOVES, NULL, NULL);
  if (p->monitor)
    g_signal_connect(p->monitor, "changed", G_CALLBACK(on_file_changed), p);
  g_object_unref(file);

  reload(p);
  focus_current_tab(t);
}
