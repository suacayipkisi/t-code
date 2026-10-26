/* Tests for the plugins (src/plugins/plugins.c, included for its static functions). */
#include "plugins/plugins.c"

#include <glib/gstdio.h>

static char *tmp_dir;

/* clang-format off */
Tab *current_tab(App *t G_GNUC_UNUSED) { return NULL; }
void status_say(App *t G_GNUC_UNUSED, const char *message G_GNUC_UNUSED) {}
void refresh_shortcuts(App *t G_GNUC_UNUSED) {}
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
  char *full = g_build_filename(tmp_dir, rel_path, NULL);
  char *dir = g_path_get_dirname(full);
  g_mkdir_with_parents(dir, 0755);
  g_free(dir);
  g_file_set_contents(full, content, -1, NULL);
  if (mode != 0)
    g_chmod(full, mode);
  g_free(full);
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

  /* Missing commands */
  write_file("invalid/p2/plugin.json", "{\"name\": \"P2\", \"executable\":\"run.sh\"}", 0644);
  write_file("invalid/p2/run.sh", "#!/bin/sh\nexit 0\n", 0755);

  /* Empty commands array */
  write_file("invalid/p3/plugin.json", "{\"name\": \"P3\", \"commands\": [], \"executable\":\"run.sh\"}", 0644);
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
  for (int i = 0; i < 5; i++)
    g_test_expect_message(NULL, G_LOG_LEVEL_WARNING, "*");
  plugins_scan_dir(&app, scan_path);
  g_test_assert_expected_messages();

  g_assert_cmpuint(app.plugins->len, ==, 0);

  plugins_stop(&app);
  g_free(scan_path);
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
                                 "echo \"ROOT=$TCODE_PROJECT_ROOT\" >> \"%s\"\n"
                                 "exit 0\n",
                                 out_path, out_path, out_path, out_path);

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
  g_assert_nonnull(strstr(contents, "ROOT=/home/user/project"));

  g_free(contents);
  g_free(out_path);
  g_free(scan_path);
  g_free(app.paths.socket);
  g_free(app.project.root);
  plugins_stop(&app);
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

int main(int argc, char **argv)
{
  tmp_dir = g_dir_make_tmp("t-code-test-plugins-XXXXXX", NULL);
  char *config_home = g_build_filename(tmp_dir, "config-home", NULL);
  g_setenv("XDG_CONFIG_HOME", config_home, TRUE);
  g_free(config_home);
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/plugins/valid-plugin", test_valid_plugin);
  g_test_add_func("/plugins/invalid-plugins", test_invalid_plugins);
  g_test_add_func("/plugins/run-command", test_run_command);
  g_test_add_func("/plugins/palette-query", test_palette_query);
  g_test_add_func("/plugins/user-config-directory", test_user_config_directory);

  int status = g_test_run();

  char *rm[] = { "rm", "-rf", tmp_dir, NULL };
  g_spawn_sync(NULL, rm, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, NULL, NULL);
  g_free(tmp_dir);
  return status;
}
