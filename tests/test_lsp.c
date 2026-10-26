/*
 * Tests for the language server client (src/lsp/client.c) with real editors. The server is this
 * program run with --fake-server: it keeps its own copy of each file, applying every change it is
 * sent, and answers each change with one diagnostic whose message is the whole text as it has it
 * (at the first "X" in it), so the tests see whether the editor and the server agree.
 */
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <glib/gstdio.h>

#define IDLE_SECONDS 1
#define FLUSH_MS 2
#define PULL_MS 2
#define SERVER_ARGV "/proc/self/exe", "--fake-server",

#include "editor_page.h"
#include "lsp/hierarchy.c" /* its panel, against the fake server */

/* The fake server -------------------------------------------------------------------------- */

static gboolean fake_utf16;
static GHashTable *fake_docs; /* uri -> GString */

/* One message from stdin, or NULL at EOF. */
static char *fake_read(void)
{
  char line[256];
  long length = -1;

  while (fgets(line, sizeof line, stdin)) {
    if (g_ascii_strncasecmp(line, "Content-Length:", 15) == 0)
      length = strtol(line + 15, NULL, 10);
    else if (strcmp(line, "\r\n") == 0 && length >= 0) {
      char *body = g_malloc0(length + 1);
      if (fread(body, 1, length, stdin) != (size_t)length) {
        g_free(body);
        return NULL;
      }
      return body;
    }
  }
  return NULL;
}

static void fake_send(const char *body)
{
  printf("Content-Length: %zu\r\n\r\n%s", strlen(body), body);
  fflush(stdout);
}

/* The byte offset of an LSP position in text. */
static gsize fake_offset(const char *text, JsonNode *position)
{
  gint64 line = json_int_at(position, "line", 0), character = json_int_at(position, "character", 0);
  const char *p = text;

  for (; line > 0 && *p; p++)
    if (*p == '\n')
      line--;
  if (!fake_utf16)
    return (p - text) + character;
  for (gint64 units = 0; units < character && *p && *p != '\n'; p = g_utf8_next_char(p))
    units += g_utf8_get_char(p) >= 0x10000 ? 2 : 1;
  return p - text;
}

/* The LSP position of a byte offset in text. */
static void fake_position(const char *text, gsize offset, int *line, int *character)
{
  const char *start = text;

  *line = 0;
  for (const char *p = text; p < text + offset; p++)
    if (*p == '\n') {
      (*line)++;
      start = p + 1;
    }
  if (!fake_utf16) {
    *character = (int)(text + offset - start);
    return;
  }
  *character = 0;
  for (const char *p = start; p < text + offset; p = g_utf8_next_char(p))
    *character += g_utf8_get_char(p) >= 0x10000 ? 2 : 1;
}

static gboolean fake_pull;    /* like neocmakelsp: diagnostics only when asked (textDocument/diagnostic) */
static gboolean fake_symbols; /* answers textDocument/documentSymbol (the Go server: the symbol tests use it) */

/* Call and type hierarchy over lines "name: callee callee" (supertypes are the callees, subtypes
 * the callers). The item of the line naming name, or NULL. */
static char *fake_hier_item(const char *uri, const char *name)
{
  GString *text = g_hash_table_lookup(fake_docs, uri);
  char **lines = g_strsplit(text ? text->str : "", "\n", -1), *item = NULL, *quoted = json_quote(uri);

  for (int i = 0; lines[i] && !item; i++) {
    const char *colon = strchr(lines[i], ':');
    if (colon && (size_t)(colon - lines[i]) == strlen(name) && strncmp(lines[i], name, colon - lines[i]) == 0)
      item = g_strdup_printf("{\"name\":\"%s\",\"kind\":12,\"uri\":%s,\"range\":{\"start\":{\"line\":%d,"
                             "\"character\":0},\"end\":{\"line\":%d,\"character\":%d}},\"selectionRange\":{"
                             "\"start\":{\"line\":%d,\"character\":0},\"end\":{\"line\":%d,\"character\":%d}}}",
                             name, quoted, i, i, (int)strlen(lines[i]), i, i, (int)strlen(name));
  }
  g_free(quoted);
  g_strfreev(lines);
  return item;
}

/* The items related to name: the callees on its line (down), or the lines calling it (up); each
 * wrapped as {"to": ...} / {"from": ...} when wrap is given. */
static char *fake_hier_related(const char *uri, const char *name, gboolean up, const char *wrap)
{
  GString *text = g_hash_table_lookup(fake_docs, uri), *out = g_string_new(NULL);
  char **lines = g_strsplit(text ? text->str : "", "\n", -1);

  for (int i = 0; lines[i]; i++) {
    char *colon = strchr(lines[i], ':');
    if (!colon)
      continue;
    char *owner = g_strndup(lines[i], colon - lines[i]);
    char **callees = g_strsplit(g_strstrip(colon + 1), " ", -1);
    for (int j = 0; callees[j]; j++) {
      const char *other = !*callees[j]                          ? NULL
                          : up && strcmp(callees[j], name) == 0 ? owner
                          : !up && strcmp(owner, name) == 0     ? callees[j]
                                                                : NULL;
      char *item = other ? fake_hier_item(uri, other) : NULL;
      if (item && wrap)
        g_string_append_printf(out, "%s{\"%s\":%s,\"fromRanges\":[]}", out->len ? "," : "", wrap, item);
      else if (item)
        g_string_append_printf(out, "%s%s", out->len ? "," : "", item);
      g_free(item);
    }
    g_strfreev(callees);
    g_free(owner);
  }
  g_strfreev(lines);
  return g_string_free(out, FALSE);
}

/* The diagnostics (a JSON array) that tell the test what the server has; with clangd's code for a
 * header not found when the text includes <missing.h>. */
static char *fake_diagnostics(GString *text)
{
  char *quoted_text = json_quote(text->str), *list;
  const char *x = strchr(text->str, 'X');
  int line = 0, character = 0;

  if (x)
    fake_position(text->str, x - text->str, &line, &character);
  list = g_strdup_printf("[{\"range\":{\"start\":{\"line\":%d,\"character\":%d},"
                         "\"end\":{\"line\":%d,\"character\":%d}},\"severity\":1,%s\"message\":%s}]",
                         line, character, line, character + (x ? 1 : 0),
                         strstr(text->str, "<missing.h>") ? "\"code\":\"pp_file_not_found\"," : "", quoted_text);
  g_free(quoted_text);
  return list;
}

static void fake_publish(const char *uri, GString *text)
{
  char *quoted_uri = json_quote(uri), *list = fake_diagnostics(text), *body;

  body = g_strdup_printf("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\",\"params\":"
                         "{\"uri\":%s,\"diagnostics\":%s}}",
                         quoted_uri, list);
  if (!fake_pull)
    fake_send(body);
  g_free(body);
  g_free(list);
  g_free(quoted_uri);
}

static char *fake_config_uri; /* the file that asked for settings */

static void fake_text_free(gpointer text)
{
  g_string_free(text, TRUE);
}

static int fake_server(void)
{
  char *body;

  fake_utf16 = g_strcmp0(g_getenv("FAKE_ENCODING"), "utf-16") == 0;
  fake_docs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, fake_text_free);
  while ((body = fake_read())) {
    JsonParser *parser = json_parser_new();
    JsonNode *message;
    const char *method, *uri;
    if (!json_parser_load_from_data(parser, body, -1, NULL)) {
      g_object_unref(parser);
      g_free(body);
      continue;
    }
    message = json_parser_get_root(parser);
    method = json_string_at(message, "method");
    uri = json_string_at(message, "params.textDocument.uri");
    if (g_strcmp0(method, "initialize") == 0) {
      fake_pull = json_at(message, "params.initializationOptions.lint") != NULL;     /* the CMake server */
      fake_symbols = json_at(message, "params.initializationOptions.hints") != NULL; /* gopls */
      char *reply =
          g_strdup_printf("{\"jsonrpc\":\"2.0\",\"id\":%" G_GINT64_FORMAT ",\"result\":{\"capabilities\":"
                          "{\"positionEncoding\":\"%s\",\"hoverProvider\":true,%s"
                          "\"executeCommandProvider\":{\"commands\":[\"fake.fix\"]},\"semanticTokensProvider\":"
                          "{\"legend\":{\"tokenTypes\":[\"function\",\"variable\",\"comment\"],"
                          "\"tokenModifiers\":[\"declaration\",\"readonly\"]}}}}}",
                          json_int_at(message, "id", 0), fake_utf16 ? "utf-16" : "utf-8",
                          fake_pull      ? "\"diagnosticProvider\":{\"interFileDependencies\":false},"
                          : fake_symbols ? "\"documentSymbolProvider\":true,\"callHierarchyProvider\":true,"
                                           "\"typeHierarchyProvider\":true,"
                                         : "");
      fake_send(reply);
      g_free(reply);
    } else if (g_strcmp0(method, "textDocument/didOpen") == 0) {
      GString *text = g_string_new(json_string_at(message, "params.textDocument.text"));
      g_hash_table_insert(fake_docs, g_strdup(uri), text);
      fake_publish(uri, text);
      if (g_str_has_suffix(uri, ".yaml")) { /* asks for settings; the answer comes back as the problem */
        g_free(fake_config_uri);
        fake_config_uri = g_strdup(uri);
        fake_send("{\"jsonrpc\":\"2.0\",\"id\":\"config\",\"method\":\"workspace/configuration\",\"params\":"
                  "{\"items\":[{\"section\":\"yaml.format\"},{\"section\":\"nothing\"}]}}");
      }
    } else if (!method && g_strcmp0(json_string_at(message, "id"), "config") == 0 && fake_config_uri) {
      GString *answer = g_string_new(NULL);
      char *result = json_to_string(json_at(message, "result"), FALSE);
      g_string_assign(answer, result);
      fake_publish(fake_config_uri, answer);
      g_string_free(answer, TRUE);
      g_free(result);
    } else if (g_strcmp0(method, "textDocument/didChange") == 0) {
      GString *text = g_hash_table_lookup(fake_docs, uri);
      JsonArray *changes = json_node_get_array(json_at(message, "params.contentChanges"));
      for (guint i = 0; text && i < json_array_get_length(changes); i++) {
        JsonNode *change = json_array_get_element(changes, i);
        const char *inserted = json_string_at(change, "text");
        if (!json_at(change, "range")) {
          g_string_assign(text, inserted);
          continue;
        }
        gsize start = fake_offset(text->str, json_at(change, "range.start"));
        gsize end = fake_offset(text->str, json_at(change, "range.end"));
        g_string_erase(text, start, end - start);
        g_string_insert(text, start, inserted);
      }
      if (text && strstr(text->str, "CRASH"))
        exit(1);
      if (text)
        fake_publish(uri, text);
    } else if (g_strcmp0(method, "textDocument/didClose") == 0) {
      g_hash_table_remove(fake_docs, uri);
    } else if (g_strcmp0(method, "textDocument/documentHighlight") == 0) {
      /* The first character of lines 0 and 2. */
      char *reply =
          g_strdup_printf("{\"jsonrpc\":\"2.0\",\"id\":%" G_GINT64_FORMAT ",\"result\":["
                          "{\"range\":{\"start\":{\"line\":0,\"character\":0},\"end\":{\"line\":0,\"character\":1}}},"
                          "{\"range\":{\"start\":{\"line\":2,\"character\":0},\"end\":{\"line\":2,\"character\":1}}}]}",
                          json_int_at(message, "id", 0));
      fake_send(reply);
      g_free(reply);
    } else if (g_strcmp0(method, "textDocument/signatureHelp") == 0) {
      /* Where it was asked from, told as the file's one diagnostic; the answer is "no signature". */
      char *quoted_uri = json_quote(uri), *reply;
      char *note = g_strdup_printf("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\",\"params\":"
                                   "{\"uri\":%s,\"diagnostics\":[{\"range\":{\"start\":{\"line\":0,\"character\":0},"
                                   "\"end\":{\"line\":0,\"character\":0}},\"message\":\"signature at %" G_GINT64_FORMAT
                                   ":%" G_GINT64_FORMAT "\"}]}}",
                                   quoted_uri, json_int_at(message, "params.position.line", 0),
                                   json_int_at(message, "params.position.character", 0));
      fake_send(note);
      reply = g_strdup_printf("{\"jsonrpc\":\"2.0\",\"id\":%" G_GINT64_FORMAT ",\"result\":null}",
                              json_int_at(message, "id", 0));
      fake_send(reply);
      g_free(reply);
      g_free(note);
      g_free(quoted_uri);
    } else if (g_strcmp0(method, "textDocument/hover") == 0) {
      char *reply =
          g_strdup_printf("{\"jsonrpc\":\"2.0\",\"id\":%" G_GINT64_FORMAT ",\"result\":{\"contents\":"
                          "{\"kind\":\"plaintext\",\"value\":\"hover at %" G_GINT64_FORMAT ":%" G_GINT64_FORMAT "\"}}}",
                          json_int_at(message, "id", 0), json_int_at(message, "params.position.line", 0),
                          json_int_at(message, "params.position.character", 0));
      fake_send(reply);
      g_free(reply);
    } else if (g_strcmp0(method, "textDocument/inlayHint") == 0) {
      /* A parameter name after each "(" with an argument, a type before each " =". */
      GString *text = g_hash_table_lookup(fake_docs, uri), *hints = g_string_new(NULL);
      for (gsize i = 0; text && i < text->len; i++) {
        const char *label = NULL;
        gsize at = i;
        if (text->str[i] == '(' && i + 1 < text->len && text->str[i + 1] != ')') {
          label = "\"p:\",\"paddingRight\":true";
          at = i + 1;
        } else if (text->str[i] == ' ' && i + 1 < text->len && text->str[i + 1] == '=') {
          label = "\": int\"";
        }
        if (!label)
          continue;
        int line, character;
        fake_position(text->str, at, &line, &character);
        g_string_append_printf(hints, "%s{\"position\":{\"line\":%d,\"character\":%d},\"label\":%s}",
                               hints->len ? "," : "", line, character, label);
      }
      char *reply = g_strdup_printf("{\"jsonrpc\":\"2.0\",\"id\":%" G_GINT64_FORMAT ",\"result\":[%s]}",
                                    json_int_at(message, "id", 0), hints->str);
      fake_send(reply);
      g_free(reply);
      g_string_free(hints, TRUE);
    } else if (g_strcmp0(method, "textDocument/prepareCallHierarchy") == 0 ||
               g_strcmp0(method, "textDocument/prepareTypeHierarchy") == 0) {
      GString *text = g_hash_table_lookup(fake_docs, uri);
      char **lines = g_strsplit(text ? text->str : "", "\n", -1);
      int line = (int)json_int_at(message, "params.position.line", 0);
      char *name = line < (int)g_strv_length(lines) && strchr(lines[line], ':')
                       ? g_strndup(lines[line], strchr(lines[line], ':') - lines[line])
                       : NULL;
      char *item = name ? fake_hier_item(uri, name) : NULL;
      char *reply = g_strdup_printf("{\"jsonrpc\":\"2.0\",\"id\":%" G_GINT64_FORMAT ",\"result\":[%s]}",
                                    json_int_at(message, "id", 0), item ? item : "");
      fake_send(reply);
      g_free(reply);
      g_free(item);
      g_free(name);
      g_strfreev(lines);
    } else if (g_str_has_prefix(method ? method : "", "callHierarchy/") ||
               g_str_has_prefix(method ? method : "", "typeHierarchy/")) {
      const char *item_uri = json_string_at(message, "params.item.uri"),
                 *name = json_string_at(message, "params.item.name");
      gboolean up = strcmp(method, "callHierarchy/incomingCalls") == 0 || strcmp(method, "typeHierarchy/subtypes") == 0;
      const char *wrap = strcmp(method, "callHierarchy/incomingCalls") == 0   ? "from"
                         : strcmp(method, "callHierarchy/outgoingCalls") == 0 ? "to"
                                                                              : NULL;
      char *related = fake_hier_related(item_uri ? item_uri : "", name ? name : "", up, wrap);
      char *reply = g_strdup_printf("{\"jsonrpc\":\"2.0\",\"id\":%" G_GINT64_FORMAT ",\"result\":[%s]}",
                                    json_int_at(message, "id", 0), related);
      fake_send(reply);
      g_free(reply);
      g_free(related);
    } else if (g_strcmp0(method, "textDocument/documentSymbol") == 0) {
      /* "class Name" ... "end" holds the "  fn name" lines in it; "fn name" alone is at the top. */
      GString *text = g_hash_table_lookup(fake_docs, uri), *out = g_string_new(NULL), *children = NULL;
      char **lines = g_strsplit(text ? text->str : "", "\n", -1);
      int class_line = -1;
      char *class_name = NULL;
      for (int i = 0; lines[i]; i++) {
        const char *line = lines[i];
        if (g_str_has_prefix(line, "class ")) {
          class_line = i;
          g_free(class_name);
          class_name = g_strdup(line + 6);
          children = g_string_new(NULL);
        } else if (g_str_has_prefix(line, "fn ") || g_str_has_prefix(line, "  fn ")) {
          int column = (int)(strstr(line, "fn ") - line) + 3;
          GString *into = children && class_line >= 0 && line[0] == ' ' ? children : out;
          g_string_append_printf(into,
                                 "%s{\"name\":\"%s\",\"kind\":12,\"range\":{\"start\":{\"line\":%d,\"character\":0},"
                                 "\"end\":{\"line\":%d,\"character\":%d}},\"selectionRange\":{\"start\":{\"line\":%d,"
                                 "\"character\":%d},\"end\":{\"line\":%d,\"character\":%d}}}",
                                 into->len ? "," : "", line + column, i, i, (int)strlen(line), i, column, i,
                                 (int)strlen(line));
        } else if (strcmp(line, "end") == 0 && class_line >= 0) {
          g_string_append_printf(out,
                                 "%s{\"name\":\"%s\",\"kind\":5,\"range\":{\"start\":{\"line\":%d,\"character\":0},"
                                 "\"end\":{\"line\":%d,\"character\":3}},\"selectionRange\":{\"start\":{\"line\":%d,"
                                 "\"character\":6},\"end\":{\"line\":%d,\"character\":6}},\"children\":[%s]}",
                                 out->len ? "," : "", class_name, class_line, i, class_line, class_line, children->str);
          g_string_free(children, TRUE);
          children = NULL;
          class_line = -1;
        }
      }
      char *reply = g_strdup_printf("{\"jsonrpc\":\"2.0\",\"id\":%" G_GINT64_FORMAT ",\"result\":[%s]}",
                                    json_int_at(message, "id", 0), out->str);
      fake_send(reply);
      g_free(reply);
      g_free(class_name);
      if (children)
        g_string_free(children, TRUE);
      g_string_free(out, TRUE);
      g_strfreev(lines);
    } else if (g_strcmp0(method, "textDocument/diagnostic") == 0) {
      GString *text = g_hash_table_lookup(fake_docs, uri);
      char *list = text ? fake_diagnostics(text) : g_strdup("[]");
      char *reply = g_strdup_printf("{\"jsonrpc\":\"2.0\",\"id\":%" G_GINT64_FORMAT
                                    ",\"result\":{\"kind\":\"full\",\"items\":%s}}",
                                    json_int_at(message, "id", 0), list);
      fake_send(reply);
      g_free(reply);
      g_free(list);
    } else if (g_strcmp0(method, "textDocument/codeAction") == 0) {
      /* For the problems sent along, asking for quick fixes only: a refactoring (not one), a fix and
       * the preferred fix, both editing the problem's character at 0:4; fixes with a command it does
       * not run and one to resolve first (neither can be applied), and one with its own command. */
      JsonNode *diagnostics = json_at(message, "params.context.diagnostics");
      gboolean asked = diagnostics && json_array_get_length(json_node_get_array(diagnostics)) == 1 &&
                       g_strcmp0(json_string_at(message, "params.context.only.0"), "quickfix") == 0;
      char *quoted_uri = json_quote(uri), *reply;
      if (asked)
        reply = g_strdup_printf(
            "{\"jsonrpc\":\"2.0\",\"id\":%" G_GINT64_FORMAT ",\"result\":["
            "{\"title\":\"Extract\",\"kind\":\"refactor\"},"
            "{\"title\":\"Rename X to Y\",\"kind\":\"quickfix\",\"edit\":{\"changes\":{%s:[{\"newText\":\"Y\","
            "\"range\":{\"start\":{\"line\":0,\"character\":4},\"end\":{\"line\":0,\"character\":5}}}]}}},"
            "{\"title\":\"Delete X\",\"kind\":\"quickfix\",\"isPreferred\":true,\"edit\":{\"changes\":{%s:[{"
            "\"newText\":\"\",\"range\":{\"start\":{\"line\":0,\"character\":4},\"end\":{\"line\":0,"
            "\"character\":5}}}]}}},"
            "{\"title\":\"Sort\",\"kind\":\"quickfix\",\"command\":{\"title\":\"Sort\",\"command\":\"json.sort\"}},"
            "{\"title\":\"Resolve me\",\"kind\":\"quickfix\",\"data\":{}},"
            "{\"title\":\"Run fake fix\",\"kind\":\"quickfix\",\"command\":{\"title\":\"Run\",\"command\":"
            "\"fake.fix\"}}]}",
            json_int_at(message, "id", 0), quoted_uri, quoted_uri);
      else
        reply = g_strdup_printf("{\"jsonrpc\":\"2.0\",\"id\":%" G_GINT64_FORMAT ",\"result\":[]}",
                                json_int_at(message, "id", 0));
      fake_send(reply);
      g_free(reply);
      g_free(quoted_uri);
    } else if (g_strcmp0(method, "workspace/symbol") == 0) {
      const char *query = json_string_at(message, "params.query");
      char *reply;
      if (g_strcmp0(query, "fail") == 0) {
        reply = g_strdup_printf("{\"jsonrpc\":\"2.0\",\"id\":%" G_GINT64_FORMAT
                                ",\"error\":{\"code\":-32603,\"message\":\"test failure\"}}",
                                json_int_at(message, "id", 0));
      } else if (g_strcmp0(query, "needle") == 0) {
        reply = g_strdup_printf("{\"jsonrpc\":\"2.0\",\"id\":%" G_GINT64_FORMAT ",\"result\":["
                                "{\"name\":\"needleSymbol\",\"kind\":12,\"containerName\":\"Box\","
                                "\"location\":{\"uri\":\"file:///tmp/t-code-test/target.c\","
                                "\"range\":{\"start\":{\"line\":1,\"character\":4},"
                                "\"end\":{\"line\":1,\"character\":16}}}}]}",
                                json_int_at(message, "id", 0));
      } else {
        reply = g_strdup_printf("{\"jsonrpc\":\"2.0\",\"id\":%" G_GINT64_FORMAT ",\"result\":[]}",
                                json_int_at(message, "id", 0));
      }
      fake_send(reply);
      g_free(reply);
    }
    g_object_unref(parser);
    g_free(body);
  }
  return 0;
}

/* Editors ------------------------------------------------------------------------------------ */

static App app;

static Tab *new_tab(const char *path, const char *text)
{
  Tab *tab = g_new0(Tab, 1);

  tab->app = &app;
  tab->kind = TAB_EDITOR;
  tab->path = g_strdup(path);
  tab->page = g_object_ref_sink(editor_page());
  editor_view_init(tab, FALSE);
  gtk_source_buffer_set_language(
      tab->buffer, gtk_source_language_manager_get_language(gtk_source_language_manager_get_default(), "c"));
  gtk_text_buffer_set_text(GTK_TEXT_BUFFER(tab->buffer), text, -1);
  picker_test_tab = tab;
  return tab;
}

static void free_tab(Tab *tab)
{
  editor_view_free(tab->view);
  g_object_unref(tab->page);
  g_free(tab->path);
  if (picker_test_tab == tab)
    picker_test_tab = NULL;
  g_free(tab);
}

static char *text_of(Tab *tab)
{
  GtkTextIter start, end;

  gtk_text_buffer_get_bounds(GTK_TEXT_BUFFER(tab->buffer), &start, &end);
  return gtk_text_buffer_get_text(GTK_TEXT_BUFFER(tab->buffer), &start, &end, TRUE);
}

/* What the server last said it has: the message of the one diagnostic. */
static const char *server_text(Tab *tab)
{
  GPtrArray *list = tab->view->diagnostics;

  return list && list->len == 1 ? ((Diagnostic *)g_ptr_array_index(list, 0))->message : NULL;
}

/* Runs the main loop until the server has the editor's text (or 5 seconds pass). */
static gboolean server_agrees(Tab *tab)
{
  gint64 deadline = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
  gboolean same = FALSE;

  while (!same && g_get_monotonic_time() < deadline) {
    char *text = text_of(tab);
    same = g_strcmp0(server_text(tab), text) == 0;
    g_free(text);
    if (!same)
      g_main_context_iteration(NULL, TRUE);
  }
  return same;
}

static void run_for(int ms)
{
  gint64 until = g_get_monotonic_time() + ms * 1000;

  while (g_get_monotonic_time() < until)
    g_main_context_iteration(NULL, FALSE);
}

static GtkTextIter iter_at(Tab *tab, int line, int offset)
{
  GtkTextIter iter;

  gtk_text_buffer_get_iter_at_line_offset(GTK_TEXT_BUFFER(tab->buffer), &iter, line, offset);
  return iter;
}

/* Edits of every kind, checked against the server's copy after each burst. */
static void edit_and_compare(void)
{
  Tab *tab = new_tab("/tmp/t-code-test/sync.c", "int a;\nçağrı X(\"🙂\");\n");
  GtkTextBuffer *buffer = GTK_TEXT_BUFFER(tab->buffer);
  GtkTextIter start, end;
  char *at;

  lsp_open(tab);
  g_assert_true(server_agrees(tab));
  /* The diagnostic is on the "X", after characters of two bytes each. */
  start = iter_at(tab, 1, 6);
  at = diagnostics_at(tab, &start, FALSE);
  g_assert_nonnull(at);
  g_free(at);

  start = iter_at(tab, 1, 0);
  gtk_text_buffer_insert(buffer, &start, "ünlü ", -1); /* typing */
  start = iter_at(tab, 1, 15);
  gtk_text_buffer_insert(buffer, &start, "🙂🙂", -1); /* after an emoji */
  g_assert_true(server_agrees(tab));

  start = iter_at(tab, 0, 0);
  end = iter_at(tab, 1, 3);
  gtk_text_buffer_delete(buffer, &start, &end); /* across lines */
  start = iter_at(tab, 0, 0);
  gtk_text_buffer_insert(buffer, &start, "a\nb\nc\n", -1); /* new lines */
  g_assert_true(server_agrees(tab));

  gtk_text_buffer_set_text(buffer, "all new X\n", -1); /* everything replaced */
  g_assert_true(server_agrees(tab));
  lsp_close(tab);
  free_tab(tab);
}

static void test_sync_utf8(void)
{
  lsp_stop(&app);
  g_setenv("FAKE_ENCODING", "utf-8", TRUE);
  edit_and_compare();
  g_assert_false(app.lsp->utf16);
}

static void test_sync_utf16(void)
{
  lsp_stop(&app);
  g_setenv("FAKE_ENCODING", "utf-16", TRUE);
  edit_and_compare();
  g_assert_true(app.lsp->utf16);
  lsp_stop(&app);
  g_setenv("FAKE_ENCODING", "utf-8", TRUE);
}

/* A file loaded again (changed on disk) goes whole. */
static void test_reload(void)
{
  Tab *tab = new_tab("/tmp/t-code-test/reload.c", "old\n");

  lsp_open(tab);
  g_assert_true(server_agrees(tab));
  tab->view->loading = TRUE; /* what the loader inserts is not sent change by change */
  gtk_text_buffer_set_text(GTK_TEXT_BUFFER(tab->buffer), "new text\n", -1);
  tab->view->loading = FALSE;
  lsp_open(tab);
  g_assert_true(server_agrees(tab));
  free_tab(tab);
}

typedef struct {
  gboolean called;
  char *value;
} Answer;

static void on_answer(JsonNode *result, gpointer data)
{
  Answer *answer = data;

  answer->called = TRUE;
  answer->value = g_strdup(result ? json_string_at(result, "contents.value") : NULL);
}

static void test_hover(void)
{
  Tab *tab = new_tab("/tmp/t-code-test/hover.c", "int a;\nint bb;\n");
  Answer answer = { 0 };
  GtkTextIter iter;
  char *position, *params;

  lsp_open(tab);
  g_assert_true(server_agrees(tab));
  iter = iter_at(tab, 1, 5);
  position = lsp_position_params(tab, &iter);
  params = g_strdup_printf("{%s}", position);
  lsp_request(tab, "textDocument/hover", params, NULL, on_answer, &answer);
  while (!answer.called)
    g_main_context_iteration(NULL, TRUE);
  g_assert_cmpstr(answer.value, ==, "hover at 1:5");
  g_free(answer.value);
  g_free(params);
  g_free(position);
  free_tab(tab);
}

static void on_populated(GObject *source G_GNUC_UNUSED, GAsyncResult *result, gpointer data)
{
  *(int *)data = g_task_propagate_boolean(G_TASK(result), NULL) ? 1 : -1;
}

/* The hover's buttons, in order. */
static GPtrArray *hover_buttons(GtkWidget *widget, GPtrArray *found)
{
  if (GTK_IS_BUTTON(widget))
    g_ptr_array_add(found, widget);
  for (GtkWidget *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child))
    hover_buttons(child, found);
  return found;
}

/* On a problem the hover offers the server's quick fixes as buttons, the preferred one first;
 * clicking one applies it. */
static void test_hover_fixes(void)
{
  Tab *tab = new_tab("/tmp/t-code-test/fixes.c", "int X;\n");
  GtkSourceHoverDisplay *display = g_object_ref_sink(g_object_new(GTK_SOURCE_TYPE_HOVER_DISPLAY, NULL));
  GtkTextIter iter;
  GPtrArray *buttons;
  int done = 0;
  char *text;

  lsp_open(tab);
  g_assert_true(server_agrees(tab));
  iter = iter_at(tab, 0, 4);
  populate(NULL, tab, &iter, display, NULL, on_populated, &done);
  while (!done)
    g_main_context_iteration(NULL, TRUE);
  g_assert_cmpint(done, ==, 1);
  buttons = hover_buttons(GTK_WIDGET(display), g_ptr_array_new());
  g_assert_cmpuint(buttons->len, ==, 3); /* "Sort" and "Resolve me" cannot be applied */
  g_assert_cmpstr(gtk_button_get_label(g_ptr_array_index(buttons, 0)), ==, "Quick Fix: Delete X");
  g_assert_cmpstr(gtk_button_get_label(g_ptr_array_index(buttons, 1)), ==, "Quick Fix: Rename X to Y");
  g_assert_cmpstr(gtk_button_get_label(g_ptr_array_index(buttons, 2)), ==, "Quick Fix: Run fake fix");
  g_signal_emit_by_name(g_ptr_array_index(buttons, 0), "clicked");
  text = text_of(tab);
  g_assert_cmpstr(text, ==, "int ;\n");
  g_free(text);
  g_ptr_array_unref(buttons);

  /* Away from the problem: no fixes asked for, no buttons. */
  g_assert_true(server_agrees(tab));
  g_object_unref(display);
  display = g_object_ref_sink(g_object_new(GTK_SOURCE_TYPE_HOVER_DISPLAY, NULL));
  iter = iter_at(tab, 0, 3);
  done = 0;
  populate(NULL, tab, &iter, display, NULL, on_populated, &done);
  while (!done)
    g_main_context_iteration(NULL, TRUE);
  buttons = hover_buttons(GTK_WIDGET(display), g_ptr_array_new());
  g_assert_cmpuint(buttons->len, ==, 0);
  g_ptr_array_unref(buttons);
  g_object_unref(display);
  free_tab(tab);
}

/* rust-analyzer gets the picked Cargo features and target as initializationOptions, pylsp the
 * picked Python as jedi's environment; other servers neither. */
static void test_toolchain_options(void)
{
  Lsp rust = { .kind = SERVER_RUST, .app = &app }, python = { .kind = SERVER_PYTHON, .app = &app };
  Lsp c = { .kind = SERVER_CLANGD, .app = &app };
  char *text;

  g_assert_null(server_options(&rust)); /* nothing picked */
  g_assert_null(server_settings(&python));
  toolchain_test_features = "json,serde";
  toolchain_test_target = "wasm32-unknown-unknown";
  toolchain_test_python = "/p/.venv/bin/python";
  text = server_options(&rust);
  g_assert_cmpstr(text, ==, "{\"cargo\":{\"features\":[\"json\",\"serde\"],\"target\":\"wasm32-unknown-unknown\"}}");
  g_free(text);
  text = server_settings(&python);
  g_assert_cmpstr(text, ==, "{\"pylsp\":{\"plugins\":{\"jedi\":{\"environment\":\"/p/.venv/bin/python\"}}}}");
  g_free(text);
  g_assert_null(server_options(&python));
  g_assert_null(server_settings(&rust));
  g_assert_null(server_options(&c));
  toolchain_test_features = toolchain_test_target = toolchain_test_python = NULL;
}

/* The JSON server validates only (no formatting, no schema downloads) and is not given generated or
 * big JSON files; YAML gets its settings; a server asking for a section of them
 * (workspace/configuration) gets it, null for one there is not. */
static void test_data_language_settings(void)
{
  Lsp json = { .kind = SERVER_JSON, .app = &app }, yaml = { .kind = SERVER_YAML, .app = &app };
  char *text = server_options(&json);
  JsonNode *node = json_from_string(text, NULL);

  g_assert_false(json_node_get_boolean(json_at(node, "provideFormatter")));
  g_assert_cmpstr(json_string_at(node, "handledSchemaProtocols.0"), ==, "file"); /* nothing downloaded */
  g_assert_null(json_at(node, "handledSchemaProtocols.1"));
  json_node_unref(node);
  g_free(text);
  text = server_settings(&json);
  node = json_from_string(text, NULL);
  g_assert_true(json_node_get_boolean(json_at(node, "json.validate.enable")));
  g_assert_false(json_node_get_boolean(json_at(node, "json.format.enable")));
  g_assert_false(json_node_get_boolean(json_at(node, "json.schemaDownload.enable")));
  json_node_unref(node);
  g_free(text);
  text = server_settings(&yaml);
  node = json_from_string(text, NULL);
  g_assert_true(json_node_get_boolean(json_at(node, "yaml.schemaStore.enable")));
  json_node_unref(node);
  g_free(text);

  char *dir = g_dir_make_tmp("t-code-json-XXXXXX", NULL), *big = g_build_filename(dir, "data.json", NULL);
  char *data = g_strnfill(MAX_JSON_BYTES + 1, ' ');
  g_assert_true(json_worth_serving("/p/settings.json"));
  g_assert_false(json_worth_serving("/p/build/compile_commands.json"));
  g_assert_false(json_worth_serving("/p/package-lock.json"));
  g_assert_false(json_worth_serving("/p/dist/app.min.json"));
  g_assert_true(g_file_set_contents(big, data, -1, NULL));
  g_assert_false(json_worth_serving(big));
  g_unlink(big);
  g_rmdir(dir);
  g_free(data);
  g_free(big);
  g_free(dir);

  Tab *tab = new_tab("/tmp/t-code-test/config.yaml", "a: 1\n");
  gtk_source_buffer_set_language(tab->buffer, NULL); /* new_tab's C would go to clangd */
  gint64 deadline = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
  lsp_open(tab);
  while (g_strcmp0(server_text(tab), "[{\"enable\":true},null]") != 0 && g_get_monotonic_time() < deadline)
    g_main_context_iteration(NULL, TRUE);
  g_assert_cmpstr(server_text(tab), ==, "[{\"enable\":true},null]");
  free_tab(tab);
}

static gboolean workspace_answered(void)
{
  return picker_test_label || (picker_test_message && strcmp(picker_test_message, "Searching workspace symbols…"));
}

static void wait_workspace_answer(void)
{
  gint64 deadline = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;

  while (!workspace_answered() && g_get_monotonic_time() < deadline) {
    g_main_context_iteration(NULL, FALSE);
    g_usleep(1000);
  }
  g_assert_true(workspace_answered());
}

static void test_workspace_symbols(void)
{
  const char *path = "/tmp/t-code-test/target.c";
  Tab *tab;

  g_mkdir_with_parents("/tmp/t-code-test", 0700);
  g_assert_true(g_file_set_contents(path, "\n    needleSymbol\n", -1, NULL));
  tab = new_tab("/tmp/t-code-test/workspace.c", "int main(void) {}\n");
  lsp_open(tab);
  g_assert_true(server_agrees(tab));
  go_to_workspace_symbol(tab);
  g_assert_nonnull(picker_test_query);
  g_assert_nonnull(picker_test_choose);

  picker_test_query(&app, "needle", 1);
  wait_workspace_answer();
  g_assert_cmpstr(picker_test_label, ==, "needleSymbol");
  g_assert_cmpstr(picker_test_detail, ==, "function in Box — target.c:2");
  picker_test_choose(&app, picker_test_payload);
  g_assert_cmpstr(opened_path, ==, path);
  g_assert_cmpint(opened_line, ==, 2);
  g_assert_cmpint(opened_column, ==, 5);

  picker_test_clear_results();
  picker_test_query(&app, "other", 2);
  wait_workspace_answer();
  g_assert_cmpstr(picker_test_message, ==, "No workspace symbols found");

  picker_test_clear_results();
  picker_test_query(&app, "fail", 3);
  wait_workspace_answer();
  g_assert_cmpstr(picker_test_message, ==, "Workspace symbol search failed");
  lsp_close(tab);
  free_tab(tab);
  g_remove(path);
}

/* The cursor resting on a name, with no selection, highlights the server's uses of it. */
static void test_document_highlight(void)
{
  Tab *tab = new_tab("/tmp/t-code-test/highlight.c", "x = 1;\ny = 2;\nx = 3;\n");
  GArray *lines = g_array_new(FALSE, FALSE, sizeof(int));
  GtkTextIter iter;
  gint64 end = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;

  lsp_open(tab);
  g_assert_true(server_agrees(tab));
  iter = iter_at(tab, 2, 0);
  gtk_text_buffer_place_cursor(GTK_TEXT_BUFFER(tab->buffer), &iter);
  while (lines->len == 0 && g_get_monotonic_time() < end) {
    g_main_context_iteration(NULL, FALSE);
    occurrences_lines(tab, lines);
  }
  g_assert_cmpint(lines->len, ==, 2);
  g_assert_cmpint(g_array_index(lines, int, 0), ==, 0);
  g_assert_cmpint(g_array_index(lines, int, 1), ==, 2);
  g_array_free(lines, TRUE);
  free_tab(tab);
}

/* clangd's inactive regions dim their lines, over the semantic colors; a new list replaces the old. */
static void test_inactive_regions(void)
{
  Tab *tab = new_tab("/tmp/t-code-test/inactive.c", "#if 0\nint a;\n#endif\nint b;\n");
  GtkTextTag *tag;
  GtkTextIter iter;
  JsonNode *regions =
      json_from_string("[{\"start\":{\"line\":1,\"character\":0},\"end\":{\"line\":1,\"character\":6}}]", NULL);
  JsonNode *none = json_from_string("[]", NULL);

  semantic_inactive(tab, regions);
  tag = gtk_text_tag_table_lookup(gtk_text_buffer_get_tag_table(GTK_TEXT_BUFFER(tab->buffer)), "tcode-inactive");
  g_assert_nonnull(tag);
  iter = iter_at(tab, 1, 2);
  g_assert_true(gtk_text_iter_has_tag(&iter, tag));
  iter = iter_at(tab, 3, 2);
  g_assert_false(gtk_text_iter_has_tag(&iter, tag));
  semantic_inactive(tab, none);
  iter = iter_at(tab, 1, 2);
  g_assert_false(gtk_text_iter_has_tag(&iter, tag));
  json_node_unref(none);
  json_node_unref(regions);
  free_tab(tab);
}

/* A cancelled request gets NULL, and its late answer goes nowhere. */
static void test_cancel(void)
{
  Tab *tab = new_tab("/tmp/t-code-test/cancel.c", "x\n");
  GCancellable *cancel = g_cancellable_new();
  Answer answer = { 0 };
  char *position;
  GtkTextIter iter;

  lsp_open(tab);
  g_assert_true(server_agrees(tab));
  iter = iter_at(tab, 0, 0);
  position = lsp_position_params(tab, &iter);
  char *params = g_strdup_printf("{%s}", position);
  lsp_request(tab, "textDocument/hover", params, cancel, on_answer, &answer);
  g_cancellable_cancel(cancel);
  while (!answer.called)
    g_main_context_iteration(NULL, TRUE);
  g_assert_null(answer.value);
  run_for(200); /* the server's answer comes, and is dropped */
  g_assert_cmpuint(g_hash_table_size(app.lsp->pending), ==, 0);
  g_object_unref(cancel);
  g_free(params);
  g_free(position);
  free_tab(tab);
}

/* A queued cancellation must not retain a freed LSP across shutdown. */
static void test_cancel_shutdown(void)
{
  Tab *tab = new_tab("/tmp/t-code-test/cancel-shutdown.c", "x\n");
  GCancellable *cancel = g_cancellable_new();
  Answer answer = { 0 };
  GtkTextIter iter;
  char *position, *params;

  lsp_open(tab);
  g_assert_true(server_agrees(tab));
  iter = iter_at(tab, 0, 0);
  position = lsp_position_params(tab, &iter);
  params = g_strdup_printf("{%s}", position);
  lsp_request(tab, "textDocument/hover", params, cancel, on_answer, &answer);
  g_cancellable_cancel(cancel); /* cancel_now is queued, but has not run */
  lsp_stop(&app);
  g_assert_true(answer.called);
  g_assert_null(answer.value);
  g_assert_null(app.lsps);
  while (g_main_context_iteration(NULL, FALSE)) {
  }

  g_object_unref(cancel);
  g_free(params);
  g_free(position);
  free_tab(tab);
}

/* An editor that goes takes its waiting requests with it: they get NULL at once. */
static void test_close_waiting(void)
{
  Tab *tab = new_tab("/tmp/t-code-test/close.c", "x\n");
  Answer answer = { 0 };
  GtkTextIter iter;
  char *position, *params;

  lsp_open(tab);
  g_assert_true(server_agrees(tab));
  iter = iter_at(tab, 0, 0);
  position = lsp_position_params(tab, &iter);
  params = g_strdup_printf("{%s}", position);
  lsp_request(tab, "textDocument/hover", params, NULL, on_answer, &answer);
  free_tab(tab);
  g_assert_true(answer.called);
  g_assert_null(answer.value);
  run_for(200);
  g_free(params);
  g_free(position);
}

/* Without editors the server stops (after IDLE_SECONDS, 1 here); the next one starts it again. */
static void test_idle_stop(void)
{
  Tab *tab = new_tab("/tmp/t-code-test/idle.c", "x\n");

  lsp_open(tab);
  g_assert_true(server_agrees(tab));
  free_tab(tab);
  g_assert_nonnull(app.lsp->ch);
  /* A timeout in seconds fires up to a second late (GLib rounds them to whole seconds). */
  for (gint64 until = g_get_monotonic_time() + 3 * G_USEC_PER_SEC; app.lsp->ch && g_get_monotonic_time() < until;)
    g_main_context_iteration(NULL, FALSE);
  g_assert_null(app.lsp->ch);

  tab = new_tab("/tmp/t-code-test/idle.c", "y\n");
  lsp_open(tab);
  g_assert_true(server_agrees(tab));
  free_tab(tab);
}

/* CMake files go to their server (neocmakelsp), which sends no diagnostics by itself: it is asked
 * for them (textDocument/diagnostic) once a change stops; the others are not asked. */
static void test_pull_diagnostics(void)
{
  Tab *tab = new_tab("/tmp/t-code-test/CMakeLists.txt", "project(X)\n");
  Tab *module = new_tab("/tmp/t-code-test/cmake/Find.cmake", "x\n");
  Tab *text = new_tab("/tmp/t-code-test/NotCMakeLists.txt", "x\n");
  Lsp cmake = { .kind = SERVER_CMAKE, .app = &app };
  GtkSourceLanguageManager *languages = gtk_source_language_manager_get_default();
  GtkTextIter end;
  char *options = server_options(&cmake);

  /* As t-code opens them (new_tab makes C files). */
  gtk_source_buffer_set_language(tab->buffer, gtk_source_language_manager_get_language(languages, "cmake"));
  gtk_source_buffer_set_language(module->buffer, gtk_source_language_manager_get_language(languages, "cmake"));
  gtk_source_buffer_set_language(text->buffer, NULL);
  g_assert_cmpint(server_kind_for_tab(tab), ==, SERVER_CMAKE);
  g_assert_cmpint(server_kind_for_tab(module), ==, SERVER_CMAKE);
  g_assert_cmpint(server_kind_for_tab(text), ==, SERVER_NONE);
  g_assert_nonnull(strstr(options, "\"scan_cmake_in_package\":false"));
  g_free(options);

  lsp_open(tab);
  g_assert_true(server_agrees(tab)); /* the first pull, after didOpen */
  g_assert_nonnull(json_at(doc_of(tab)->lsp->capabilities, "diagnosticProvider"));
  gtk_text_buffer_get_end_iter(GTK_TEXT_BUFFER(tab->buffer), &end);
  gtk_text_buffer_insert(GTK_TEXT_BUFFER(tab->buffer), &end, "add_executable(a main.c)\n", -1);
  g_assert_true(server_agrees(tab)); /* pulled again after the change */
  free_tab(text);
  free_tab(module);
  free_tab(tab);
}

/* window/showDocument: a file opens at the selection's start; a file to show "externally" and other
 * schemes are refused (web pages go to the browser, which a test does not start). */
static void test_show_document(void)
{
  JsonNode *file = json_from_string("{\"uri\":\"file:///tmp/t-code-test/shown.go\",\"selection\":{\"start\":"
                                    "{\"line\":4,\"character\":2},\"end\":{\"line\":4,\"character\":2}}}",
                                    NULL);
  JsonNode *outside = json_from_string("{\"uri\":\"file:///tmp/x.pdf\",\"external\":true}", NULL);
  JsonNode *other = json_from_string("{\"uri\":\"ftp://example.invalid/x\"}", NULL);

  g_assert_true(navigate_show_document(&app, file));
  g_assert_cmpstr(opened_path, ==, "/tmp/t-code-test/shown.go");
  g_assert_cmpint(opened_line, ==, 5);
  g_assert_cmpint(opened_column, ==, 3);
  g_assert_false(navigate_show_document(&app, outside));
  g_assert_false(navigate_show_document(&app, other));
  json_node_unref(other);
  json_node_unref(outside);
  json_node_unref(file);
}

/* The letter-spacing tag on the character at line:column, or 0. */
static int spacing_at(Tab *tab, int line, int column)
{
  GtkTextIter iter;
  GSList *tags;
  int spacing = 0;

  gtk_text_buffer_get_iter_at_line_offset(GTK_TEXT_BUFFER(tab->buffer), &iter, line, column);
  tags = gtk_text_iter_get_tags(&iter);
  for (GSList *t = tags; t; t = t->next) {
    gboolean set;
    int value;
    g_object_get(t->data, "letter-spacing-set", &set, "letter-spacing", &value, NULL);
    if (set)
      spacing = value / PANGO_SCALE;
  }
  g_slist_free(tags);
  return spacing;
}

/* Inlay hints come for the text and sit in gaps: on the space before or after them, or on the
 * character before (hidden, drawn again); an edit asks again; the setting takes them away. */
static void test_inlay_hints(void)
{
  Tab *tab = new_tab("/tmp/t-code-test/inlay.c", "int x = f(a, b);\n");
  gint64 until;
  char *text = NULL;

  app.settings.inlay_hints = TRUE;
  lsp_open(tab);
  g_assert_true(server_agrees(tab));
  until = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
  while (g_get_monotonic_time() < until && (!text || !*text)) {
    g_free(text);
    g_main_context_iteration(NULL, FALSE);
    text = inlay_text(tab);
  }
  g_assert_cmpstr(text, ==, "0:5 : int\n0:10 p: \n");
  g_free(text);
  g_assert_cmpint(spacing_at(tab, 0, 5), >, 0); /* " =": the space after ": int" */
  g_assert_cmpint(spacing_at(tab, 0, 9), >, 0); /* "(a": no space, the "(" holds it */
  g_assert_cmpint(spacing_at(tab, 0, 10), ==, 0);

  GtkTextIter end;
  gtk_text_buffer_get_end_iter(GTK_TEXT_BUFFER(tab->buffer), &end);
  gtk_text_buffer_insert(GTK_TEXT_BUFFER(tab->buffer), &end, "g(c);\n", -1);
  until = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
  text = NULL;
  while (g_get_monotonic_time() < until && (!text || !strstr(text, "1:2"))) {
    g_free(text);
    g_main_context_iteration(NULL, FALSE);
    text = inlay_text(tab);
  }
  g_assert_cmpstr(text, ==, "0:5 : int\n0:10 p: \n1:2 p: \n");
  g_free(text);

  Lsp go = { .kind = SERVER_GO, .app = &app }, ts = { .kind = SERVER_TYPESCRIPT, .app = &app };
  char *options = server_options(&go); /* gopls and TypeScript give hints only when asked to */
  g_assert_nonnull(strstr(options, "\"parameterNames\":true"));
  g_free(options);
  options = server_options(&ts);
  g_assert_nonnull(strstr(options, "includeInlayParameterNameHints"));
  g_free(options);

  app.settings.inlay_hints = FALSE;
  inlay_apply_setting(tab);
  text = inlay_text(tab);
  g_assert_cmpstr(text, ==, "");
  g_free(text);
  g_assert_cmpint(spacing_at(tab, 0, 9), ==, 0);
  free_tab(tab);
}

/* The symbols come as a tree; the breadcrumbs show the file and the symbols around the cursor; a
 * flat list (SymbolInformation) is nested by the ranges. */
static void test_symbols(void)
{
  Tab *tab = new_tab("/tmp/t-code-test/shapes.go", "class Shape\n  fn area\n  fn draw\nend\nfn main\n");
  GtkTextIter iter;
  GPtrArray *roots;
  char *text;

  gtk_source_buffer_set_language(
      tab->buffer, gtk_source_language_manager_get_language(gtk_source_language_manager_get_default(), "go"));
  app.settings.breadcrumbs = TRUE;
  lsp_open(tab);
  g_assert_true(server_agrees(tab));
  for (gint64 until = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
       !symbols_roots(tab) && g_get_monotonic_time() < until;)
    g_main_context_iteration(NULL, FALSE);
  roots = symbols_roots(tab);
  g_assert_nonnull(roots);
  g_assert_cmpuint(roots->len, ==, 2);
  g_assert_cmpstr(((Sym *)roots->pdata[0])->name, ==, "Shape");
  g_assert_cmpuint(((Sym *)roots->pdata[0])->children->len, ==, 2);
  g_assert_cmpstr(((Sym *)roots->pdata[1])->name, ==, "main");

  gtk_text_buffer_get_iter_at_line_offset(GTK_TEXT_BUFFER(tab->buffer), &iter, 2, 5);
  gtk_text_buffer_place_cursor(GTK_TEXT_BUFFER(tab->buffer), &iter);
  breadcrumbs_update(tab);
  text = breadcrumbs_text(tab);
  g_assert_true(g_str_has_suffix(text, "shapes.go › Shape › draw"));
  g_free(text);

  JsonNode *flat = json_from_string(
      "[{\"name\":\"inner\",\"kind\":12,\"location\":{\"uri\":\"file:///x\",\"range\":{\"start\":{\"line\":1,"
      "\"character\":2},\"end\":{\"line\":1,\"character\":9}}}},{\"name\":\"Outer\",\"kind\":5,\"location\":{"
      "\"uri\":\"file:///x\",\"range\":{\"start\":{\"line\":0,\"character\":0},\"end\":{\"line\":3,"
      "\"character\":3}}}}]",
      NULL);
  GPtrArray *nested = symbols_from_information(tab, json_node_get_array(flat), FALSE);
  g_assert_cmpuint(nested->len, ==, 1);
  g_assert_cmpstr(((Sym *)nested->pdata[0])->name, ==, "Outer");
  g_assert_cmpstr(((Sym *)((Sym *)nested->pdata[0])->children->pdata[0])->name, ==, "inner");
  g_ptr_array_unref(nested);
  json_node_unref(flat);
  free_tab(tab);
}

/* Waits until the hierarchy's text holds part (or 5 seconds pass); returns it. */
static char *hierarchy_waiting_for(const char *part)
{
  char *text = NULL;

  for (gint64 until = g_get_monotonic_time() + 5 * G_USEC_PER_SEC; g_get_monotonic_time() < until;) {
    g_free(text);
    text = hierarchy_text();
    if (strstr(text, part))
      break;
    g_main_context_iteration(NULL, FALSE);
  }
  return text;
}

/* Callers of the function at the cursor, their callers as their rows show; callees; a type's
 * supertypes; a server without a hierarchy says so. */
static void test_hierarchy(void)
{
  GtkWidget *panel = g_object_ref_sink(hierarchy_panel(&app));
  Tab *tab = new_tab("/tmp/t-code-test/hier.go", "main: run parse\nrun: parse\nparse:\n");
  Tab *c = new_tab("/tmp/t-code-test/hier.c", "int x;\n");
  GtkTextIter iter;
  char *text;

  gtk_source_buffer_set_language(
      tab->buffer, gtk_source_language_manager_get_language(gtk_source_language_manager_get_default(), "go"));
  picker_test_tab = tab; /* find_tab finds it */
  lsp_open(tab);
  g_assert_true(server_agrees(tab));
  gtk_text_buffer_get_iter_at_line(GTK_TEXT_BUFFER(tab->buffer), &iter, 2);
  gtk_text_buffer_place_cursor(GTK_TEXT_BUFFER(tab->buffer), &iter);

  hierarchy_show(tab, FALSE);
  text = hierarchy_waiting_for("run hier.go:2");
  g_assert_true(g_str_has_prefix(text, "[Callers of parse]\nparse hier.go:3\n  main hier.go:1\n  run hier.go:2\n"));
  g_free(text);

  hierarchy_set_up(FALSE);
  text = hierarchy_waiting_for("[Calls from parse]");
  g_assert_cmpstr(text, ==, "[Calls from parse]\nparse hier.go:3\n"); /* it calls nothing */
  g_free(text);

  gtk_text_buffer_get_iter_at_line(GTK_TEXT_BUFFER(tab->buffer), &iter, 1);
  gtk_text_buffer_place_cursor(GTK_TEXT_BUFFER(tab->buffer), &iter);
  hierarchy_set_up(TRUE);
  hierarchy_show(tab, TRUE);
  text = hierarchy_waiting_for("  parse hier.go:3");
  g_assert_true(g_str_has_prefix(text, "[Supertypes of run]\nrun hier.go:2\n  parse hier.go:3\n"));
  g_free(text);

  lsp_open(c); /* the C server here offers no hierarchy */
  g_assert_true(server_agrees(c));
  hierarchy_show(c, FALSE);
  text = hierarchy_text();
  g_assert_cmpstr(text, ==, "[]\n(The language server has no call hierarchy for this file.)\n");
  g_free(text);
  picker_test_tab = NULL;
  free_tab(c);
  free_tab(tab);
  g_object_unref(panel);
}

/* Feeds the server's $/progress for token (JSON: "a" or 7) to tab's server. */
static void send_progress(Tab *tab, const char *token, const char *value)
{
  char *body = g_strdup_printf("{\"method\":\"$/progress\",\"params\":{\"token\":%s,\"value\":%s}}", token, value);
  JsonNode *message = json_from_string(body, NULL);

  progress(doc_of(tab)->lsp, message);
  json_node_unref(message);
  g_free(body);
}

/* The status bar's progress follows each token's begin, reports and end, the one that spoke last
 * first; the server's name comes before it. */
static void test_progress(void)
{
  Tab *tab = new_tab("/tmp/t-code-test/progress.c", "x\n");
  char *text;

  lsp_open(tab);
  g_assert_true(server_agrees(tab));
  g_assert_null(lsp_progress(&app)); /* ready and idle */
  send_progress(tab, "\"a\"", "{\"kind\":\"begin\",\"title\":\"Indexing\",\"percentage\":0}");
  send_progress(tab, "\"a\"", "{\"kind\":\"report\",\"message\":\"3/10\",\"percentage\":30}");
  text = lsp_progress(&app);
  g_assert_cmpstr(text, ==, "exe: Indexing 30% (3/10)"); /* the fake server is /proc/self/exe */
  g_free(text);
  send_progress(tab, "7", "{\"kind\":\"begin\",\"title\":\"Loading\"}");
  text = lsp_progress(&app);
  g_assert_cmpstr(text, ==, "exe: Loading");
  g_free(text);
  send_progress(tab, "7", "{\"kind\":\"end\"}");
  text = lsp_progress(&app);
  g_assert_cmpstr(text, ==, "exe: Indexing 30% (3/10)"); /* the other one still runs */
  g_free(text);
  send_progress(tab, "\"a\"", "{\"kind\":\"end\"}");
  g_assert_null(lsp_progress(&app));
  send_progress(tab, "\"b\"", "{\"kind\":\"begin\",\"title\":\"Fetching\"}");
  server_stop(doc_of(tab)->lsp); /* its work ends with it, without an "end" */
  g_assert_null(lsp_progress(&app));
  free_tab(tab);
}

/* At most MAX_OPEN_DOCS files stay open in clangd at once; touching an evicted tab re-opens it. */
static void test_lru_pool(void)
{
  Tab *tabs[MAX_OPEN_DOCS + 2];

  for (int i = 0; i < MAX_OPEN_DOCS + 2; i++) {
    char *path = g_strdup_printf("/tmp/t-code-test/lru%d.c", i);
    char *text = g_strdup_printf("int x%d;\n", i);
    tabs[i] = new_tab(path, text);
    lsp_open(tabs[i]);
    g_assert_true(server_agrees(tabs[i]));
    g_free(text);
    g_free(path);
  }
  /* The oldest two were evicted with didClose; the newest MAX_OPEN_DOCS stay open. */
  g_assert_false(lsp_has(tabs[0]));
  g_assert_false(lsp_has(tabs[1]));
  for (int i = 2; i < MAX_OPEN_DOCS + 2; i++)
    g_assert_true(lsp_has(tabs[i]));

  /* Touching tabs[2] makes it newest; touching evicted tabs[0] re-opens it and evicts tabs[3]. */
  lsp_touch(tabs[2]);
  lsp_touch(tabs[0]);
  g_assert_true(lsp_has(tabs[0]));
  g_assert_true(server_agrees(tabs[0]));
  g_assert_true(lsp_has(tabs[2]));
  g_assert_false(lsp_has(tabs[3]));

  for (int i = 0; i < MAX_OPEN_DOCS + 2; i++)
    free_tab(tabs[i]);
}

/* A server that crashes starts again, and gets the files again. */
static void test_crash(void)
{
  Tab *tab = new_tab("/tmp/t-code-test/crash.c", "x\n");
  GtkTextIter start;

  lsp_stop(&app);
  lsp_open(tab);
  g_assert_true(server_agrees(tab));
  start = iter_at(tab, 0, 0);
  gtk_text_buffer_insert(GTK_TEXT_BUFFER(tab->buffer), &start, "CRASH ", -1);
  run_for(500);
  start = iter_at(tab, 0, 0);
  GtkTextIter end = iter_at(tab, 0, 6);
  gtk_text_buffer_delete(GTK_TEXT_BUFFER(tab->buffer), &start, &end);
  g_assert_true(server_agrees(tab)); /* the restarted server has "x" */
  g_assert_cmpint(app.lsp->crashes, ==, 1);
  free_tab(tab);
}

/* A file with text in a folder. */
static void put(const char *folder, const char *name, const char *text)
{
  char *path = g_build_filename(folder, name, NULL), *parent = g_path_get_dirname(path);

  g_mkdir_with_parents(parent, 0700);
  g_assert_true(g_file_set_contents(path, text, -1, NULL));
  g_free(parent);
  g_free(path);
}

/* The compilation database is looked for like clangd does, upwards from the file; a misspelled one
 * is found between the file and the project folder; the system headers under a sysroot. */
static void test_compile_db(void)
{
  char *root = g_dir_make_tmp("t-code-db-XXXXXX", NULL), *file = g_build_filename(root, "src", "main.c", NULL);
  char *found;

  put(root, "src/main.c", "int main(void) {}\n");
  g_assert_null(compile_db_find(file));
  g_assert_null(compile_db_misnamed(file, root));

  put(root, "compile-flags.txt", "-I/usr/include/glib-2.0\n");
  g_assert_null(compile_db_find(file));
  found = compile_db_misnamed(file, root);
  g_assert_true(g_str_has_suffix(found, "/compile-flags.txt"));
  g_free(found);

  put(root, "build/compile_commands.json", "[]\n");
  found = compile_db_find(file);
  g_assert_true(g_str_has_suffix(found, "/build/compile_commands.json"));
  g_free(found);
  put(root, "src/compile_flags.txt", "-Wall\n"); /* nearer to the file */
  found = compile_db_find(file);
  g_assert_true(g_str_has_suffix(found, "/src/compile_flags.txt"));
  g_free(found);

  g_assert_false(compile_db_system_headers(root, FALSE)); /* no C library there */
  put(root, "usr/include/stdio.h", "");
  g_assert_true(compile_db_system_headers(root, FALSE));
  g_assert_false(compile_db_system_headers(root, TRUE)); /* nor a C++ one */
  put(root, "usr/include/c++/14/vector", "");
  g_assert_true(compile_db_system_headers(root, TRUE));

  g_free(file);
  g_free(root); /* left in the temporary folder */
}

/* A header not found shows a hint: the C library's headers when they are not installed, else no
 * compile flags (naming the misspelled file); a compile_flags.txt hides it; a restart gives the
 * server the open files again. */
static void test_flags_hint(void)
{
  char *root = g_dir_make_tmp("t-code-hint-XXXXXX", NULL), *file = g_build_filename(root, "main.c", NULL);
  char *old_root = app.project.root, *hint;
  Tab *tab;

  const char *problem = NULL;

  put(root, "main.c", "");
  put(root, "compile-flags.txt", "-Iinclude\n");
  app.project.root = root;
  sysroot = root; /* no C library headers there */
  tab = new_tab(file, "#include <missing.h>\n");
  lsp_open(tab);
  g_assert_true(server_agrees(tab));
  hint = lsp_flags_hint(&app, &problem);
  g_assert_cmpstr(problem, ==, "No C headers");
  g_assert_nonnull(strstr(hint, "sudo apt install build-essential"));
  g_free(hint);

  put(root, "usr/include/stdio.h", "");
  hint = lsp_flags_hint(&app, &problem);
  g_assert_cmpstr(problem, ==, "No compile flags");
  g_assert_nonnull(strstr(hint, "\"compile-flags.txt\": rename it to compile_flags.txt"));
  g_free(hint);

  put(root, "compile_flags.txt", "-Iinclude\n");
  g_assert_null(lsp_flags_hint(&app, &problem));
  sysroot = "/";

  lsp_restart(&app);
  g_assert_false(lsp_has(tab));
  diagnostics_clear(tab);
  g_assert_true(server_agrees(tab)); /* the new server got the file */
  g_assert_true(lsp_has(tab));

  lsp_close(tab);
  free_tab(tab);
  app.project.root = old_root;
  g_free(file);
  g_free(root);
}

/* Opening and closing a file many times leaves nothing behind. */
static void test_churn(void)
{
  Tab *tab = new_tab("/tmp/t-code-test/churn.c", "int x;\n");
  size_t before;

  /* The buffer's undo history grows with every edit: that is not the client's. */
  gtk_text_buffer_set_enable_undo(GTK_TEXT_BUFFER(tab->buffer), FALSE);
  lsp_open(tab);
  g_assert_true(server_agrees(tab));
  for (int round = 0; round < 2; round++) {
    if (round == 1) {
      malloc_trim(0);
      before = mallinfo2().uordblks;
    }
    for (int i = 0; i < 200; i++) {
      GtkTextIter start, end;
      lsp_close(tab);
      /* The last answer is not the one to wait for: each round waits for the server, or
             * the messages not yet read would pile up and look like a leak. */
      diagnostics_clear(tab);
      lsp_open(tab);
      g_assert_true(server_agrees(tab));
      start = iter_at(tab, 0, 0);
      gtk_text_buffer_insert(GTK_TEXT_BUFFER(tab->buffer), &start, "a", -1);
      g_assert_true(server_agrees(tab));
      start = iter_at(tab, 0, 0);
      end = iter_at(tab, 0, 1);
      gtk_text_buffer_delete(GTK_TEXT_BUFFER(tab->buffer), &start, &end);
      g_assert_true(server_agrees(tab));
    }
  }
  malloc_trim(0);
  g_test_message("heap grew by %zd bytes", (ssize_t)(mallinfo2().uordblks - before));
  g_assert_cmpint((gssize)(mallinfo2().uordblks - before), <, 8 * 1024); /* a leak of 130 bytes a round is 26 KB */
  free_tab(tab);
}

/* Typing "(" (which closes itself) asks for the parameter hint from between the two brackets. */
static void test_signature_position(void)
{
  Tab *tab = new_tab("/tmp/t-code-test/signature.c", "f");
  GtkTextIter end;
  gint64 deadline;

  lsp_open(tab);
  g_assert_true(server_agrees(tab));
  gtk_text_buffer_get_end_iter(GTK_TEXT_BUFFER(tab->buffer), &end);
  gtk_text_buffer_place_cursor(GTK_TEXT_BUFFER(tab->buffer), &end);
  g_assert_true(on_typing_key(NULL, GDK_KEY_parenleft, 0, 0, tab));
  for (deadline = g_get_monotonic_time() + 3 * G_USEC_PER_SEC;
       !g_str_has_prefix(server_text(tab) ? server_text(tab) : "", "signature") && g_get_monotonic_time() < deadline;)
    g_main_context_iteration(NULL, FALSE);
  g_assert_cmpstr(server_text(tab), ==, "signature at 0:2"); /* f(|), not f()| */
  free_tab(tab);
}

/* Completion, hints and edits --------------------------------------------------------------- */

static JsonNode *json_of(const char *json)
{
  JsonParser *parser = json_parser_new();
  JsonNode *node;

  g_assert_true(json_parser_load_from_data(parser, json, -1, NULL));
  node = json_node_copy(json_parser_get_root(parser));
  g_object_unref(parser);
  return node;
}

/* clangd's items: the name without its "•", the parameters apart, the snippet; typing filters
 * them, best match first. */
static void test_completion_items(void)
{
  TcodeCompletion *provider = g_object_new(TCODE_TYPE_COMPLETION, NULL);
  JsonNode *answer = json_of(
      "{\"isIncomplete\":true,\"items\":["
      "{\"label\":\" print_all(int n)\",\"labelDetails\":{\"detail\":\"(int n)\"},\"detail\":\"void\","
      "\"kind\":3,\"sortText\":\"2\",\"filterText\":\"print_all\",\"insertTextFormat\":2,"
      "\"textEdit\":{\"newText\":\"print_all(${1:int n})\",\"range\":{\"start\":{\"line\":0,\"character\":0},"
      "\"end\":{\"line\":0,\"character\":2}}}},"
      "{\"label\":\"•printf\",\"kind\":3,\"sortText\":\"1\",\"filterText\":\"printf\",\"insertText\":\"printf\","
      "\"additionalTextEdits\":[{\"newText\":\"#include <stdio.h>\\n\",\"range\":{\"start\":{\"line\":0,"
      "\"character\":0},\"end\":{\"line\":0,\"character\":0}}}]},"
      "{\"label\":\"value\",\"kind\":6,\"sortText\":\"3\"}]}");
  TcodeProposal *first;

  take_answer(provider, answer, "pr");
  g_assert_true(provider->incomplete);
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(provider->shown)), ==, 2); /* not "value" */
  first = g_list_model_get_item(G_LIST_MODEL(provider->shown), 0);
  g_assert_cmpstr(first->label, ==, "printf"); /* the "•" (an #include comes with it) is gone */
  g_assert_nonnull(first->more);
  g_object_unref(first);

  refill(provider, "pra"); /* "print_all": p, r, then a */
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(provider->shown)), ==, 1);
  first = g_list_model_get_item(G_LIST_MODEL(provider->shown), 0);
  g_assert_cmpstr(first->label, ==, "print_all");
  g_assert_cmpstr(first->params, ==, "(int n)");
  g_assert_cmpstr(first->detail, ==, "void");
  g_assert_true(first->snippet);
  g_assert_cmpstr(first->text, ==, "print_all(${1:int n})");
  g_assert_nonnull(first->edit_start);
  g_object_unref(first);

  refill(provider, "");
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(provider->shown)), ==, 3);
  json_node_unref(answer);
  g_object_unref(provider);
}

/* Long signatures and types are cut, by characters, so the list stays narrow. */
static void test_completion_shorten(void)
{
  char *text = shorten("g_application_add_main_option(GApplication *application)", 32);

  g_assert_cmpstr(text, ==, "g_application_add_main_option(G…");
  g_assert_cmpint(g_utf8_strlen(text, -1), ==, 32);
  g_free(text);
  text = shorten("const GActionEntry[1]", 30);
  g_assert_cmpstr(text, ==, "const GActionEntry[1]");
  g_free(text);
  text = shorten("çöğüşı", 4);
  g_assert_cmpstr(text, ==, "çöğ…");
  g_free(text);
  g_assert_null(shorten(NULL, 4));
}

/* The argument being typed is bold; overloads are counted. */
static void test_signature_markup(void)
{
  JsonNode *offsets =
      json_of("{\"activeSignature\":1,\"activeParameter\":1,\"signatures\":["
              "{\"label\":\"f(int a)\",\"parameters\":[{\"label\":[2,7]}]},"
              "{\"label\":\"f(int a, char *b) -> int\",\"parameters\":[{\"label\":[2,7]},"
              "{\"label\":[9,16]}],\"documentation\":{\"kind\":\"plaintext\",\"value\":\"Does f.\"}}]}");
  JsonNode *names = json_of("{\"signatures\":[{\"label\":\"g(x, y)\",\"activeParameter\":0,"
                            "\"parameters\":[{\"label\":\"x\"},{\"label\":\"y\"}]}]}");
  JsonNode *none = json_of("{\"signatures\":[]}");
  char *markup = hint_markup(offsets);

  g_assert_nonnull(strstr(markup, "2/2"));
  char *want = g_strdup_printf("f(int a, <b><span foreground=\"%s\">char *b</span></b>) -&gt; int",
                               palette_hex(PAL_INFO)); /* the theme's */
  g_assert_nonnull(strstr(markup, want));
  g_free(want);
  g_assert_nonnull(strstr(markup, "Does f."));
  g_free(markup);
  markup = hint_markup(names);
  want = g_strdup_printf("g(<b><span foreground=\"%s\">x</span></b>, y)", palette_hex(PAL_INFO));
  g_assert_nonnull(strstr(markup, want));
  g_free(want);
  g_free(markup);
  g_assert_null(hint_markup(none));
  json_node_unref(none);
  json_node_unref(names);
  json_node_unref(offsets);
}

/* Edits land where the server meant them, whatever the earlier ones did; two at one place keep
 * their order. */
static void test_edits(void)
{
  const char *edits_json = "[{\"range\":{\"start\":{\"line\":0,\"character\":4},\"end\":{\"line\":0,\"character\":5}},"
                           "\"newText\":\"value\"},"
                           "{\"range\":{\"start\":{\"line\":1,\"character\":0},\"end\":{\"line\":1,\"character\":0}},"
                           "\"newText\":\"A\"},"
                           "{\"range\":{\"start\":{\"line\":1,\"character\":0},\"end\":{\"line\":1,\"character\":0}},"
                           "\"newText\":\"B\"},"
                           "{\"range\":{\"start\":{\"line\":1,\"character\":7},\"end\":{\"line\":1,\"character\":8}},"
                           "\"newText\":\"value\"}]";
  JsonNode *edits = json_of(edits_json);
  Tab *tab = new_tab("/tmp/t-code-test/edits.c", "int x;\nreturn x;\n");
  char *text = edits_apply_text("int x;\nreturn x;\n", edits, FALSE), *buffer_text;

  g_assert_cmpstr(text, ==, "int value;\nABreturn value;\n");
  edits_apply(tab, edits, lsp_utf16(tab));
  buffer_text = text_of(tab);
  g_assert_cmpstr(buffer_text, ==, text);
  g_free(buffer_text);
  g_free(text);
  free_tab(tab);
  json_node_unref(edits);
}

/* Positions after a non-ASCII character: bytes in UTF-8, units in UTF-16. */
static void test_edits_encodings(void)
{
  JsonNode *utf8 = json_of("[{\"range\":{\"start\":{\"line\":0,\"character\":4},\"end\":{\"line\":0,"
                           "\"character\":5}},\"newText\":\"Z\"}]");
  JsonNode *utf16 = json_of("[{\"range\":{\"start\":{\"line\":0,\"character\":3},\"end\":{\"line\":0,"
                            "\"character\":4}},\"newText\":\"Z\"}]");
  char *a = edits_apply_text("ğüx y", utf8, FALSE), *b = edits_apply_text("🙂x y", utf16, TRUE);
  Tab *tab = new_tab("/tmp/t-code-test/edits-utf8.py", "ğüx y");
  char *buffer_text;

  g_assert_cmpstr(a, ==, "ğüZ y");
  g_assert_cmpstr(b, ==, "🙂xZy");
  edits_apply(tab, utf8, FALSE); /* Workspace edits use the requesting server's encoding. */
  buffer_text = text_of(tab);
  g_assert_cmpstr(buffer_text, ==, "ğüZ y");
  g_free(buffer_text);
  free_tab(tab);
  g_free(b);
  g_free(a);
  json_node_unref(utf16);
  json_node_unref(utf8);
}

/* A server's column in a line with Turkish letters (two bytes each) and an emoji (two UTF-16
 * units): the editor's column counts characters. */
static void test_column_in(void)
{
  g_assert_cmpint(column_in("çağ x", 6, FALSE), ==, 5); /* "x": after 3 letters of 2 + 2 + 1 bytes and a space */
  g_assert_cmpint(column_in("çağ x", 4, TRUE), ==, 5);
  g_assert_cmpint(column_in("🙂y", 2, TRUE), ==, 2);
  g_assert_cmpint(column_in("🙂y", 4, FALSE), ==, 2);
  g_assert_cmpint(column_in("ab", 99, FALSE), ==, 3); /* past the end: the line's end */
}

/* Text typed right after a problem is not underlined with it (until the server says so). */
static void test_problem_end(void)
{
  Tab *tab = new_tab("/tmp/t-code-test/end.c", "abc\n");
  JsonNode *list = json_of("[{\"range\":{\"start\":{\"line\":0,\"character\":0},\"end\":{\"line\":0,"
                           "\"character\":3}},\"severity\":1,\"message\":\"m\"}]");
  GtkTextIter at;
  char *message;

  diagnostics_show(tab, list);
  gtk_text_buffer_get_iter_at_line_offset(GTK_TEXT_BUFFER(tab->buffer), &at, 0, 3);
  gtk_text_buffer_insert(GTK_TEXT_BUFFER(tab->buffer), &at, "def", -1);
  gtk_text_buffer_get_iter_at_line_offset(GTK_TEXT_BUFFER(tab->buffer), &at, 0, 5);
  message = diagnostics_at(tab, &at, FALSE);
  g_assert_null(message);
  gtk_text_buffer_get_iter_at_line_offset(GTK_TEXT_BUFFER(tab->buffer), &at, 0, 1);
  message = diagnostics_at(tab, &at, FALSE);
  g_assert_cmpstr(message, ==, "error: m");
  g_free(message);
  json_node_unref(list);
  free_tab(tab);
}

/* Runs the Problems panel's waiting rebuild. */
static void flush_problems(void)
{
  while (pv.update_id)
    g_main_context_iteration(NULL, TRUE);
}

/* The Problems panel lists every editor's diagnostics with their counts, a row opens its place, and
 * rebuilding the list again and again leaves nothing behind. */
static void test_problems_panel(void)
{
  GtkWidget *panel;
  Tab *tab = new_tab("/tmp/t-code-test/problems.c", "int a\nint b;\n");
  JsonNode *list = json_of("[{\"range\":{\"start\":{\"line\":0,\"character\":4},\"end\":{\"line\":0,"
                           "\"character\":5}},\"severity\":1,\"message\":\"expected ';'\"},"
                           "{\"range\":{\"start\":{\"line\":1,\"character\":4},\"end\":{\"line\":1,"
                           "\"character\":5}},\"severity\":2,\"message\":\"unused b\"}]");
  GtkListBoxRow *row;
  size_t before = 0;

  panel = g_object_ref_sink(problems_panel(&app));
  page_test_tabs[0] = tab;
  page_test_tab_count = 1;
  diagnostics_show(tab, list);
  g_assert_cmpuint(pv.update_id, !=, 0); /* not yet: a burst of files makes one rebuild */
  problems_update(&app);
  problems_update(&app);
  flush_problems();
  g_assert_cmpstr(gtk_label_get_text(pv.err_label), ==, "1 Errors");
  g_assert_cmpstr(gtk_label_get_text(pv.warn_label), ==, "1 Warnings");
  g_assert_cmpstr(gtk_label_get_text(pv.info_label), ==, "0 Notes");
  g_assert_nonnull(gtk_list_box_get_row_at_index(GTK_LIST_BOX(pv.list), 1));
  g_assert_null(gtk_list_box_get_row_at_index(GTK_LIST_BOX(pv.list), 2));
  g_assert_false(gtk_widget_get_visible(pv.empty_label));

  picker_test_reset();
  row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(pv.list), 1);
  g_signal_emit_by_name(pv.list, "row-activated", row);
  g_assert_cmpstr(opened_path, ==, "/tmp/t-code-test/problems.c");
  g_assert_cmpint(opened_line, ==, 2);
  g_assert_cmpint(opened_column, ==, 5);

  for (int round = 0; round < 2; round++) {
    if (round == 1) {
      malloc_trim(0);
      before = mallinfo2().uordblks;
    }
    for (int i = 0; i < 500; i++) {
      problems_update(&app);
      flush_problems();
    }
  }
  malloc_trim(0);
  g_test_message("heap grew by %zd bytes", (ssize_t)(mallinfo2().uordblks - before));
  g_assert_cmpint((gssize)(mallinfo2().uordblks - before), <, 8 * 1024); /* 500 leaked names are 16 KB */

  diagnostics_clear(tab);
  flush_problems();
  g_assert_cmpstr(gtk_label_get_text(pv.err_label), ==, "0 Errors");
  g_assert_null(gtk_list_box_get_row_at_index(GTK_LIST_BOX(pv.list), 0));
  g_assert_true(gtk_widget_get_visible(pv.empty_label));

  /* The window goes with a rebuild still waiting: it must not run on the freed widgets. */
  problems_update(&app);
  g_assert_cmpuint(pv.update_id, !=, 0);
  g_object_unref(panel);
  g_assert_cmpuint(pv.update_id, ==, 0);
  g_assert_null(pv.list);
  problems_update(&app); /* no panel: nothing waits */
  g_assert_cmpuint(pv.update_id, ==, 0);
  while (g_main_context_iteration(NULL, FALSE))
    ;
  page_test_tab_count = 0;
  json_node_unref(list);
  free_tab(tab);
}

/* A snippet GtkSourceView cannot take goes in as plain text: the defaults, no tab stops. */
static void test_snippet_text(void)
{
  char *text = snippet_text("f(${1:int x}, $2)$0");

  g_assert_cmpstr(text, ==, "f(int x, )");
  g_free(text);
  text = snippet_text("a\\$b ${10:c}");
  g_assert_cmpstr(text, ==, "a$b c");
  g_free(text);
}

/* Semantic tokens get their colors through the server's legend; a readonly variable is a
 * constant, a type without a color (comment) gets none. */
static void test_semantic_colors(void)
{
  Tab *tab = new_tab("/tmp/t-code-test/semantic.c", "int f; int v; int c; // x\n");
  JsonNode *tokens = json_of("{\"data\":[0,4,1,0,0, 0,7,1,1,0, 0,7,1,1,2, 0,4,4,2,0]}");
  GtkTextTagTable *table = gtk_text_buffer_get_tag_table(GTK_TEXT_BUFFER(tab->buffer));
  GtkTextIter at;

  lsp_open(tab);
  g_assert_true(server_agrees(tab));
  apply(tab, tokens);
  gtk_text_buffer_get_iter_at_offset(GTK_TEXT_BUFFER(tab->buffer), &at, 4); /* f */
  g_assert_true(gtk_text_iter_has_tag(&at, gtk_text_tag_table_lookup(table, "semantic-function")));
  gtk_text_buffer_get_iter_at_offset(GTK_TEXT_BUFFER(tab->buffer), &at, 11); /* v */
  g_assert_true(gtk_text_iter_has_tag(&at, gtk_text_tag_table_lookup(table, "semantic-variable")));
  gtk_text_buffer_get_iter_at_offset(GTK_TEXT_BUFFER(tab->buffer), &at, 18); /* c, readonly */
  g_assert_true(gtk_text_iter_has_tag(&at, gtk_text_tag_table_lookup(table, "semantic-constant")));
  gtk_text_buffer_get_iter_at_offset(GTK_TEXT_BUFFER(tab->buffer), &at, 22); /* the comment */
  GSList *tags = gtk_text_iter_get_tags(&at);
  for (GSList *each = tags; each; each = each->next) {
    char *name = NULL;
    g_object_get(each->data, "name", &name, NULL);
    g_assert_false(name && g_str_has_prefix(name, "semantic-"));
    g_free(name);
  }
  g_slist_free(tags);
  json_node_unref(tokens);
  free_tab(tab);
}

static void test_semantic_timeout_close(void)
{
  Tab *tab = new_tab("/tmp/t-code-test/semantic-timeout.c", "");

  semantic_update(tab);
  g_assert_nonnull(tab->view->semantic);
  g_assert_cmpuint(tab->view->semantic->update_id, !=, 0);
  free_tab(tab);
  run_for(UPDATE_MS + 50);
}

/* The color named "semantic-<color>" at offset, or NULL for none. */
static const char *semantic_color_at(Tab *tab, int offset)
{
  static char color[32];
  GtkTextIter at;
  GSList *tags;

  gtk_text_buffer_get_iter_at_offset(GTK_TEXT_BUFFER(tab->buffer), &at, offset);
  tags = gtk_text_iter_get_tags(&at);
  color[0] = '\0';
  for (GSList *each = tags; each; each = each->next) {
    char *name = NULL;
    g_object_get(each->data, "name", &name, NULL);
    if (name && g_str_has_prefix(name, "semantic-"))
      g_strlcpy(color, name + strlen("semantic-"), sizeof color);
    g_free(name);
  }
  g_slist_free(tags);
  return color[0] ? color : NULL;
}

/* full/delta: the edits go into the kept tokens and only their lines are colored again; an answer
 * for an older text is kept too, its lines colored with the next answer. */
static void test_semantic_delta(void)
{
  Tab *tab = new_tab("/tmp/t-code-test/delta.c", "int f;\nint v;\nint w;\n");
  Semantic *s;
  JsonNode *full = json_of("{\"resultId\":\"1\",\"data\":[0,4,1,0,0, 1,4,1,1,0, 1,4,1,1,0]}");
  JsonNode *delta, *nothing;
  GtkTextIter at;

  lsp_open(tab);
  g_assert_true(server_agrees(tab));
  s = tab->view->semantic;
  g_assert_nonnull(s);
  apply(tab, full);
  g_assert_cmpstr(s->result_id, ==, "1");
  g_assert_cmpuint(s->data->len, ==, 15);
  g_assert_cmpstr(semantic_color_at(tab, 11), ==, "variable"); /* v, a variable */

  /* v becomes a function: the server sends the second token's type only. */
  delta = json_of("{\"resultId\":\"2\",\"edits\":[{\"start\":8,\"deleteCount\":1,\"data\":[0]}]}");
  apply_delta(s, delta, TRUE);
  g_assert_cmpstr(s->result_id, ==, "2");
  g_assert_cmpstr(semantic_color_at(tab, 11), ==, "function");
  g_assert_cmpstr(semantic_color_at(tab, 4), ==, "function");  /* f: unchanged */
  g_assert_cmpstr(semantic_color_at(tab, 18), ==, "variable"); /* w: unchanged */
  json_node_unref(delta);

  /* A line typed above: the colors move with the text. The answer for it comes after more typing:
   * kept, and its lines colored with the next answer. */
  gtk_text_buffer_get_start_iter(GTK_TEXT_BUFFER(tab->buffer), &at);
  gtk_text_buffer_insert(GTK_TEXT_BUFFER(tab->buffer), &at, "int g;\n", -1);
  g_assert_cmpstr(semantic_color_at(tab, 7 + 4), ==, "function"); /* f, moved down a line */
  delta = json_of("{\"resultId\":\"3\",\"edits\":[{\"start\":0,\"deleteCount\":0,\"data\":[0,4,1,1,0]},"
                  "{\"start\":0,\"deleteCount\":1,\"data\":[1]}]}");
  apply_delta(s, delta, FALSE);
  g_assert_cmpuint(s->data->len, ==, 20);
  g_assert_null(semantic_color_at(tab, 4)); /* g: not colored yet */
  g_assert_nonnull(s->dirty_from);
  nothing = json_of("{\"resultId\":\"4\",\"edits\":[]}");
  apply_delta(s, nothing, TRUE);
  g_assert_cmpstr(semantic_color_at(tab, 4), ==, "variable");  /* g */
  g_assert_cmpstr(semantic_color_at(tab, 11), ==, "function"); /* f */
  g_assert_null(s->dirty_from);

  json_node_unref(nothing);
  json_node_unref(delta);
  json_node_unref(full);
  free_tab(tab); /* frees s */
}

/* Each file goes to its language's server (one per language), which hears its languageId; the
 * servers stop once their files close. */
static void test_language_routing(void)
{
  static const struct {
    const char *path, *source, *id; /* source: GtkSourceView's language */
    ServerKind kind;
  } files[] = {
    { "routing.cpp", "cpp", "cpp", SERVER_CLANGD },
    { "routing.py", "python", "python", SERVER_PYTHON },
    { "routing.go", "go", "go", SERVER_GO },
    { "routing.rs", "rust", "rust", SERVER_RUST },
    { "routing.ts", "typescript", "typescript", SERVER_TYPESCRIPT },
    { "routing.tsx", "typescript-jsx", "typescriptreact", SERVER_TYPESCRIPT },
    { "routing.mjs", "js", "javascript", SERVER_TYPESCRIPT },
    { "routing.jsx", "js", "javascriptreact", SERVER_TYPESCRIPT },
    { "routing.json", "json", "json", SERVER_JSON },
    { "routing.jsonc", "json", "jsonc", SERVER_JSON },
    { "routing.yml", "yaml", "yaml", SERVER_YAML },
    { "routing.md", "markdown", "markdown", SERVER_MARKDOWN },
  };
  GtkSourceLanguageManager *languages = gtk_source_language_manager_get_default();
  Tab *tabs[G_N_ELEMENTS(files)];
  Lsp *servers[G_N_ELEMENTS(files)];

  lsp_stop(&app); /* Earlier integration tests can leave idle servers in the shared App. */
  for (guint i = 0; i < G_N_ELEMENTS(files); i++) {
    char *path = g_build_filename("/tmp/t-code-test", files[i].path, NULL);
    tabs[i] = new_tab(path, "x\n");
    gtk_source_buffer_set_language(tabs[i]->buffer,
                                   gtk_source_language_manager_get_language(languages, files[i].source));
    g_assert_true(lsp_wants(tabs[i]));
    lsp_open(tabs[i]);
    g_free(path);
  }
  for (guint i = 0; i < G_N_ELEMENTS(files); i++) {
    g_assert_true(server_agrees(tabs[i]));
    servers[i] = doc_of(tabs[i])->lsp;
    g_assert_cmpint(servers[i]->kind, ==, files[i].kind);
    g_assert_cmpstr(language_id(doc_of(tabs[i])), ==, files[i].id);
  }
  g_assert_true(servers[4] == servers[7]); /* TypeScript's serves JavaScript too */
  g_assert_true(servers[8] == servers[9]); /* .json and .jsonc */
  g_assert_true(servers[1] != servers[2] && servers[2] != servers[3] && servers[3] != servers[4]);
  g_assert_true(servers[4] != servers[8] && servers[8] != servers[10] && servers[10] != servers[11]);
  for (guint i = 0; i < G_N_ELEMENTS(files); i++)
    lsp_close(tabs[i]);
  run_for(2600);
  for (guint i = 0; i < G_N_ELEMENTS(files); i++)
    g_assert_null(servers[i]->ch);
  lsp_stop(&app);
  g_assert_null(app.lsps);
  g_assert_null(app.lsp);
  for (guint i = 0; i < G_N_ELEMENTS(files); i++)
    free_tab(tabs[i]);
}

/* The folder a server process runs in, from /proc. */
static char *server_cwd(Lsp *l)
{
  char *link = g_strdup_printf("/proc/%s/cwd", g_subprocess_get_identifier(l->process));
  char *cwd = g_file_read_link(link, NULL);

  g_free(link);
  return cwd;
}

/* A project switch stops every server (lsp_stop); the next file starts one in the new folder. */
static void test_project_switch(void)
{
  char *first = g_dir_make_tmp("t-code-first-XXXXXX", NULL), *second = g_dir_make_tmp("t-code-second-XXXXXX", NULL);
  char *old_root = app.project.root, *cwd;
  Tab *tab;
  Lsp *l;

  lsp_stop(&app);
  app.project.root = first;
  tab = new_tab("/tmp/t-code-test/switch.c", "int a;\n");
  lsp_open(tab);
  g_assert_true(server_agrees(tab));
  l = doc_of(tab)->lsp;
  g_assert_cmpstr(l->workspace, ==, first);
  cwd = server_cwd(l);
  g_assert_cmpstr(cwd, ==, first);
  g_free(cwd);

  lsp_stop(&app); /* what switch_project does before loading the new folder */
  g_assert_false(lsp_has(tab));
  app.project.root = second;
  diagnostics_clear(tab);
  lsp_open(tab);
  g_assert_true(server_agrees(tab));
  l = doc_of(tab)->lsp;
  g_assert_cmpuint(app.lsps->len, ==, 1);
  g_assert_cmpstr(l->workspace, ==, second);
  cwd = server_cwd(l);
  g_assert_cmpstr(cwd, ==, second);
  g_free(cwd);

  lsp_close(tab);
  free_tab(tab);
  lsp_stop(&app);
  app.project.root = old_root;
  g_rmdir(first);
  g_rmdir(second);
  g_free(first);
  g_free(second);
}

int main(int argc, char **argv)
{
  if (argc > 1 && strcmp(argv[1], "--fake-server") == 0)
    return fake_server();
  g_test_init(&argc, &argv, NULL);
  if (!gtk_init_check()) {
    g_test_skip("no display");
    return g_test_run();
  }
  gtk_source_init();
  app.project.root = g_strdup("/tmp");
  app.settings.tab_size = 4;
  app.settings.font_scale = 1.0;
  app.paths.styles = g_canonicalize_filename("styles", NULL);
  g_test_add_func("/lsp/sync-utf8", test_sync_utf8);
  g_test_add_func("/lsp/sync-utf16", test_sync_utf16);
  g_test_add_func("/lsp/reload", test_reload);
  g_test_add_func("/lsp/hover", test_hover);
  g_test_add_func("/lsp/hover-fixes", test_hover_fixes);
  g_test_add_func("/lsp/toolchain-options", test_toolchain_options);
  g_test_add_func("/lsp/data-language-settings", test_data_language_settings);
  g_test_add_func("/lsp/document-highlight", test_document_highlight);
  g_test_add_func("/lsp/inactive-regions", test_inactive_regions);
  g_test_add_func("/lsp/cancel", test_cancel);
  g_test_add_func("/lsp/close-waiting", test_close_waiting);
  g_test_add_func("/lsp/idle-stop", test_idle_stop);
  g_test_add_func("/lsp/progress", test_progress);
  g_test_add_func("/lsp/pull-diagnostics", test_pull_diagnostics);
  g_test_add_func("/lsp/show-document", test_show_document);
  g_test_add_func("/lsp/inlay-hints", test_inlay_hints);
  g_test_add_func("/lsp/symbols", test_symbols);
  g_test_add_func("/lsp/hierarchy", test_hierarchy);
  g_test_add_func("/lsp/lru-pool", test_lru_pool);
  g_test_add_func("/lsp/crash", test_crash);
  g_test_add_func("/lsp/churn", test_churn);
  g_test_add_func("/lsp/compile-db", test_compile_db);
  g_test_add_func("/lsp/flags-hint", test_flags_hint);
  g_test_add_func("/lsp/signature-position", test_signature_position);
  g_test_add_func("/lsp/column-in", test_column_in);
  g_test_add_func("/lsp/snippet-text", test_snippet_text);
  g_test_add_func("/lsp/semantic-colors", test_semantic_colors);
  g_test_add_func("/lsp/semantic-timeout-close", test_semantic_timeout_close);
  g_test_add_func("/lsp/semantic-delta", test_semantic_delta);
  g_test_add_func("/lsp/problem-end", test_problem_end);
  g_test_add_func("/lsp/problems-panel", test_problems_panel);
  g_test_add_func("/lsp/completion-items", test_completion_items);
  g_test_add_func("/lsp/completion-shorten", test_completion_shorten);
  g_test_add_func("/lsp/signature-markup", test_signature_markup);
  g_test_add_func("/lsp/edits", test_edits);
  g_test_add_func("/lsp/edits-encodings", test_edits_encodings);
  g_test_add_func("/lsp/workspace-symbols", test_workspace_symbols);
  g_test_add_func("/lsp/language-routing", test_language_routing);
  g_test_add_func("/lsp/cancel-shutdown", test_cancel_shutdown);
  g_test_add_func("/lsp/project-switch", test_project_switch);
  return g_test_run();
}
