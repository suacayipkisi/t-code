/*
 * .t-code/tasks.json (or t-code.json) in a project's root (see runner.h): its commands for build,
 * run, debug and test, with args, env and cwd; "profiles" with a "default" one, or the commands at
 * its top.
 */
#include "project/run/runner.h"

#include <json-glib/json-glib.h>

typedef struct {
  char *name;
  char *build;
  char *run;
  char *debug;
  char *test;
  char *args;
  char *env;
  char *cwd;
} TaskProfile;

static void task_profile_clear(TaskProfile *tp)
{
  g_free(tp->name);
  g_free(tp->build);
  g_free(tp->run);
  g_free(tp->debug);
  g_free(tp->test);
  g_free(tp->args);
  g_free(tp->env);
  g_free(tp->cwd);
}

static char *parse_task_args(JsonObject *obj)
{
  if (!json_object_has_member(obj, "args"))
    return NULL;
  JsonNode *node = json_object_get_member(obj, "args");
  if (JSON_NODE_HOLDS_ARRAY(node)) {
    JsonArray *arr = json_node_get_array(node);
    guint len = json_array_get_length(arr);
    GString *s = g_string_new("");
    for (guint i = 0; i < len; i++) {
      const char *arg = json_array_get_string_element(arr, i);
      char *quoted = g_shell_quote(arg ? arg : "");
      if (s->len > 0)
        g_string_append_c(s, ' ');
      g_string_append(s, quoted);
      g_free(quoted);
    }
    return g_string_free(s, FALSE);
  }
  if (JSON_NODE_HOLDS_VALUE(node)) {
    const char *str = json_node_get_string(node);
    return str ? g_strdup(str) : NULL;
  }
  return NULL;
}

static char *parse_task_env(JsonObject *obj)
{
  if (!json_object_has_member(obj, "env"))
    return NULL;
  JsonNode *node = json_object_get_member(obj, "env");
  if (!JSON_NODE_HOLDS_OBJECT(node))
    return NULL;
  JsonObject *env_obj = json_node_get_object(node);
  GList *members = json_object_get_members(env_obj);
  GString *s = g_string_new("");
  for (GList *l = members; l; l = l->next) {
    const char *key = l->data;
    const char *val = json_object_get_string_member(env_obj, key);
    char *quoted = g_shell_quote(val ? val : "");
    if (s->len > 0)
      g_string_append_c(s, ' ');
    g_string_append_printf(s, "%s=%s", key, quoted);
    g_free(quoted);
  }
  g_list_free(members);
  return g_string_free(s, FALSE);
}

static gboolean load_task_profile_from_obj(JsonObject *obj, const char *name, TaskProfile *tp)
{
  tp->name = g_strdup(name);
  if (json_object_has_member(obj, "build"))
    tp->build = g_strdup(json_object_get_string_member(obj, "build"));
  if (json_object_has_member(obj, "run"))
    tp->run = g_strdup(json_object_get_string_member(obj, "run"));
  if (json_object_has_member(obj, "debug"))
    tp->debug = g_strdup(json_object_get_string_member(obj, "debug"));
  if (json_object_has_member(obj, "test"))
    tp->test = g_strdup(json_object_get_string_member(obj, "test"));
  if (json_object_has_member(obj, "cwd"))
    tp->cwd = g_strdup(json_object_get_string_member(obj, "cwd"));
  tp->args = parse_task_args(obj);
  tp->env = parse_task_env(obj);
  return tp->build != NULL || tp->run != NULL || tp->debug != NULL || tp->test != NULL;
}

static gboolean load_task_profile(const char *root, TaskProfile *tp)
{
  char *paths[] = { g_build_filename(root, ".t-code", "tasks.json", NULL), g_build_filename(root, "t-code.json", NULL),
                    NULL };
  char *chosen = NULL;
  for (int i = 0; paths[i]; i++) {
    if (g_file_test(paths[i], G_FILE_TEST_IS_REGULAR)) {
      chosen = paths[i];
      break;
    }
  }
  if (!chosen) {
    for (int i = 0; paths[i]; i++)
      g_free(paths[i]);
    return FALSE;
  }

  JsonParser *parser = json_parser_new();
  gboolean ok = FALSE;
  if (json_parser_load_from_file(parser, chosen, NULL)) {
    JsonNode *rnode = json_parser_get_root(parser);
    if (rnode && JSON_NODE_HOLDS_OBJECT(rnode)) {
      JsonObject *root_obj = json_node_get_object(rnode);
      if (json_object_has_member(root_obj, "profiles")) {
        JsonNode *pnode = json_object_get_member(root_obj, "profiles");
        if (JSON_NODE_HOLDS_OBJECT(pnode)) {
          JsonObject *profiles_obj = json_node_get_object(pnode);
          const char *target =
              json_object_has_member(root_obj, "default") ? json_object_get_string_member(root_obj, "default") : NULL;
          JsonObject *target_obj = NULL;
          if (target && json_object_has_member(profiles_obj, target)) {
            target_obj = json_object_get_object_member(profiles_obj, target);
          } else {
            GList *m = json_object_get_members(profiles_obj);
            if (m) {
              target = m->data;
              target_obj = json_object_get_object_member(profiles_obj, target);
              g_list_free(m);
            }
          }
          if (target_obj)
            ok = load_task_profile_from_obj(target_obj, target ? target : "default", tp);
        }
      }
      if (!ok)
        ok = load_task_profile_from_obj(root_obj, "default", tp);
    }
  }
  g_object_unref(parser);
  for (int i = 0; paths[i]; i++)
    g_free(paths[i]);
  return ok;
}

/* The folder a task profile's commands run in: its cwd (relative to root, the tasks file's folder)
 * or root. */
static char *task_folder(const char *root, const TaskProfile *tp)
{
  if (!tp->cwd)
    return g_strdup(root);
  return g_path_is_absolute(tp->cwd) ? g_strdup(tp->cwd) : g_build_filename(root, tp->cwd, NULL);
}

/* The shell command that builds, runs or debugs according to .t-code/tasks.json (or t-code.json). */
char *tasks_command(const char *root, Goal goal)
{
  TaskProfile tp = { 0 };
  if (!load_task_profile(root, &tp))
    return goal == GOAL_TEST ? g_strdup("echo 't-code: could not read .t-code/tasks.json'; false")
                             : g_strdup("echo 't-code: could not read .t-code/tasks.json'");

  char *workdir = task_folder(root, &tp);
  char *qdir = g_shell_quote(workdir);
  char *cmd = NULL;

  if (goal == GOAL_BUILD) {
    if (tp.build)
      cmd = g_strdup_printf("cd %s && %s", qdir, tp.build);
    else
      cmd = g_strdup("echo 'Build OK (no build command configured in tasks.json)'");
  } else if (goal == GOAL_TEST) {
    cmd = tp.test && *tp.test ? g_strdup_printf("cd %s && %s%s%s", qdir, tp.env && *tp.env ? tp.env : "",
                                                tp.env && *tp.env ? " " : "", tp.test)
                              : g_strdup("echo 't-code: no test command configured in tasks.json'; false");
  } else if (goal == GOAL_RUN) {
    const char *run_cmd = tp.run ? tp.run : tp.debug;
    if (run_cmd) {
      GString *exec_line = g_string_new("");
      g_string_append_printf(exec_line, "cd %s && ", qdir);
      if (tp.env && *tp.env)
        g_string_append_printf(exec_line, "%s ", tp.env);
      g_string_append(exec_line, run_cmd);
      if (tp.args && *tp.args)
        g_string_append_printf(exec_line, " %s", tp.args);

      if (tp.build)
        cmd = g_strdup_printf("cd %s && %s && %s", qdir, tp.build, exec_line->str);
      else
        cmd = g_strdup(exec_line->str);
      g_string_free(exec_line, TRUE);
    } else {
      cmd = g_strdup("echo 't-code: no run command configured in tasks.json'");
    }
  } else if (goal == GOAL_DEBUG) {
    const char *dbg_exe = tp.debug ? tp.debug : tp.run;
    if (dbg_exe) {
      char *full_exe = g_path_is_absolute(dbg_exe) ? g_strdup(dbg_exe) : g_build_filename(workdir, dbg_exe, NULL);
      char *qexe = g_shell_quote(full_exe);
      char *debug = send_verb("debug", qexe);
      if (tp.build)
        cmd = g_strdup_printf("cd %s && %s && %s", qdir, tp.build, debug);
      else
        cmd = g_strdup(debug);
      g_free(debug);
      g_free(qexe);
      g_free(full_exe);
    } else {
      cmd = g_strdup("echo 't-code: no debug target configured in tasks.json'");
    }
  }

  g_free(qdir);
  g_free(workdir);
  task_profile_clear(&tp);
  return cmd ? cmd : g_strdup("true");
}

char *tasks_name(const char *root)
{
  TaskProfile tp = { 0 };
  char *name = load_task_profile(root, &tp) ? g_strdup(tp.name) : NULL;

  task_profile_clear(&tp);
  return name;
}

char *tasks_folder(const char *root)
{
  TaskProfile tp = { 0 };
  char *folder = load_task_profile(root, &tp) ? task_folder(root, &tp) : g_strdup(root);

  task_profile_clear(&tp);
  return folder;
}
