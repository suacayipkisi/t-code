/*
 * Editing commands and typing help (see editing.h). Each command is one undo step.
 */
#include "editor/editing.h"

#include "editor/editorview.h"
#include "editor/multicursor.h"

#include <string.h>

static gboolean editable(Tab *tab)
{
  return gtk_text_view_get_editable(GTK_TEXT_VIEW(tab->source));
}

/* Lines ------------------------------------------------------------------------------------- */

/* The lines the selection touches (the cursor's alone without one). A selection that ends at a
 * line's start leaves that line out. */
static void selected_lines(GtkTextBuffer *buffer, int *first, int *last)
{
  GtkTextIter start, end;

  gtk_text_buffer_get_selection_bounds(buffer, &start, &end);
  *first = gtk_text_iter_get_line(&start);
  *last = gtk_text_iter_get_line(&end);
  if (*last > *first && gtk_text_iter_starts_line(&end))
    (*last)--;
}

/* From the start of line first to the end of line last (before its line break). */
static void line_bounds(GtkTextBuffer *buffer, int first, int last, GtkTextIter *start, GtkTextIter *end)
{
  gtk_text_buffer_get_iter_at_line(buffer, start, first);
  gtk_text_buffer_get_iter_at_line(buffer, end, last);
  if (!gtk_text_iter_ends_line(end))
    gtk_text_iter_forward_to_line_end(end);
}

/* A line's text, without its line break. */
static char *line_text(GtkTextBuffer *buffer, int line)
{
  GtkTextIter start, end;

  line_bounds(buffer, line, line, &start, &end);
  return gtk_text_buffer_get_text(buffer, &start, &end, TRUE);
}

/* Bytes of leading blanks; the whole length for a blank line. */
static int lead(const char *text)
{
  return (int)strspn(text, " \t");
}

/* Comments ----------------------------------------------------------------------------------- */

/* Line comments: in when every non-blank line has one, else out at the smallest indentation (so
 * they line up). Blank lines stay as they are. */
static void toggle_line_comments(GtkTextBuffer *buffer, int first, int last, const char *token)
{
  size_t n = strlen(token);
  gboolean all = TRUE, any = FALSE;
  int indent = G_MAXINT;

  for (int line = first; line <= last; line++) {
    char *text = line_text(buffer, line);
    int k = lead(text);
    if (text[k]) {
      any = TRUE;
      indent = MIN(indent, k);
      all = all && strncmp(text + k, token, n) == 0;
    }
    g_free(text);
  }
  if (!any)
    return;
  for (int line = first; line <= last; line++) {
    char *text = line_text(buffer, line);
    int k = lead(text);
    GtkTextIter start, end;
    if (text[k] && all) { /* the token, and the space after it */
      gtk_text_buffer_get_iter_at_line_offset(buffer, &start, line, k);
      gtk_text_buffer_get_iter_at_line_offset(buffer, &end, line, k + (int)n + (text[k + n] == ' '));
      gtk_text_buffer_delete(buffer, &start, &end);
    } else if (text[k]) {
      char *with_space = g_strconcat(token, " ", NULL);
      gtk_text_buffer_get_iter_at_line_offset(buffer, &start, line, indent);
      gtk_text_buffer_insert(buffer, &start, with_space, -1);
      g_free(with_space);
    }
    g_free(text);
  }
}

static gboolean is_blank(gunichar c)
{
  return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/* GtkTextCharPredicate: not a blank. */
static gboolean not_blank(gunichar c, gpointer data G_GNUC_UNUSED)
{
  return !is_blank(c);
}

/* A block comment around the lines' text, or off it when it is one already. */
static void toggle_block_comment(GtkTextBuffer *buffer, int first, int last, const char *open, const char *close)
{
  GtkTextIter start, end;
  char *text, *replacement;
  size_t n, o = strlen(open), c = strlen(close);

  line_bounds(buffer, first, last, &start, &end);
  /* The text without the blanks around it. */
  if (is_blank(gtk_text_iter_get_char(&start)))
    gtk_text_iter_forward_find_char(&start, not_blank, NULL, &end);
  if (gtk_text_iter_compare(&start, &end) >= 0)
    return; /* blank lines */
  gtk_text_iter_backward_find_char(&end, not_blank, NULL, &start);
  gtk_text_iter_forward_char(&end);
  text = gtk_text_buffer_get_text(buffer, &start, &end, TRUE);
  n = strlen(text);
  if (n >= o + c && g_str_has_prefix(text, open) && g_str_has_suffix(text, close)) {
    size_t from = o + (text[o] == ' '), to = n - c - (n - c > from && text[n - c - 1] == ' ');
    replacement = g_strndup(text + from, to > from ? to - from : 0);
  } else {
    replacement = g_strconcat(open, " ", text, " ", close, NULL);
  }
  gtk_text_buffer_delete(buffer, &start, &end);
  gtk_text_buffer_insert(buffer, &start, replacement, -1);
  g_free(replacement);
  g_free(text);
}

void editor_toggle_comment(Tab *tab)
{
  GtkTextBuffer *buffer = GTK_TEXT_BUFFER(tab->buffer);
  GtkSourceLanguage *language = gtk_source_buffer_get_language(tab->buffer);
  const char *line_token, *open, *close;
  int first, last;

  if (!editable(tab) || !language)
    return;
  line_token = gtk_source_language_get_metadata(language, "line-comment-start");
  open = gtk_source_language_get_metadata(language, "block-comment-start");
  close = gtk_source_language_get_metadata(language, "block-comment-end");
  selected_lines(buffer, &first, &last);
  gtk_text_buffer_begin_user_action(buffer);
  if (line_token)
    toggle_line_comments(buffer, first, last, line_token);
  else if (open && close)
    toggle_block_comment(buffer, first, last, open, close);
  gtk_text_buffer_end_user_action(buffer);
}

/* Lines -------------------------------------------------------------------------------------- */

void editor_duplicate_lines(Tab *tab)
{
  GtkTextBuffer *buffer = GTK_TEXT_BUFFER(tab->buffer);
  GtkTextIter start, end, cursor, bound;
  int first, last, cursor_line, cursor_offset, bound_line, bound_offset;
  char *text, *copy;

  if (!editable(tab))
    return;
  selected_lines(buffer, &first, &last);
  gtk_text_buffer_get_iter_at_mark(buffer, &cursor, gtk_text_buffer_get_insert(buffer));
  gtk_text_buffer_get_iter_at_mark(buffer, &bound, gtk_text_buffer_get_selection_bound(buffer));
  cursor_line = gtk_text_iter_get_line(&cursor);
  cursor_offset = gtk_text_iter_get_line_offset(&cursor);
  bound_line = gtk_text_iter_get_line(&bound);
  bound_offset = gtk_text_iter_get_line_offset(&bound);

  line_bounds(buffer, first, last, &start, &end);
  text = gtk_text_buffer_get_text(buffer, &start, &end, TRUE);
  copy = g_strconcat("\n", text, NULL);
  gtk_text_buffer_begin_user_action(buffer);
  gtk_text_buffer_insert(buffer, &end, copy, -1);
  gtk_text_buffer_end_user_action(buffer);

  /* The selection moves to the copy. */
  int lines = last - first + 1;
  gtk_text_buffer_get_iter_at_line_offset(buffer, &cursor, cursor_line + lines, cursor_offset);
  gtk_text_buffer_get_iter_at_line_offset(buffer, &bound, bound_line + lines, bound_offset);
  gtk_text_buffer_select_range(buffer, &cursor, &bound);
  editor_view_reveal_cursor(tab);
  g_free(copy);
  g_free(text);
}

void editor_move_lines(Tab *tab, gboolean down)
{
  if (editable(tab))
    g_signal_emit_by_name(tab->source, "move-lines", down);
}

void editor_delete_lines(Tab *tab)
{
  GtkTextBuffer *buffer = GTK_TEXT_BUFFER(tab->buffer);
  GtkTextIter start, end;
  int first, last;

  if (!editable(tab))
    return;
  selected_lines(buffer, &first, &last);
  gtk_text_buffer_get_iter_at_line(buffer, &start, first);
  if (last + 1 < gtk_text_buffer_get_line_count(buffer)) {
    gtk_text_buffer_get_iter_at_line(buffer, &end, last + 1);
  } else { /* the last line: the line break before it goes */
    gtk_text_buffer_get_end_iter(buffer, &end);
    if (first > 0)
      gtk_text_iter_backward_char(&start);
  }
  gtk_text_buffer_begin_user_action(buffer);
  gtk_text_buffer_delete(buffer, &start, &end);
  gtk_text_buffer_end_user_action(buffer);
}

void editor_redo(Tab *tab)
{
  if (editable(tab) && gtk_text_buffer_get_can_redo(GTK_TEXT_BUFFER(tab->buffer)))
    gtk_text_buffer_redo(GTK_TEXT_BUFFER(tab->buffer));
}

/* Typing help -------------------------------------------------------------------------------- */

/* The closer of an opening bracket or quote, or 0. */
static gunichar closer_of(gunichar c)
{
  switch (c) {
    case '(': return ')';
    case '[': return ']';
    case '{': return '}';
    case '"': return '"';
    case '\'': return '\'';
    default: return 0;
  }
}

static void cursor_iter(GtkTextBuffer *buffer, GtkTextIter *iter)
{
  gtk_text_buffer_get_iter_at_mark(buffer, iter, gtk_text_buffer_get_insert(buffer));
}

/* Whether the closers typed for the user are still right after the cursor, untouched. */
static gboolean closers_ahead(Tab *tab)
{
  EditorView *v = tab->view;
  GtkTextBuffer *buffer = GTK_TEXT_BUFFER(tab->buffer);
  GtkTextIter cursor, end;
  char *text;
  gboolean ahead;

  if (!v->closers || !*v->closers)
    return FALSE;
  cursor_iter(buffer, &cursor);
  gtk_text_buffer_get_iter_at_mark(buffer, &end, v->closers_end);
  if (gtk_text_iter_compare(&cursor, &end) > 0)
    return FALSE;
  text = gtk_text_buffer_get_text(buffer, &cursor, &end, TRUE);
  ahead = strcmp(text, v->closers) == 0;
  g_free(text);
  return ahead;
}

/* Drops the first of the closers (stepped over, or deleted with its opener). */
static void drop_closer(EditorView *v)
{
  memmove(v->closers, g_utf8_next_char(v->closers), strlen(g_utf8_next_char(v->closers)) + 1);
}

/* Typing a closer that was typed for the user steps over it. */
static gboolean step_over(Tab *tab, gunichar c)
{
  GtkTextBuffer *buffer = GTK_TEXT_BUFFER(tab->buffer);
  GtkTextIter cursor;

  if (!closers_ahead(tab) || g_utf8_get_char(tab->view->closers) != c)
    return FALSE;
  cursor_iter(buffer, &cursor);
  gtk_text_iter_forward_char(&cursor);
  gtk_text_buffer_place_cursor(buffer, &cursor);
  drop_closer(tab->view);
  return TRUE;
}

/* Backspace between a pair typed for the user, "(|)", deletes both. */
static gboolean delete_pair(Tab *tab)
{
  GtkTextBuffer *buffer = GTK_TEXT_BUFFER(tab->buffer);
  GtkTextIter before, after;

  if (gtk_text_buffer_get_has_selection(buffer) || !closers_ahead(tab))
    return FALSE;
  cursor_iter(buffer, &before);
  after = before;
  if (!gtk_text_iter_backward_char(&before) ||
      closer_of(gtk_text_iter_get_char(&before)) != g_utf8_get_char(tab->view->closers))
    return FALSE;
  gtk_text_iter_forward_char(&after);
  gtk_text_buffer_begin_user_action(buffer);
  gtk_text_buffer_delete(buffer, &before, &after);
  gtk_text_buffer_end_user_action(buffer);
  drop_closer(tab->view);
  return TRUE;
}

/* Whether a closer may follow here: before blanks, the line's end or other closers (so "(" before a
 * word types just "("). */
static gboolean closer_fits(const GtkTextIter *cursor)
{
  gunichar next = gtk_text_iter_get_char(cursor);

  return next == 0 || next == '\n' || next == '\r' || next == ' ' || next == '\t' || strchr(")]};,", (int)next);
}

/* Whether the cursor is in a comment; at the end of a "//" line too, which the comment's context
 * leaves out. */
static gboolean in_comment(Tab *tab, const GtkTextIter *cursor)
{
  GtkTextIter before = *cursor;

  if (gtk_source_buffer_iter_has_context_class(tab->buffer, cursor, "comment"))
    return TRUE;
  return gtk_text_iter_ends_line(cursor) && gtk_text_iter_backward_char(&before) &&
         gtk_source_buffer_iter_has_context_class(tab->buffer, &before, "comment");
}

/* Typing an opening bracket or quote: its closer too, the cursor between them; with a selection,
 * the pair goes around it. Not in comments, nor quotes in strings or after a letter ("don't"). */
static gboolean open_pair(Tab *tab, gunichar open)
{
  EditorView *v = tab->view;
  GtkTextBuffer *buffer = GTK_TEXT_BUFFER(tab->buffer);
  gunichar close = closer_of(open);
  gboolean quote = open == close;
  GtkTextIter cursor, before, start, end;
  char pair[3] = { (char)open, (char)close, 0 }; /* brackets and quotes are ASCII */

  cursor_iter(buffer, &cursor);
  if (in_comment(tab, &cursor) || (quote && gtk_source_buffer_iter_has_context_class(tab->buffer, &cursor, "string")))
    return FALSE;
  if (gtk_text_buffer_get_selection_bounds(buffer, &start, &end)) {
    char *inside = gtk_text_buffer_get_text(buffer, &start, &end, TRUE);
    char *wrapped = g_strdup_printf("%c%s%c", (char)open, inside, (char)close); /* all ASCII */
    int length = (int)g_utf8_strlen(inside, -1);
    gtk_text_buffer_begin_user_action(buffer);
    gtk_text_buffer_delete(buffer, &start, &end);
    gtk_text_buffer_insert(buffer, &start, wrapped, -1); /* start ends up after it */
    gtk_text_buffer_end_user_action(buffer);
    /* The selection stays on the text inside the pair. */
    gtk_text_iter_backward_char(&start);
    end = start;
    gtk_text_iter_backward_chars(&start, length);
    gtk_text_buffer_select_range(buffer, &end, &start);
    g_free(wrapped);
    g_free(inside);
    return TRUE;
  }
  before = cursor;
  if (!closer_fits(&cursor))
    return FALSE;
  if (quote && gtk_text_iter_backward_char(&before) &&
      (g_unichar_isalnum(gtk_text_iter_get_char(&before)) || gtk_text_iter_get_char(&before) == '\\' ||
       gtk_text_iter_get_char(&before) == '_'))
    return FALSE;

  gtk_text_buffer_begin_user_action(buffer);
  gtk_text_buffer_insert(buffer, &cursor, pair, -1);
  gtk_text_buffer_end_user_action(buffer);
  /* The closers ahead: this one, then the ones before it if they are still there. */
  if (!closers_ahead(tab)) {
    g_free(v->closers);
    v->closers = g_strdup("");
    if (!v->closers_end)
      v->closers_end = gtk_text_buffer_create_mark(buffer, NULL, &cursor, TRUE);
    else
      gtk_text_buffer_move_mark(buffer, v->closers_end, &cursor);
  }
  gtk_text_iter_backward_char(&cursor);
  gtk_text_buffer_place_cursor(buffer, &cursor);
  char *closers = g_strconcat(pair + 1, v->closers, NULL);
  g_free(v->closers);
  v->closers = closers;
  return TRUE;
}

/* The indentation of one level: a tab, or the tab width in spaces. */
static char *indent_unit(Tab *tab)
{
  if (!gtk_source_view_get_insert_spaces_instead_of_tabs(tab->source))
    return g_strdup("\t");
  return g_strnfill(gtk_source_view_get_tab_width(tab->source), ' ');
}

/* Enter after an opening bracket ("(", "[", "{", "<", blanks after it allowed): the new line one
 * level deeper; between a pair ("{|}") the closer goes to a line of its own below it. Not in
 * comments or strings. */
static gboolean enter_after_open(Tab *tab)
{
  GtkTextBuffer *buffer = GTK_TEXT_BUFFER(tab->buffer);
  GtkTextIter cursor, before, after;
  char *line, *indent, *unit, *text;
  gunichar open, close;
  gboolean pair;

  if (gtk_text_buffer_get_has_selection(buffer))
    return FALSE;
  cursor_iter(buffer, &cursor);
  before = cursor;
  do {
    if (gtk_text_iter_starts_line(&before) || !gtk_text_iter_backward_char(&before))
      return FALSE;
  } while (gtk_text_iter_get_char(&before) == ' ' || gtk_text_iter_get_char(&before) == '\t');
  open = gtk_text_iter_get_char(&before);
  close = open == '<' ? '>' : closer_of(open);
  if (!close || open == '"' || open == '\'' || in_comment(tab, &before) ||
      gtk_source_buffer_iter_has_context_class(tab->buffer, &before, "string"))
    return FALSE;
  after = cursor; /* the blanks around the cursor go: none trail the bracket or start the new line */
  while (gtk_text_iter_get_char(&after) == ' ' || gtk_text_iter_get_char(&after) == '\t')
    gtk_text_iter_forward_char(&after);
  pair = gtk_text_iter_get_char(&after) == close;
  line = line_text(buffer, gtk_text_iter_get_line(&cursor));
  indent = g_strndup(line, lead(line));
  unit = indent_unit(tab);
  text = pair ? g_strconcat("\n", indent, unit, "\n", indent, NULL) : g_strconcat("\n", indent, unit, NULL);
  gtk_text_iter_forward_char(&before);
  gtk_text_buffer_begin_user_action(buffer);
  gtk_text_buffer_delete(buffer, &before, &after);
  gtk_text_buffer_insert(buffer, &before, text, -1);
  gtk_text_buffer_end_user_action(buffer);
  if (pair)
    gtk_text_iter_backward_chars(&before, (int)g_utf8_strlen(indent, -1) + 1);
  gtk_text_buffer_place_cursor(buffer, &before);
  gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(tab->source), gtk_text_buffer_get_insert(buffer));
  g_clear_pointer(&tab->view->closers, g_free); /* the closer is on another line now */
  g_free(text);
  g_free(unit);
  g_free(indent);
  g_free(line);
  return TRUE;
}

/* "}" typed on a line of blanks only: one level of indentation less first. */
static void outdent_brace(Tab *tab)
{
  GtkTextBuffer *buffer = GTK_TEXT_BUFFER(tab->buffer);
  GtkTextIter cursor, start;
  char *before, *unit;

  cursor_iter(buffer, &cursor);
  start = cursor;
  gtk_text_iter_set_line_offset(&start, 0);
  before = gtk_text_buffer_get_text(buffer, &start, &cursor, TRUE);
  unit = indent_unit(tab);
  if (*before && lead(before) == (int)strlen(before)) {
    GtkTextIter from = cursor;
    int cut = g_str_has_suffix(before, unit) ? (int)strlen(unit) : 1;
    gtk_text_iter_backward_chars(&from, cut);
    gtk_text_buffer_delete(buffer, &from, &cursor);
  }
  g_free(unit);
  g_free(before);
}

/* Keys before the view sees them: the typing help. */
static gboolean on_typing_key(GtkEventControllerKey *keys G_GNUC_UNUSED, guint key, guint code G_GNUC_UNUSED,
                              GdkModifierType state, Tab *tab)
{
  GtkTextBuffer *buffer = GTK_TEXT_BUFFER(tab->buffer);
  gunichar c;

  if (!editable(tab) || (state & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK)) || multicursor_active(tab))
    return FALSE; /* with more cursors, multicursor.c types */
  if (key == GDK_KEY_Return || key == GDK_KEY_KP_Enter)
    return !(state & GDK_SHIFT_MASK) && enter_after_open(tab);
  if (key == GDK_KEY_BackSpace)
    return delete_pair(tab);
  c = gdk_keyval_to_unicode(key);
  if ((c == ')' || c == ']' || c == '}' || c == '"' || c == '\'') && step_over(tab, c))
    return TRUE;
  if (c == '}' && !gtk_text_buffer_get_has_selection(buffer)) {
    gtk_text_buffer_begin_user_action(buffer);
    outdent_brace(tab);
    gtk_text_buffer_insert_at_cursor(buffer, "}", -1);
    gtk_text_buffer_end_user_action(buffer);
    return TRUE;
  }
  if (closer_of(c))
    return open_pair(tab, c);
  return FALSE;
}

/* Words ------------------------------------------------------------------------------------ */

static gboolean is_word_char(gunichar c)
{
  return g_unichar_isalnum(c) || c == '_';
}

gboolean editor_word_at(const GtkTextIter *iter, GtkTextIter *start, GtkTextIter *end)
{
  *start = *iter;
  if (!is_word_char(gtk_text_iter_get_char(start))) { /* just after a word: that one */
    if (!gtk_text_iter_backward_char(start) || !is_word_char(gtk_text_iter_get_char(start)))
      return FALSE;
  }
  *end = *start;
  while (gtk_text_iter_backward_char(start) && is_word_char(gtk_text_iter_get_char(start)))
    ;
  if (!is_word_char(gtk_text_iter_get_char(start)))
    gtk_text_iter_forward_char(start);
  while (is_word_char(gtk_text_iter_get_char(end)) && gtk_text_iter_forward_char(end))
    ;
  return TRUE;
}

/* A double click (and the drag after it) selects names only: letters, digits and "_", never the
 * brackets or punctuation next to them (GtkSourceView's own words run up to the next blank). */
static gboolean on_extend_selection(GtkTextView *view G_GNUC_UNUSED, GtkTextExtendSelection granularity,
                                    const GtkTextIter *location, GtkTextIter *start, GtkTextIter *end,
                                    gpointer data G_GNUC_UNUSED)
{
  if (granularity != GTK_TEXT_EXTEND_SELECTION_WORD)
    return GDK_EVENT_PROPAGATE; /* a triple click's line */
  if (!editor_word_at(location, start, end))
    *start = *end = *location;
  return GDK_EVENT_STOP;
}

void editing_attach(Tab *tab)
{
  GtkEventController *keys = gtk_event_controller_key_new();

  g_signal_connect(tab->source, "extend-selection", G_CALLBACK(on_extend_selection), NULL);
  gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
  g_signal_connect(keys, "key-pressed", G_CALLBACK(on_typing_key), tab);
  gtk_widget_add_controller(GTK_WIDGET(tab->source), keys);
}
