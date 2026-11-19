/*
 * The Plugins window (see manager.h). Installed: the plugins on the left, the one picked on the
 * right; it looks at the plugins again every REFRESH_MS while it is open: a reload rebuilds the list
 * (keeping the pick by id), new output or another state updates the details. Browse: the registry's
 * index, fetched when the page first shows, with Install / Update. And the three warnings an
 * unreviewed plugin gets: on install, on first enable, on first run.
 */
#include "plugins/manager.h"

#include "application/identity.h"
#include "plugins/plugins.h"
#include "plugins/registry.h"
#include "window/palette.h"
#include "window/statusbar.h"

#define REFRESH_MS 500

static const char *const css =
    ".tcode-plugins { background: var(--tc-panel); }"
    ".tcode-plugins .plugin-list { background: var(--tc-frame); border-right: 1px solid var(--tc-border); }"
    ".tcode-plugins .plugin-list row { padding: 8px 12px; }"
    ".tcode-plugins .title { font-weight: bold; font-size: larger; color: var(--tc-fg-bright); }"
    ".tcode-plugins .section { font-weight: bold; color: var(--tc-fg-bright); margin-top: 14px; }"
    ".tcode-plugins .note { color: var(--tc-fg-dim); font-size: smaller; }"
    ".tcode-plugins .off { color: var(--tc-fg-dim); font-style: italic; }"
    ".tcode-plugins .output { font-family: monospace; font-size: smaller; padding: 6px; }"
    ".tcode-plugins .browse-list row { padding: 10px 14px; border-bottom: 1px solid var(--tc-border); }"
    ".tcode-plugins .unreviewed { color: var(--tc-warning); }"
    ".tcode-plugins .reviewed { color: var(--tc-git-added); }";

typedef struct {
  App *app;
  GtkWidget *window, *list, *details;
  GtkWidget *state, *start, *output; /* the details' parts that change while they show */
  char *picked;                      /* the plugin shown on the right, by id */
  guint generation;                  /* plugins_generation() when the list was built */
  guint printed;                     /* the picked plugin's lines printed when its output was shown */
  PluginState shown_state;
  guint refresh_id;
  GtkWidget *stack, *search, *browse_list, *browse_status; /* Browse */
  GPtrArray *entries;                                      /* RegistryEntry *: the index, once fetched */
  gboolean fetching;
} Manager;

static Manager *manager; /* while the window is open */

static Plugin *picked(void)
{
  return manager->picked ? plugin_find(manager->app, manager->picked) : NULL;
}

/* What it is doing, in a word or three. */
static const char *state_text(Plugin *p)
{
  if (!p->enabled)
    return "off";
  if (!p->starts)
    return "on";
  switch (p->state) {
    case PLUGIN_RUNNING: return "running";
    case PLUGIN_STOPPED: return "stopped";
    case PLUGIN_FAILED: return "failed";
    default: return "not started";
  }
}

static const char *source_text(PluginSource source)
{
  switch (source) {
    case PLUGIN_FROM_SYSTEM: return "the system (/usr/share/t-code/plugins)";
    case PLUGIN_FROM_TCODE: return "t-code itself";
    default: return "your plugins folder";
  }
}

static GtkWidget *label(const char *text, const char *css_class)
{
  GtkWidget *l = gtk_label_new(text);

  gtk_label_set_xalign(GTK_LABEL(l), 0);
  gtk_label_set_wrap(GTK_LABEL(l), TRUE);
  if (css_class)
    gtk_widget_add_css_class(l, css_class);
  return l;
}

static void add_section(GtkWidget *box, const char *title)
{
  gtk_box_append(GTK_BOX(box), label(title, "section"));
}

/* "Name: value" lines in a grid; a selectable value can be copied. */
static void add_fact(GtkWidget *grid, int *row, const char *name, const char *value, gboolean selectable)
{
  GtkWidget *v = label(value, NULL);

  gtk_label_set_selectable(GTK_LABEL(v), selectable);
  gtk_grid_attach(GTK_GRID(grid), label(name, "note"), 0, *row, 1, 1);
  gtk_grid_attach(GTK_GRID(grid), v, 1, (*row)++, 1, 1);
}

/* The details --------------------------------------------------------------------------------- */

static void show_details(void);
static void rebuild_list(void);

static void show_output(Plugin *p)
{
  GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(manager->output));
  GString *text = g_string_new(NULL);
  GtkTextIter end;

  for (GList *l = p->output.head; l; l = l->next)
    g_string_append_printf(text, "%s\n", (char *)l->data);
  gtk_text_buffer_set_text(buffer, text->len ? text->str : "(nothing printed yet: a quiet one)", -1);
  gtk_text_buffer_get_end_iter(buffer, &end);
  gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(manager->output), &end, 0, FALSE, 0, 0);
  g_string_free(text, TRUE);
  manager->printed = p->printed;
}

static void show_state(Plugin *p)
{
  gtk_label_set_text(GTK_LABEL(manager->state), state_text(p));
  if (manager->start) {
    gtk_button_set_label(GTK_BUTTON(manager->start), p->running ? "Stop" : "Start");
    gtk_widget_set_sensitive(manager->start, p->enabled);
  }
  manager->shown_state = p->state;
}

/* A question with two buttons over parent; yes(id) runs on the second. */
typedef void (*Answered)(App *t, const char *id, const char *extra);

typedef struct {
  App *app;
  char *id, *extra;
  Answered yes;
} Question;

static void on_answer(GObject *dialog, GAsyncResult *result, gpointer data)
{
  Question *q = data;

  if (gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(dialog), result, NULL) == 1)
    q->yes(q->app, q->id, q->extra);
  g_free(q->id);
  g_free(q->extra);
  g_free(q);
}

static void ask(App *t, GtkWindow *parent, const char *title, const char *detail, const char *no, const char *yes,
                Answered then, const char *id, const char *extra)
{
  GtkAlertDialog *dialog = gtk_alert_dialog_new("%s", title);
  const char *buttons[] = { no, yes, NULL };
  Question *q = g_new0(Question, 1);

  q->app = t;
  q->id = g_strdup(id);
  q->extra = g_strdup(extra);
  q->yes = then;
  gtk_alert_dialog_set_detail(dialog, detail);
  gtk_alert_dialog_set_buttons(dialog, buttons);
  gtk_alert_dialog_set_cancel_button(dialog, 0);
  gtk_alert_dialog_set_default_button(dialog, 0);
  gtk_alert_dialog_choose(dialog, parent, NULL, on_answer, q);
  g_object_unref(dialog);
}

static void turn_on(App *t, const char *id, const char *extra G_GNUC_UNUSED)
{
  Plugin *p = plugin_find(t, id);

  if (!p)
    return;
  plugin_warned(p, REGISTRY_WARNED_ENABLE);
  plugin_set_enabled(t, p, TRUE);
  status_say(t, "Plugin on: welcome aboard");
  if (manager) {
    show_details();
    rebuild_list();
  }
}

static void on_enabled(GtkSwitch *toggle, GParamSpec *spec G_GNUC_UNUSED, gpointer data G_GNUC_UNUSED)
{
  Plugin *p = picked();
  gboolean on = gtk_switch_get_active(toggle);

  if (!p || p->enabled == on)
    return;
  if (on && plugin_unreviewed(p) && !(p->record->warned & REGISTRY_WARNED_ENABLE)) {
    char *title = g_strdup_printf("Second heads-up: %s is still not reviewed", p->name);
    g_signal_handlers_block_by_func(toggle, on_enabled, NULL);
    gtk_switch_set_active(toggle, FALSE); /* until the answer */
    g_signal_handlers_unblock_by_func(toggle, on_enabled, NULL);
    ask(manager->app, GTK_WINDOW(manager->window), title,
        "On, its commands and keys work and it may start with the project. It runs with your rights. Turn it on?",
        "Keep It Off", "Turn On", turn_on, p->id, NULL);
    g_free(title);
    return;
  }
  plugin_set_enabled(manager->app, p, on);
  status_say(manager->app, p->enabled ? "Plugin on: welcome back" : "Plugin off: it is taking a nap");
  show_state(p);
  rebuild_list();
}

/* The third warning ------------------------------------------------------------------------------ */

static GHashTable *asking; /* the plugin ids whose first run is being asked about */

static void run_first(App *t, const char *id, const char *command)
{
  Plugin *p = plugin_find(t, id);

  g_hash_table_remove(asking, id);
  if (!p)
    return;
  plugin_warned(p, REGISTRY_WARNED_RUN);
  if (command)
    plugin_run_command(t, id, command);
  else
    plugin_start(t, p);
}

static void on_run_answer(GObject *dialog, GAsyncResult *result, gpointer data)
{
  Question *q = data;

  if (gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(dialog), result, NULL) == 1)
    run_first(q->app, q->id, q->extra);
  else
    g_hash_table_remove(asking, q->id);
  g_free(q->id);
  g_free(q->extra);
  g_free(q);
}

void plugins_confirm_run(App *t, Plugin *p, const char *command_id)
{
  GtkAlertDialog *dialog;
  const char *buttons[] = { "Not Now", "Run It", NULL };
  Question *q;

  if (!asking)
    asking = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  if (g_hash_table_contains(asking, p->id)) /* one question at a time */
    return;
  g_hash_table_add(asking, g_strdup(p->id));
  q = g_new0(Question, 1);
  q->app = t;
  q->id = g_strdup(p->id);
  q->extra = g_strdup(command_id);
  dialog = gtk_alert_dialog_new("Last heads-up before %s runs", p->name);
  gtk_alert_dialog_set_detail(dialog, "It is not reviewed yet and this is its first run. It runs with your rights. "
                                      "After this t-code stops asking, until a new version comes.");
  gtk_alert_dialog_set_buttons(dialog, buttons);
  gtk_alert_dialog_set_cancel_button(dialog, 0);
  gtk_alert_dialog_set_default_button(dialog, 0);
  gtk_alert_dialog_choose(dialog, manager ? GTK_WINDOW(manager->window) : t->window, NULL, on_run_answer, q);
  g_object_unref(dialog);
}

static void on_start(GtkButton *button G_GNUC_UNUSED, gpointer data G_GNUC_UNUSED)
{
  Plugin *p = picked();

  if (!p)
    return;
  if (p->running)
    plugin_stop(p);
  else if (!plugin_start(manager->app, p))
    status_say(manager->app, "That plugin would not start: its output may say why");
  show_state(p);
}

static void on_open_folder(GtkButton *button G_GNUC_UNUSED, gpointer data G_GNUC_UNUSED)
{
  Plugin *p = picked();

  if (p)
    plugin_open_folder(manager->app, p->dir);
}

static void on_uninstall_answer(GObject *dialog, GAsyncResult *result, gpointer data)
{
  char *id = data;
  Plugin *p = manager && gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(dialog), result, NULL) == 1
                  ? plugin_find(manager->app, id)
                  : NULL;
  GError *error = NULL;

  if (p) {
    char *name = g_strdup(p->name);
    if (plugin_uninstall(manager->app, p, &error)) {
      char *said = g_strdup_printf("%s is gone. It had a good run", name);
      status_say(manager->app, said);
      g_free(said);
    } else {
      GtkAlertDialog *failed = gtk_alert_dialog_new("Could not uninstall %s", name);
      gtk_alert_dialog_set_detail(failed, error->message);
      gtk_alert_dialog_show(failed, GTK_WINDOW(manager->window));
      g_object_unref(failed);
      g_clear_error(&error);
    }
    g_free(name);
  }
  g_free(id);
}

static void on_uninstall(GtkButton *button G_GNUC_UNUSED, gpointer data G_GNUC_UNUSED)
{
  Plugin *p = picked();
  const char *buttons[] = { "Keep It", "Uninstall", NULL };
  GtkAlertDialog *dialog;
  char *detail;

  if (!p)
    return;
  dialog = gtk_alert_dialog_new("Uninstall %s?", p->name);
  detail = g_strdup_printf("Its folder is deleted for good:\n%s", p->dir);
  gtk_alert_dialog_set_detail(dialog, detail);
  gtk_alert_dialog_set_buttons(dialog, buttons);
  gtk_alert_dialog_set_cancel_button(dialog, 0);
  gtk_alert_dialog_set_default_button(dialog, 0);
  gtk_alert_dialog_choose(dialog, GTK_WINDOW(manager->window), NULL, on_uninstall_answer, g_strdup(p->id));
  g_object_unref(dialog);
  g_free(detail);
}

static GtkWidget *button(const char *text, GCallback clicked)
{
  GtkWidget *b = gtk_button_new_with_label(text);

  g_signal_connect(b, "clicked", clicked, NULL);
  return b;
}

/* What p brings besides commands: its language servers, Run, language specs, snippets. */
static void add_brings(GtkWidget *box, Plugin *p)
{
  gboolean any = FALSE;
  GHashTableIter iter;
  gpointer pattern, command;

  add_section(box, "Brings");
  for (guint i = 0; p->servers && i < p->servers->len; i++) {
    PluginServer *s = g_ptr_array_index(p->servers, i);
    char *files = g_strjoinv(" ", s->extensions[0] ? s->extensions : s->languages);
    char *line = g_strdup_printf("Language server %s, for %s", s->name, files);
    gtk_box_append(GTK_BOX(box), label(line, NULL));
    g_free(line);
    g_free(files);
    any = TRUE;
  }
  g_hash_table_iter_init(&iter, p->run);
  while (g_hash_table_iter_next(&iter, &pattern, &command)) {
    char *line = g_strdup_printf("Run (F5) on %s files: %s", (char *)pattern, (char *)command);
    gtk_box_append(GTK_BOX(box), label(line, NULL));
    g_free(line);
    any = TRUE;
  }
  if (p->language_specs)
    gtk_box_append(GTK_BOX(box), label("Language specs (read when t-code starts)", NULL));
  if (p->snippets)
    gtk_box_append(GTK_BOX(box), label("Snippets (read when t-code starts)", NULL));
  if (!any && !p->language_specs && !p->snippets)
    gtk_box_append(GTK_BOX(box), label("Nothing but its commands", "note"));
}

static void add_commands(GtkWidget *box, Plugin *p)
{
  add_section(box, "Commands");
  for (guint i = 0; i < p->commands->len; i++) {
    PluginCommand *cmd = g_ptr_array_index(p->commands, i);
    char *line = cmd->keys && *cmd->keys ? g_strdup_printf("%s    %s", cmd->name, cmd->keys) : g_strdup(cmd->name);
    gtk_box_append(GTK_BOX(box), label(line, NULL));
    g_free(line);
  }
  if (!p->commands->len)
    gtk_box_append(GTK_BOX(box), label("None", "note"));
}

static void add_permissions(GtkWidget *box, Plugin *p)
{
  static const struct {
    PluginPermissions bit;
    const char *text;
  } all[] = {
    { PLUGIN_EDIT, "edit: changes the text in editors" },
    { PLUGIN_TERMINAL, "terminal: opens terminal tabs and runs programs in them" },
    { PLUGIN_NETWORK, "network: talks to the internet (declared, not checked)" },
    { PLUGIN_START, "start: starts with the project" },
  };

  add_section(box, "Permissions");
  if (!p->api) {
    gtk_box_append(GTK_BOX(box), label("Not checked: a plugin from before API versions", "note"));
    return;
  }
  for (guint i = 0; i < G_N_ELEMENTS(all); i++)
    if (p->permissions & all[i].bit)
      gtk_box_append(GTK_BOX(box), label(all[i].text, NULL));
  if (!p->permissions)
    gtk_box_append(GTK_BOX(box), label("None asked for", "note"));
}

static void show_details(void)
{
  Plugin *p = picked();
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4), *grid, *controls, *toggle, *scroll;
  char *api;
  int row = 0;

  manager->state = manager->start = manager->output = NULL;
  gtk_widget_set_margin_start(box, 20);
  gtk_widget_set_margin_end(box, 20);
  gtk_widget_set_margin_top(box, 16);
  gtk_widget_set_margin_bottom(box, 16);
  if (!p) {
    gtk_box_append(GTK_BOX(box), label(manager->app->plugins && manager->app->plugins->len
                                           ? "Pick a plugin on the left."
                                           : "No plugins yet. Put one in your plugins folder (Open Plugins "
                                             "Folder) and press Reload.",
                                       "note"));
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(manager->details), box);
    return;
  }
  gtk_box_append(GTK_BOX(box), label(p->name, "title"));
  if (*p->description)
    gtk_box_append(GTK_BOX(box), label(p->description, NULL));

  grid = gtk_grid_new();
  gtk_grid_set_column_spacing(GTK_GRID(grid), 16);
  gtk_grid_set_row_spacing(GTK_GRID(grid), 2);
  gtk_widget_set_margin_top(grid, 10);
  api = p->api ? g_strdup_printf("tapi_%d", p->api) : g_strdup("none (a plugin from before API versions)");
  add_fact(grid, &row, "Id", p->id, TRUE);
  add_fact(grid, &row, "Version", *p->version ? p->version : "not given", FALSE);
  add_fact(grid, &row, "API", api, FALSE);
  add_fact(grid, &row, "From", source_text(p->source), FALSE);
  add_fact(grid, &row, "Folder", p->dir, TRUE);
  if (p->record) {
    char *short_commit = g_strndup(p->record->commit, 12);
    char *registry = g_strdup_printf("%s at %s", p->record->reviewed ? "reviewed" : "not reviewed yet", short_commit);
    add_fact(grid, &row, "Registry", registry, FALSE);
    g_free(registry);
    g_free(short_commit);
  } else if (p->source == PLUGIN_FROM_USER) {
    add_fact(grid, &row, "Registry", "not from it: installed by hand", FALSE);
  }
  manager->state = label("", NULL);
  gtk_grid_attach(GTK_GRID(grid), label("State", "note"), 0, row, 1, 1);
  gtk_grid_attach(GTK_GRID(grid), manager->state, 1, row++, 1, 1);
  gtk_box_append(GTK_BOX(box), grid);
  g_free(api);

  controls = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_margin_top(controls, 10);
  toggle = gtk_switch_new();
  gtk_switch_set_active(GTK_SWITCH(toggle), p->enabled);
  gtk_widget_set_valign(toggle, GTK_ALIGN_CENTER);
  gtk_widget_set_tooltip_text(toggle, "On / off: off, its commands, keys and servers go and it does not start");
  g_signal_connect(toggle, "notify::active", G_CALLBACK(on_enabled), NULL);
  gtk_box_append(GTK_BOX(controls), toggle);
  if (p->starts) {
    manager->start = button("Start", G_CALLBACK(on_start));
    gtk_box_append(GTK_BOX(controls), manager->start);
  }
  gtk_box_append(GTK_BOX(controls), button("Open Folder", G_CALLBACK(on_open_folder)));
  if (p->source == PLUGIN_FROM_USER)
    gtk_box_append(GTK_BOX(controls), button("Uninstall", G_CALLBACK(on_uninstall)));
  gtk_box_append(GTK_BOX(box), controls);

  add_commands(box, p);
  add_permissions(box, p);
  add_brings(box, p);

  add_section(box, "Output");
  manager->output = gtk_text_view_new();
  gtk_text_view_set_editable(GTK_TEXT_VIEW(manager->output), FALSE);
  gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(manager->output), GTK_WRAP_WORD_CHAR);
  gtk_widget_add_css_class(manager->output, "output");
  scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scroll), 160);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), manager->output);
  gtk_box_append(GTK_BOX(box), scroll);

  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(manager->details), box);
  show_state(p);
  show_output(p);
}

/* The list --------------------------------------------------------------------------------------- */

static void on_row_selected(GtkListBox *list G_GNUC_UNUSED, GtkListBoxRow *row, gpointer data G_GNUC_UNUSED)
{
  if (!row)
    return;
  g_set_str(&manager->picked, g_object_get_data(G_OBJECT(row), "plugin-id"));
  show_details();
}

static void rebuild_list(void)
{
  GPtrArray *plugins = manager->app->plugins;
  GtkListBoxRow *keep = NULL;
  GtkWidget *child;

  g_signal_handlers_block_by_func(manager->list, on_row_selected, NULL);
  while ((child = gtk_widget_get_first_child(manager->list)))
    gtk_list_box_remove(GTK_LIST_BOX(manager->list), child);
  for (guint i = 0; plugins && i < plugins->len; i++) {
    Plugin *p = g_ptr_array_index(plugins, i);
    GtkWidget *row = gtk_list_box_row_new(), *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    char *line = g_strdup_printf("%s%s%s", *p->version ? p->version : "", *p->version ? " · " : "", state_text(p));
    GtkWidget *name = label(p->name, p->enabled ? NULL : "off"), *note = label(line, "note");
    gtk_box_append(GTK_BOX(box), name);
    gtk_box_append(GTK_BOX(box), note);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
    g_object_set_data_full(G_OBJECT(row), "plugin-id", g_strdup(p->id), g_free);
    gtk_list_box_append(GTK_LIST_BOX(manager->list), row);
    if (!keep && (!manager->picked || g_strcmp0(manager->picked, p->id) == 0))
      keep = GTK_LIST_BOX_ROW(row);
    g_free(line);
  }
  if (keep)
    gtk_list_box_select_row(GTK_LIST_BOX(manager->list), keep);
  g_signal_handlers_unblock_by_func(manager->list, on_row_selected, NULL);
  manager->generation = plugins_generation();
}

/* Every REFRESH_MS: a reload rebuilds everything, new output or another state the parts that show
 * them. */
static gboolean refresh(gpointer data G_GNUC_UNUSED)
{
  Plugin *p;

  if (manager->generation != plugins_generation()) {
    if (!picked()) /* uninstalled, or gone from its folder */
      g_clear_pointer(&manager->picked, g_free);
    if (!manager->picked && manager->app->plugins && manager->app->plugins->len)
      g_set_str(&manager->picked, ((Plugin *)g_ptr_array_index(manager->app->plugins, 0))->id);
    rebuild_list();
    show_details();
    return G_SOURCE_CONTINUE;
  }
  p = picked();
  if (p && manager->output && p->printed != manager->printed)
    show_output(p);
  if (p && manager->state && p->state != manager->shown_state) {
    show_state(p);
    rebuild_list();
  }
  return G_SOURCE_CONTINUE;
}

static void on_reload(GtkButton *b G_GNUC_UNUSED, gpointer data G_GNUC_UNUSED)
{
  plugins_reload(manager->app);
  refresh(NULL);
}

static void on_open_plugins_folder(GtkButton *b G_GNUC_UNUSED, gpointer data G_GNUC_UNUSED)
{
  open_plugins_folder(manager->app);
}

/* Browse ----------------------------------------------------------------------------------------- */

static void fill_browse(void);

/* What Install was told: its own copy of the entry, the window may close meanwhile. */
typedef struct {
  App *app;
  RegistryEntry *entry;
} Job;

static RegistryEntry *entry_copy(const RegistryEntry *e)
{
  RegistryEntry *c = g_new0(RegistryEntry, 1);

  c->id = g_strdup(e->id);
  c->name = g_strdup(e->name);
  c->description = g_strdup(e->description);
  c->version = g_strdup(e->version);
  c->api = g_strdup(e->api);
  c->repository = g_strdup(e->repository);
  c->commit = g_strdup(e->commit);
  c->path = g_strdup(e->path);
  c->tree = g_strdup(e->tree);
  c->permissions = g_strdupv(e->permissions);
  c->categories = g_strdupv(e->categories);
  c->reviewed = e->reviewed;
  return c;
}

static RegistryEntry *entry_named(const char *id)
{
  for (guint i = 0; manager && manager->entries && i < manager->entries->len; i++) {
    RegistryEntry *e = g_ptr_array_index(manager->entries, i);
    if (strcmp(e->id, id) == 0)
      return e;
  }
  return NULL;
}

/* The download is in place (or not): an unreviewed plugin starts off, the plugins load again. */
static void on_installed(const char *error, gpointer data)
{
  Job *job = data;
  RegistryEntry *e = job->entry;

  if (error) {
    char *said = g_strdup_printf("%s: %s", e->name, error);
    status_say(job->app, said);
    if (manager) {
      GtkAlertDialog *failed = gtk_alert_dialog_new("Could not install %s", e->name);
      gtk_alert_dialog_set_detail(failed, error);
      gtk_alert_dialog_show(failed, GTK_WINDOW(manager->window));
      g_object_unref(failed);
    }
    g_free(said);
  } else {
    RegistryRecord record = { .repository = e->repository,
                              .commit = e->commit,
                              .reviewed = e->reviewed,
                              .warned = e->reviewed ? 0 : REGISTRY_WARNED_INSTALL };
    char *said = g_strdup_printf(e->reviewed ? "%s is in. Have fun"
                                             : "%s is in, switched off for now: turn it on when "
                                               "you are ready",
                                 e->name);
    registry_set_record(e->id, &record);
    if (!e->reviewed)
      plugins_choose_disabled(e->id, TRUE);
    plugins_reload(job->app);
    status_say(job->app, said);
    g_free(said);
    if (manager)
      g_set_str(&manager->picked, e->id);
  }
  if (manager)
    fill_browse();
  registry_entry_free(e);
  g_free(job);
}

static void install(App *t, const char *id, const char *extra G_GNUC_UNUSED)
{
  RegistryEntry *e = entry_named(id);
  char *dir = g_build_filename(g_get_user_config_dir(), TCODE_NAME, "plugins", NULL), *said;
  Job *job;

  if (!e)
    return;
  job = g_new0(Job, 1);
  job->app = t;
  job->entry = entry_copy(e);
  said = g_strdup_printf("Fetching %s…", e->name);
  status_say(t, said);
  registry_install(job->entry, dir, on_installed, job);
  g_free(said);
  g_free(dir);
}

static void on_install(GtkButton *b, gpointer data G_GNUC_UNUSED)
{
  RegistryEntry *e = entry_named(g_object_get_data(G_OBJECT(b), "entry-id"));

  if (!e)
    return;
  gtk_widget_set_sensitive(GTK_WIDGET(b), FALSE);
  gtk_button_set_label(b, "Installing…");
  if (e->reviewed) {
    install(manager->app, e->id, NULL);
  } else {
    char *title = g_strdup_printf("Heads up: %s is not reviewed yet", e->name);
    ask(manager->app, GTK_WINDOW(manager->window), title,
        "The registry's checks passed, but no maintainer has read this version yet. It runs with your rights, like "
        "any program you start. It installs switched off.",
        "Cancel", "Install Anyway", install, e->id, NULL);
    g_free(title);
    fill_browse(); /* Cancel leaves the button as it was */
  }
}

/* "tapi_2" from a newer t-code than this one. */
static gboolean too_new(const char *api)
{
  return g_str_has_prefix(api, "tapi_") && g_ascii_strtoll(api + 5, NULL, 10) > TAPI_LATEST;
}

static gboolean matches(RegistryEntry *e, const char *query)
{
  char *text, *low, *q;
  gboolean found;

  if (!query || !*query)
    return TRUE;
  text = g_strjoin(" ", e->id, e->name, e->description, NULL);
  for (int i = 0; e->categories[i]; i++) {
    char *more = g_strconcat(text, " ", e->categories[i], NULL);
    g_free(text);
    text = more;
  }
  low = g_utf8_strdown(text, -1);
  q = g_utf8_strdown(query, -1);
  found = strstr(low, q) != NULL;
  g_free(q);
  g_free(low);
  g_free(text);
  return found;
}

static GtkWidget *entry_row(RegistryEntry *e)
{
  Plugin *p = plugin_find(manager->app, e->id);
  GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12), *texts = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  GtkWidget *action, *badge;
  char *line, *categories = g_strjoinv(", ", e->categories), *asks = g_strjoinv(", ", e->permissions);

  gtk_box_append(GTK_BOX(texts), label(e->name, "section"));
  line = g_strdup_printf("%s%s%s", e->version, *e->version && *categories ? " · " : "", categories);
  gtk_box_append(GTK_BOX(texts), label(line, "note"));
  g_free(line);
  badge = label(e->reviewed ? "Reviewed by a maintainer" : "Not reviewed yet: checks passed, no human has read it",
                e->reviewed ? "reviewed" : "unreviewed");
  gtk_box_append(GTK_BOX(texts), badge);
  if (*e->description)
    gtk_box_append(GTK_BOX(texts), label(e->description, NULL));
  line = g_strdup_printf("Asks for: %s", *asks ? asks : "nothing");
  gtk_box_append(GTK_BOX(texts), label(line, "note"));
  g_free(line);
  gtk_widget_set_hexpand(texts, TRUE);
  gtk_box_append(GTK_BOX(row), texts);

  if (too_new(e->api))
    action = gtk_button_new_with_label("Needs a newer t-code");
  else if (p && p->record && strcmp(p->record->commit, e->commit) == 0)
    action = gtk_button_new_with_label("Installed");
  else if (p && p->record)
    action = gtk_button_new_with_label("Update");
  else if (p)
    action = gtk_button_new_with_label("Already here");
  else
    action = gtk_button_new_with_label("Install");
  gtk_widget_set_sensitive(action, !too_new(e->api) && (!p || (p->record && strcmp(p->record->commit, e->commit))));
  if (p && !p->record)
    gtk_widget_set_tooltip_text(action, "A plugin with this id is installed by hand (or came with t-code)");
  g_object_set_data_full(G_OBJECT(action), "entry-id", g_strdup(e->id), g_free);
  g_signal_connect(action, "clicked", G_CALLBACK(on_install), NULL);
  gtk_widget_set_valign(action, GTK_ALIGN_CENTER);
  gtk_box_append(GTK_BOX(row), action);
  g_free(asks);
  g_free(categories);
  return row;
}

static void fill_browse(void)
{
  const char *query = gtk_editable_get_text(GTK_EDITABLE(manager->search));
  GtkWidget *child;
  int shown = 0;

  while ((child = gtk_widget_get_first_child(manager->browse_list)))
    gtk_list_box_remove(GTK_LIST_BOX(manager->browse_list), child);
  for (guint i = 0; manager->entries && i < manager->entries->len; i++) {
    RegistryEntry *e = g_ptr_array_index(manager->entries, i);
    if (matches(e, query)) {
      gtk_list_box_append(GTK_LIST_BOX(manager->browse_list), entry_row(e));
      shown++;
    }
  }
  if (manager->entries)
    gtk_label_set_text(GTK_LABEL(manager->browse_status),
                       !manager->entries->len ? "The registry is empty for now. Be the first to publish one!"
                       : shown                ? ""
                                              : "Nothing matches that. Try fewer letters?");
}

/* A maintainer reviewed the commit someone has installed: its record says so, no more warnings. */
static void note_reviews(void)
{
  for (guint i = 0; manager->app->plugins && i < manager->app->plugins->len; i++) {
    Plugin *p = g_ptr_array_index(manager->app->plugins, i);
    RegistryEntry *e = p->record ? entry_named(p->id) : NULL;
    if (e && e->reviewed && !p->record->reviewed && strcmp(e->commit, p->record->commit) == 0) {
      p->record->reviewed = TRUE;
      registry_set_record(p->id, p->record);
    }
  }
}

static void on_fetched(GPtrArray *entries, const char *error, gpointer data G_GNUC_UNUSED)
{
  if (!manager) {
    if (entries)
      g_ptr_array_unref(entries);
    return;
  }
  manager->fetching = FALSE;
  if (!entries) {
    gtk_label_set_text(GTK_LABEL(manager->browse_status), error);
    return;
  }
  if (manager->entries)
    g_ptr_array_unref(manager->entries);
  manager->entries = entries;
  note_reviews();
  fill_browse();
  show_details();
}

static void fetch(void)
{
  if (manager->fetching)
    return;
  manager->fetching = TRUE;
  gtk_label_set_text(GTK_LABEL(manager->browse_status), "Asking the registry what is out there…");
  registry_fetch(on_fetched, NULL);
}

static void on_page(GtkStack *stack, GParamSpec *spec G_GNUC_UNUSED, gpointer data G_GNUC_UNUSED)
{
  if (g_strcmp0(gtk_stack_get_visible_child_name(stack), "browse") == 0 && !manager->entries)
    fetch();
}

static void on_search(GtkSearchEntry *entry G_GNUC_UNUSED, gpointer data G_GNUC_UNUSED)
{
  fill_browse();
}

static GtkWidget *browse_page(void)
{
  GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8), *scroll = gtk_scrolled_window_new();

  manager->search = gtk_search_entry_new();
  gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(manager->search), "Search the registry (name, category)");
  g_signal_connect(manager->search, "search-changed", G_CALLBACK(on_search), NULL);
  manager->browse_status = label("", "note");
  manager->browse_list = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(manager->browse_list), GTK_SELECTION_NONE);
  gtk_widget_add_css_class(manager->browse_list, "browse-list");
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), manager->browse_list);
  gtk_widget_set_vexpand(scroll, TRUE);
  gtk_widget_set_margin_start(page, 16);
  gtk_widget_set_margin_end(page, 16);
  gtk_widget_set_margin_top(page, 12);
  gtk_box_append(GTK_BOX(page), manager->search);
  gtk_box_append(GTK_BOX(page), manager->browse_status);
  gtk_box_append(GTK_BOX(page), scroll);
  return page;
}

static void on_destroy(GtkWidget *window G_GNUC_UNUSED, gpointer data G_GNUC_UNUSED)
{
  g_clear_handle_id(&manager->refresh_id, g_source_remove);
  g_free(manager->picked);
  if (manager->entries)
    g_ptr_array_unref(manager->entries);
  g_clear_pointer(&manager, g_free);
}

void show_plugins_window(App *t)
{
  GtkWidget *header, *panes, *side, *scroll, *switcher;
  GtkEventController *keys;

  if (manager) {
    gtk_window_present(GTK_WINDOW(manager->window));
    return;
  }
  palette_add_css(css);
  manager = g_new0(Manager, 1);
  manager->app = t;
  manager->window = gtk_window_new();
  gtk_window_set_title(GTK_WINDOW(manager->window), "Plugins");
  gtk_window_set_transient_for(GTK_WINDOW(manager->window), t->window);
  gtk_window_set_default_size(GTK_WINDOW(manager->window), 860, 560);
  gtk_widget_add_css_class(manager->window, "tcode-plugins");

  header = gtk_header_bar_new();
  gtk_header_bar_pack_start(GTK_HEADER_BAR(header), button("Reload", G_CALLBACK(on_reload)));
  gtk_header_bar_pack_start(GTK_HEADER_BAR(header), button("Open Plugins Folder", G_CALLBACK(on_open_plugins_folder)));
  gtk_window_set_titlebar(GTK_WINDOW(manager->window), header);
  manager->stack = gtk_stack_new();
  switcher = gtk_stack_switcher_new();
  gtk_stack_switcher_set_stack(GTK_STACK_SWITCHER(switcher), GTK_STACK(manager->stack));
  gtk_header_bar_set_title_widget(GTK_HEADER_BAR(header), switcher);

  manager->list = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(manager->list), GTK_SELECTION_BROWSE);
  g_signal_connect(manager->list, "row-selected", G_CALLBACK(on_row_selected), NULL);
  side = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(side), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(side), manager->list);
  gtk_widget_set_size_request(side, 220, -1);
  gtk_widget_add_css_class(side, "plugin-list");
  manager->details = scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_hexpand(scroll, TRUE);
  panes = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_append(GTK_BOX(panes), side);
  gtk_box_append(GTK_BOX(panes), scroll);
  gtk_stack_add_titled(GTK_STACK(manager->stack), panes, "installed", "Installed");
  gtk_stack_add_titled(GTK_STACK(manager->stack), browse_page(), "browse", "Browse");
  g_signal_connect(manager->stack, "notify::visible-child", G_CALLBACK(on_page), NULL);
  gtk_window_set_child(GTK_WINDOW(manager->window), manager->stack);

  keys = gtk_shortcut_controller_new(); /* Escape closes it */
  gtk_shortcut_controller_add_shortcut(
      GTK_SHORTCUT_CONTROLLER(keys),
      gtk_shortcut_new(gtk_keyval_trigger_new(GDK_KEY_Escape, 0), gtk_named_action_new("window.close")));
  gtk_widget_add_controller(manager->window, keys);
  g_signal_connect(manager->window, "destroy", G_CALLBACK(on_destroy), NULL);

  if (t->plugins && t->plugins->len)
    g_set_str(&manager->picked, ((Plugin *)g_ptr_array_index(t->plugins, 0))->id);
  rebuild_list();
  show_details();
  manager->refresh_id = g_timeout_add(REFRESH_MS, refresh, NULL);
  gtk_window_present(GTK_WINDOW(manager->window));
}
