/*
 * The control socket's requests (the protocol, tapi_1, is docs/plugins.md's): one table row per
 * command, with what it needs (an editor, an absolute path) and the permission a plugin must declare
 * for it.
 *
 * A connection that says "hello tapi_1" gets one answer per request ("ok", "ok <value>",
 * "error <code> <message>": the code is the promise, the message is for people and may be playful
 * and change any time); "hello tapi_1 <token>" also names the plugin ($TCODE_PLUGIN_TOKEN), whose
 * status items, diagnostics and terminal tabs are its own and whose permissions apply. A legacy
 * connection (no hello) is answered as before the API had a version.
 *
 * "_debug <program>" and "_debug-failed <dir>" are internal (F5's build, through t-code --send); a
 * legacy connection may still call them "debug" and "debug-failed".
 */
#include "ipc/client.h"

#include "application/identity.h"
#include "debug/debugger.h"
#include "editor/editor.h"
#include "editor/preview/preview.h"
#include "editor/tabs.h"
#include "git/compare.h"
#include "git/scm/scm.h"
#include "lsp/inline/diagnostics.h"
#include "lsp/client/client.h"
#include "lsp/json.h"
#include "plugins/plugins.h"
#include "plugins/views.h"
#include "popups/picker.h"
#include "project/project.h"
#include "window/secondwindow.h"
#include "window/statusbar.h"
#include "window/terminal.h"
#include <json-glib/json-glib.h>
#include <stdio.h>
#include <string.h>

#ifndef TCODE_VERSION /* set by the Makefile */
#define TCODE_VERSION "unknown"
#endif

/* Helpers ------------------------------------------------------------------------------------ */

/* The plugin a connection speaks for, as it owns things ("socket" for a script or the user). */
static const char *owner_of(Client *c)
{
  return c->owner ? c->owner : "socket";
}

/* The active tab, when it is an editor with its text. */
static Tab *active_editor(App *t)
{
  Tab *tab = current_tab(t);

  return tab && tab->kind == TAB_EDITOR && tab->buffer ? tab : NULL;
}

/* The place at line and column (1-based, characters), kept inside the text. */
static void iter_at_place(GtkTextBuffer *buffer, int line, int column, GtkTextIter *iter)
{
  GtkTextIter line_end;

  if (!gtk_text_buffer_get_iter_at_line(buffer, iter, MAX(line, 1) - 1)) {
    gtk_text_buffer_get_end_iter(buffer, iter);
    return;
  }
  line_end = *iter;
  if (!gtk_text_iter_ends_line(&line_end))
    gtk_text_iter_forward_to_line_end(&line_end);
  gtk_text_iter_set_line_offset(iter, CLAMP(column - 1, 0, gtk_text_iter_get_line_offset(&line_end)));
}

/* "ok", then text and a NUL (a legacy connection: only text and the NUL). */
static void send_text(Client *c, const char *text)
{
  if (c->api)
    client_send(c, "ok\n", 3);
  client_send(c, text, strlen(text) + 1);
}

/* A JSON request argument, or NULL (and the answer said why). */
static JsonNode *parse_json(Client *c, const char *arg)
{
  JsonParser *parser = json_parser_new();
  JsonNode *root = NULL;
  GError *error = NULL;

  if (json_parser_load_from_data(parser, arg, -1, &error) && json_parser_get_root(parser))
    root = json_node_copy(json_parser_get_root(parser));
  else
    answer_error(c, "bad-argument", error ? error->message : "needs JSON");
  g_clear_error(&error);
  g_object_unref(parser);
  return root;
}

/* Saying hello, reading -------------------------------------------------------------------------- */

/* "hello [tapi_N [token]]": the connection speaks tapi_N from now on (the latest when none is named),
 * for the plugin $TCODE_PLUGIN_TOKEN was given to. */
static gboolean do_hello(Client *c, const char *arg)
{
  char **words = g_strsplit(arg, " ", 3);
  const char *version = words[0] ? words[0] : "", *token = version[0] && words[1] ? words[1] : NULL;
  int number = 0;

  if (!*version)
    number = TAPI_LATEST;
  else if (g_str_has_prefix(version, "tapi_")) {
    char *end = NULL;
    gint64 n = g_ascii_strtoll(version + 5, &end, 10);
    if (end && end != version + 5 && !*end && n >= 1 && n <= TAPI_LATEST)
      number = (int)n;
  }
  if (!number) {
    char *line =
        g_strdup_printf("error unsupported-api t-code %s speaks tapi_1 to tapi_%d", TCODE_VERSION, TAPI_LATEST);
    send_line(c, line);
    g_free(line);
  } else if (token && !plugin_by_token(c->app, token)) {
    send_line(c, "error bad-argument never seen that token, were the plugins reloaded?");
  } else {
    c->api = number;
    if (token)
      g_set_str(&c->owner, plugin_by_token(c->app, token)->id);
    char *line = g_strdup_printf("ok tapi_%d t-code %s", number, TCODE_VERSION);
    send_line(c, line);
    g_free(line);
  }
  g_strfreev(words);
  return TRUE;
}

static gboolean do_version(Client *c, const char *arg G_GNUC_UNUSED)
{
  char *line = g_strdup_printf("ok tapi_%d t-code %s", TAPI_LATEST, TCODE_VERSION); /* legacy ones too */

  send_line(c, line);
  g_free(line);
  return TRUE;
}

static gboolean do_subscribe(Client *c, const char *arg G_GNUC_UNUSED)
{
  client_subscribe(c);
  return TRUE;
}

/* get-file, get-project: one line; a legacy connection gets it bare. */
static void send_value(Client *c, const char *value)
{
  if (c->api)
    answer_ok(c, value);
  else
    send_line(c, value);
}

static gboolean do_get_file(Client *c, const char *arg G_GNUC_UNUSED)
{
  Tab *tab = current_tab(c->app);

  send_value(c, tab && tab->kind == TAB_EDITOR && tab->path ? tab->path : "");
  return TRUE;
}

static gboolean do_get_project(Client *c, const char *arg G_GNUC_UNUSED)
{
  send_value(c, c->app->project.root ? c->app->project.root : "");
  return TRUE;
}

static gboolean do_get_text(Client *c, const char *arg G_GNUC_UNUSED)
{
  Tab *tab = active_editor(c->app);
  GtkTextIter start, end;
  char *text = NULL;

  if (tab) {
    gtk_text_buffer_get_bounds(GTK_TEXT_BUFFER(tab->buffer), &start, &end);
    text = gtk_text_buffer_get_text(GTK_TEXT_BUFFER(tab->buffer), &start, &end, TRUE);
  }
  send_text(c, text ? text : "");
  g_free(text);
  return TRUE;
}

static gboolean do_get_cursor(Client *c, const char *arg G_GNUC_UNUSED)
{
  GtkTextBuffer *buffer = GTK_TEXT_BUFFER(active_editor(c->app)->buffer);
  GtkTextIter iter;

  gtk_text_buffer_get_iter_at_mark(buffer, &iter, gtk_text_buffer_get_insert(buffer));
  char *place = g_strdup_printf("%d %d", gtk_text_iter_get_line(&iter) + 1, gtk_text_iter_get_line_offset(&iter) + 1);
  send_value(c, place);
  g_free(place);
  return TRUE;
}

static gboolean do_get_selection(Client *c, const char *arg G_GNUC_UNUSED)
{
  GtkTextBuffer *buffer = GTK_TEXT_BUFFER(active_editor(c->app)->buffer);
  GtkTextIter start, end;
  char *text = gtk_text_buffer_get_selection_bounds(buffer, &start, &end)
                   ? gtk_text_buffer_get_text(buffer, &start, &end, TRUE)
                   : g_strdup("");

  send_text(c, text);
  g_free(text);
  return TRUE;
}

/* The selection's start and end ("<line> <col> <line> <col>"); both the cursor when nothing is
 * selected. */
static gboolean do_get_selection_range(Client *c, const char *arg G_GNUC_UNUSED)
{
  GtkTextBuffer *buffer = GTK_TEXT_BUFFER(active_editor(c->app)->buffer);
  GtkTextIter start, end;

  gtk_text_buffer_get_selection_bounds(buffer, &start, &end); /* none: both at the cursor */
  char *range =
      g_strdup_printf("%d %d %d %d", gtk_text_iter_get_line(&start) + 1, gtk_text_iter_get_line_offset(&start) + 1,
                      gtk_text_iter_get_line(&end) + 1, gtk_text_iter_get_line_offset(&end) + 1);
  send_value(c, range);
  g_free(range);
  return TRUE;
}

/* Editing ------------------------------------------------------------------------------------ */

static gboolean do_insert(Client *c, const char *arg)
{
  gtk_text_buffer_insert_at_cursor(GTK_TEXT_BUFFER(active_editor(c->app)->buffer), arg, -1);
  answer_ok(c, NULL);
  return TRUE;
}

static gboolean do_append(Client *c, const char *arg)
{
  GtkTextBuffer *buffer = GTK_TEXT_BUFFER(active_editor(c->app)->buffer);
  GtkTextIter end;

  gtk_text_buffer_get_end_iter(buffer, &end);
  gtk_text_buffer_insert(buffer, &end, arg, -1);
  answer_ok(c, NULL);
  return TRUE;
}

static gboolean do_replace_all(Client *c, const char *arg)
{
  gtk_text_buffer_set_text(GTK_TEXT_BUFFER(active_editor(c->app)->buffer), arg, -1);
  answer_ok(c, NULL);
  return TRUE;
}

static gboolean do_save(Client *c, const char *arg G_GNUC_UNUSED)
{
  Tab *tab = active_editor(c->app);

  if (tab->path)
    socket_saving(c->app, tab->path); /* its did_save: "by":"plugin" */
  save_editor(tab);
  answer_ok(c, NULL);
  return TRUE;
}

static gboolean do_set_selection(Client *c, const char *arg)
{
  Tab *tab = active_editor(c->app);
  GtkTextBuffer *buffer = GTK_TEXT_BUFFER(tab->buffer);
  int l1, c1, l2, c2;
  GtkTextIter start, end;

  if (sscanf(arg, "%d %d %d %d", &l1, &c1, &l2, &c2) != 4) {
    answer_error(c, "bad-argument", "needs four numbers: <line> <column> <line> <column>");
    return TRUE;
  }
  iter_at_place(buffer, l1, c1, &start);
  iter_at_place(buffer, l2, c2, &end);
  gtk_text_buffer_select_range(buffer, &end, &start); /* the cursor at the second place */
  gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(tab->source), gtk_text_buffer_get_insert(buffer));
  answer_ok(c, NULL);
  return TRUE;
}

/* edit [{"range":[l1,c1,l2,c2],"text":".."}, ...]: all of them as one undo step. The ranges are the
 * text's before any of them (they must not overlap); the cursor stays where it was. */
static gboolean do_edit(Client *c, const char *arg)
{
  GtkTextBuffer *buffer = GTK_TEXT_BUFFER(active_editor(c->app)->buffer);
  JsonNode *root = parse_json(c, arg);
  JsonArray *edits = root && JSON_NODE_HOLDS_ARRAY(root) ? json_node_get_array(root) : NULL;
  guint n = edits ? json_array_get_length(edits) : 0;
  GPtrArray *marks = g_ptr_array_new();
  GPtrArray *texts = g_ptr_array_new();

  if (root && !edits)
    answer_error(c, "bad-argument", "needs a list of edits");
  for (guint i = 0; edits && i < n; i++) {
    JsonNode *edit = json_array_get_element(edits, i);
    JsonObject *object = JSON_NODE_HOLDS_OBJECT(edit) ? json_node_get_object(edit) : NULL;
    JsonNode *range_node = object ? json_object_get_member(object, "range") : NULL;
    JsonArray *range = range_node && JSON_NODE_HOLDS_ARRAY(range_node) ? json_node_get_array(range_node) : NULL;
    JsonNode *text = object ? json_object_get_member(object, "text") : NULL;
    GtkTextIter start, end;

    if (!range || json_array_get_length(range) != 4 || !text || !JSON_NODE_HOLDS_VALUE(text) ||
        json_node_get_value_type(text) != G_TYPE_STRING) {
      answer_error(c, "bad-argument", "each edit needs \"range\": [line, column, line, column] and \"text\"");
      edits = NULL;
      break;
    }
    iter_at_place(buffer, (int)json_array_get_int_element(range, 0), (int)json_array_get_int_element(range, 1), &start);
    iter_at_place(buffer, (int)json_array_get_int_element(range, 2), (int)json_array_get_int_element(range, 3), &end);
    if (gtk_text_iter_compare(&end, &start) < 0)
      end = start;
    g_ptr_array_add(marks, gtk_text_buffer_create_mark(buffer, NULL, &start, TRUE));
    g_ptr_array_add(marks, gtk_text_buffer_create_mark(buffer, NULL, &end, FALSE));
    g_ptr_array_add(texts, (gpointer)json_node_get_string(text));
  }
  if (edits) {
    gtk_text_buffer_begin_user_action(buffer);
    for (guint i = 0; i < texts->len; i++) {
      GtkTextIter start, end;
      gtk_text_buffer_get_iter_at_mark(buffer, &start, g_ptr_array_index(marks, 2 * i));
      gtk_text_buffer_get_iter_at_mark(buffer, &end, g_ptr_array_index(marks, 2 * i + 1));
      gtk_text_buffer_delete(buffer, &start, &end);
      gtk_text_buffer_insert(buffer, &start, g_ptr_array_index(texts, i), -1);
    }
    gtk_text_buffer_end_user_action(buffer);
    answer_ok(c, NULL);
  }
  for (guint i = 0; i < marks->len; i++)
    gtk_text_buffer_delete_mark(buffer, g_ptr_array_index(marks, i));
  g_ptr_array_unref(marks);
  g_ptr_array_unref(texts);
  if (root)
    json_node_unref(root);
  return TRUE;
}

/* Files and folders -------------------------------------------------------------------------- */

/* path[:line[:column]]: the path, and the place when one ends it (0: none). A file whose own name
 * ends that way is taken as it is. */
static char *split_place(const char *arg, int *line, int *column)
{
  const char *colon = strrchr(arg, ':');
  char *end = NULL;

  *line = *column = 0;
  if (g_file_test(arg, G_FILE_TEST_EXISTS) || !colon || !g_ascii_isdigit(colon[1]))
    return g_strdup(arg);
  int last = (int)strtol(colon + 1, &end, 10);
  if (*end)
    return g_strdup(arg);
  const char *before = colon;
  while (before > arg && g_ascii_isdigit(before[-1]))
    before--;
  if (before > arg && before[-1] == ':' && before < colon) { /* path:line:column */
    *line = atoi(before);
    *column = last;
    return g_strndup(arg, before - 1 - arg);
  }
  *line = last;
  return g_strndup(arg, colon - arg);
}

static gboolean do_open(Client *c, const char *arg)
{
  int line, column;
  char *path = split_place(arg, &line, &column);

  if (line > 0)
    open_file_at(c->app, path, line, column);
  else
    open_file(c->app, path);
  g_free(path);
  answer_ok(c, NULL);
  return TRUE;
}

/* clang-format off */
static gboolean do_open_side(Client *c, const char *arg) { open_to_side(c->app, arg); answer_ok(c, NULL); return TRUE; }
static gboolean do_open_window(Client *c, const char *arg) { open_in_new_window(c->app, arg); answer_ok(c, NULL); return TRUE; }
static gboolean do_preview(Client *c, const char *arg) { open_preview(c->app, arg); answer_ok(c, NULL); return TRUE; }
static gboolean do_compare(Client *c, const char *arg) { compare_with_head(c->app, arg); answer_ok(c, NULL); return TRUE; }
static gboolean do_history(Client *c, const char *arg) { file_history(c->app, arg); answer_ok(c, NULL); return TRUE; }
static gboolean do_terminal(Client *c, const char *arg) { cd_in_shell(c->app, arg); answer_ok(c, NULL); return TRUE; }
static gboolean do_debug(Client *c, const char *arg) { debug_launch(c->app, arg); answer_ok(c, NULL); return TRUE; }
static gboolean do_debug_failed(Client *c, const char *arg G_GNUC_UNUSED) { debug_build_failed(c->app); answer_ok(c, NULL); return TRUE; }
static gboolean do_say(Client *c, const char *arg) { status_say(c->app, arg); answer_ok(c, NULL); return TRUE; }
static gboolean do_scm_message(Client *c, const char *arg) { scm_set_message(arg); answer_ok(c, NULL); return TRUE; }
static gboolean do_lsp_refresh(Client *c, const char *arg G_GNUC_UNUSED) { lsp_reattach(c->app); answer_ok(c, NULL); return TRUE; }
static gboolean do_lsp_restart(Client *c, const char *arg G_GNUC_UNUSED) { lsp_restart(c->app); answer_ok(c, NULL); return TRUE; }
/* clang-format on */

/* Views ------------------------------------------------------------------------------------- */

/* A view id: one word without "/" (it names the view as "view:<plugin>/<id>"). */
static gboolean good_view_id(const char *id)
{
  return id && *id && !strpbrk(id, " \t\n/");
}

/* The strings of a JSON list, or NULL when it is not one of strings. */
static char **string_list(JsonNode *node)
{
  JsonArray *array = node && JSON_NODE_HOLDS_ARRAY(node) ? json_node_get_array(node) : NULL;
  GStrvBuilder *list;

  if (!array)
    return NULL;
  list = g_strv_builder_new();
  for (guint i = 0; i < json_array_get_length(array); i++) {
    JsonNode *item = json_array_get_element(array, i);
    if (!JSON_NODE_HOLDS_VALUE(item) || json_node_get_value_type(item) != G_TYPE_STRING) {
      g_strv_builder_unref(list);
      return NULL;
    }
    g_strv_builder_add(list, json_node_get_string(item));
  }
  char **strings = g_strv_builder_end(list);
  g_strv_builder_unref(list);
  return strings;
}

/* A JSON object's member that is a list of strings (g_strfreev); NULL when it is not there. *bad is set
 * when it is there and not such a list. */
static char **member_list(JsonObject *object, const char *name, gboolean *bad)
{
  JsonNode *node = json_object_get_member(object, name);
  char **list = node ? string_list(node) : NULL;

  if (node && !list)
    *bad = TRUE;
  return list;
}

/* view-open {"id":"…","title":"…","where":"left|bottom|right|editor|side","icon":"…","text":"…",
 * "markdown":true,"input":"placeholder","modes":["…"],"mode":"…","show":false}. */
static gboolean do_view_open(Client *c, const char *arg)
{
  JsonNode *root = parse_json(c, arg);
  JsonObject *object = root && JSON_NODE_HOLDS_OBJECT(root) ? json_node_get_object(root) : NULL;
  const char *id = object ? json_object_get_string_member_with_default(object, "id", NULL) : NULL;
  const char *where = object ? json_object_get_string_member_with_default(object, "where", "bottom") : NULL;
  gboolean bad = FALSE;
  char **modes = object ? member_list(object, "modes", &bad) : NULL;
  ViewOptions options = { 0 };
  ViewPlace place;

  if (!root)
    return TRUE; /* parse_json answered */
  if (object) {
    options.markdown = json_object_get_boolean_member_with_default(object, "markdown", FALSE);
    options.input = json_object_get_string_member_with_default(object, "input", NULL);
    options.modes = modes;
    options.mode = json_object_get_string_member_with_default(object, "mode", NULL);
    options.hidden = !json_object_get_boolean_member_with_default(object, "show", TRUE);
  }
  if (!object || !good_view_id(id))
    answer_error(c, "bad-argument", "a view needs an \"id\": one word without slashes");
  else if (!view_place(where, &place))
    answer_error(c, "bad-argument", "\"where\" is left, bottom, right, editor or side");
  else if (bad)
    answer_error(c, "bad-argument", "\"modes\" is a list of strings");
  else {
    view_open(c->app, owner_of(c), id, json_object_get_string_member_with_default(object, "title", NULL), place,
              json_object_get_string_member_with_default(object, "icon", NULL),
              json_object_get_string_member_with_default(object, "text", NULL), &options);
    answer_ok(c, NULL);
  }
  g_strfreev(modes);
  json_node_unref(root);
  return TRUE;
}

/* "<id> <text>": the id, and where the text after it starts (NULL: none). */
static char *view_arg(const char *arg, const char **text)
{
  const char *space = strchr(arg, ' ');

  if (text)
    *text = space ? space + 1 : "";
  return space ? g_strndup(arg, space - arg) : g_strdup(arg);
}

static gboolean no_view(Client *c, const char *id)
{
  char *message = g_strdup_printf("no view \"%s\" of this plugin's is open", id);

  answer_error(c, "no-view", message);
  g_free(message);
  return TRUE;
}

/* For a request on a view's input when it has none, or no such view. */
static gboolean no_input_view(Client *c, const char *id)
{
  char *message = g_strdup_printf("no view \"%s\" of this plugin's with an input is open", id);

  answer_error(c, "no-view", message);
  g_free(message);
  return TRUE;
}

/* view-set / view-append / view-extend <id> <text>. */
static gboolean set_view_text(Client *c, const char *arg, ViewTextChange change)
{
  const char *text;
  char *id = view_arg(arg, &text);

  if (view_set_text(c->app, owner_of(c), id, text, change))
    answer_ok(c, NULL);
  else
    no_view(c, id);
  g_free(id);
  return TRUE;
}

static gboolean do_view_set(Client *c, const char *arg)
{
  return set_view_text(c, arg, VIEW_TEXT_SET);
}

static gboolean do_view_append(Client *c, const char *arg)
{
  return set_view_text(c, arg, VIEW_TEXT_APPEND);
}

static gboolean do_view_extend(Client *c, const char *arg)
{
  return set_view_text(c, arg, VIEW_TEXT_EXTEND);
}

/* view-prompt {"id":"…","prompt":"…","text":"…","choices":["Allow","Deny"]}; no choices: the prompt goes. */
static gboolean do_view_prompt(Client *c, const char *arg)
{
  JsonNode *root = parse_json(c, arg);
  JsonObject *object = root && JSON_NODE_HOLDS_OBJECT(root) ? json_node_get_object(root) : NULL;
  const char *id = object ? json_object_get_string_member_with_default(object, "id", NULL) : NULL;
  const char *prompt = object ? json_object_get_string_member_with_default(object, "prompt", NULL) : NULL;
  gboolean bad = FALSE;
  char **choices = object ? member_list(object, "choices", &bad) : NULL;

  if (!root)
    return TRUE; /* parse_json answered */
  if (!object || !good_view_id(id) || !prompt || !*prompt || bad)
    answer_error(c, "bad-argument", "a prompt needs the view's \"id\", its own \"prompt\" and \"choices\" (strings)");
  else if (view_prompt(c->app, owner_of(c), id, prompt, json_object_get_string_member_with_default(object, "text", ""),
                       choices))
    answer_ok(c, NULL);
  else
    no_input_view(c, id);
  g_strfreev(choices);
  json_node_unref(root);
  return TRUE;
}

/* view-busy <id> [text]: the busy line with its Stop, or none without text. */
static gboolean do_view_busy(Client *c, const char *arg)
{
  const char *text;
  char *id = view_arg(arg, &text);

  if (view_busy(c->app, owner_of(c), id, *text ? text : NULL))
    answer_ok(c, NULL);
  else
    no_input_view(c, id);
  g_free(id);
  return TRUE;
}

/* view-input <id> <text>: the text in the view's input box, which gets the keyboard. */
static gboolean do_view_input(Client *c, const char *arg)
{
  const char *text;
  char *id = view_arg(arg, &text);

  if (view_input(c->app, owner_of(c), id, text))
    answer_ok(c, NULL);
  else
    no_input_view(c, id);
  g_free(id);
  return TRUE;
}

static gboolean do_view_show(Client *c, const char *arg)
{
  if (view_show(c->app, owner_of(c), arg))
    answer_ok(c, NULL);
  else
    no_view(c, arg);
  return TRUE;
}

static gboolean do_view_close(Client *c, const char *arg)
{
  if (view_close(c->app, owner_of(c), arg))
    answer_ok(c, NULL);
  else
    no_view(c, arg);
  return TRUE;
}

/* Language servers and debuggers ------------------------------------------------------------- */

/* The answer to lsp-command's request: "ok <result as JSON>" (null too), or an error. */
static void on_lsp_command(JsonNode *result, gpointer data)
{
  Client *c = data;

  if (result) {
    char *json = json_to_string(result, FALSE);
    answer_ok(c, json);
    g_free(json);
  } else {
    answer_error(c, "failed", "the language server said no (or went away)");
  }
  client_resume(c);
  client_unref(c);
}

/* lsp-command {"command":..,"arguments":[..],"path":..}: workspace/executeCommand on the language server
 * of path's editor (the active one without a path). */
static gboolean do_lsp_command(Client *c, const char *arg)
{
  JsonNode *root = parse_json(c, arg);
  const char *command = root ? json_string_at(root, "command") : NULL,
             *path = root ? json_string_at(root, "path") : NULL;
  JsonNode *arguments = root ? json_at(root, "arguments") : NULL;
  Tab *tab = path ? find_tab(c->app, TAB_EDITOR, path) : active_editor(c->app);

  if (!root)
    return TRUE;
  if (!command || !*command || (arguments && !JSON_NODE_HOLDS_ARRAY(arguments)) || (path && !g_path_is_absolute(path)))
    answer_error(c, "bad-argument", "needs {\"command\": .., \"arguments\": [..], \"path\": <absolute path>}");
  else if (!tab || !tab->view)
    answer_error(c, "no-editor", path ? "that file is not open in an editor" : "no editor here, open a file first");
  else if (!lsp_has(tab))
    answer_error(c, "no-server", "that file has no language server (yet: it may still be starting)");
  else {
    char *quoted = json_quote(command), *list = arguments ? json_to_string(arguments, FALSE) : g_strdup("[]");
    char *params = g_strdup_printf("{\"command\":%s,\"arguments\":%s}", quoted, list);
    client_ref(c);
    lsp_request(tab, "workspace/executeCommand", params, NULL, on_lsp_command, c);
    g_free(params);
    g_free(list);
    g_free(quoted);
    json_node_unref(root);
    return FALSE;
  }
  json_node_unref(root);
  return TRUE;
}

/* debug-start {"name":..,"connect":"host:port" | "argv":[..],"request":..,"arguments":{..},...}: a
 * debugging session with the plugin's debugger. */
static gboolean do_debug_start(Client *c, const char *arg)
{
  JsonNode *root = parse_json(c, arg);
  JsonNode *arguments = root ? json_at(root, "arguments") : NULL, *argv = root ? json_at(root, "argv") : NULL;
  char **args = string_list(argv), *json = NULL, *error = NULL;
  const char *cwd = root ? json_string_at(root, "cwd") : NULL;

  if (!root)
    return TRUE;
  if ((arguments && !JSON_NODE_HOLDS_OBJECT(arguments)) || (argv && !args) || (cwd && !g_path_is_absolute(cwd))) {
    answer_error(c, "bad-argument", "\"arguments\" is an object, \"argv\" a list of strings, \"cwd\" an absolute path");
  } else {
    json = arguments ? json_to_string(arguments, FALSE) : NULL;
    DebugAdapter adapter = { .name = json_string_at(root, "name"),
                             .id = json_string_at(root, "adapter-id"),
                             .connect = json_string_at(root, "connect"),
                             .argv = args,
                             .request = json_string_at(root, "request") ? json_string_at(root, "request") : "launch",
                             .arguments = json,
                             .cwd = cwd,
                             .shown = json_string_at(root, "shown") };
    if (debug_start_adapter(c->app, &adapter, &error))
      answer_ok(c, NULL);
    else
      answer_error(c, strstr(error, "runs already") ? "busy" : "bad-argument", error);
  }
  g_free(error);
  g_free(json);
  g_strfreev(args);
  json_node_unref(root);
  return TRUE;
}

static gboolean do_project(Client *c, const char *arg)
{
  if (!g_file_test(arg, G_FILE_TEST_IS_DIR)) {
    answer_error(c, "bad-argument", "that is not a folder");
    return TRUE;
  }
  open_project(c->app, arg);
  answer_ok(c, NULL);
  return TRUE;
}

static gboolean do_copy(Client *c, const char *arg)
{
  gdk_clipboard_set_text(gtk_widget_get_clipboard(GTK_WIDGET(c->app->window)), arg);
  answer_ok(c, NULL);
  return TRUE;
}

/* The status bar --------------------------------------------------------------------------- */

/* "<id> <text>": the plugin's own status bar item named id, made the first time. */
static gboolean do_status_set(Client *c, const char *arg)
{
  const char *space = strchr(arg, ' ');
  char *id = space ? g_strndup(arg, space - arg) : g_strdup(arg);

  if (!*id) {
    answer_error(c, "bad-argument", "needs <id> <text>");
  } else {
    char *key = g_strdup_printf("%s/%s", owner_of(c), id);
    status_plugin_set(c->app, key, space ? space + 1 : "");
    g_free(key);
    answer_ok(c, NULL);
  }
  g_free(id);
  return TRUE;
}

static gboolean do_status_clear(Client *c, const char *arg)
{
  char *key = g_strdup_printf("%s/%s", owner_of(c), arg);

  status_plugin_clear(c->app, key);
  g_free(key);
  answer_ok(c, NULL);
  return TRUE;
}

/* Asking the user (pick, ask) ---------------------------------------------------------------- */

/* The one question showing in the picker: its connection waits for the answer. */
static struct {
  Client *client;
  gboolean ask;      /* a line of text, not a row of items */
  GPtrArray *labels; /* the items (pick) */
  GPtrArray *details;
} asking;

/* The answer goes back and the connection's next requests are read. */
static void answered(const char *code, const char *value)
{
  Client *c = asking.client;

  asking.client = NULL;
  g_clear_pointer(&asking.labels, g_ptr_array_unref);
  g_clear_pointer(&asking.details, g_ptr_array_unref);
  if (code)
    answer_error(c, code, value);
  else
    answer_ok(c, value);
  client_resume(c);
  client_unref(c);
}

static void pick_query(App *t, const char *query, guint generation)
{
  int shown = 0;

  for (guint i = 0; asking.labels && i < asking.labels->len; i++) {
    const char *label = g_ptr_array_index(asking.labels, i);
    if (*query && fuzzy_score(query, label) < 0)
      continue;
    char *payload = g_strdup_printf("%u", i);
    picker_add(t, generation, label, g_ptr_array_index(asking.details, i), payload);
    g_free(payload);
    shown++;
  }
  picker_message(t, generation, shown ? NULL : "Nothing matches that");
}

static void ask_query(App *t, const char *query, guint generation)
{
  if (*query)
    picker_add(t, generation, query, "Enter", query);
  picker_message(t, generation, *query ? NULL : "Type away, then press Enter");
}

static void asked_choose(App *t G_GNUC_UNUSED, const char *payload)
{
  if (!asking.client)
    return;
  if (asking.ask) {
    answered(NULL, payload);
    return;
  }
  guint index = (guint)atoi(payload);
  char *value = g_strdup_printf("%u %s", index, (char *)g_ptr_array_index(asking.labels, index));
  answered(NULL, value);
  g_free(value);
}

static void asked_cancel(App *t G_GNUC_UNUSED)
{
  if (asking.client)
    answered("canceled", "the picker closed and nobody picked anything");
}

/* Shows the picker for c's question; the answer comes later. */
static gboolean start_asking(Client *c, const char *title, gboolean ask, GPtrArray *labels, GPtrArray *details)
{
  if (asking.client) { /* another question still shows: it gives way */
    picker_set_preview(c->app, NULL, NULL);
    answered("canceled", "another question cut in line");
  }
  client_ref(c);
  asking.client = c;
  asking.ask = ask;
  asking.labels = labels;
  asking.details = details;
  picker_open(c->app, title, ask ? ask_query : pick_query, asked_choose);
  picker_set_preview(c->app, NULL, asked_cancel);
  return FALSE;
}

/* pick {"title":..,"items":["label", {"label":..,"detail":..}, ...]}: "ok <index> <label>". */
static gboolean do_pick(Client *c, const char *arg)
{
  JsonNode *root = parse_json(c, arg);
  JsonNode *items =
      root && JSON_NODE_HOLDS_OBJECT(root) ? json_object_get_member(json_node_get_object(root), "items") : NULL;
  JsonArray *list = items && JSON_NODE_HOLDS_ARRAY(items) ? json_node_get_array(items) : NULL;
  GPtrArray *labels, *details;
  gboolean later;

  if (!root)
    return TRUE;
  if (!list || json_array_get_length(list) == 0) {
    answer_error(c, "bad-argument", "needs {\"title\": .., \"items\": [..]} with items");
    json_node_unref(root);
    return TRUE;
  }
  labels = g_ptr_array_new_with_free_func(g_free);
  details = g_ptr_array_new_with_free_func(g_free);
  for (guint i = 0; i < json_array_get_length(list); i++) {
    JsonNode *item = json_array_get_element(list, i);
    gboolean plain = JSON_NODE_HOLDS_VALUE(item) && json_node_get_value_type(item) == G_TYPE_STRING;
    const char *label = plain ? json_node_get_string(item) : json_string_at(item, "label");
    g_ptr_array_add(labels, g_strdup(label ? label : ""));
    g_ptr_array_add(details, g_strdup(plain ? NULL : json_string_at(item, "detail")));
  }
  later = start_asking(c, json_string_at(root, "title") ? json_string_at(root, "title") : "Pick one", FALSE, labels,
                       details);
  json_node_unref(root);
  return later;
}

/* ask <question>: "ok <text>". */
static gboolean do_ask(Client *c, const char *arg)
{
  return start_asking(c, *arg ? arg : "Answer", TRUE, NULL, NULL);
}

/* Diagnostics -------------------------------------------------------------------------------- */

/* diagnostics-set {"path":..,"items":[..]}: the plugin's diagnostics for that file. */
static gboolean do_diagnostics_set(Client *c, const char *arg)
{
  JsonNode *root = parse_json(c, arg);
  const char *path = root ? json_string_at(root, "path") : NULL;
  JsonNode *items =
      root && JSON_NODE_HOLDS_OBJECT(root) ? json_object_get_member(json_node_get_object(root), "items") : NULL;

  if (!root)
    return TRUE;
  if (!path || !g_path_is_absolute(path) || (items && !JSON_NODE_HOLDS_ARRAY(items)))
    answer_error(c, "bad-argument", "needs {\"path\": <absolute path>, \"items\": [..]}");
  else {
    diagnostics_set_owned(c->app, path, owner_of(c), items);
    answer_ok(c, NULL);
  }
  json_node_unref(root);
  return TRUE;
}

/* diagnostics-clear [path]: the plugin's for one file, or for all. */
static gboolean do_diagnostics_clear(Client *c, const char *arg)
{
  if (*arg && !g_path_is_absolute(arg)) {
    answer_error(c, "bad-argument", "needs an absolute path, or nothing at all");
    return TRUE;
  }
  diagnostics_clear_owned(c->app, *arg ? arg : NULL, owner_of(c));
  answer_ok(c, NULL);
  return TRUE;
}

/* Terminal tabs ------------------------------------------------------------------------------ */

static int terminal_ids; /* the last terminal-open's id */

/* The connection's plugin's terminal tab "<id> ..." names; *rest is what follows the id. NULL (and the
 * answer said why) when there is none. */
static VteTerminal *own_terminal(Client *c, const char *arg, const char **rest)
{
  char *end = NULL;
  long id = strtol(arg, &end, 10);

  for (int i = 0; end != arg; i++) {
    VteTerminal *term = shell_at(c->app, i);
    if (!term)
      break;
    if (GPOINTER_TO_INT(g_object_get_data(G_OBJECT(term), "t-code-api-id")) == id &&
        g_strcmp0(g_object_get_data(G_OBJECT(term), "t-code-api-owner"), owner_of(c)) == 0) {
      if (rest)
        *rest = *end == ' ' ? end + 1 : end;
      return term;
    }
  }
  answer_error(c, "bad-argument", "no terminal tab with that id is yours");
  return NULL;
}

/* terminal-open <title> [-- program args...]: "ok <id>". Without a program, the user's shell. */
static gboolean do_terminal_open(Client *c, const char *arg)
{
  char *padded = g_strconcat(" ", arg, NULL);
  char *separator = strstr(padded, " -- ");
  char *title = g_strstrip(separator ? g_strndup(padded, separator - padded) : g_strdup(padded));
  char **argv = NULL;
  GError *error = NULL;

  if (separator && !g_shell_parse_argv(separator + 4, NULL, &argv, &error)) {
    answer_error(c, "bad-argument", error->message);
    g_error_free(error);
  } else {
    VteTerminal *term = open_program(c->app, *title ? title : owner_of(c), argv);
    char *id = g_strdup_printf("%d", ++terminal_ids);
    g_object_set_data(G_OBJECT(term), "t-code-api-id", GINT_TO_POINTER(terminal_ids));
    g_object_set_data_full(G_OBJECT(term), "t-code-api-owner", g_strdup(owner_of(c)), g_free);
    answer_ok(c, id);
    g_free(id);
  }
  g_strfreev(argv);
  g_free(title);
  g_free(padded);
  return TRUE;
}

/* terminal-send <id> <text>: types the text and Enter. */
static gboolean do_terminal_send(Client *c, const char *arg)
{
  const char *text = NULL;
  VteTerminal *term = own_terminal(c, arg, &text);

  if (term) {
    shell_type(term, text, -1);
    shell_type(term, "\r", 1);
    answer_ok(c, NULL);
  }
  return TRUE;
}

/* terminal-interrupt <id>: Ctrl+C. */
static gboolean do_terminal_interrupt(Client *c, const char *arg)
{
  VteTerminal *term = own_terminal(c, arg, NULL);

  if (term) {
    shell_type(term, "\x03", 1);
    answer_ok(c, NULL);
  }
  return TRUE;
}

/* terminal-close-text <id> [text]: what is typed into the tab when t-code closes (no text: nothing). */
static gboolean do_terminal_close_text(Client *c, const char *arg)
{
  const char *text = NULL;
  VteTerminal *term = own_terminal(c, arg, &text);

  if (term) {
    shell_set_close_text(term, text);
    answer_ok(c, NULL);
  }
  return TRUE;
}

static gboolean do_terminal_close(Client *c, const char *arg)
{
  VteTerminal *term = own_terminal(c, arg, NULL);

  if (term) {
    close_shell_tab(c->app, term);
    answer_ok(c, NULL);
  }
  return TRUE;
}

/* The table ---------------------------------------------------------------------------------- */

typedef enum {
  NEEDS_NOTHING,
  NEEDS_EDITOR, /* the active tab is an editor */
  NEEDS_PATH,   /* the argument is an absolute path */
} Needs;

typedef struct {
  const char *name;
  Needs needs;
  PluginPermissions permission; /* a plugin's connection needs it declared */
  gboolean legacy_only;         /* an old name, for connections that did not say hello */
  gboolean (*run)(Client *c, const char *arg);
} Request;

/* clang-format off */
static const Request requests[] = {
  { "hello",              NEEDS_NOTHING, 0,               FALSE, do_hello },
  { "version",            NEEDS_NOTHING, 0,               FALSE, do_version },
  { "subscribe",          NEEDS_NOTHING, 0,               FALSE, do_subscribe },
  { "get-file",           NEEDS_NOTHING, 0,               FALSE, do_get_file },
  { "get-project",        NEEDS_NOTHING, 0,               FALSE, do_get_project },
  { "get-text",           NEEDS_NOTHING, 0,               FALSE, do_get_text },
  { "get-cursor",         NEEDS_EDITOR,  0,               FALSE, do_get_cursor },
  { "get-selection",      NEEDS_EDITOR,  0,               FALSE, do_get_selection },
  { "set-selection",      NEEDS_EDITOR,  0,               FALSE, do_set_selection },
  { "get-selection-range", NEEDS_EDITOR, 0,               FALSE, do_get_selection_range },
  { "insert",             NEEDS_EDITOR,  PLUGIN_EDIT,     FALSE, do_insert },
  { "append",             NEEDS_EDITOR,  PLUGIN_EDIT,     FALSE, do_append },
  { "replace-all",        NEEDS_EDITOR,  PLUGIN_EDIT,     FALSE, do_replace_all },
  { "edit",               NEEDS_EDITOR,  PLUGIN_EDIT,     FALSE, do_edit },
  { "save",               NEEDS_EDITOR,  0,               FALSE, do_save },
  { "open",               NEEDS_PATH,    0,               FALSE, do_open },
  { "open-side",          NEEDS_PATH,    0,               FALSE, do_open_side },
  { "open-window",        NEEDS_PATH,    0,               FALSE, do_open_window },
  { "preview",            NEEDS_PATH,    0,               FALSE, do_preview },
  { "compare",            NEEDS_PATH,    0,               FALSE, do_compare },
  { "history",            NEEDS_PATH,    0,               FALSE, do_history },
  { "project",            NEEDS_PATH,    0,               FALSE, do_project },
  { "terminal",           NEEDS_PATH,    0,               FALSE, do_terminal },
  { "say",                NEEDS_NOTHING, 0,               FALSE, do_say },
  { "copy",               NEEDS_NOTHING, 0,               FALSE, do_copy },
  { "status-set",         NEEDS_NOTHING, 0,               FALSE, do_status_set },
  { "status-clear",       NEEDS_NOTHING, 0,               FALSE, do_status_clear },
  { "pick",               NEEDS_NOTHING, 0,               FALSE, do_pick },
  { "ask",                NEEDS_NOTHING, 0,               FALSE, do_ask },
  { "diagnostics-set",    NEEDS_NOTHING, 0,               FALSE, do_diagnostics_set },
  { "diagnostics-clear",  NEEDS_NOTHING, 0,               FALSE, do_diagnostics_clear },
  { "terminal-open",      NEEDS_NOTHING, PLUGIN_TERMINAL, FALSE, do_terminal_open },
  { "terminal-send",      NEEDS_NOTHING, 0,               FALSE, do_terminal_send },
  { "terminal-interrupt", NEEDS_NOTHING, 0,               FALSE, do_terminal_interrupt },
  { "terminal-close",     NEEDS_NOTHING, 0,               FALSE, do_terminal_close },
  { "terminal-close-text", NEEDS_NOTHING, 0,              FALSE, do_terminal_close_text },
  { "scm-message",        NEEDS_NOTHING, 0,               FALSE, do_scm_message },
  { "view-open",          NEEDS_NOTHING, 0,               FALSE, do_view_open },
  { "view-set",           NEEDS_NOTHING, 0,               FALSE, do_view_set },
  { "view-append",        NEEDS_NOTHING, 0,               FALSE, do_view_append },
  { "view-extend",        NEEDS_NOTHING, 0,               FALSE, do_view_extend },
  { "view-prompt",        NEEDS_NOTHING, 0,               FALSE, do_view_prompt },
  { "view-busy",          NEEDS_NOTHING, 0,               FALSE, do_view_busy },
  { "view-input",         NEEDS_NOTHING, 0,               FALSE, do_view_input },
  { "view-show",          NEEDS_NOTHING, 0,               FALSE, do_view_show },
  { "view-close",         NEEDS_NOTHING, 0,               FALSE, do_view_close },
  { "lsp-refresh",        NEEDS_NOTHING, 0,               FALSE, do_lsp_refresh },
  { "lsp-restart",        NEEDS_NOTHING, 0,               FALSE, do_lsp_restart },
  { "lsp-command",        NEEDS_NOTHING, 0,               FALSE, do_lsp_command },
  { "debug-start",        NEEDS_NOTHING, 0,               FALSE, do_debug_start },
  { "_debug",             NEEDS_PATH,    0,               FALSE, do_debug },
  { "_debug-failed",      NEEDS_NOTHING, 0,               FALSE, do_debug_failed },
  { "debug",              NEEDS_PATH,    0,               TRUE,  do_debug },
  { "debug-failed",       NEEDS_NOTHING, 0,               TRUE,  do_debug_failed },
};
/* clang-format on */

/* Whether the connection may make a request that needs permission: a plugin's only when its manifest
 * declares it; a script's (no plugin named) always. */
static gboolean permitted(Client *c, PluginPermissions permission)
{
  Plugin *plugin;

  if (!permission || !c->owner)
    return TRUE;
  plugin = plugin_find(c->app, c->owner);
  return plugin && (plugin->permissions & permission);
}

gboolean handle(Client *c, const char *command, const char *arg)
{
  for (guint i = 0; i < G_N_ELEMENTS(requests); i++) {
    const Request *r = &requests[i];
    if (strcmp(r->name, command) != 0 || (r->legacy_only && c->api))
      continue;
    if (r->needs == NEEDS_EDITOR && !active_editor(c->app))
      answer_error(c, "no-editor", "no editor here, open a file first");
    else if (r->needs == NEEDS_PATH && !g_path_is_absolute(arg))
      answer_error(c, "bad-argument", "paths start with / around here (absolute paths only)");
    else if (!permitted(c, r->permission)) {
      char *message = g_strdup_printf("plugin.json does not ask for \"%s\" (add it to \"permissions\")",
                                      r->permission == PLUGIN_EDIT ? "edit" : "terminal");
      answer_error(c, "not-permitted", message);
      g_free(message);
    } else
      return r->run(c, arg);
    return TRUE;
  }
  char *message = g_strdup_printf("never heard of %s", command);
  answer_error(c, "unknown-command", message);
  g_free(message);
  return TRUE;
}

/* tapi answers ------------------------------------------------------------------------------- */

void answer_ok(Client *c, const char *value)
{
  char *line;

  if (!c->api)
    return;
  line = value ? g_strconcat("ok ", value, NULL) : g_strdup("ok");
  send_line(c, line);
  g_free(line);
}

void answer_error(Client *c, const char *code, const char *message)
{
  char *line;

  if (!c->api)
    return;
  line = g_strdup_printf("error %s %s", code, message);
  send_line(c, line);
  g_free(line);
}
