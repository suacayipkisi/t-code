/*
 * The Memory View (see memory.h). The expression is evaluated in the shown frame (context "watch"):
 * its memoryReference (a pointer's target, an array's start), else its value when that is an address,
 * else the address of it ("&(n)"). readMemory then gives COUNT bytes there (base64); the dump goes
 * to a file in t->paths.temp opened read-only, like the Disassembly View's.
 */
#include "debug/memory.h"

#include "debug/dap.h"
#include "debug/debugger.h"
#include "debug/view/debugview.h"
#include "editor/editor.h"
#include "editor/tabs.h"
#include "lsp/json.h"
#include "popups/picker.h"

#include <string.h>

#define FILE_NAME "Memory"
#define COUNT 256      /* bytes shown */
#define PER_LINE 8     /* bytes a line: fits an editor beside the source */
#define MAX_CHANGES 24 /* changed bytes listed under the dump */

static struct {
  char *path;
  char *expression; /* what the user asked for */
  guint64 address;  /* where it pointed; 0: not yet found */
  GBytes *last;     /* the bytes read there last, to tell the changes */
  guint generation; /* answers for an older pause are dropped */
} mem;

static const char *view_path(App *t)
{
  if (!mem.path)
    mem.path = g_build_filename(t->paths.temp, FILE_NAME, NULL);
  return mem.path;
}

static Tab *view_tab(App *t)
{
  return mem.path ? find_tab(t, TAB_EDITOR, mem.path) : NULL;
}

char *memory_dump_text(const char *expression, guint64 address, const guchar *bytes, gsize count, const guchar *changed)
{
  GString *text = g_string_new("");
  int listed = 0;

  g_string_append_printf(text,
                         "%s at 0x%" G_GINT64_MODIFIER "x: %" G_GSIZE_FORMAT " bytes, read again at each pause\n\n",
                         expression, address, count);
  for (gsize at = 0; at < count; at += PER_LINE) {
    g_string_append_printf(text, "0x%012" G_GINT64_MODIFIER "x  ", address + at);
    for (gsize i = at; i < at + PER_LINE; i++) {
      if (i < count)
        g_string_append_printf(text, "%02x ", bytes[i]);
      else
        g_string_append(text, "   ");
      if (i == at + PER_LINE / 2 - 1)
        g_string_append_c(text, ' ');
    }
    g_string_append_c(text, ' ');
    for (gsize i = at; i < at + PER_LINE && i < count; i++)
      g_string_append_c(text, bytes[i] >= 0x20 && bytes[i] < 0x7f ? (char)bytes[i] : '.');
    g_string_append_c(text, '\n');
  }
  for (gsize i = 0; changed && i < count; i++) {
    if (changed[i] == bytes[i])
      continue;
    if (listed == 0)
      g_string_append(text, "\nChanged since the last pause:\n");
    if (++listed > MAX_CHANGES) {
      g_string_append(text, "  ...\n");
      break;
    }
    g_string_append_printf(text, "  0x%012" G_GINT64_MODIFIER "x  %02x -> %02x\n", address + i, changed[i], bytes[i]);
  }
  return g_string_free(text, FALSE);
}

/* The view shows text (opened, when open). */
static void show_text(App *t, const char *text, gboolean open)
{
  char *dir = g_path_get_dirname(view_path(t));
  Tab *tab;

  g_mkdir_with_parents(dir, 0700);
  g_file_set_contents(view_path(t), text, -1, NULL);
  if ((tab = view_tab(t))) {
    editor_reload(tab);
    if (open)
      select_tab(tab);
  } else if (open) {
    side_group(t);
    open_editor(t, view_path(t), 1, 1, TRUE);
  }
  g_free(dir);
}

typedef struct {
  App *app;
  guint generation;
  gboolean open;
  gboolean address_of; /* the second try: "&(expression)" */
} Asked;

static void on_memory(gpointer data, JsonNode *body, const char *error)
{
  Asked *a = data;
  const char *encoded = json_string_at(body, "data");
  guchar *bytes = NULL;
  gsize count = 0;
  char *text;

  if (a->generation != mem.generation)
    return;
  if (encoded)
    bytes = g_base64_decode(encoded, &count);
  if (error || !bytes || count == 0) {
    text = g_strdup_printf("%s at 0x%" G_GINT64_MODIFIER "x: this memory cannot be read (%s).\n\nIt is not the "
                           "program's: a NULL or wild pointer, or memory already given back.\n",
                           mem.expression, mem.address, error ? error : "no bytes");
    g_clear_pointer(&mem.last, g_bytes_unref);
  } else {
    gsize last_size = 0;
    const guchar *last = mem.last ? g_bytes_get_data(mem.last, &last_size) : NULL;
    text = memory_dump_text(mem.expression, mem.address, bytes, count, last && last_size == count ? last : NULL);
    if (mem.last)
      g_bytes_unref(mem.last);
    mem.last = g_bytes_new(bytes, count);
  }
  show_text(a->app, text, a->open);
  g_free(text);
  g_free(bytes);
}

/* Reads the bytes at mem.address. */
static void read_memory(App *t, gboolean open)
{
  Dap *dap = debug_dap(t);
  Asked *a;
  char *args;

  if (!dap || !mem.address)
    return;
  a = g_new0(Asked, 1);
  *a = (Asked){ t, ++mem.generation, open, FALSE };
  args = g_strdup_printf("{\"memoryReference\":\"0x%" G_GINT64_MODIFIER "x\",\"count\":%d}", mem.address, COUNT);
  dap_request_full(dap, "readMemory", args, on_memory, a, g_free);
  g_free(args);
}

static void evaluate(App *t, const char *expression, gboolean open, gboolean address_of);

guint64 memory_value_address(const char *value)
{
  const char *at = value && !strchr(value, '{') ? strstr(value, "0x") : NULL;

  return at ? g_ascii_strtoull(at, NULL, 16) : 0;
}

static void on_evaluated(gpointer data, JsonNode *body, const char *error)
{
  Asked *a = data;
  const char *reference = json_string_at(body, "memoryReference"), *result = json_string_at(body, "result");
  guint64 address = reference ? g_ascii_strtoull(reference, NULL, 0) : 0;

  if (a->generation != mem.generation)
    return;
  if (!address && !error)
    address = memory_value_address(result); /* a pointer's value; not a struct's members */
  if (!address && !a->address_of) {         /* a plain variable: where it is */
    evaluate(a->app, mem.expression, a->open, TRUE);
    return;
  }
  if (!address) {
    char *text = g_strdup_printf("%s: no address (%s).\n\nName a pointer (p), an array (arr), a variable (n) or an "
                                 "address (0x7ffd1230).\n",
                                 mem.expression,
                                 error    ? error
                                 : result ? result
                                          : "?");
    show_text(a->app, text, a->open);
    g_free(text);
    return;
  }
  mem.address = address;
  g_clear_pointer(&mem.last, g_bytes_unref);
  read_memory(a->app, a->open);
}

/* Evaluates expression (address_of: "&(expression)") in the shown frame for its address. */
static void evaluate(App *t, const char *expression, gboolean open, gboolean address_of)
{
  gint64 frame = debug_view_frame_id();
  Dap *dap = debug_dap(t);

  if (!dap) /* the session ended while the first answer was on its way */
    return;
  char *what = address_of ? g_strdup_printf("&(%s)", expression) : g_strdup(expression), *quoted = json_quote(what);
  Asked *a = g_new0(Asked, 1);
  char *args =
      frame >= 0
          ? g_strdup_printf("{\"expression\":%s,\"frameId\":%" G_GINT64_FORMAT ",\"context\":\"watch\"}", quoted, frame)
          : g_strdup_printf("{\"expression\":%s,\"context\":\"watch\"}", quoted);

  *a = (Asked){ t, ++mem.generation, open, address_of };
  dap_request_full(dap, "evaluate", args, on_evaluated, a, g_free);
  g_free(args);
  g_free(quoted);
  g_free(what);
}

void memory_show(App *t, const char *expression)
{
  if (!debug_dap(t) || debug_view_frame_id() < 0) {
    debug_say(t, "Pause the program first: the Memory View shows the paused program's memory.");
    return;
  }
  g_set_str(&mem.expression, expression);
  mem.address = 0;
  evaluate(t, expression, TRUE, FALSE);
}

static void expression_query(App *t, const char *text, guint generation)
{
  if (!*text) {
    picker_message(t, generation, "Type a pointer (p), an array (arr), a variable (n) or an address (0x7ffd1230)");
    return;
  }
  char *label = g_strdup_printf("Show the memory at %s", text);
  picker_add(t, generation, label, "256 bytes, as hex and as characters", text);
  g_free(label);
}

static void expression_choose(App *t, const char *expression)
{
  memory_show(t, expression);
}

void memory_open(App *t)
{
  Tab *tab;

  if ((tab = view_tab(t)) && debug_view_frame_id() < 0) { /* not paused: just show it */
    select_tab(tab);
    return;
  }
  if (!debug_dap(t) || debug_view_frame_id() < 0) {
    debug_say(t, "Pause the program first: the Memory View shows the paused program's memory.");
    return;
  }
  picker_open(t, "Show the memory at… (a pointer, an array, a variable, an address)", expression_query,
              expression_choose);
}

void memory_update(App *t)
{
  if (view_tab(t) && mem.address)
    read_memory(t, FALSE);
  else
    mem.generation++;
}

void memory_ended(App *t G_GNUC_UNUSED)
{
  mem.generation++;
  mem.address = 0;
  g_clear_pointer(&mem.last, g_bytes_unref);
}
