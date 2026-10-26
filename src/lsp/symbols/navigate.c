/*
 * Moving around the code (see navigate.h). Each command asks the server about the symbol at the
 * cursor; lists (references) and the new name (rename) go through the popup list (popups/picker.c).
 */
#include "lsp/symbols/navigate.h"

#include "editor/editor.h"
#include "editor/editorview.h"
#include "editor/tabs.h"
#include "git/gitstatus.h"
#include "lsp/client.h"
#include "lsp/inline/diagnostics.h"
#include "lsp/symbols/edits.h"
#include "popups/picker.h"

#include <stdlib.h>
#include <string.h>

#define MAX_BACK 50 /* places Go Back remembers */

/* A place in a file: 1-based line and column. */
typedef struct {
  char *path;
  int line, column;
} Place;

static GQueue back = G_QUEUE_INIT; /* Place: newest first */

static void place_free(Place *place)
{
  g_free(place->path);
  g_free(place);
}

/* The request for the symbol at the cursor: '{"textDocument": ..., "position": ...}' plus extra. */
static char *at_cursor(Tab *tab, const char *extra)
{
  GtkTextBuffer *buffer = GTK_TEXT_BUFFER(tab->buffer);
  GtkTextIter cursor;
  char *position, *params;

  gtk_text_buffer_get_iter_at_mark(buffer, &cursor, gtk_text_buffer_get_insert(buffer));
  position = lsp_position_params(tab, &cursor);
  params = g_strdup_printf("{%s%s}", position, extra ? extra : "");
  g_free(position);
  return params;
}

/* The cursor's line and column (1-based, the column in characters: tab->cursor.column has tabs
 * expanded). */
static void cursor_place(Tab *tab, int *line, int *column)
{
  GtkTextBuffer *buffer = GTK_TEXT_BUFFER(tab->buffer);
  GtkTextIter cursor;

  gtk_text_buffer_get_iter_at_mark(buffer, &cursor, gtk_text_buffer_get_insert(buffer));
  *line = gtk_text_iter_get_line(&cursor) + 1;
  *column = gtk_text_iter_get_line_offset(&cursor) + 1;
}

/* Remembers where the cursor is, for Go Back. */
static void remember(Tab *tab)
{
  Place *place = g_new0(Place, 1);

  place->path = g_strdup(tab->path);
  cursor_place(tab, &place->line, &place->column);
  g_queue_push_head(&back, place);
  while (g_queue_get_length(&back) > MAX_BACK)
    place_free(g_queue_pop_tail(&back));
}

static char *line_of(App *t, const char *path, int line, GHashTable *files);

/* The 1-based column (in characters) of an LSP character position in a line's text (UTF-8 bytes,
 * or UTF-16 units). */
static int column_in(const char *text, gint64 character, gboolean utf16)
{
  const char *p = text;

  for (gint64 at = 0; *p;) {
    at += utf16 ? (g_utf8_get_char(p) >= 0x10000 ? 2 : 1) : (gint64)(g_utf8_next_char(p) - p);
    if (at > character)
      break;
    p = g_utf8_next_char(p);
  }
  return (int)g_utf8_pointer_to_offset(text, p) + 1;
}

/* Opens path at an LSP position: from its tab's text when it is open and loaded, else from the
 * file's line. */
static void open_at(App *t, const char *path, JsonNode *position, gboolean utf16)
{
  Tab *tab = find_tab(t, TAB_EDITOR, path);
  int line = (int)json_int_at(position, "line", 0) + 1;
  gint64 character = json_int_at(position, "character", 0);

  if (tab && tab->view && !tab->view->loading) {
    GtkTextIter iter;
    select_tab(tab);
    lsp_iter_at_encoding(tab, position, &iter, utf16);
    editor_goto(tab, line, gtk_text_iter_get_line_offset(&iter) + 1);
  } else {
    GHashTable *files = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_strfreev);
    char *text = line_of(t, path, line - 1, files);
    open_file_at(t, path, line, column_in(text, character, utf16));
    g_free(text);
    g_hash_table_destroy(files);
  }
}

/* Definition --------------------------------------------------------------------------------- */

typedef struct {
  App *app;
  char *path;     /* the editor asking, by path: it may close meanwhile */
  gboolean save;  /* formatting: save once it is done */
  gboolean utf16; /* position encoding of the requesting server */
  guint changes;  /* formatting: the text's change count when it was asked */
} Asked;

static void asked_free(Asked *asked)
{
  g_free(asked->path);
  g_free(asked);
}

/* A Location, a list of them, or of LocationLinks: the first one. */
static void on_definition(JsonNode *result, gpointer data)
{
  Asked *asked = data;
  JsonNode *first = result && JSON_NODE_HOLDS_ARRAY(result) ? json_at(result, "0") : result;
  const char *uri = json_string_at(first, "uri") ? json_string_at(first, "uri") : json_string_at(first, "targetUri");
  JsonNode *start =
      json_at(first, "range.start") ? json_at(first, "range.start") : json_at(first, "targetSelectionRange.start");
  char *path = uri ? lsp_uri_to_path(uri) : NULL;
  Tab *from = find_tab(asked->app, TAB_EDITOR, asked->path);

  if (path && start) {
    if (from)
      remember(from);
    open_at(asked->app, path, start, asked->utf16);
  }
  g_free(path);
  asked_free(asked);
}

void go_to_definition(Tab *tab)
{
  Asked *asked = g_new0(Asked, 1);
  char *params = at_cursor(tab, NULL);

  asked->app = tab->app;
  asked->path = g_strdup(tab->path);
  asked->utf16 = lsp_utf16(tab);
  lsp_request(tab, "textDocument/definition", params, NULL, on_definition, asked);
  g_free(params);
}

void go_back(App *t)
{
  Place *place = g_queue_pop_head(&back);

  if (!place)
    return;
  open_file_at(t, place->path, place->line, place->column);
  place_free(place);
}

/* Ctrl+click: the definition of what was clicked. */
static void on_click(GtkGestureClick *click, int n G_GNUC_UNUSED, double x, double y, Tab *tab)
{
  GdkModifierType state = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(click));
  GtkTextView *view = GTK_TEXT_VIEW(tab->source);
  GtkTextIter iter;
  int bx, by;

  if (!(state & GDK_CONTROL_MASK) || !lsp_has(tab))
    return;
  gtk_text_view_window_to_buffer_coords(view, GTK_TEXT_WINDOW_WIDGET, (int)x, (int)y, &bx, &by);
  gtk_text_view_get_iter_at_location(view, &iter, bx, by);
  gtk_text_buffer_place_cursor(GTK_TEXT_BUFFER(tab->buffer), &iter);
  gtk_gesture_set_state(GTK_GESTURE(click), GTK_EVENT_SEQUENCE_CLAIMED); /* no selection starts */
  go_to_definition(tab);
}

void navigate_attach(Tab *tab)
{
  GtkGesture *click = gtk_gesture_click_new();

  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_PRIMARY);
  gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(click), GTK_PHASE_CAPTURE);
  g_signal_connect(click, "pressed", G_CALLBACK(on_click), tab);
  gtk_widget_add_controller(GTK_WIDGET(tab->source), GTK_EVENT_CONTROLLER(click));
}

/* References -------------------------------------------------------------------------------- */

/* The last list: each "path \x1f line \x1f column \x1f shown text". */
static GPtrArray *references;

/* A line of a file, from its open tab or else from disk (lines caches the files read). */
static char *line_of(App *t, const char *path, int line, GHashTable *files)
{
  Tab *tab = find_tab(t, TAB_EDITOR, path);
  char **lines;

  if (tab && tab->view && !tab->view->loading) {
    GtkTextIter start, end;
    gtk_text_buffer_get_iter_at_line(GTK_TEXT_BUFFER(tab->buffer), &start, line);
    end = start;
    if (!gtk_text_iter_ends_line(&end))
      gtk_text_iter_forward_to_line_end(&end);
    return gtk_text_buffer_get_text(GTK_TEXT_BUFFER(tab->buffer), &start, &end, TRUE);
  }
  if (!(lines = g_hash_table_lookup(files, path))) {
    char *text = NULL;
    g_file_get_contents(path, &text, NULL, NULL);
    lines = g_strsplit(text ? text : "", "\n", -1);
    g_hash_table_insert(files, g_strdup(path), lines);
    g_free(text);
  }
  return line < (int)g_strv_length(lines) ? g_strdup(lines[line]) : g_strdup("");
}

static void references_query(App *t, const char *text, guint generation)
{
  int shown = 0;

  for (guint i = 0; references && i < references->len; i++) {
    char **f = g_strsplit(g_ptr_array_index(references, i), "\x1f", 4);
    char *name = g_path_get_basename(f[0]);
    char *label = g_strdup_printf("%s:%s  %s", name, f[1], f[3]);
    if (!*text || fuzzy_score(text, label) >= 0) {
      char *payload = g_strjoin("\x1f", f[0], f[1], f[2], NULL);
      picker_add(t, generation, label, f[0], payload);
      g_free(payload);
      shown++;
    }
    g_free(label);
    g_free(name);
    g_strfreev(f);
  }
  if (!shown)
    picker_message(t, generation, references && references->len ? "No matching references" : "No references");
}

static void references_choose(App *t, const char *payload)
{
  char **f = g_strsplit(payload, "\x1f", 3);

  open_file_at(t, f[0], atoi(f[1]), atoi(f[2]));
  g_strfreev(f);
}

static void on_references(JsonNode *result, gpointer data)
{
  Asked *asked = data;
  JsonArray *array = result && JSON_NODE_HOLDS_ARRAY(result) ? json_node_get_array(result) : NULL;
  GHashTable *files = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_strfreev);
  Tab *from = find_tab(asked->app, TAB_EDITOR, asked->path);
  char *title;

  if (!references)
    references = g_ptr_array_new_with_free_func(g_free);
  g_ptr_array_set_size(references, 0);
  for (guint i = 0; array && i < json_array_get_length(array); i++) {
    JsonNode *ref = json_array_get_element(array, i);
    char *path = lsp_uri_to_path(json_string_at(ref, "uri") ? json_string_at(ref, "uri") : "");
    int line = (int)json_int_at(ref, "range.start.line", 0);
    char *text = path ? line_of(asked->app, path, line, files) : NULL;
    if (path) {
      int column = column_in(text, json_int_at(ref, "range.start.character", 0), asked->utf16);
      g_ptr_array_add(references, g_strdup_printf("%s\x1f%d\x1f%d\x1f%s", path, line + 1, column, g_strstrip(text)));
    }
    g_free(text);
    g_free(path);
  }
  if (from)
    remember(from);
  title = g_strdup_printf("%u references: pick one to go to", references->len);
  picker_open(asked->app, title, references_query, references_choose);
  g_free(title);
  g_hash_table_destroy(files);
  asked_free(asked);
}

void find_references(Tab *tab)
{
  Asked *asked = g_new0(Asked, 1);
  char *params = at_cursor(tab, ",\"context\":{\"includeDeclaration\":true}");

  asked->app = tab->app;
  asked->path = g_strdup(tab->path);
  asked->utf16 = lsp_utf16(tab);
  lsp_request(tab, "textDocument/references", params, NULL, on_references, asked);
  g_free(params);
}

/* Source / header -------------------------------------------------------------------------- */

/* Without the server: a file beside path with the same name and the other kind of extension. */
static char *counterpart(const char *path)
{
  static const char *const sources[] = { ".c", ".cc", ".cpp", ".cxx", NULL };
  static const char *const headers[] = { ".h", ".hh", ".hpp", ".hxx", NULL };
  const char *dot = strrchr(path, '.');
  const char *const *other = NULL;

  if (!dot || strchr(dot, '/'))
    return NULL;
  for (int i = 0; sources[i] && !other; i++)
    if (!strcmp(dot, sources[i]))
      other = headers;
  for (int i = 0; headers[i] && !other; i++)
    if (!strcmp(dot, headers[i]))
      other = sources;
  for (int i = 0; other && other[i]; i++) {
    char *candidate = g_strdup_printf("%.*s%s", (int)(dot - path), path, other[i]);
    if (g_file_test(candidate, G_FILE_TEST_IS_REGULAR))
      return candidate;
    g_free(candidate);
  }
  return NULL;
}

static void on_switched(JsonNode *result, gpointer data)
{
  Asked *asked = data;
  const char *uri = result && JSON_NODE_HOLDS_VALUE(result) ? json_node_get_string(result) : NULL;
  char *path = uri ? lsp_uri_to_path(uri) : NULL;

  if (!path)
    path = counterpart(asked->path);
  if (path)
    open_file(asked->app, path);
  g_free(path);
  asked_free(asked);
}

void switch_source_header(Tab *tab)
{
  Asked *asked = g_new0(Asked, 1);
  char *document = lsp_document_params(tab), *params = g_strdup_printf("{%s}", document);

  asked->app = tab->app;
  asked->path = g_strdup(tab->path);
  asked->utf16 = lsp_utf16(tab);
  if (lsp_has(tab))
    lsp_request(tab, "textDocument/switchSourceHeader", params, NULL, on_switched, asked);
  else
    on_switched(NULL, asked);
  g_free(params);
  g_free(document);
}

/* Symbols in the file ------------------------------------------------------------------------ */

static GPtrArray *symbols; /* "line\x1fcolumn\x1fname\x1fwhat" of the last answer */

static const char *kind_name(gint64 kind)
{
  switch (kind) {
    case 2: return "module";
    case 3: return "namespace";
    case 5: return "class";
    case 6: return "method";
    case 8: return "field";
    case 9: return "constructor";
    case 10: return "enum";
    case 12: return "function";
    case 13: return "variable";
    case 14: return "constant";
    case 22: return "enumerator";
    case 23: return "struct";
    case 26: return "type parameter";
    default: return "";
  }
}

/* DocumentSymbols with their children, or SymbolInformations, into symbols (inside: whose). */
static void add_symbols(Tab *tab, JsonArray *array, const char *inside)
{
  for (guint i = 0; array && i < json_array_get_length(array); i++) {
    JsonNode *symbol = json_array_get_element(array, i);
    const char *name = json_string_at(symbol, "name");
    JsonNode *at = json_at(symbol, "selectionRange.start");
    GtkTextIter iter;
    if (!at)
      at = json_at(symbol, "location.range.start");
    if (!name || !at)
      continue;
    lsp_iter_at(tab, at, &iter);
    const char *kind = kind_name(json_int_at(symbol, "kind", 0));
    char *what = inside ? g_strdup_printf("%s in %s", kind, inside) : g_strdup(kind);
    g_ptr_array_add(symbols, g_strdup_printf("%d\x1f%d\x1f%s\x1f%s", gtk_text_iter_get_line(&iter) + 1,
                                             gtk_text_iter_get_line_offset(&iter) + 1, name, what));
    g_free(what);
    JsonNode *children = json_at(symbol, "children");
    if (children && JSON_NODE_HOLDS_ARRAY(children))
      add_symbols(tab, json_node_get_array(children), name);
  }
}

static void symbols_query(App *t, const char *text, guint generation)
{
  int shown = 0;

  for (guint i = 0; symbols && i < symbols->len; i++) {
    char **f = g_strsplit(g_ptr_array_index(symbols, i), "\x1f", 4);
    if (!*text || fuzzy_score(text, f[2]) >= 0) {
      char *detail = g_strdup_printf("%s  :%s", f[3], f[0]);
      char *payload = g_strjoin("\x1f", f[0], f[1], NULL);
      picker_add(t, generation, f[2], detail, payload);
      g_free(payload);
      g_free(detail);
      shown++;
    }
    g_strfreev(f);
  }
  if (!shown)
    picker_message(t, generation, symbols && symbols->len ? "No matching symbols" : "No symbols in this file");
}

static char *symbols_path; /* the file they are in */

static void symbols_choose(App *t, const char *payload)
{
  char **f = g_strsplit(payload, "\x1f", 2);

  if (symbols_path)
    open_file_at(t, symbols_path, atoi(f[0]), atoi(f[1]));
  g_strfreev(f);
}

static void on_symbols(JsonNode *result, gpointer data)
{
  Asked *asked = data;
  Tab *tab = find_tab(asked->app, TAB_EDITOR, asked->path);

  if (!symbols)
    symbols = g_ptr_array_new_with_free_func(g_free);
  g_ptr_array_set_size(symbols, 0);
  if (tab && tab->view && result && JSON_NODE_HOLDS_ARRAY(result))
    add_symbols(tab, json_node_get_array(result), NULL);
  if (tab) {
    g_free(symbols_path);
    symbols_path = g_strdup(tab->path);
    remember(tab);
    picker_open(asked->app, "Go to a symbol in this file", symbols_query, symbols_choose);
  }
  asked_free(asked);
}

void go_to_symbol(Tab *tab)
{
  Asked *asked = g_new0(Asked, 1);
  char *document = lsp_document_params(tab), *params = g_strdup_printf("{%s}", document);

  asked->app = tab->app;
  asked->path = g_strdup(tab->path);
  asked->utf16 = lsp_utf16(tab);
  lsp_request(tab, "textDocument/documentSymbol", params, NULL, on_symbols, asked);
  g_free(params);
  g_free(document);
}
/* Workspace symbols ---------------------------------------------------------------------------- */

typedef struct {
  App *app;
  guint generation;
  char *query;
  gboolean utf16;
} SymbolRequest;

static char *workspace_path;
static void on_workspace_symbols(JsonNode *result, gpointer data);

static void symbol_request_free(SymbolRequest *request)
{
  g_free(request->query);
  g_free(request);
}

static void workspace_symbols_query(App *t, const char *text, guint generation)
{
  Tab *tab = workspace_path ? find_tab(t, TAB_EDITOR, workspace_path) : NULL;
  SymbolRequest *request;
  char *quoted, *params;

  if (!tab || !lsp_has(tab)) {
    picker_message(t, generation, "Language server is not ready");
    return;
  }
  request = g_new0(SymbolRequest, 1);
  request->app = t;
  request->generation = generation;
  request->query = g_strdup(text);
  quoted = json_quote(text);
  request->utf16 = lsp_utf16(tab);
  params = g_strdup_printf("{\"query\":%s}", quoted);
  picker_message(t, generation, "Searching workspace symbols…");
  lsp_request(tab, "workspace/symbol", params, NULL, on_workspace_symbols, request);
  g_free(params);
  g_free(quoted);
}

static void workspace_symbols_choose(App *t, const char *payload)
{
  char **fields = g_strsplit(payload, "\x1f", 4);
  int line = atoi(fields[1]);
  gint64 character = g_ascii_strtoll(fields[2], NULL, 10);
  GHashTable *files = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_strfreev);
  char *text = line_of(t, fields[0], line, files);
  int column = column_in(text, character, strcmp(fields[3], "1") == 0);

  open_file_at(t, fields[0], line + 1, column);
  g_free(text);
  g_hash_table_destroy(files);
  g_strfreev(fields);
}

static void on_workspace_symbols(JsonNode *result, gpointer data)
{
  SymbolRequest *request = data;
  JsonArray *array = result && JSON_NODE_HOLDS_ARRAY(result) ? json_node_get_array(result) : NULL;
  int shown = 0;

  if (!array) {
    picker_message(request->app, request->generation, "Workspace symbol search failed");
    symbol_request_free(request);
    return;
  }
  for (guint i = 0; i < json_array_get_length(array); i++) {
    JsonNode *symbol = json_array_get_element(array, i);
    const char *name = json_string_at(symbol, "name");
    const char *uri = json_string_at(symbol, "location.uri");
    JsonNode *position = json_at(symbol, "location.range.start");
    char *path, *basename, *detail, *payload;
    gint64 line, character;
    char *match;

    if (!name || !uri || !position)
      continue;
    path = lsp_uri_to_path(uri);
    if (!path)
      continue;
    match = g_strdup_printf("%s %s", name, path);
    if (fuzzy_score(request->query, match) < 0) {
      g_free(match);
      g_free(path);
      continue;
    }
    g_free(match);
    line = json_int_at(position, "line", 0);
    character = json_int_at(position, "character", 0);
    basename = g_path_get_basename(path);
    const char *container = json_string_at(symbol, "containerName");
    detail = g_strdup_printf("%s%s%s — %s:%" G_GINT64_FORMAT, kind_name(json_int_at(symbol, "kind", 0)),
                             container ? " in " : "", container ? container : "", basename, line + 1);
    payload = g_strdup_printf("%s\x1f%" G_GINT64_FORMAT "\x1f%" G_GINT64_FORMAT "\x1f%d", path, line, character,
                              request->utf16);
    picker_add(request->app, request->generation, name, detail, payload);
    shown++;
    g_free(payload);
    g_free(detail);
    g_free(basename);
    g_free(path);
  }
  if (!shown)
    picker_message(request->app, request->generation,
                   json_array_get_length(array) ? "No matching workspace symbols" : "No workspace symbols found");
  symbol_request_free(request);
}

void go_to_workspace_symbol(Tab *tab)
{
  if (!tab || !tab->view)
    return;
  g_free(workspace_path);
  workspace_path = g_strdup(tab->path);
  picker_open(tab->app, "Go to a workspace symbol", workspace_symbols_query, workspace_symbols_choose);
}

/* Quick fixes ------------------------------------------------------------------------------ */

static GPtrArray *actions; /* JSON of the last answer's CodeActions and Commands */
static char *actions_path; /* the file they are for */

static void actions_query(App *t, const char *text, guint generation)
{
  int shown = 0;

  for (guint i = 0; actions && i < actions->len; i++) {
    JsonNode *action = json_from_string(g_ptr_array_index(actions, i), NULL);
    const char *title = json_string_at(action, "title");
    if (title && (!*text || fuzzy_score(text, title) >= 0)) {
      char *payload = g_strdup_printf("%u", i);
      const char *kind = json_string_at(action, "kind");
      picker_add(t, generation, title,
                 json_at(action, "isPreferred") && json_node_get_boolean(json_at(action, "isPreferred")) ? "preferred"
                 : g_strcmp0(kind, "quickfix") == 0                                                      ? "fix"
                 : kind                                                                                  ? kind
                                                                                                         : "",
                 payload);
      g_free(payload);
      shown++;
    }
    json_node_unref(action);
  }
  if (!shown)
    picker_message(t, generation, actions && actions->len ? "No matching fixes" : "No quick fixes here");
}

static gboolean actions_utf16;

void navigate_apply_action(Tab *tab, JsonNode *action, gboolean utf16)
{
  JsonNode *command = json_at(action, "command");

  if (json_at(action, "edit"))
    navigate_apply_edit(tab->app, json_at(action, "edit"), FALSE, utf16);
  if (command && JSON_NODE_HOLDS_VALUE(command))
    command = action; /* a Command: {title, command, arguments} */
  if (command && JSON_NODE_HOLDS_OBJECT(command) && json_string_at(command, "command")) {
    JsonNode *arguments = json_at(command, "arguments");
    char *name = json_quote(json_string_at(command, "command"));
    char *list = arguments ? json_to_string(arguments, FALSE) : g_strdup("[]");
    char *params = g_strdup_printf("{\"command\":%s,\"arguments\":%s}", name, list);
    lsp_request(tab, "workspace/executeCommand", params, NULL, NULL, NULL);
    g_free(params);
    g_free(list);
    g_free(name);
  }
}

gboolean navigate_show_document(App *t, JsonNode *params)
{
  const char *uri = json_string_at(params, "uri");
  JsonNode *external = json_at(params, "external");
  char *scheme = uri ? g_uri_parse_scheme(uri) : NULL;
  gboolean shown = FALSE;

  if (g_strcmp0(scheme, "file") == 0 && !(external && json_node_get_boolean(external))) {
    char *path = lsp_uri_to_path(uri);
    if (path) {
      open_file_at(t, path, (int)json_int_at(params, "selection.start.line", 0) + 1,
                   (int)json_int_at(params, "selection.start.character", 0) + 1);
      shown = TRUE;
    }
    g_free(path);
  } else if (g_strcmp0(scheme, "http") == 0 || g_strcmp0(scheme, "https") == 0) {
    GtkUriLauncher *launcher = gtk_uri_launcher_new(uri);
    gtk_uri_launcher_launch(launcher, t->window, NULL, NULL, NULL);
    g_object_unref(launcher);
    shown = TRUE;
  }
  g_free(scheme);
  return shown;
}

gboolean navigate_action_runnable(Tab *tab, JsonNode *action)
{
  JsonNode *command = json_at(action, "command"), *commands;
  const char *name;

  if (json_at(action, "edit"))
    return TRUE;
  if (command && JSON_NODE_HOLDS_VALUE(command))
    command = action; /* a Command: {title, command, arguments} */
  name = command && JSON_NODE_HOLDS_OBJECT(command) ? json_string_at(command, "command") : NULL;
  commands = name && tab ? lsp_capability(tab, "executeCommandProvider.commands") : NULL;
  for (guint i = 0;
       commands && JSON_NODE_HOLDS_ARRAY(commands) && i < json_array_get_length(json_node_get_array(commands)); i++)
    if (g_strcmp0(json_array_get_string_element(json_node_get_array(commands), i), name) == 0)
      return TRUE;
  return FALSE;
}

static void actions_choose(App *t, const char *payload)
{
  guint i = (guint)atoi(payload);
  Tab *tab = actions_path ? find_tab(t, TAB_EDITOR, actions_path) : NULL;

  if (!tab || !actions || i >= actions->len)
    return;
  JsonNode *action = json_from_string(g_ptr_array_index(actions, i), NULL);
  navigate_apply_action(tab, action, actions_utf16);
  json_node_unref(action);
}

static void on_actions(JsonNode *result, gpointer data)
{
  Asked *asked = data;
  JsonArray *array = result && JSON_NODE_HOLDS_ARRAY(result) ? json_node_get_array(result) : NULL;
  Tab *tab = find_tab(asked->app, TAB_EDITOR, asked->path);

  if (!actions)
    actions = g_ptr_array_new_with_free_func(g_free);
  g_ptr_array_set_size(actions, 0);
  for (guint i = 0; array && i < json_array_get_length(array); i++) {
    JsonNode *action = json_array_get_element(array, i);
    gboolean preferred = json_at(action, "isPreferred") && json_node_get_boolean(json_at(action, "isPreferred"));
    if (!navigate_action_runnable(tab, action))
      continue;
    char *text = json_to_string(action, FALSE);
    if (preferred)
      g_ptr_array_insert(actions, 0, text); /* the server's best fix first */
    else
      g_ptr_array_add(actions, text);
  }
  g_free(actions_path);
  actions_path = g_strdup(asked->path);
  actions_utf16 = asked->utf16;
  picker_open(asked->app, "Quick fix: pick one to apply", actions_query, actions_choose);
  asked_free(asked);
}

void quick_fix(Tab *tab)
{
  GtkTextBuffer *buffer = GTK_TEXT_BUFFER(tab->buffer);
  GtkTextIter start, end;

  if (!lsp_has(tab) || !gtk_text_view_get_editable(GTK_TEXT_VIEW(tab->source)))
    return;
  if (!gtk_text_buffer_get_selection_bounds(buffer, &start, &end)) { /* the cursor's line */
    gtk_text_iter_set_line_offset(&start, 0);
    if (!gtk_text_iter_ends_line(&end))
      gtk_text_iter_forward_to_line_end(&end);
  }
  Asked *asked = g_new0(Asked, 1);
  char *document = lsp_document_params(tab), *range = lsp_range(tab, &start, &end);
  char *diagnostics = diagnostics_json(tab, gtk_text_iter_get_line(&start), gtk_text_iter_get_line(&end));
  char *params = g_strdup_printf("{%s,\"range\":%s,\"context\":{\"diagnostics\":%s}}", document, range, diagnostics);

  asked->app = tab->app;
  asked->path = g_strdup(tab->path);
  asked->utf16 = lsp_utf16(tab);
  lsp_request(tab, "textDocument/codeAction", params, NULL, on_actions, asked);
  g_free(params);
  g_free(diagnostics);
  g_free(range);
  g_free(document);
}

/* Formatting --------------------------------------------------------------------------------- */

static void on_formatted(JsonNode *result, gpointer data)
{
  Asked *asked = data;
  Tab *tab = find_tab(asked->app, TAB_EDITOR, asked->path);

  /* Typed on meanwhile: the edits are for the text it had, and would land in the wrong places. */
  if (tab && result && tab->view->changes == asked->changes)
    edits_apply(tab, result, asked->utf16);
  if (tab && asked->save)
    save_editor(tab); /* formatted or not (no server, nothing to change) */
  asked_free(asked);
}

/* Asks for the formatting; save: the file is saved once it is in. */
static void format_then(Tab *tab, gboolean save)
{
  Asked *asked = g_new0(Asked, 1);
  char *document = lsp_document_params(tab);
  char *params = g_strdup_printf("{%s,\"options\":{\"tabSize\":%d,\"insertSpaces\":%s}}", document,
                                 gtk_source_view_get_tab_width(tab->source),
                                 gtk_source_view_get_insert_spaces_instead_of_tabs(tab->source) ? "true" : "false");

  if (!gtk_text_view_get_editable(GTK_TEXT_VIEW(tab->source))) {
    g_free(asked);
  } else {
    asked->app = tab->app;
    asked->path = g_strdup(tab->path);
    asked->save = save;
    asked->utf16 = lsp_utf16(tab);
    asked->changes = tab->view->changes;
    lsp_request(tab, "textDocument/formatting", params, NULL, on_formatted, asked);
  }
  g_free(params);
  g_free(document);
}

void format_document(Tab *tab)
{
  format_then(tab, FALSE);
}

void save_with_format(Tab *tab)
{
  if (tab->app->settings.format_on_save && lsp_has(tab))
    format_then(tab, TRUE);
  else
    save_editor(tab);
}

/* Renaming ----------------------------------------------------------------------------------- */

static char *rename_path; /* the editor whose symbol is renamed */
static int rename_line, rename_column;
static char *old_name;

/* Applies one file's edits: to its editor when it is open (saved when save), else to the file. */
static void edit_file(App *t, const char *uri, JsonNode *edits, gboolean save, gboolean utf16)
{
  char *path = lsp_uri_to_path(uri), *text = NULL;
  Tab *tab = path ? find_tab(t, TAB_EDITOR, path) : NULL;

  if (tab && tab->view && !tab->view->loading) {
    edits_apply(tab, edits, utf16);
    if (save)
      save_editor(tab); /* like the files that are not open, which are written at once */
  } else if (path && g_file_get_contents(path, &text, NULL, NULL)) {
    char *changed = edits_apply_text(text, edits, utf16);
    g_file_set_contents(path, changed, -1, NULL);
    g_free(changed);
  }
  g_free(text);
  g_free(path);
}

/* A WorkspaceEdit: "changes" (uri -> edits) or "documentChanges" (a list of uri and edits). */
void navigate_apply_edit(App *t, JsonNode *edit, gboolean save, gboolean utf16)
{
  JsonNode *changes = json_at(edit, "changes"), *documents = json_at(edit, "documentChanges");

  if (changes && JSON_NODE_HOLDS_OBJECT(changes)) {
    JsonObject *object = json_node_get_object(changes);
    GList *uris = json_object_get_members(object);
    for (GList *each = uris; each; each = each->next)
      edit_file(t, each->data, json_object_get_member(object, each->data), save, utf16);
    g_list_free(uris);
  }
  for (guint i = 0;
       documents && JSON_NODE_HOLDS_ARRAY(documents) && i < json_array_get_length(json_node_get_array(documents));
       i++) {
    JsonNode *document = json_array_get_element(json_node_get_array(documents), i);
    const char *uri = json_string_at(document, "textDocument.uri");
    if (uri)
      edit_file(t, uri, json_at(document, "edits"), save, utf16);
  }
  refresh_branch(t); /* Source Control shows the changed files */
}

static void on_renamed(JsonNode *result, gpointer data)
{
  Asked *asked = data;

  navigate_apply_edit(asked->app, result, TRUE, asked->utf16);
  asked_free(asked);
}

static void rename_query(App *t, const char *text, guint generation)
{
  if (*text && strcmp(text, old_name) != 0) {
    char *label = g_strdup_printf("Rename %s to %s", old_name, text);
    picker_add(t, generation, label, "Enter", text);
    g_free(label);
  } else {
    picker_message(t, generation, "Type the new name, then Enter");
  }
}

static void rename_choose(App *t, const char *name)
{
  Tab *tab = find_tab(t, TAB_EDITOR, rename_path);
  Asked *asked;
  char *quoted, *extra, *params;

  if (!tab)
    return;
  editor_goto(tab, rename_line, rename_column); /* where it was asked, wherever the cursor went */
  asked = g_new0(Asked, 1);
  asked->app = t;
  asked->path = g_strdup(tab->path);
  asked->utf16 = lsp_utf16(tab);
  quoted = json_quote(name);
  extra = g_strconcat(",\"newName\":", quoted, NULL);
  params = at_cursor(tab, extra);
  lsp_request(tab, "textDocument/rename", params, NULL, on_renamed, asked);
  g_free(params);
  g_free(extra);
  g_free(quoted);
}

void rename_symbol(Tab *tab)
{
  GtkTextBuffer *buffer = GTK_TEXT_BUFFER(tab->buffer);
  GtkTextIter start, end;
  char *title;

  if (!lsp_has(tab) || !gtk_text_view_get_editable(GTK_TEXT_VIEW(tab->source)))
    return;
  /* The word at the cursor, for the title. */
  gtk_text_buffer_get_iter_at_mark(buffer, &start, gtk_text_buffer_get_insert(buffer));
  end = start;
  while (!gtk_text_iter_starts_line(&start)) {
    GtkTextIter before = start;
    gtk_text_iter_backward_char(&before);
    if (!g_unichar_isalnum(gtk_text_iter_get_char(&before)) && gtk_text_iter_get_char(&before) != '_')
      break;
    start = before;
  }
  while (g_unichar_isalnum(gtk_text_iter_get_char(&end)) || gtk_text_iter_get_char(&end) == '_')
    gtk_text_iter_forward_char(&end);
  g_free(old_name);
  old_name = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
  if (!*old_name)
    return;
  g_free(rename_path);
  rename_path = g_strdup(tab->path);
  cursor_place(tab, &rename_line, &rename_column);
  title = g_strdup_printf("New name for %s", old_name);
  picker_open(tab->app, title, rename_query, rename_choose);
  g_free(title);
}
