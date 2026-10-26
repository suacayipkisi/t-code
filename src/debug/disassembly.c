/*
 * The Disassembly View (see disassembly.h). gdb's "disassemble /s $pc" in the shown frame (evaluate,
 * context "repl") gives the whole function with its source lines; that text, without gdb's first and
 * last lines and its "=>", goes to a file in t->paths.temp opened read-only. Each line's address is
 * kept, for the instruction breakpoints (setInstructionBreakpoints, all of them each time).
 */
#include "debug/disassembly.h"

#include "debug/dap.h"
#include "debug/debugger.h"
#include "debug/view/debugview.h"
#include "editor/gutter/breakpoints.h"
#include "editor/editor.h"
#include "editor/tabs.h"
#include "lsp/json.h"

#include <stdlib.h>
#include <string.h>

#define FILE_NAME "Disassembly"

static struct {
  GArray *addresses;   /* guint64 per line of the view (0-based), 0 where there is no instruction */
  GArray *breakpoints; /* guint64: the instructions with a breakpoint */
  guint generation;    /* answers for an older pause are dropped */
} dis;

/* The view's file. */
static const char *view_path(App *t)
{
  if (!t->debug.disassembly)
    t->debug.disassembly = g_build_filename(t->paths.temp, FILE_NAME, NULL);
  return t->debug.disassembly;
}

static Tab *view_tab(App *t)
{
  return t->debug.disassembly ? find_tab(t, TAB_EDITOR, t->debug.disassembly) : NULL;
}

gboolean disassembly_is(App *t, Tab *tab)
{
  return tab && t->debug.disassembly && g_strcmp0(tab->path, t->debug.disassembly) == 0;
}

gboolean disassembly_shown(App *t)
{
  return disassembly_is(t, current_tab(t));
}

/* line with its tabs as spaces, to stops every 8 columns as gdb meant them (the editor's tab width is
 * the project's). */
static void append_expanded(GString *text, const char *line)
{
  int column = 0;

  for (const char *c = line; *c; c++) {
    if (*c == '\t') {
      do
        g_string_append_c(text, ' ');
      while (++column % 8);
    } else {
      g_string_append_c(text, *c);
      if (((guchar)*c & 0xc0) != 0x80) /* a character's first byte */
        column++;
    }
  }
}

/* gdb's text, made the view's: its lines' addresses into dis.addresses, the next instruction's line
 * (1-based) into *next. */
static char *view_text(const char *dump, int *next)
{
  char **lines = g_strsplit(dump, "\n", -1);
  GString *text = g_string_new("");
  int n = 0;

  g_array_set_size(dis.addresses, 0);
  *next = 0;
  for (int i = 0; lines[i]; i++) {
    char *line = lines[i];
    guint64 address = 0;
    if (g_str_has_prefix(line, "End of assembler dump") || (i == 0 && g_str_has_prefix(line, "Dump of")) ||
        (!lines[i + 1] && !*line))
      continue;
    if (g_str_has_prefix(line, "=> ")) {
      *next = n + 1;
      line[0] = line[1] = ' ';
    }
    if (g_str_has_prefix(line, "   0x")) {
      address = g_ascii_strtoull(line + 3, NULL, 16);
      char *zeros = line + 5, *digits = zeros; /* 0x0000000000401126 -> 0x401126 */
      while (*digits == '0' && g_ascii_isxdigit(digits[1]))
        digits++;
      memmove(zeros, digits, strlen(digits) + 1);
    }
    g_array_append_val(dis.addresses, address);
    if (*line == '/' && g_str_has_suffix(line, ":")) { /* the source file: its name is enough */
      char *name = g_path_get_basename(line);
      g_string_append_printf(text, "%s\n", name);
      g_free(name);
    } else {
      append_expanded(text, line);
      g_string_append_c(text, '\n');
    }
    n++;
  }
  g_strfreev(lines);
  return g_string_free(text, FALSE);
}

typedef struct {
  App *app;
  guint generation;
  gboolean open; /* open the tab (the command), not just update it */
} Asked;

static void on_dump(gpointer data, JsonNode *body, const char *error)
{
  Asked *a = data;
  App *t = a->app;
  const char *dump = json_string_at(body, "result");
  char *text, *dir;
  int next;
  Tab *tab;

  if (a->generation != dis.generation)
    return;
  if (error || !dump) {
    if (a->open) {
      char *line = g_strdup_printf("No disassembly: %s", error ? error : "gdb gave none");
      debug_say(t, line);
      g_free(line);
    }
    return;
  }
  text = view_text(dump, &next);
  dir = g_path_get_dirname(view_path(t));
  g_mkdir_with_parents(dir, 0700);
  g_file_set_contents(view_path(t), text, -1, NULL);
  breakpoints_show_instruction(t, next);
  tab = view_tab(t);
  if (tab) {
    editor_reload(tab);
    editor_goto(tab, MAX(next, 1), 1);
  } else if (a->open) {
    set_split(t, TRUE);
    t->editors.active = 1;
    open_editor(t, view_path(t), MAX(next, 1), 1, TRUE);
  }
  g_free(dir);
  g_free(text);
}

/* Asks gdb for the shown frame's function. */
static void ask(App *t, gboolean open)
{
  gint64 frame = debug_view_frame_id();
  Dap *dap = debug_dap(t);
  Asked *a;
  char *args;

  if (frame < 0 || !dap) {
    if (open)
      debug_say(t, "Pause the program first: the Disassembly View shows the paused function's instructions.");
    return;
  }
  a = g_new0(Asked, 1);
  *a = (Asked){ t, ++dis.generation, open };
  args = g_strdup_printf(
      "{\"expression\":\"disassemble /s $pc\",\"frameId\":%" G_GINT64_FORMAT ",\"context\":\"repl\"}", frame);
  dap_request_full(dap, "evaluate", args, on_dump, a, g_free);
  g_free(args);
}

static void ensure_arrays(void)
{
  if (!dis.addresses) {
    dis.addresses = g_array_new(FALSE, TRUE, sizeof(guint64));
    dis.breakpoints = g_array_new(FALSE, TRUE, sizeof(guint64));
  }
}

void disassembly_open(App *t)
{
  Tab *tab;

  ensure_arrays();
  if ((tab = view_tab(t)) && debug_view_frame_id() < 0) { /* not paused: just show it */
    select_tab(tab);
    return;
  }
  ask(t, TRUE);
}

void disassembly_update(App *t)
{
  ensure_arrays();
  if (view_tab(t))
    ask(t, FALSE);
  else
    dis.generation++;
}

void disassembly_ended(App *t)
{
  ensure_arrays();
  dis.generation++;
  g_array_set_size(dis.breakpoints, 0);
  breakpoints_show_instruction(t, 0);
}

/* Instruction breakpoints -------------------------------------------------------------------- */

static int breakpoint_index(guint64 address)
{
  for (guint i = 0; dis.breakpoints && i < dis.breakpoints->len; i++)
    if (g_array_index(dis.breakpoints, guint64, i) == address)
      return (int)i;
  return -1;
}

static guint64 address_at(int line)
{
  return dis.addresses && line >= 0 && (guint)line < dis.addresses->len ? g_array_index(dis.addresses, guint64, line)
                                                                        : 0;
}

gboolean disassembly_breakpoint_at(App *t G_GNUC_UNUSED, int line)
{
  guint64 address = address_at(line);

  return address && breakpoint_index(address) >= 0;
}

void disassembly_toggle_breakpoint(App *t, int line)
{
  guint64 address = address_at(line);
  Dap *dap = debug_dap(t);
  GString *args;
  int index;
  Tab *tab;

  if (!address || !dap) /* not an instruction's line, or the addresses are another run's */
    return;
  index = breakpoint_index(address);
  if (index >= 0)
    g_array_remove_index(dis.breakpoints, index);
  else
    g_array_append_val(dis.breakpoints, address);
  args = g_string_new("{\"breakpoints\":[");
  for (guint i = 0; i < dis.breakpoints->len; i++)
    g_string_append_printf(args, "%s{\"instructionReference\":\"0x%" G_GINT64_MODIFIER "x\"}", i ? "," : "",
                           g_array_index(dis.breakpoints, guint64, i));
  g_string_append(args, "]}");
  dap_request(dap, "setInstructionBreakpoints", args->str, NULL, NULL);
  g_string_free(args, TRUE);
  if ((tab = view_tab(t)))
    gtk_widget_queue_draw(GTK_WIDGET(tab->source));
}
