/*
 * Reading and writing JSON messages (see json.h).
 */
#include "lsp/json.h"

#include <string.h>

JsonNode *json_at(JsonNode *node, const char *path)
{
  /* Part by part without splitting the path: this runs a few times for each of a big answer's
   * thousands of items. */
  for (const char *part = path; node && part;) {
    const char *dot = strchr(part, '.');
    size_t length = dot ? (size_t)(dot - part) : strlen(part);
    char buffer[64], *name = length < sizeof buffer ? buffer : g_malloc(length + 1); /* a long one: a server's */
    memcpy(name, part, length);
    name[length] = '\0';
    if (JSON_NODE_HOLDS_OBJECT(node)) {
      JsonObject *object = json_node_get_object(node);
      node = json_object_get_member(object, name); /* NULL when it has none */
    } else if (JSON_NODE_HOLDS_ARRAY(node) && g_ascii_isdigit(name[0])) {
      JsonArray *array = json_node_get_array(node);
      guint index = (guint)g_ascii_strtoull(name, NULL, 10);
      node = index < json_array_get_length(array) ? json_array_get_element(array, index) : NULL;
    } else {
      node = NULL;
    }
    if (name != buffer)
      g_free(name);
    part = dot ? dot + 1 : NULL;
  }
  return node && !JSON_NODE_HOLDS_NULL(node) ? node : NULL;
}

const char *json_string_at(JsonNode *node, const char *path)
{
  node = json_at(node, path);
  return node && json_node_get_value_type(node) == G_TYPE_STRING ? json_node_get_string(node) : NULL;
}

gint64 json_int_at(JsonNode *node, const char *path, gint64 fallback)
{
  node = json_at(node, path);
  return node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) != G_TYPE_STRING
             ? json_node_get_int(node)
             : fallback;
}

char *json_quote(const char *text)
{
  JsonNode *node = json_node_init_string(json_node_alloc(), text);
  char *quoted = json_to_string(node, FALSE);

  json_node_unref(node);
  return quoted;
}
