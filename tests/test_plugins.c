/* Tests for the plugins (src/plugins/plugins.c, included for its static functions; and
 * src/lsp/servers.c, which runs the language servers plugins declare). */
#include "lsp/servers.c"
#include "plugins/plugins.c"
#include "util.h"

#include <glib/gstdio.h>

static char *tmp_dir;

static gboolean runs_empty(gpointer data G_GNUC_UNUSED)
{
  return runs == NULL;
}

/* clang-format off */
Tab *current_tab(App *t G_GNUC_UNUSED) { return NULL; }
void status_say(App *t G_GNUC_UNUSED, const char *message G_GNUC_UNUSED) {}
void refresh_shortcuts(App *t G_GNUC_UNUSED) {}
void lsp_reattach(App *t G_GNUC_UNUSED) {}
char *toolchain_python(App *t G_GNUC_UNUSED, const char *root G_GNUC_UNUSED) { return NULL; }
char *toolchain_rust_features(App *t G_GNUC_UNUSED) { return NULL; }
char *toolchain_rust_target(App *t G_GNUC_UNUSED) { return NULL; }
char *langenv_rust_options(const char *features G_GNUC_UNUSED, const char *target G_GNUC_UNUSED) { return NULL; }
char *json_quote(const char *text) { return g_strdup_printf("\"%s\"", text); }
/* clang-format on */

/* The palette's rows ("label|detail|payload"). */
static GPtrArray *test_picker_rows = NULL;

void picker_add(App *t G_GNUC_UNUSED, guint generation G_GNUC_UNUSED, const char *label, const char *detail,
                const char *payload)
{
  if (!test_picker_rows)
    test_picker_rows = g_ptr_array_new_with_free_func(g_free);
  g_ptr_array_add(test_picker_rows, g_strdup_printf("%s|%s|%s", label, detail ? detail : "", payload));
}

/* The picker's scoring stands in as a match when the query is in the text, ignoring case. */
int fuzzy_score(const char *query, const char *text)
{
  char *q = g_ascii_strdown(query, -1), *t = g_ascii_strdown(text, -1);
  int score = strstr(t, q) ? 1 : -1;

  g_free(t);
  g_free(q);
  return score;
}

static void write_file(const char *rel_path, const char *content, int mode)
{
  g_free(write_at(tmp_dir, rel_path, content, -1, mode));
}

/* tmp_dir/valid/test-plug: two commands, the first with a shortcut. */
static void write_valid_plugin(void)
{
  const char *json = "{\n"
                     "  \"name\": \"Test Plugin\",\n"
                     "  \"version\": \"1.2.3\",\n"
                     "  \"description\": \"A sample test plugin\",\n"
                     "  \"executable\": \"run.sh\",\n"
                     "  \"commands\": [\n"
                     "    { \"id\": \"c1\", \"name\": \"Command One\", \"keys\": \"Ctrl+Alt+1\", "
                     "\"trigger\": \"<Control><Alt>1\" },\n"
                     "    { \"id\": \"c2\", \"name\": \"Command Two\" }\n"
                     "  ]\n"
                     "}\n";

  write_file("valid/test-plug/plugin.json", json, 0644);
  write_file("valid/test-plug/run.sh", "#!/bin/sh\nexit 0\n", 0755);
}

static void test_valid_plugin(void)
{
  App app = { 0 };

  write_valid_plugin();

  char *scan_path = g_build_filename(tmp_dir, "valid", NULL);
  app.plugins = g_ptr_array_new_with_free_func((GDestroyNotify)plugin_free);
  plugins_scan_dir(&app, scan_path);

  g_assert_cmpuint(app.plugins->len, ==, 1);
  Plugin *p = g_ptr_array_index(app.plugins, 0);
  g_assert_cmpstr(p->id, ==, "test-plug");
  g_assert_cmpstr(p->name, ==, "Test Plugin");
  g_assert_cmpstr(p->version, ==, "1.2.3");
  g_assert_cmpstr(p->description, ==, "A sample test plugin");
  g_assert_cmpuint(p->commands->len, ==, 2);

  PluginCommand *c1 = g_ptr_array_index(p->commands, 0);
  g_assert_cmpstr(c1->id, ==, "c1");
  g_assert_cmpstr(c1->name, ==, "Command One");
  g_assert_cmpstr(c1->keys, ==, "Ctrl+Alt+1");
  g_assert_cmpstr(c1->trigger, ==, "<Control><Alt>1");
  GtkShortcutController *controller = GTK_SHORTCUT_CONTROLLER(gtk_shortcut_controller_new());
  plugins_add_shortcuts(controller, &app);
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(controller)), ==, 1);
  g_object_unref(controller);
  g_assert_true(c1->plugin == p);

  PluginCommand *c2 = g_ptr_array_index(p->commands, 1);
  g_assert_cmpstr(c2->id, ==, "c2");
  g_assert_cmpstr(c2->name, ==, "Command Two");
  g_assert_null(c2->keys);
  g_assert_cmpint(p->api, ==, 0); /* no "api": a plugin from before versions */

  plugins_stop(&app);
  g_free(scan_path);
}

/* A tapi_1 plugin: "keys" is GTK's shortcut syntax and its label is made from it; "keys" GTK cannot
 * read stays a label only. A plugin without commands needs no program. */
static void test_tapi_manifest(void)
{
  App app = { 0 };

  write_file("tapi/keyed/plugin.json",
             "{\"name\": \"Keyed\", \"api\": \"tapi_1\", \"executable\": \"run.sh\", \"commands\": ["
             "{\"id\": \"k\", \"name\": \"Keyed Command\", \"keys\": \"<Control><Alt>k\"},"
             "{\"id\": \"l\", \"name\": \"Labeled\", \"keys\": \"Ctrl+Alt+L\"}]}",
             0644);
  write_file("tapi/keyed/run.sh", "#!/bin/sh\nexit 0\n", 0755);
  write_file("tapi/static-only/plugin.json",
             "{\"name\": \"Static\", \"api\": \"tapi_1\", \"permissions\": [\"edit\", \"network\", \"bogus\"]}", 0644);

  char *scan_path = g_build_filename(tmp_dir, "tapi", NULL);
  app.plugins = g_ptr_array_new_with_free_func((GDestroyNotify)plugin_free);
  g_test_expect_message(NULL, G_LOG_LEVEL_WARNING, "*unknown permission 'bogus'*");
  plugins_scan_dir(&app, scan_path);
  g_test_assert_expected_messages();
  g_assert_cmpuint(app.plugins->len, ==, 2);

  Plugin *keyed = plugin_find(&app, "keyed"), *still = plugin_find(&app, "static-only");
  g_assert_nonnull(keyed);
  g_assert_cmpint(keyed->api, ==, 1);
  PluginCommand *k = g_ptr_array_index(keyed->commands, 0), *l = g_ptr_array_index(keyed->commands, 1);
  g_assert_cmpstr(k->trigger, ==, "<Control><Alt>k");
  g_assert_cmpstr(k->keys, ==, "Ctrl+Alt+K");
  g_assert_null(l->trigger);
  g_assert_cmpstr(l->keys, ==, "Ctrl+Alt+L");

  g_assert_nonnull(still);
  g_assert_cmpint(still->permissions, ==, PLUGIN_EDIT | PLUGIN_NETWORK);
  g_assert_cmpint(keyed->permissions, ==, 0);
  g_assert_nonnull(keyed->token); /* $TCODE_PLUGIN_TOKEN: each its own */
  g_assert_cmpstr(keyed->token, !=, still->token);
  g_assert_true(plugin_by_token(&app, still->token) == still);
  g_assert_null(plugin_by_token(&app, "nope"));
  g_assert_null(still->executable);
  g_assert_cmpuint(still->commands->len, ==, 0);

  GtkShortcutController *controller = GTK_SHORTCUT_CONTROLLER(gtk_shortcut_controller_new());
  plugins_add_shortcuts(controller, &app);
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(controller)), ==, 1);
  g_object_unref(controller);

  plugins_stop(&app);
  g_free(scan_path);
}

static void test_invalid_plugins(void)
{
  App app = { 0 };

  /* Missing name */
  write_file("invalid/p1/plugin.json", "{\"commands\": [{\"id\":\"a\",\"name\":\"A\"}], \"executable\":\"run.sh\"}",
             0644);
  write_file("invalid/p1/run.sh", "#!/bin/sh\nexit 0\n", 0755);

  /* Commands that are not a list */
  write_file("invalid/p2/plugin.json", "{\"name\": \"P2\", \"commands\": \"run\", \"executable\":\"run.sh\"}", 0644);
  write_file("invalid/p2/run.sh", "#!/bin/sh\nexit 0\n", 0755);

  /* A plugin API this t-code does not speak */
  write_file("invalid/p3/plugin.json",
             "{\"name\": \"P3\", \"api\": \"tapi_99\", \"commands\": [{\"id\":\"a\",\"name\":\"A\"}], "
             "\"executable\":\"run.sh\"}",
             0644);
  write_file("invalid/p3/run.sh", "#!/bin/sh\nexit 0\n", 0755);

  /* Malformed JSON */
  write_file("invalid/p4/plugin.json", "NOT JSON", 0644);
  write_file("invalid/p4/run.sh", "#!/bin/sh\nexit 0\n", 0755);

  /* Non-existent executable */
  write_file("invalid/p5/plugin.json",
             "{\"name\": \"P5\", \"commands\": [{\"id\":\"a\",\"name\":\"A\"}], \"executable\":\"missing.sh\"}", 0644);

  /* Non-executable file */
  write_file("invalid/p6/plugin.json",
             "{\"name\": \"P6\", \"commands\": [{\"id\":\"a\",\"name\":\"A\"}], \"executable\":\"run.sh\"}", 0644);
  write_file("invalid/p6/run.sh", "#!/bin/sh\nexit 0\n", 0644); /* Not executable */

  char *scan_path = g_build_filename(tmp_dir, "invalid", NULL);
  app.plugins = g_ptr_array_new_with_free_func((GDestroyNotify)plugin_free);
  for (int i = 0; i < 6; i++)
    g_test_expect_message(NULL, G_LOG_LEVEL_WARNING, "*");
  plugins_scan_dir(&app, scan_path);
  g_test_assert_expected_messages();

  g_assert_cmpuint(app.plugins->len, ==, 0);

  plugins_stop(&app);
  g_free(scan_path);
}

/* Spins the main loop until done(data) or ms pass; whether done. */
static gboolean spin_until(gboolean (*done)(gpointer), gpointer data, int ms)
{
  gint64 until = g_get_monotonic_time() + ms * 1000;

  while (!done(data) && g_get_monotonic_time() < until) {
    g_main_context_iteration(NULL, FALSE);
    g_usleep(5000);
  }
  return done(data);
}

/* Whether a plugin printed a line holding text. */
static gboolean printed(Plugin *p, const char *text)
{
  for (GList *l = p->output.head; l; l = l->next)
    if (strstr(l->data, text))
      return TRUE;
  return FALSE;
}

typedef struct {
  Plugin *plugin;
  const char *text;
} Printed;

static gboolean has_printed(gpointer data)
{
  Printed *w = data;
  return printed(w->plugin, w->text);
}

static gboolean not_running(gpointer data)
{
  return ((Plugin *)data)->state != PLUGIN_RUNNING || !((Plugin *)data)->running;
}

static gboolean run_gone(gpointer data)
{
  return process_ended(GPOINTER_TO_INT(data));
}

/* A command runs the plugin's program with the socket, plugin, command and project in its environment. */
static void test_run_command(void)
{
  App app = { 0 };
  char *out_path = g_build_filename(tmp_dir, "exec_output.txt", NULL);
  g_unlink(out_path);

  char *script = g_strdup_printf("#!/bin/sh\n"
                                 "echo \"SOCKET=$TCODE_SOCKET\" > \"%s\"\n"
                                 "echo \"PLUGIN=$TCODE_PLUGIN_ID\" >> \"%s\"\n"
                                 "echo \"CMD=$TCODE_COMMAND_ID\" >> \"%s\"\n"
                                 "echo \"API=$TCODE_API\" >> \"%s\"\n"
                                 "echo \"TOKEN=$TCODE_PLUGIN_TOKEN\" >> \"%s\"\n"
                                 "echo \"ROOT=$TCODE_PROJECT_ROOT\" >> \"%s\"\n"
                                 "echo printed\n"
                                 "exit 0\n",
                                 out_path, out_path, out_path, out_path, out_path, out_path);

  write_file("exec/runner/plugin.json",
             "{\"name\": \"Runner\", \"commands\": [{\"id\":\"go\",\"name\":\"Run Go\"}], \"executable\":\"run.sh\"}",
             0644);
  write_file("exec/runner/run.sh", script, 0755);
  g_free(script);

  app.paths.socket = g_strdup("/tmp/test-tcode.sock");
  app.project.root = g_strdup("/home/user/project");

  char *scan_path = g_build_filename(tmp_dir, "exec", NULL);
  app.plugins = g_ptr_array_new_with_free_func((GDestroyNotify)plugin_free);
  plugins_scan_dir(&app, scan_path);
  g_assert_cmpuint(app.plugins->len, ==, 1);

  plugin_run_command(&app, "runner", "go");

  /* Spin the main context until the script has written its last line (it writes them one by one:
   * the file is there before it is whole). */
  char *contents = NULL;
  for (int iters = 0; iters < 100 && !(contents && strstr(contents, "ROOT=")); iters++) {
    g_main_context_iteration(NULL, FALSE);
    g_usleep(20000); /* 20ms */
    g_clear_pointer(&contents, g_free);
    g_file_get_contents(out_path, &contents, NULL, NULL);
  }
  g_assert_nonnull(contents);
  g_assert_nonnull(strstr(contents, "SOCKET=/tmp/test-tcode.sock"));
  g_assert_nonnull(strstr(contents, "PLUGIN=runner"));
  g_assert_nonnull(strstr(contents, "CMD=go"));
  g_assert_nonnull(strstr(contents, "API=tapi_1"));
  char *token = g_strdup_printf("TOKEN=%s", plugin_find(&app, "runner")->token);
  g_assert_nonnull(strstr(contents, token));
  g_free(token);
  g_assert_nonnull(strstr(contents, "ROOT=/home/user/project"));
  g_assert_true(spin_until(has_printed, &(Printed){ plugin_find(&app, "runner"), "printed" }, 3000));
  g_assert_true(spin_until(runs_empty, NULL, 3000)); /* it ended and its output was read to the end */

  g_free(contents);
  g_free(out_path);
  g_free(scan_path);
  g_free(app.paths.socket);
  g_free(app.project.root);
  plugins_stop(&app);
}
/* A plugin with "start": "project" runs while a project is open: with command id "start", its output
 * kept, stopped (SIGTERM) when the project closes and on quit. None starts without a project. */
static void test_start_project(void)
{
  App app = { 0 };
  char *scan_path = g_build_filename(tmp_dir, "start", NULL);

  write_file("start/listener/plugin.json",
             "{\"name\": \"Listener\", \"api\": \"tapi_1\", \"start\": \"project\", \"permissions\": [\"start\"]}",
             0644);
  write_file("start/listener/run.sh",
             "#!/bin/sh\n"
             "trap 'echo \"bye from $TCODE_COMMAND_ID\"; exit 0' TERM\n"
             "echo \"hello from $1\"\n"
             "echo oops >&2\n"
             "while :; do sleep 0.05; done\n",
             0755);
  app.plugins = g_ptr_array_new_with_free_func((GDestroyNotify)plugin_free);
  plugins_scan_dir(&app, scan_path);
  Plugin *p = plugin_find(&app, "listener");
  g_assert_nonnull(p);
  g_assert_true(p->starts);
  g_assert_nonnull(p->executable); /* "start" needs a program, even without commands */

  app.project.root = g_strdup(g_get_home_dir()); /* no folder open */
  plugins_start_project(&app);
  g_assert_null(p->running);
  g_assert_cmpint(p->state, ==, PLUGIN_IDLE);

  g_set_str(&app.project.root, tmp_dir);
  plugins_start_project(&app);
  g_assert_nonnull(p->running);
  g_assert_cmpint(p->state, ==, PLUGIN_RUNNING);
  plugins_start_project(&app); /* once only */
  g_assert_cmpuint(g_list_length(runs), ==, 1);
  g_assert_true(spin_until(has_printed, &(Printed){ p, "oops" }, 3000));
  g_assert_true(printed(p, "hello from start")); /* stderr too, in order */

  plugins_stop_project(&app);
  g_assert_cmpint(p->state, ==, PLUGIN_STOPPED);
  g_assert_true(spin_until(has_printed, &(Printed){ p, "bye from start" }, 3000));
  g_assert_true(spin_until(runs_empty, NULL, 3000));

  /* t-code quits: the program goes before plugins_stop returns. */
  g_assert_true(plugin_start(&app, p));
  pid_t pid = p->running->pid;
  g_assert_true(spin_until(has_printed, &(Printed){ p, "hello" }, 3000));
  plugins_stop(&app);
  g_assert_true(process_ended(pid));
  g_assert_true(spin_until(runs_empty, NULL, 3000));

  g_free(app.project.root);
  g_free(scan_path);
}

/* One that does not go on SIGTERM is killed STOP_MS later; one that exits with an error failed. */
static void test_start_stubborn(void)
{
  App app = { 0 };
  char *scan_path = g_build_filename(tmp_dir, "stubborn", NULL);

  write_file("stubborn/deaf/plugin.json", "{\"name\": \"Deaf\", \"start\": \"project\"}", 0644);
  write_file("stubborn/deaf/run.sh", "#!/bin/sh\ntrap '' TERM\necho up\nwhile :; do sleep 0.05; done\n", 0755);
  write_file("stubborn/broken/plugin.json", "{\"name\": \"Broken\", \"start\": \"project\"}", 0644);
  write_file("stubborn/broken/run.sh", "#!/bin/sh\necho nope\nexit 3\n", 0755);
  app.plugins = g_ptr_array_new_with_free_func((GDestroyNotify)plugin_free);
  plugins_scan_dir(&app, scan_path);
  app.project.root = g_strdup(tmp_dir);

  Plugin *deaf = plugin_find(&app, "deaf"), *broken = plugin_find(&app, "broken");
  g_assert_true(deaf->starts); /* a plugin from before versions: "start" is not checked */
  g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING, "Plugin 'Broken' exited with status 3");
  plugins_start_project(&app);
  g_assert_true(spin_until(not_running, broken, 3000));
  g_test_assert_expected_messages();
  g_assert_cmpint(broken->state, ==, PLUGIN_FAILED);
  g_assert_true(printed(broken, "nope"));

  g_assert_true(spin_until(has_printed, &(Printed){ deaf, "up" }, 3000));
  pid_t pid = deaf->running->pid;
  plugin_stop(deaf);
  g_assert_false(spin_until(run_gone, GINT_TO_POINTER(pid), STOP_MS / 2));
  g_assert_true(spin_until(run_gone, GINT_TO_POINTER(pid), STOP_MS * 2));
  g_assert_true(spin_until(runs_empty, NULL, 3000));

  plugins_stop(&app);
  g_free(app.project.root);
  g_free(scan_path);
}

/* "start" is "project" only, and a tapi_1 plugin asks for the "start" permission. */
static void test_start_manifest(void)
{
  App app = { 0 };
  char *scan_path = g_build_filename(tmp_dir, "start-manifest", NULL);

  write_file("start-manifest/unasked/plugin.json",
             "{\"name\": \"Unasked\", \"api\": \"tapi_1\", \"start\": \"project\"}", 0644);
  write_file("start-manifest/unasked/run.sh", "#!/bin/sh\n", 0755);
  write_file("start-manifest/always/plugin.json", "{\"name\": \"Always\", \"start\": \"always\"}", 0644);
  write_file("start-manifest/no-program/plugin.json",
             "{\"name\": \"No Program\", \"api\": \"tapi_1\", \"start\": \"project\", \"permissions\": [\"start\"]}",
             0644);
  app.plugins = g_ptr_array_new_with_free_func((GDestroyNotify)plugin_free);
  /* One at a time: a folder's order is the file system's. */
  const char *plugins[][2] = {
    { "always", "*\"start\" can only be \"project\"*" },
    { "no-program", "*No Program*no executable*" },
    { "unasked", "*Unasked*needs \"start\" in \"permissions\"*" },
  };
  for (guint i = 0; i < G_N_ELEMENTS(plugins); i++) {
    char *dir = g_build_filename(scan_path, plugins[i][0], NULL);
    g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING, plugins[i][1]);
    load_plugin_from_dir(&app, dir, plugins[i][0]);
    g_test_assert_expected_messages();
    g_free(dir);
  }

  g_assert_false(plugin_find(&app, "unasked")->starts);
  g_assert_false(plugin_find(&app, "always")->starts);
  g_assert_null(plugin_find(&app, "no-program"));
  plugins_stop(&app);
  g_free(scan_path);
}

static void test_palette_query(void)
{
  App app = { 0 };

  write_valid_plugin();
  char *scan_path = g_build_filename(tmp_dir, "valid", NULL);
  app.plugins = g_ptr_array_new_with_free_func((GDestroyNotify)plugin_free);
  plugins_scan_dir(&app, scan_path);

  if (test_picker_rows) {
    g_ptr_array_free(test_picker_rows, TRUE);
    test_picker_rows = NULL;
  }

  int matches = plugins_palette_query(&app, "Command", 1);
  g_assert_cmpint(matches, ==, 2);
  g_assert_nonnull(test_picker_rows);
  g_assert_cmpuint(test_picker_rows->len, ==, 2);

  gboolean handled = plugins_palette_choose(&app, "plugin:test-plug:c1");
  g_assert_true(handled);

  gboolean unhandled = plugins_palette_choose(&app, "nonexistent");
  g_assert_false(unhandled);

  if (test_picker_rows) {
    g_ptr_array_free(test_picker_rows, TRUE);
    test_picker_rows = NULL;
  }

  plugins_stop(&app);
  g_free(scan_path);
}
static void test_user_config_directory(void)
{
  write_file("config-home/t-code/plugins/config-only-unique/plugin.json",
             "{\"name\":\"Config Plugin\",\"commands\":[{\"id\":\"run\",\"name\":\"Run\"}],"
             "\"executable\":\"run.sh\"}",
             0644);
  write_file("config-home/t-code/plugins/config-only-unique/run.sh", "#!/bin/sh\nexit 0\n", 0755);
  App app = { 0 };
  gboolean found = FALSE;

  plugins_init(&app);
  for (guint i = 0; app.plugins && i < app.plugins->len; i++) {
    Plugin *plugin = g_ptr_array_index(app.plugins, i);
    if (g_strcmp0(plugin->id, "config-only-unique") == 0)
      found = TRUE;
  }
  g_assert_true(found);
  plugins_stop(&app);
}

/* "lsp", "run", "language-specs" and "snippets": what is kept, and what is left out with which
 * warning. */
static void test_contributions(void)
{
  App app = { 0 };
  char *dir = g_build_filename(tmp_dir, "contributions", "matlab", NULL);
  Plugin *p;
  PluginServer *s;

  write_file("contributions/matlab/plugin.json",
             "{\"name\": \"MATLAB\", \"api\": \"tapi_1\",\n"
             " \"commands\": [{\"id\": \"matlab.run\", \"name\": \"MATLAB: Run File\"}],\n"
             " \"run\": {\".m\": \"matlab.run\", \".mlx\": \"matlab.nope\"},\n"
             " \"language-specs\": \"specs\", \"snippets\": \"../elsewhere\",\n"
             " \"lsp\": [\n"
             "  {\"argv\": [\"bin/matlab-ls\", \"--stdio\"], \"extensions\": [\".m\"], \"language-id\": \"matlab\",\n"
             "   \"options\": {\"a\": 1}, \"settings\": {\"b\": [true]}},\n"
             "  {\"name\": \"other\", \"argv\": [\"x\"], \"languages\": [\"octave\"],\n"
             "   \"language-id\": {\".m\": \"octave\"}},\n"
             "  {\"name\": \"other\", \"argv\": [\"y\"], \"languages\": [\"octave\"]},\n"
             "  {\"name\": \"no-files\", \"argv\": [\"z\"]},\n"
             "  {\"name\": \"no-argv\", \"extensions\": [\".m\"]},\n"
             "  {\"name\": \"bad-options\", \"argv\": [\"z\"], \"extensions\": [\".m\"], \"options\": 3},\n"
             "  {\"name\": \"bad-id\", \"argv\": [\"z\"], \"extensions\": [\".m\"], \"language-id\": 3},\n"
             "  7\n"
             " ]}",
             0644);
  write_file("contributions/matlab/run.sh", "#!/bin/sh\n", 0755);
  write_file("contributions/matlab/specs/matlab.lang", "<language/>", 0644);
  app.plugins = g_ptr_array_new_with_free_func((GDestroyNotify)plugin_free);
  /* In the manifest's order: the commands', then "lsp", "run", the folders. */
  g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING, "*two language servers named 'other'*");
  g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING, "*a language server says no files*");
  g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING, "*a language server needs \"argv\"*");
  g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING, "*\"options\" and \"settings\" are JSON objects*");
  g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING, "*\"language-id\" is a string*");
  g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING, "*a language server is not an object*");
  g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING, "*\"run\" for '.mlx' does not name one of its commands*");
  g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING, "*\"snippets\" is not a folder inside the plugin's*");
  load_plugin_from_dir(&app, dir, "matlab");
  g_test_assert_expected_messages();

  p = plugin_find(&app, "matlab");
  g_assert_nonnull(p);
  g_assert_cmpuint(p->servers->len, ==, 2);
  s = g_ptr_array_index(p->servers, 0);
  g_assert_cmpstr(s->name, ==, "matlab-ls"); /* argv[0]'s name */
  g_assert_cmpstr(s->argv[1], ==, "--stdio");
  g_assert_cmpstr(s->language_id, ==, "matlab");
  g_assert_cmpstr(s->options, ==, "{\"a\":1}");
  g_assert_cmpstr(s->settings, ==, "{\"b\":[true]}");
  s = g_ptr_array_index(p->servers, 1);
  g_assert_cmpstr(s->argv[0], ==, "x");
  g_assert_cmpstr(s->languages[0], ==, "octave");
  g_assert_null(s->extensions[0]);
  g_assert_cmpstr(s->language_ids[0], ==, ".m");
  g_assert_cmpstr(s->language_ids[1], ==, "octave");
  g_assert_true(g_str_has_suffix(p->language_specs, "/matlab/specs"));
  g_assert_null(p->snippets);

  g_assert_cmpuint(g_hash_table_size(p->run), ==, 1);
  g_assert_true(plugins_run_command_for(&app, "/w/script.m") == g_ptr_array_index(p->commands, 0));
  g_assert_null(plugins_run_command_for(&app, "/w/script.mlx"));
  g_assert_null(plugins_run_command_for(&app, "/w/.m")); /* only a name ending in it */
  g_assert_null(plugins_run_command_for(&app, NULL));

  char *const patterns[] = { ".m", "CMakeLists.txt", NULL };
  g_assert_true(plugin_name_matches("a.m", patterns));
  g_assert_true(plugin_name_matches("CMakeLists.txt", patterns));
  g_assert_false(plugin_name_matches("xCMakeLists.txt", patterns));
  g_assert_false(plugin_name_matches("a.mm", patterns));

  plugins_stop(&app);
  g_free(dir);
}

/* argv without the flags only one side has: clangd's --clang-tidy (from Settings) and
 * --malloc-trim (glibc only). */
static char *argv_text(char **argv)
{
  GString *text = g_string_new(NULL);

  for (int i = 0; argv[i]; i++)
    if (!g_str_has_prefix(argv[i], "--clang-tidy") && strcmp(argv[i], "--malloc-trim") != 0)
      g_string_append_printf(text, "%s ", argv[i]);
  g_strfreev(argv);
  return g_string_free(text, FALSE);
}

/* tests/plugins: every built-in server again as a test plugin. With the built-in ones off, each file
 * goes to its plugin's server, which starts like the built-in one (program, flags, options,
 * settings, languageId); turned on, an installed built-in one wins again. */
static void test_lsp_test_plugins(void)
{
  static const struct {
    const char *name, *source; /* source: GtkSourceView's language */
    ServerKind kind;
  } files[] = {
    { "a.c", "c", SERVER_CLANGD },
    { "a.h", "chdr", SERVER_CLANGD },
    { "a.cpp", "cpp", SERVER_CLANGD },
    { "a.py", "python", SERVER_PYTHON },
    { "a.go", "go", SERVER_GO },
    { "a.rs", "rust", SERVER_RUST },
    { "a.ts", "typescript", SERVER_TYPESCRIPT },
    { "a.tsx", "typescript-jsx", SERVER_TYPESCRIPT },
    { "a.mjs", "js", SERVER_TYPESCRIPT },
    { "a.jsx", "js", SERVER_TYPESCRIPT },
    { "a.json", "json", SERVER_JSON },
    { "a.jsonc", "json", SERVER_JSON },
    { "a.yml", "yaml", SERVER_YAML },
    { "a.md", "markdown", SERVER_MARKDOWN },
    { "CMakeLists.txt", "cmake", SERVER_CMAKE },
    { "a.cmake", "cmake", SERVER_CMAKE },
  };
  GtkSourceLanguageManager *languages = gtk_source_language_manager_get_default();
  App app = { 0 };
  gboolean served[SERVER_NONE] = { 0 };

  app.plugins = g_ptr_array_new_with_free_func((GDestroyNotify)plugin_free);
  plugins_scan_dir(&app, "tests/plugins");
  g_assert_cmpuint(app.plugins->len, ==, SERVER_NONE);
  for (guint i = 0; i < G_N_ELEMENTS(files); i++) {
    Tab tab = { .kind = TAB_EDITOR, .app = &app };
    ServerKind builtin = files[i].kind, kind;
    char *plugin_id = g_strconcat("lsp-", server_id(builtin), NULL), *mine, *theirs;

    tab.path = g_build_filename("/tmp/t-code-test", files[i].name, NULL);
    tab.buffer =
        gtk_source_buffer_new_with_language(gtk_source_language_manager_get_language(languages, files[i].source));
    g_assert_nonnull(plugin_find(&app, plugin_id));

    app.settings.servers_off = ~0u;
    kind = server_kind_for_tab(&tab);
    g_assert_cmpint(kind, >=, SERVER_PLUGIN);
    g_assert_cmpstr(server_name(kind), ==, server_id(builtin));
    g_assert_true(server_installed(&app, kind));
    g_assert_null(server_missing(&app, kind)); /* no install offers for a plugin's */
    g_assert_null(server_language(kind));
    g_assert_cmpstr(server_language_id(kind, &tab), ==, server_language_id(builtin, &tab));
    mine = argv_text(server_argv(&app, kind));
    theirs = argv_text(server_argv(&app, builtin));
    g_assert_cmpstr(mine, ==, theirs);
    g_free(mine);
    g_free(theirs);
    mine = server_options(kind, &app), theirs = server_options(builtin, &app);
    g_assert_cmpstr(mine, ==, theirs);
    g_free(mine);
    g_free(theirs);
    mine = server_settings(kind, &app), theirs = server_settings(builtin, &app);
    g_assert_cmpstr(mine, ==, theirs);
    g_free(mine);
    g_free(theirs);
    g_assert_cmpint(server_kind_for_tab(&tab), ==, kind); /* the same number every time */

    app.settings.servers_off = 0; /* on: the built-in one, when it is there */
    g_assert_cmpint(server_kind_for_tab(&tab), ==, server_installed(&app, builtin) ? builtin : kind);
    served[builtin] = TRUE;
    g_object_unref(tab.buffer);
    g_free(tab.path);
    g_free(plugin_id);
  }
  for (int kind = 0; kind < SERVER_NONE; kind++)
    g_assert_true(served[kind]);
  plugins_stop(&app);
}

/* Settings' ids for the built-in servers. */
static void test_server_ids(void)
{
  App app = { 0 };

  for (int kind = 0; kind < SERVER_NONE; kind++) {
    g_assert_cmpint(server_by_id(server_id(kind)), ==, kind);
    g_assert_nonnull(server_title(kind));
    g_assert_true(server_enabled(&app, kind));
  }
  g_assert_cmpint(server_by_id("nope"), ==, SERVER_NONE);
  g_assert_cmpint(server_by_id(NULL), ==, SERVER_NONE);
  app.settings.servers_off = 1u << SERVER_GO;
  g_assert_false(server_enabled(&app, SERVER_GO));
  g_assert_true(server_enabled(&app, SERVER_RUST));
  g_assert_true(server_enabled(&app, SERVER_PLUGIN));
}

int main(int argc, char **argv)
{
  tmp_dir = g_dir_make_tmp("t-code-test-plugins-XXXXXX", NULL);
  char *config_home = g_build_filename(tmp_dir, "config-home", NULL);
  g_setenv("XDG_CONFIG_HOME", config_home, TRUE);
  g_free(config_home);
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/plugins/valid-plugin", test_valid_plugin);
  g_test_add_func("/plugins/invalid-plugins", test_invalid_plugins);
  g_test_add_func("/plugins/tapi-manifest", test_tapi_manifest);
  g_test_add_func("/plugins/run-command", test_run_command);
  g_test_add_func("/plugins/start-project", test_start_project);
  g_test_add_func("/plugins/start-stubborn", test_start_stubborn);
  g_test_add_func("/plugins/start-manifest", test_start_manifest);
  g_test_add_func("/plugins/contributions", test_contributions);
  g_test_add_func("/plugins/lsp-test-plugins", test_lsp_test_plugins);
  g_test_add_func("/plugins/server-ids", test_server_ids);
  g_test_add_func("/plugins/palette-query", test_palette_query);
  g_test_add_func("/plugins/user-config-directory", test_user_config_directory);

  int status = g_test_run();

  char *rm[] = { "rm", "-rf", tmp_dir, NULL };
  g_spawn_sync(NULL, rm, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, NULL, NULL);
  g_free(tmp_dir);
  return status;
}
