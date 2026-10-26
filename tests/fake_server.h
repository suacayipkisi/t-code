/*
 * The language server test_lsp.c runs: itself with --fake-server, reading LSP messages on stdin and
 * answering on stdout. It keeps its own copy of each file, applying every change it is sent, and
 * answers each change with one diagnostic whose message is the whole text as it has it (at the
 * first "X" in it). From that text it also answers hover, quick fixes, signature help, inlay hints,
 * highlights, symbols, workspace symbols, call and type hierarchy, and pulled diagnostics.
 * Included by test_lsp.c only, after editor_page.h (the JSON helpers).
 */
#pragma once

#include <stdio.h>
#include <string.h>

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
