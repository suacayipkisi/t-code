/*
 * The Process Monitor window (see processes.h). Each look reads /proc (application/processes.c)
 * with the owners the modules name; the process grid is built again only when the processes changed,
 * else its numbers are updated in place (an End button keeps its hover and click).
 */
#define _DEFAULT_SOURCE
#include "dialogs/processes.h"

#include "application/processes.h"
#include "debug/debugger.h"
#include "lsp/client/client.h"
#include "plugins/plugins.h"
#include "window/palette.h"
#include "window/statusbar.h"
#include "window/terminal.h"

#include <signal.h>
#include <string.h>
#include <unistd.h>

#define REFRESH_MS 2000
#define AFTER_END_MS 300 /* an ended process: looked at again this soon */

static const char *const css =
    ".tcode-processes { background: var(--tc-panel); }"
    ".tcode-processes .summary { font-weight: bold; font-size: larger; color: var(--tc-fg-bright); }"
    ".tcode-processes .section { font-weight: bold; color: var(--tc-fg-bright); margin-top: 10px; }"
    ".tcode-processes .head { color: var(--tc-fg-dim); font-size: smaller; }"
    ".tcode-processes .note { color: var(--tc-fg-dim); font-size: smaller; }"
    ".tcode-processes .number { font-feature-settings: \"tnum\"; }"
    ".tcode-processes .busy { color: var(--tc-warning); }";

typedef struct {
  int pid;
  guint64 start;
  ProcKind kind;
  char *name;
  GtkWidget *cpu, *memory, *rss, *threads;
} Row;

typedef struct {
  App *app;
  GtkWidget *window, *summary, *owners, *grid;
  ProcSampler *sampler;
  GArray *rows;      /* Row: the process grid's, in its order */
  char *shape;       /* the pids and owners the grid was built for */
  gboolean measured; /* a sample before this one: CPU is known */
  guint refresh_id;
} Monitor;

static Monitor *monitor; /* while the window is open */

static void row_clear(gpointer data)
{
  g_free(((Row *)data)->name);
}

static GtkWidget *cell(const char *text, const char *css_class, gboolean number)
{
  GtkWidget *l = gtk_label_new(text);

  gtk_label_set_xalign(GTK_LABEL(l), number ? 1 : 0);
  gtk_label_set_ellipsize(GTK_LABEL(l), number ? PANGO_ELLIPSIZE_NONE : PANGO_ELLIPSIZE_END);
  if (number)
    gtk_widget_add_css_class(l, "number");
  if (css_class)
    gtk_widget_add_css_class(l, css_class);
  return l;
}

static void clear_grid(GtkWidget *grid)
{
  GtkWidget *child;

  while ((child = gtk_widget_get_first_child(grid)))
    gtk_grid_remove(GTK_GRID(grid), child);
}

/* The column names; numbers: how many after the first are numbers (right aligned). */
static void head_row(GtkWidget *grid, const char *const *names, int count, int numbers)
{
  for (int i = 0; i < count; i++) {
    GtkWidget *head = cell(names[i], "head", i > 0 && i <= numbers);
    if (g_str_has_prefix(names[i], "Memory"))
      gtk_widget_set_tooltip_text(head, "PSS: its own memory plus its share of what it shares with others");
    else if (strcmp(names[i], "RSS") == 0)
      gtk_widget_set_tooltip_text(head, "RSS: every page it uses, shared ones counted in full (what most system "
                                        "monitors show)");
    gtk_grid_attach(GTK_GRID(grid), head, i, 0, 1, 1);
  }
}

static char *cpu_text(double cpu)
{
  return monitor->measured ? g_strdup_printf("%.1f%%", cpu) : g_strdup("–");
}

static void set_cpu(GtkWidget *label, double cpu)
{
  char *text = cpu_text(cpu);

  gtk_label_set_text(GTK_LABEL(label), text);
  if (monitor->measured && cpu >= 50)
    gtk_widget_add_css_class(label, "busy");
  else
    gtk_widget_remove_css_class(label, "busy");
  g_free(text);
}

static void set_memory(GtkWidget *label, guint64 bytes)
{
  char *text = procs_format_memory(bytes);

  gtk_label_set_text(GTK_LABEL(label), text);
  g_free(text);
}

/* By owner ------------------------------------------------------------------------------------ */

typedef struct {
  const ProcOwner *owner;
  int count;
  double cpu;
  guint64 memory, rss;
} Total;

static int by_memory(gconstpointer a, gconstpointer b)
{
  const Total *x = a, *y = b;

  return x->memory < y->memory ? 1 : x->memory > y->memory ? -1 : 0;
}

/* The owners' totals, the biggest first, and everything's in the summary line. */
static void show_totals(GPtrArray *procs)
{
  static const char *const names[] = { "Owner", "Processes", "CPU", "Memory (PSS)", "RSS" };
  GArray *totals = g_array_new(FALSE, TRUE, sizeof(Total));
  double cpu = 0;
  guint64 memory = 0, rss = 0;

  for (guint i = 0; i < procs->len; i++) {
    Proc *p = g_ptr_array_index(procs, i);
    Total *t = NULL;
    for (guint j = 0; j < totals->len && !t; j++)
      if (g_array_index(totals, Total, j).owner == p->owner)
        t = &g_array_index(totals, Total, j);
    if (!t) {
      g_array_set_size(totals, totals->len + 1);
      t = &g_array_index(totals, Total, totals->len - 1);
      t->owner = p->owner;
    }
    t->count++;
    t->cpu += p->cpu;
    t->memory += p->memory;
    t->rss += p->rss;
    cpu += p->cpu;
    memory += p->memory;
    rss += p->rss;
  }
  g_array_sort(totals, by_memory);

  clear_grid(monitor->owners);
  head_row(monitor->owners, names, G_N_ELEMENTS(names), 4);
  for (guint i = 0; i < totals->len; i++) {
    Total *t = &g_array_index(totals, Total, i);
    char *count = g_strdup_printf("%d", t->count);
    GtkWidget *owner = cell(t->owner->label, NULL, FALSE), *c = cell("", NULL, TRUE), *m = cell("", NULL, TRUE);
    gtk_widget_set_hexpand(owner, TRUE);
    set_cpu(c, t->cpu);
    GtkWidget *r = cell("", "note", TRUE);
    set_memory(m, t->memory);
    set_memory(r, t->rss);
    gtk_grid_attach(GTK_GRID(monitor->owners), owner, 0, (int)i + 1, 1, 1);
    gtk_grid_attach(GTK_GRID(monitor->owners), cell(count, NULL, TRUE), 1, (int)i + 1, 1, 1);
    gtk_grid_attach(GTK_GRID(monitor->owners), c, 2, (int)i + 1, 1, 1);
    gtk_grid_attach(GTK_GRID(monitor->owners), m, 3, (int)i + 1, 1, 1);
    gtk_grid_attach(GTK_GRID(monitor->owners), r, 4, (int)i + 1, 1, 1);
    g_free(count);
  }

  char *total = procs_format_memory(memory), *resident = procs_format_memory(rss), *usage = cpu_text(cpu);
  char *summary = g_strdup_printf("%u processes · %s CPU · %s memory (%s RSS)", procs->len, usage, total, resident);
  gtk_label_set_text(GTK_LABEL(monitor->summary), summary);
  g_free(summary);
  g_free(usage);
  g_free(total);
  g_free(resident);
  g_array_free(totals, TRUE);
}

/* The processes ------------------------------------------------------------------------------- */

static void refresh_soon(void);

static void on_end(GtkButton *button, gpointer data G_GNUC_UNUSED)
{
  int index = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "row"));
  Row *row;
  gboolean sent;
  char *message;

  if (!monitor || index < 0 || (guint)index >= monitor->rows->len)
    return;
  row = &g_array_index(monitor->rows, Row, index);
  sent = (row->kind == PROC_PLUGIN && plugins_end_process(row->pid)) || procs_signal(row->pid, row->start, SIGTERM);
  message = sent ? g_strdup_printf("Ended %s (%d)", row->name, row->pid)
                 : g_strdup_printf("%s (%d) is already gone", row->name, row->pid);
  status_say(monitor->app, message);
  g_free(message);
  gtk_widget_set_sensitive(GTK_WIDGET(button), FALSE);
  refresh_soon();
}

/* "pid/start/owner ..." for each process: the grid is built again when it changes. */
static char *shape_of(GPtrArray *procs)
{
  GString *shape = g_string_new(NULL);

  for (guint i = 0; i < procs->len; i++) {
    Proc *p = g_ptr_array_index(procs, i);
    g_string_append_printf(shape, "%d/%" G_GUINT64_FORMAT "/%s ", p->pid, p->start, p->owner->label);
  }
  return g_string_free(shape, FALSE);
}

static void build_grid(GPtrArray *procs)
{
  static const char *const names[] = { "Process", "PID", "CPU", "Memory (PSS)", "RSS", "Threads", "Owner" };
  int self = getpid();

  clear_grid(monitor->grid);
  g_array_set_size(monitor->rows, 0);
  head_row(monitor->grid, names, G_N_ELEMENTS(names), 5);
  for (guint i = 0; i < procs->len; i++) {
    Proc *p = g_ptr_array_index(procs, i);
    Row row = { .pid = p->pid, .start = p->start, .kind = p->owner->kind, .name = g_strdup(p->name) };
    char *pid = g_strdup_printf("%d", p->pid);
    GtkWidget *name = cell(p->name, NULL, FALSE);
    int y = (int)i + 1;

    gtk_widget_set_margin_start(name, 16 * MIN(p->depth, 8));
    gtk_widget_set_hexpand(name, TRUE);
    gtk_widget_set_tooltip_text(name, p->command);
    row.cpu = cell("", NULL, TRUE);
    row.memory = cell("", NULL, TRUE);
    row.rss = cell("", "note", TRUE);
    row.threads = cell("", NULL, TRUE);
    gtk_grid_attach(GTK_GRID(monitor->grid), name, 0, y, 1, 1);
    gtk_grid_attach(GTK_GRID(monitor->grid), cell(pid, NULL, TRUE), 1, y, 1, 1);
    gtk_grid_attach(GTK_GRID(monitor->grid), row.cpu, 2, y, 1, 1);
    gtk_grid_attach(GTK_GRID(monitor->grid), row.memory, 3, y, 1, 1);
    gtk_grid_attach(GTK_GRID(monitor->grid), row.rss, 4, y, 1, 1);
    gtk_grid_attach(GTK_GRID(monitor->grid), row.threads, 5, y, 1, 1);
    gtk_grid_attach(GTK_GRID(monitor->grid), cell(p->owner->label, "note", FALSE), 6, y, 1, 1);
    if (p->pid != self) {
      GtkWidget *end = gtk_button_new_with_label("End");
      gtk_widget_set_tooltip_text(end, "Ask it to quit (SIGTERM)");
      gtk_widget_add_css_class(end, "flat");
      g_object_set_data(G_OBJECT(end), "row", GINT_TO_POINTER((int)i));
      g_signal_connect(end, "clicked", G_CALLBACK(on_end), NULL);
      gtk_grid_attach(GTK_GRID(monitor->grid), end, 7, y, 1, 1);
    }
    g_array_append_val(monitor->rows, row);
    g_free(pid);
  }
}

static void refresh_now(void)
{
  App *t = monitor->app;
  GHashTable *owners = procs_owners_new();
  GPtrArray *procs;
  char *shape;

  plugins_own_processes(owners);
  lsp_own_processes(t, owners);
  debug_own_processes(t, owners);
  terminals_own_processes(t, owners);
  procs = procs_sample(monitor->sampler, getpid(), owners);

  shape = shape_of(procs);
  if (g_strcmp0(shape, monitor->shape) != 0) {
    build_grid(procs);
    g_free(monitor->shape);
    monitor->shape = shape;
  } else
    g_free(shape);
  for (guint i = 0; i < procs->len && i < monitor->rows->len; i++) {
    Proc *p = g_ptr_array_index(procs, i);
    Row *row = &g_array_index(monitor->rows, Row, i);
    char *threads = g_strdup_printf("%d", p->threads);
    set_cpu(row->cpu, p->cpu);
    set_memory(row->memory, p->memory);
    set_memory(row->rss, p->rss);
    gtk_label_set_text(GTK_LABEL(row->threads), threads);
    g_free(threads);
  }
  show_totals(procs);
  monitor->measured = TRUE;
  g_ptr_array_unref(procs); /* before owners: each Proc points into it */
  g_hash_table_unref(owners);
}

static gboolean refresh(gpointer data G_GNUC_UNUSED)
{
  refresh_now();
  return G_SOURCE_CONTINUE;
}

/* An ended process: look again shortly, then every REFRESH_MS from there. */
static gboolean refresh_once(gpointer data G_GNUC_UNUSED)
{
  refresh_now();
  monitor->refresh_id = g_timeout_add(REFRESH_MS, refresh, NULL);
  return G_SOURCE_REMOVE;
}

static void refresh_soon(void)
{
  g_clear_handle_id(&monitor->refresh_id, g_source_remove);
  monitor->refresh_id = g_timeout_add(AFTER_END_MS, refresh_once, NULL);
}

static void on_destroy(GtkWidget *window G_GNUC_UNUSED, gpointer data G_GNUC_UNUSED)
{
  g_clear_handle_id(&monitor->refresh_id, g_source_remove);
  procs_sampler_free(monitor->sampler);
  g_array_free(monitor->rows, TRUE);
  g_free(monitor->shape);
  g_clear_pointer(&monitor, g_free);
}

static GtkWidget *table(void)
{
  GtkWidget *grid = gtk_grid_new();

  gtk_grid_set_column_spacing(GTK_GRID(grid), 18);
  gtk_grid_set_row_spacing(GTK_GRID(grid), 2);
  return grid;
}

void show_processes(App *t)
{
  GtkWidget *box, *scroll, *note;
  GtkEventController *keys;

  if (monitor) {
    gtk_window_present(GTK_WINDOW(monitor->window));
    return;
  }
  palette_add_css(css);
  monitor = g_new0(Monitor, 1);
  monitor->app = t;
  monitor->sampler = procs_sampler_new();
  monitor->rows = g_array_new(FALSE, TRUE, sizeof(Row));
  g_array_set_clear_func(monitor->rows, row_clear);
  monitor->window = gtk_window_new();
  gtk_window_set_title(GTK_WINDOW(monitor->window), "Process Monitor");
  gtk_window_set_transient_for(GTK_WINDOW(monitor->window), t->window);
  gtk_widget_add_css_class(monitor->window, "tcode-dialog"); /* window/theme.c: its edge */
  gtk_window_set_default_size(GTK_WINDOW(monitor->window), 820, 600);
  gtk_widget_add_css_class(monitor->window, "tcode-processes");

  box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_margin_start(box, 16);
  gtk_widget_set_margin_end(box, 16);
  gtk_widget_set_margin_top(box, 12);
  gtk_widget_set_margin_bottom(box, 12);
  monitor->summary = cell("", "summary", FALSE);
  note = cell("Memory (PSS) is about what ending a process gives back: a library that several programs share "
              "(GTK, glib) is split between them. RSS counts those shared pages in full for every process, so "
              "it is bigger and its sum is more than is really used; system monitors that add up RSS per app "
              "show that bigger number. CPU is percent of one core.",
              "note", FALSE);
  gtk_label_set_wrap(GTK_LABEL(note), TRUE);
  gtk_label_set_ellipsize(GTK_LABEL(note), PANGO_ELLIPSIZE_NONE);
  monitor->owners = table();
  monitor->grid = table();
  gtk_box_append(GTK_BOX(box), monitor->summary);
  gtk_box_append(GTK_BOX(box), note);
  gtk_box_append(GTK_BOX(box), cell("By owner", "section", FALSE));
  gtk_box_append(GTK_BOX(box), monitor->owners);
  gtk_box_append(GTK_BOX(box), cell("Processes", "section", FALSE));
  gtk_box_append(GTK_BOX(box), monitor->grid);
  scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), box);
  gtk_window_set_child(GTK_WINDOW(monitor->window), scroll);

  keys = gtk_shortcut_controller_new(); /* Escape closes it */
  gtk_shortcut_controller_add_shortcut(
      GTK_SHORTCUT_CONTROLLER(keys),
      gtk_shortcut_new(gtk_keyval_trigger_new(GDK_KEY_Escape, 0), gtk_named_action_new("window.close")));
  gtk_widget_add_controller(monitor->window, keys);
  g_signal_connect(monitor->window, "destroy", G_CALLBACK(on_destroy), NULL);

  refresh_now();
  monitor->refresh_id = g_timeout_add(REFRESH_MS, refresh, NULL);
  gtk_window_present(GTK_WINDOW(monitor->window));
}
