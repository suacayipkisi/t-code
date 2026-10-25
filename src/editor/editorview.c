/*
 * An editor tab's page (see editorview.h): the source view, the minimap next to it, the style
 * scheme, the font and the settings.
 */
#include "editor/editorview.h"

#include "editor/detect.h"
#include "debug/debughover.h"
#include "editor/breakpoints.h"
#include "editor/diffgutter.h"
#include "editor/editing.h"
#include "editor/findbar.h"
#include "editor/folding.h"
#include "editor/guides.h"
#include "editor/occurrences.h"
#include "editor/sticky.h"
#include "editor/tabs.h"
#include "lsp/client.h"
#include "lsp/completion.h"
#include "lsp/diagnostics.h"
#include "lsp/hover.h"
#include "lsp/navigate.h"
#include "lsp/semantic.h"
#include "lsp/signature.h"
#include "window/commands.h"
#include "window/statusbar.h"
#include "window/terminal.h"
#include "window/palette.h"

#include <string.h>

static const char *const css =
    ".tcode-editor scrollbar { background: none; border: none; }"
    ".tcode-editor scrollbar slider { border-radius: 6px; background: rgba(121, 121, 121, .4); }"
    ".tcode-editor scrollbar.vertical slider { min-width: 6px; min-height: 24px; }"
    /* Long lines without word wrap: a bar as easy to see and grab as the vertical one. */
    ".tcode-editor scrollbar.horizontal slider { min-height: 8px; min-width: 24px; }"
    ".tcode-editor scrollbar slider:hover { background: rgba(100, 100, 100, .7); }"
    ".tcode-editor textview, .tcode-editor textview text { background: var(--tc-panel); }"
    ".tcode-editor textview border.left gutter { background: var(--tc-panel); padding-right: 12px; }"
    ".tcode-editor .tcode-minimap { border-left: 1px solid var(--tc-border); }"
    ".tcode-editor .tcode-overview { border-left: 1px solid var(--tc-border); }"
    ".tcode-conflict { background: var(--tc-warning-bg); color: var(--tc-fg); padding: 4px 8px; border-bottom: 1px solid var(--tc-warning-border); }"
    ".tcode-conflict button { padding: 2px 10px; min-height: 0; }"
    /* Completion: square and compact, not GtkSourceView's rounded
     * list of 28px rows. */
    "GtkSourceAssistant.completion { border-radius: 0; background: var(--tc-raised);"
    "  box-shadow: 0 0 0 1px var(--tc-border-strong), 0 2px 8px rgba(0, 0, 0, .36); outline: none; }"
    "GtkSourceAssistant.completion.above-line { box-shadow: 0 0 0 1px var(--tc-border-strong), 0 -2px 8px rgba(0, 0, 0, .36); }"
    "GtkSourceAssistant.completion list, GtkSourceAssistant.completion list row,"
    "GtkSourceAssistant.completion scrollbar.vertical, GtkSourceAssistant.completion box.details"
    "  { border-radius: 0; }"
    "GtkSourceAssistant.completion list row { min-height: 0; padding: 0; color: var(--tc-fg); }"
    "GtkSourceAssistant.completion list row:hover:not(:selected) { background: var(--tc-hover); }"
    "GtkSourceAssistant.completion list row:selected, GtkSourceAssistant.completion list row:selected:backdrop"
    "  { background: var(--tc-selection); color: var(--tc-fg-strong); box-shadow: inset 0 0 0 1px var(--tc-accent); }"
    "GtkSourceAssistant.completion list row cell.icon { padding: 2px 4px 2px 6px; min-width: 14px; min-height: 14px;"
    "  -gtk-icon-size: 14px; }"
    "GtkSourceAssistant.completion list row cell.typed-text { min-height: 20px; margin: 0 4px; }"
    "GtkSourceAssistant.completion list row cell.after { margin: 0 8px 0 24px; opacity: .6; }"
    "GtkSourceAssistant.completion box.details { background: var(--tc-raised); border-top: 1px solid var(--tc-border-strong); }";

#define OVERVIEW_WIDTH 8 /* pixels */

static GtkCssProvider *font_provider; /* the editor font; editor_view_set_font() rewrites it */

/* The first editor: the CSS all editors share, and where the style scheme is. */
static void init_look(App *t)
{
  GtkCssProvider *provider = gtk_css_provider_new();
  GtkSourceStyleSchemeManager *schemes = gtk_source_style_scheme_manager_get_default();

  gtk_css_provider_load_from_string(provider, css);
  gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(provider),
                                             GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref(provider);
  font_provider = gtk_css_provider_new();
  gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(font_provider),
                                             GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  editor_view_set_font(t);
  if (t->paths.styles)
    gtk_source_style_scheme_manager_prepend_search_path(schemes, t->paths.styles);
  char *mine = user_colors_file(), *dir = g_path_get_dirname(mine);
  gtk_source_style_scheme_manager_prepend_search_path(schemes, dir); /* the user's copy comes first */
  g_free(dir);
  g_free(mine);
}

char *user_colors_file(void)
{
  return g_build_filename(g_get_user_config_dir(), "t-code", "styles", "dark-classic.xml", NULL);
}

void editor_view_reload_colors(void)
{
  gtk_source_style_scheme_manager_force_rescan(gtk_source_style_scheme_manager_get_default());
}

void editor_view_set_font(App *t)
{
  PangoFontDescription *desc = pango_font_description_from_string(t->settings.font ? t->settings.font : DEFAULT_FONT);
  double size = pango_font_description_get_size(desc) / (double)PANGO_SCALE;
  char points[G_ASCII_DTOSTR_BUF_SIZE], *rule;

  if (!font_provider)
    return;
  if (size <= 0)
    size = 11;
  /* "11.5", also where the locale writes "11,5" (CSS would reject it) */
  g_ascii_formatd(points, sizeof points, "%.1f", size * t->font_scale);
  rule = g_strdup_printf(
      ".tcode-editor textview.tcode-source { font-family: \"%s\", monospace; font-size: %spt;"
      " font-feature-settings: \"calt\" 0, \"liga\" 0; }", /* "->" stays two characters */
      pango_font_description_get_family(desc) ? pango_font_description_get_family(desc) : "monospace", points);
  gtk_css_provider_load_from_string(font_provider, rule);
  g_free(rule);
  pango_font_description_free(desc);
}

GtkSourceLanguage *editor_language(const char *path, const char *text)
{
  GtkSourceLanguageManager *languages = gtk_source_language_manager_get_default();
  char *type = g_content_type_guess(path, NULL, 0, NULL);
  GtkSourceLanguage *language = gtk_source_language_manager_guess_language(languages, path, type);

  if (language && g_strcmp0(gtk_source_language_get_id(language), "chdr") == 0 && text && is_cxx_text(text, -1))
    language = gtk_source_language_manager_get_language(languages, "cpphdr");
  g_free(type);
  return language;
}

void editor_view_reveal_cursor(Tab *tab)
{
  GtkTextView *view = GTK_TEXT_VIEW(tab->source);
  GtkTextMark *insert = gtk_text_buffer_get_insert(GTK_TEXT_BUFFER(tab->buffer));
  GtkAdjustment *vertical = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(view));
  GtkAdjustment *horizontal = gtk_scrollable_get_hadjustment(GTK_SCROLLABLE(view));
  double sideways = horizontal ? gtk_adjustment_get_value(horizontal) : 0;
  GdkRectangle visible, cursor;
  GtkTextIter iter;
  gboolean seen_sideways;

  gtk_text_buffer_get_iter_at_mark(GTK_TEXT_BUFFER(tab->buffer), &iter, insert);
  gtk_text_view_get_visible_rect(view, &visible);
  gtk_text_view_get_iter_location(view, &iter, &cursor);
  seen_sideways = cursor.x >= visible.x && cursor.x + MAX(cursor.width, 1) <= visible.x + visible.width;
  if (vertical && (cursor.y < visible.y || cursor.y + cursor.height > visible.y + visible.height))
    gtk_adjustment_set_value(vertical, cursor.y + cursor.height / 2.0 - gtk_adjustment_get_page_size(vertical) / 2);
  /* Far lines have estimated heights until laid out: this makes it exact. It also moved a cursor that
   * was in view sideways by a few pixels (cutting off the first letters): that is undone. */
  gtk_text_view_scroll_mark_onscreen(view, insert);
  if (horizontal && seen_sideways)
    gtk_adjustment_set_value(horizontal, sideways);
}

void editor_view_apply_settings(Tab *tab)
{
  App *t = tab->app;
  EditorView *v = tab->view;
  GtkSourceView *view = tab->source;
  GtkSourceLanguage *language = gtk_source_buffer_get_language(tab->buffer);

  /* The settings, then the file's own, the project's config files, the project's choice. */
  v->tabs = !t->settings.tabs_to_spaces;
  v->width = t->settings.tab_size;
  v->indent_from = "Settings";
  if (v->own_indent) {
    v->tabs = v->own_tabs;
    v->width = v->own_width;
    v->indent_from = "the file's own indentation";
  }
  if (v->config.tabs >= 0)
    v->tabs = v->config.tabs;
  if (v->config.width > 0)
    v->width = v->config.width;
  if (v->config_from)
    v->indent_from = v->config_from;
  if (t->project.indent.width > 0) {
    v->tabs = t->project.indent.tabs;
    v->width = t->project.indent.width;
    v->indent_from = "this project's choice";
  }
  if (language && strcmp(gtk_source_language_get_id(language), "makefile") == 0)
    v->tabs = TRUE; /* make's recipes start with a tab */

  gtk_source_view_set_tab_width(view, MAX(v->width, 1));
  gtk_source_view_set_indent_width(view, -1); /* the tab width */
  gtk_source_view_set_insert_spaces_instead_of_tabs(view, !v->tabs);
  gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view), t->settings.word_wrap ? GTK_WRAP_WORD_CHAR : GTK_WRAP_NONE);
  gtk_source_view_set_show_right_margin(view, t->settings.ruler > 0);
  if (t->settings.ruler > 0)
    gtk_source_view_set_right_margin_position(view, t->settings.ruler);
  gtk_source_buffer_set_highlight_syntax(tab->buffer, !t->plain);
  gtk_source_buffer_set_style_scheme(tab->buffer, editor_style_scheme(t)); /* a new one after a rescan */
  occurrences_recolor(tab);
  gtk_widget_queue_draw(
      GTK_WIDGET(gtk_source_view_get_gutter(view, GTK_TEXT_WINDOW_LEFT))); /* the line numbers, arrows */
  gtk_widget_set_visible(v->map, t->settings.minimap);
  if (v->overview) /* follows the minimap, or the whole file without it */
    gtk_widget_queue_draw(v->overview);
  if (lsp_has(tab))
    semantic_update(tab);                  /* code coloring may have been turned off or on */
  gtk_widget_queue_draw(GTK_WIDGET(view)); /* the indent guides, turned off or on */
  if (tab->compare.old_side)               /* a comparison's right side is in no tab list */
    editor_view_apply_settings(tab->compare.old_side);
  sticky_apply_settings(tab);
  status_show_indent(t);
}

/* The problems strip ------------------------------------------------------------------------- */

#define MARK_HEIGHT 3.0

/* Whether the strip follows the minimap (shown and laid out); else it is the whole file. */
static gboolean strip_follows_map(EditorView *v)
{
  return v->map && gtk_widget_get_visible(v->map) && gtk_widget_get_mapped(v->map);
}

/* The top of line's mark on a strip height pixels high: beside the line in the minimap (a line
 * above or below the part it shows at its top or bottom edge); without the minimap, where the line
 * is in the whole file. The strip and the minimap are side by side, with the same top and height. */
static double strip_y(Tab *tab, int line, int height)
{
  GtkTextBuffer *buffer = GTK_TEXT_BUFFER(tab->buffer);

  if (strip_follows_map(tab->view)) {
    GtkTextView *map = GTK_TEXT_VIEW(tab->view->map);
    GtkTextIter iter;
    int y, h, top;
    gtk_text_buffer_get_iter_at_line(buffer, &iter, line);
    gtk_text_view_get_line_yrange(map, &iter, &y, &h);
    gtk_text_view_buffer_to_window_coords(map, GTK_TEXT_WINDOW_WIDGET, 0, y, NULL, &top);
    return CLAMP(top + (h - MARK_HEIGHT) / 2, 0, height - MARK_HEIGHT);
  }
  return MIN(line / (double)MAX(gtk_text_buffer_get_line_count(buffer), 1) * height, height - MARK_HEIGHT);
}

/* The line at y on the strip: the inverse of strip_y. */
static int strip_line(Tab *tab, double y, int height)
{
  GtkTextBuffer *buffer = GTK_TEXT_BUFFER(tab->buffer);

  if (strip_follows_map(tab->view)) {
    GtkTextView *map = GTK_TEXT_VIEW(tab->view->map);
    GtkTextIter iter;
    int buffer_y;
    gtk_text_view_window_to_buffer_coords(map, GTK_TEXT_WINDOW_WIDGET, 0, (int)y, NULL, &buffer_y);
    gtk_text_view_get_line_at_y(map, &iter, buffer_y, NULL);
    return gtk_text_iter_get_line(&iter);
  }
  return (int)(y / MAX(height, 1) * gtk_text_buffer_get_line_count(buffer));
}

#define GIT_WIDTH 3 /* the strip's left part: git's added, modified and deleted lines */

/* A mark per line with problems, beside that line in the minimap (see strip_y); errors over
 * warnings over the selected name's occurrences. Left of them, git's changes in their gutter colors,
 * a run of changed lines as one bar. */
static void draw_overview(GtkDrawingArea *area G_GNUC_UNUSED, cairo_t *cr, int width, int height, gpointer data)
{
  Tab *tab = data;
  int total = diff_gutter_line_total(tab);

  for (int line = 0; line < total; line++) {
    PalColor color, next;
    int last = line;
    if (!diff_gutter_mark(tab, line, &color))
      continue;
    while (last + 1 < total && diff_gutter_mark(tab, last + 1, &next) && next == color)
      last++;
    double top = strip_y(tab, line, height), bottom = strip_y(tab, last, height) + MARK_HEIGHT;
    gdk_cairo_set_source_rgba(cr, palette_rgba(color));
    cairo_rectangle(cr, 0, top, GIT_WIDTH, bottom - top);
    cairo_fill(cr);
    line = last;
  }
  GArray *lines[3] = { g_array_new(FALSE, FALSE, sizeof(int)), g_array_new(FALSE, FALSE, sizeof(int)),
                       g_array_new(FALSE, FALSE, sizeof(int)) };
  static const PalColor inks[3] = { PAL_FG_DIM, PAL_WARNING, PAL_ERROR };

  occurrences_lines(tab, lines[0]);
  diagnostics_lines(tab, lines[2], lines[1]);
  for (int k = 0; k < 3; k++) {
    gdk_cairo_set_source_rgba(cr, palette_rgba(inks[k]));
    for (guint i = 0; i < lines[k]->len; i++)
      cairo_rectangle(cr, GIT_WIDTH + 1, strip_y(tab, g_array_index(lines[k], int, i), height), width - GIT_WIDTH - 1,
                      MARK_HEIGHT);
    cairo_fill(cr);
    g_array_free(lines[k], TRUE);
  }
}

/* A click on the strip goes to that line (the one beside it in the minimap). */
static void on_overview_click(GtkGestureClick *click, int n G_GNUC_UNUSED, double x G_GNUC_UNUSED, double y, Tab *tab)
{
  GtkWidget *strip = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(click));
  GtkTextBuffer *buffer = GTK_TEXT_BUFFER(tab->buffer);
  int line = strip_line(tab, y, gtk_widget_get_height(strip));
  GtkTextIter iter;

  gtk_text_buffer_get_iter_at_line(buffer, &iter, line);
  gtk_text_buffer_place_cursor(buffer, &iter);
  editor_view_reveal_cursor(tab);
  gtk_widget_grab_focus(GTK_WIDGET(tab->source));
}

/* The strip is drawn again when the minimap scrolls (notify::vadjustment: when it gets a new one). */
static void watch_map_scroll(GObject *map, GParamSpec *spec G_GNUC_UNUSED, GtkWidget *strip)
{
  GtkAdjustment *adjustment = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(map));

  if (adjustment)
    g_signal_connect_object(adjustment, "value-changed", G_CALLBACK(gtk_widget_queue_draw), strip, G_CONNECT_SWAPPED);
}

void editor_view_show_problems(Tab *tab)
{
  if (!tab || !tab->view)
    return;
  if (tab->view->overview)
    gtk_widget_queue_draw(tab->view->overview);
  if (tab->view->map)
    gtk_widget_queue_draw(tab->view->map);
  if (tab->source)
    gtk_widget_queue_draw(GTK_WIDGET(tab->source));
}

/* The minimap -------------------------------------------------------------------------------- */

#define TCODE_TYPE_MAP (tcode_map_get_type())
G_DECLARE_FINAL_TYPE(TcodeMap, tcode_map, TCODE, MAP, GtkSourceMap)

struct _TcodeMap {
  GtkSourceMap parent;
  Tab *tab;
};

G_DEFINE_FINAL_TYPE(TcodeMap, tcode_map, GTK_SOURCE_TYPE_MAP)

static void tcode_map_snapshot_layer(GtkTextView *view, GtkTextViewLayer layer, GtkSnapshot *snapshot)
{
  TcodeMap *self = TCODE_MAP(view);

  GTK_TEXT_VIEW_CLASS(tcode_map_parent_class)->snapshot_layer(view, layer, snapshot);

  if (layer == GTK_TEXT_VIEW_LAYER_ABOVE_TEXT && self->tab && self->tab->view && !self->tab->view->loading) {
    Tab *tab = self->tab;
    GtkTextBuffer *buffer = GTK_TEXT_BUFFER(tab->buffer);
    GArray *lines[2] = { g_array_new(FALSE, FALSE, sizeof(int)), g_array_new(FALSE, FALSE, sizeof(int)) };
    diagnostics_lines(tab, lines[1], lines[0]); /* lines[0] = warnings, lines[1] = errors */

    int total_lines = gtk_text_buffer_get_line_count(buffer);
    gboolean *seen = g_new0(gboolean, total_lines + 1);
    float map_w = (float)gtk_widget_get_width(GTK_WIDGET(view));

    GdkRGBA edge_rgba[2] = { *palette_rgba(PAL_WARNING), *palette_rgba(PAL_ERROR) };
    GdkRGBA bg_rgba[2] = { edge_rgba[0], edge_rgba[1] };
    bg_rgba[0].alpha = 0.35f;
    bg_rgba[1].alpha = 0.40f;

    /* Draw errors first (k=1) then warnings (k=0), so error colors take precedence on the same line */
    for (int k = 1; k >= 0; k--) {
      for (guint i = 0; i < lines[k]->len; i++) {
        int line_idx = g_array_index(lines[k], int, i);
        if (line_idx < 0 || line_idx >= total_lines || seen[line_idx])
          continue;
        seen[line_idx] = TRUE;

        GtkTextIter iter;
        int y, h;
        gtk_text_buffer_get_iter_at_line(buffer, &iter, line_idx);
        gtk_text_view_get_line_yrange(view, &iter, &y, &h);
        int draw_h = MAX(h, 2);

        /* Highlight across the full minimap line */
        graphene_rect_t row_rect = GRAPHENE_RECT_INIT(0.0f, (float)y, map_w, (float)draw_h);
        gtk_snapshot_append_color(snapshot, &bg_rgba[k], &row_rect);

        /* Solid indicator on the right edge */
        graphene_rect_t edge_rect = GRAPHENE_RECT_INIT(map_w - 3.0f, (float)y, 3.0f, (float)draw_h);
        gtk_snapshot_append_color(snapshot, &edge_rgba[k], &edge_rect);
      }
    }
    g_array_free(lines[0], TRUE);
    g_array_free(lines[1], TRUE);
    g_free(seen);
  }
}

static void tcode_map_class_init(TcodeMapClass *klass)
{
  GTK_TEXT_VIEW_CLASS(klass)->snapshot_layer = tcode_map_snapshot_layer;
}

static void tcode_map_init(TcodeMap *self G_GNUC_UNUSED)
{
}

static GtkWidget *tcode_map_new(Tab *tab)
{
  TcodeMap *map = g_object_new(TCODE_TYPE_MAP, NULL);
  map->tab = tab;
  return GTK_WIDGET(map);
}

/* The page ----------------------------------------------------------------------------------- */

GtkWidget *editor_page(void)
{
  GtkWidget *page = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

  gtk_widget_add_css_class(page, "tcode-editor");
  return page;
}

GtkSourceStyleScheme *editor_style_scheme(App *t)
{
  static gboolean styled;

  if (!styled) {
    init_look(t);
    styled = TRUE;
  }
  return palette_scheme(t);
}

/* The editor's right-click menu: after GTK's own (cut, copy, paste), commands of the command
 * palette for the code under the cursor. */
static const char *const menu_commands[][2] = {
  { "Quick Fix…", "Quick Fix…" },
  { NULL, NULL },
  { "Go to Definition", "Go to Definition" },
  { "Find All References", "Find All References" },
  { "Go to Symbol…", "Go to Symbol in File…" },
  { "Switch Source / Header", "Switch Source / Header" },
  { "Rename Symbol…", "Rename Symbol" },
  { "Format Document", "Format Document" },
  { NULL, NULL },
  { "Toggle Line Comment", "Toggle Line Comment" },
  { NULL, NULL },
  { "Toggle Breakpoint", "Debug: Toggle Breakpoint" },
  { "Add to Watch", "Debug: Add to Watch" },
  { NULL, NULL },
  { "Compare with HEAD", "Git: Compare with HEAD" },
  { "File History…", "Git: File History…" },
};

static void on_menu_command(GSimpleAction *action G_GNUC_UNUSED, GVariant *name, Tab *tab)
{
  select_tab(tab); /* the commands work on the current tab */
  run_command(tab->app, g_variant_get_string(name, NULL));
}

/* A right click outside the selection puts the cursor there first, so that the menu's commands
 * are about what was clicked (GtkTextView leaves it where it was). */
static void on_menu_press(GtkGestureClick *click, int n G_GNUC_UNUSED, double x, double y, Tab *tab)
{
  GtkTextView *view = GTK_TEXT_VIEW(tab->source);
  GtkTextBuffer *buffer = GTK_TEXT_BUFFER(tab->buffer);
  GtkTextIter at, start, end;
  int bx, by;

  gtk_text_view_window_to_buffer_coords(view, GTK_TEXT_WINDOW_WIDGET, (int)x, (int)y, &bx, &by);
  gtk_text_view_get_iter_at_location(view, &at, bx, by);
  if (!gtk_text_buffer_get_selection_bounds(buffer, &start, &end) || !gtk_text_iter_in_range(&at, &start, &end))
    gtk_text_buffer_place_cursor(buffer, &at);
  /* The click is ours and the menu is opened by hand: letting the click on to GTK's own handler
   * did not open it. */
  gtk_gesture_set_state(GTK_GESTURE(click), GTK_EVENT_SEQUENCE_CLAIMED);
  gtk_widget_activate_action(GTK_WIDGET(view), "menu.popup", NULL);
}

static void attach_menu(Tab *tab)
{
  GMenu *menu = g_menu_new(), *section = g_menu_new();
  GSimpleActionGroup *group = g_simple_action_group_new();
  GSimpleAction *action = g_simple_action_new("run", G_VARIANT_TYPE_STRING);
  GtkGesture *press = gtk_gesture_click_new();

  g_signal_connect(action, "activate", G_CALLBACK(on_menu_command), tab);
  g_action_map_add_action(G_ACTION_MAP(group), G_ACTION(action));
  gtk_widget_insert_action_group(GTK_WIDGET(tab->source), "editor", G_ACTION_GROUP(group));
  for (size_t i = 0; i < G_N_ELEMENTS(menu_commands); i++) {
    if (!menu_commands[i][0]) {
      g_menu_append_section(menu, NULL, G_MENU_MODEL(section));
      g_object_unref(section);
      section = g_menu_new();
      continue;
    }
    GMenuItem *item = g_menu_item_new(menu_commands[i][0], NULL);
    g_menu_item_set_action_and_target(item, "editor.run", "s", menu_commands[i][1]);
    g_menu_append_item(section, item);
    g_object_unref(item);
  }
  g_menu_append_section(menu, NULL, G_MENU_MODEL(section));
  gtk_text_view_set_extra_menu(GTK_TEXT_VIEW(tab->source), G_MENU_MODEL(menu));
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(press), GDK_BUTTON_SECONDARY);
  gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(press), GTK_PHASE_CAPTURE);
  g_signal_connect(press, "pressed", G_CALLBACK(on_menu_press), tab);
  gtk_widget_add_controller(GTK_WIDGET(tab->source), GTK_EVENT_CONTROLLER(press));
  g_object_unref(section);
  g_object_unref(menu);
  g_object_unref(action);
  g_object_unref(group);
}

void editor_view_init(Tab *tab, gboolean readonly)
{
  App *t = tab->app;
  EditorView *v = g_new0(EditorView, 1);
  GtkWidget *page = tab->page;
  GtkWidget *scrolled = gtk_scrolled_window_new();
  GtkSourceBuffer *buffer;
  GtkSourceView *view;

  v->config = (IndentStyle){ -1, 0 }; /* until loaded */

  buffer = gtk_source_buffer_new(NULL);
  gtk_source_buffer_set_style_scheme(buffer, editor_style_scheme(t));
  gtk_source_buffer_set_highlight_matching_brackets(buffer, TRUE);
  /* The buffer holds the file exactly, its last line break too (shown as an empty last line):
   * what is saved, what clangd gets and what the diff gutter compares are the same text. */
  gtk_source_buffer_set_implicit_trailing_newline(buffer, FALSE);
  view = guides_view_new(tab, buffer); /* our reference to the buffer stays the tab's */

  tab->view = v;
  tab->source = view;
  tab->buffer = buffer;
  v->tab = tab;
  v->readonly = readonly;
  v->file = gtk_source_file_new();
  v->cancel = g_cancellable_new();
  v->next_waiters = g_array_new(FALSE, FALSE, sizeof(SaveWaiter));
  editing_attach(tab);
  occurrences_attach(tab);
  diff_gutter_attach(tab);
  folding_attach(tab);
  breakpoints_attach(tab);
  hover_attach(tab);
  debug_hover_attach(tab);
  completion_attach(tab);
  navigate_attach(tab);
  attach_menu(tab);

  gtk_widget_add_css_class(GTK_WIDGET(view), "tcode-source");
  gtk_text_view_set_monospace(GTK_TEXT_VIEW(view), TRUE);
  gtk_text_view_set_editable(GTK_TEXT_VIEW(view), !readonly);
  gtk_text_view_set_left_margin(GTK_TEXT_VIEW(view), 4);
  gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(view), 200); /* the last line can come up from the bottom */
  gtk_source_view_set_show_line_numbers(view, FALSE);        /* folding.c's: they leave folded lines out */
  gtk_source_view_set_highlight_current_line(view, TRUE);
  gtk_source_view_set_auto_indent(view, TRUE);
  gtk_source_view_set_indent_on_tab(view, TRUE);
  /* Tab / Shift+Tab move between a completed function's parameters: GtkSourceView only does
   * that for pushed snippets with this on (it has no snippet files of its own to expand). */
  gtk_source_view_set_enable_snippets(view, TRUE);
  gtk_source_view_set_smart_backspace(view, TRUE);
  gtk_source_view_set_smart_home_end(view, GTK_SOURCE_SMART_HOME_END_BEFORE);

  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), GTK_WIDGET(view));
  v->overlay = gtk_overlay_new(); /* the find bar goes over the text */
  gtk_overlay_set_child(GTK_OVERLAY(v->overlay), scrolled);
  gtk_widget_set_vexpand(v->overlay, TRUE);
  v->column = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_hexpand(v->column, TRUE);
  gtk_box_append(GTK_BOX(v->column), v->overlay);
  gtk_box_append(GTK_BOX(page), v->column);

  v->map = tcode_map_new(tab);
  gtk_source_map_set_view(GTK_SOURCE_MAP(v->map), view);
  gtk_widget_add_css_class(v->map, "tcode-minimap");
  gtk_box_append(GTK_BOX(page), v->map);

  GtkGesture *click = gtk_gesture_click_new();
  v->overview = gtk_drawing_area_new();
  gtk_widget_set_size_request(v->overview, OVERVIEW_WIDTH, -1);
  gtk_widget_add_css_class(v->overview, "tcode-overview");
  gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(v->overview), draw_overview, tab, NULL);
  g_signal_connect(click, "pressed", G_CALLBACK(on_overview_click), tab);
  gtk_widget_add_controller(v->overview, GTK_EVENT_CONTROLLER(click));
  gtk_box_append(GTK_BOX(page), v->overview);
  /* The marks move with the minimap: it scrolls with the view, and has its own adjustment. */
  g_signal_connect_object(gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scrolled)), "value-changed",
                          G_CALLBACK(gtk_widget_queue_draw), v->overview, G_CONNECT_SWAPPED);
  g_signal_connect_object(v->map, "notify::vadjustment", G_CALLBACK(watch_map_scroll), v->overview, 0);
  watch_map_scroll(G_OBJECT(v->map), NULL, v->overview);
  g_signal_connect_object(buffer, "changed", G_CALLBACK(gtk_widget_queue_draw), v->overview, G_CONNECT_SWAPPED);
  sticky_attach(tab);    /* over the text's top */
  signature_attach(tab); /* in the overlay */
  editor_view_apply_settings(tab);
}

void editor_view_free(EditorView *v)
{
  if (!v)
    return;
  g_cancellable_cancel(v->cancel); /* loads and saves in flight drop their answer */
  signature_free(v->signature);
  semantic_free(v->semantic); /* cancel debounce before closing its LSP document */
  v->semantic = NULL;
  lsp_close(v->tab);
  diagnostics_clear(v->tab);
  occurrences_free(v->occurrences);
  findbar_free(v->find);
  diff_gutter_free(v->diff);
  folding_free(v->folding);
  breakpoints_free(v->breakpoints);
  sticky_free(v->sticky);
  g_free(v->closers);
  for (guint i = 0; i < v->next_waiters->len; i++) {
    SaveWaiter *w = &g_array_index(v->next_waiters, SaveWaiter, i);
    w->func(w->data, FALSE);
  }
  g_array_free(v->next_waiters, TRUE);
  if (v->monitor) {
    g_file_monitor_cancel(v->monitor);
    g_object_unref(v->monitor);
  }
  if (v->autosave_id)
    g_source_remove(v->autosave_id);
  if (v->reload_id)
    g_source_remove(v->reload_id);
  if (v->scroll_id)
    g_source_remove(v->scroll_id);
  if (v->recovery_id)
    g_source_remove(v->recovery_id);
  g_signal_handlers_disconnect_by_data(v->tab->buffer, v->tab);
  g_object_unref(v->tab->buffer); /* the page went first: the tab still holds it */
  g_object_unref(v->cancel);
  g_object_unref(v->file);
  g_free(v->etag);
  g_free(v);
}
