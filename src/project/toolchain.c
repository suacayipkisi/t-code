/*
 * The project's Python interpreter and Rust features and target (see toolchain.h), kept in the
 * session as the project's "python", "rust-features" and "rust-target", and in memory: the status
 * bar asks for them on every diagnostics update.
 */
#include "project/toolchain.h"

#include "editor/tabs.h"
#include "lsp/client.h"
#include "popups/picker.h"
#include "project/langenv.h"
#include "project/session.h"
#include "window/statusbar.h"

#include <string.h>

#define RESTART_MS 1500 /* features are switched one by one: the server restarts once for them */

enum { PYTHON, FEATURES, TARGET, KEYS };
static const char *const keys[KEYS] = { "python", "rust-features", "rust-target" };

static struct {
  char *root; /* the project they are for */
  char *values[KEYS];
} kept;

static struct {
  App *app;
  const char *language;
  guint id;
} restart;

/* The project's value for key (NULL: the default), read from the session once per project. */
static const char *value_of(App *t, int key)
{
  if (g_strcmp0(kept.root, t->project.root) != 0) {
    g_set_str(&kept.root, t->project.root);
    for (int i = 0; i < KEYS; i++) {
      g_free(kept.values[i]);
      kept.values[i] = session_project_value(t, keys[i]);
    }
  }
  return kept.values[key];
}

static gboolean restart_now(gpointer data G_GNUC_UNUSED)
{
  restart.id = 0;
  if (!restart.app->closing)
    lsp_restart_language(restart.app, restart.language);
  return G_SOURCE_REMOVE;
}

void toolchain_set(App *t, const char *key, const char *value)
{
  int k = 0;
  const char *language;

  while (k < KEYS && strcmp(keys[k], key) != 0)
    k++;
  g_return_if_fail(k < KEYS);
  language = k == PYTHON ? "python" : "rust";
  value_of(t, k); /* this project's values */
  g_set_str(&kept.values[k], value && *value ? value : NULL);
  session_set_project_value(t, key, kept.values[k]);
  if (restart.id && (restart.app != t || g_strcmp0(restart.language, language) != 0)) {
    g_source_remove(restart.id);
    restart_now(NULL); /* the other one now */
  }
  g_clear_handle_id(&restart.id, g_source_remove);
  restart.app = t;
  restart.language = language;
  restart.id = g_timeout_add(RESTART_MS, restart_now, NULL);
  update_status(t);
}

/* Python ------------------------------------------------------------------------------------- */

static GPtrArray *interpreters; /* of the picker that is open */

char *toolchain_python(App *t, const char *root)
{
  const char *python = value_of(t, PYTHON);

  if (python && g_file_test(python, G_FILE_TEST_IS_EXECUTABLE))
    return g_strdup(python);
  return langenv_venv_python(root ? root : t->project.root);
}

static void python_query(App *t, const char *text, guint generation)
{
  char *current = toolchain_python(t, NULL);

  for (guint i = 0; i < interpreters->len; i++) {
    const char *python = g_ptr_array_index(interpreters, i);
    char *name = langenv_interpreter_name(python),
         *label = g_strdup_printf("%s%s", name, g_strcmp0(current, python) ? "" : " ✓");
    if (!*text || fuzzy_score(text, python) >= 0)
      picker_add(t, generation, label, python, python);
    g_free(label);
    g_free(name);
  }
  if (!interpreters->len)
    picker_message(t, generation, "No Python found: make a venv (python3 -m venv .venv)");
  g_free(current);
}

static void python_choose(App *t, const char *payload)
{
  toolchain_set(t, "python", payload);
}

void toolchain_pick_python(App *t)
{
  if (interpreters)
    g_ptr_array_unref(interpreters);
  interpreters = langenv_interpreters(t->project.root);
  picker_open(t, "Python interpreter for this project", python_query, python_choose);
}

/* Rust --------------------------------------------------------------------------------------- */

char *toolchain_rust_features(App *t)
{
  return g_strdup(value_of(t, FEATURES));
}

char *toolchain_rust_target(App *t)
{
  return g_strdup(value_of(t, TARGET));
}

static GStrv features; /* Cargo.toml's, for the picker that is open */

/* The Cargo.toml of the current file's crate, else the project's. */
static char *cargo_toml(App *t)
{
  Tab *tab = current_tab(t);
  char *dir = tab && tab->kind == TAB_EDITOR && tab->path ? g_path_get_dirname(tab->path) : g_strdup(t->project.root);

  char *path = NULL;

  while (!path) {
    char *candidate = g_build_filename(dir, "Cargo.toml", NULL), *up = g_path_get_dirname(dir);
    gboolean top = strcmp(up, dir) == 0;
    if (g_file_test(candidate, G_FILE_TEST_IS_REGULAR))
      path = candidate;
    else
      g_free(candidate);
    g_free(dir);
    dir = up;
    if (top)
      break;
  }
  g_free(dir);
  if (path)
    return path;
  return g_build_filename(t->project.root, "Cargo.toml", NULL);
}

/* Whether name is in the comma-separated list. */
static gboolean listed(const char *list, const char *name)
{
  char **names = list ? g_strsplit(list, ",", -1) : NULL;
  gboolean found = names && g_strv_contains((const char *const *)names, name);

  g_strfreev(names);
  return found;
}

static void features_query(App *t, const char *text, guint generation)
{
  char *chosen = toolchain_rust_features(t);
  gboolean all = g_strcmp0(chosen, "all") == 0;

  if (!*text) {
    picker_add(t, generation, !chosen ? "Default features ✓" : "Default features", "cargo's", ":default");
    picker_add(t, generation, all ? "All features ✓" : "All features", "--all-features", ":all");
  }
  for (int i = 0; features && features[i]; i++) {
    if (*text && fuzzy_score(text, features[i]) < 0)
      continue;
    char *label = g_strdup_printf("%s%s", features[i], !all && listed(chosen, features[i]) ? " ✓" : "");
    picker_add(t, generation, label, "switch on or off", features[i]);
    g_free(label);
  }
  if ((!features || !features[0]) && *text)
    picker_message(t, generation, "Cargo.toml has no [features]");
  g_free(chosen);
}

static gboolean reopen_features(gpointer data)
{
  toolchain_pick_features(data);
  return G_SOURCE_REMOVE;
}

/* The default or all features, or one switched on or off (the list opens again for the next). */
static void features_choose(App *t, const char *payload)
{
  char *chosen = toolchain_rust_features(t);
  GString *list = g_string_new(NULL);

  if (*payload == ':') { /* ":default" or ":all": no feature has a colon */
    toolchain_set(t, "rust-features", strcmp(payload + 1, "all") == 0 ? "all" : NULL);
  } else {
    gboolean on = g_strcmp0(chosen, "all") == 0 || !listed(chosen, payload);
    char **names = chosen && g_strcmp0(chosen, "all") ? g_strsplit(chosen, ",", -1) : g_new0(char *, 1);
    for (int i = 0; names[i]; i++)
      if (strcmp(names[i], payload) != 0)
        g_string_append_printf(list, "%s%s", list->len ? "," : "", names[i]);
    if (on)
      g_string_append_printf(list, "%s%s", list->len ? "," : "", payload);
    toolchain_set(t, "rust-features", list->str);
    g_idle_add(reopen_features, t);
    g_strfreev(names);
  }
  g_string_free(list, TRUE);
  g_free(chosen);
}

void toolchain_pick_features(App *t)
{
  char *path = cargo_toml(t);

  g_strfreev(features);
  features = langenv_cargo_features(path);
  picker_open(t, "Cargo features for rust-analyzer, Build, Run and Test", features_query, features_choose);
  g_free(path);
}

static GStrv targets; /* installed, for the picker that is open */

static void target_query(App *t, const char *text, guint generation)
{
  char *chosen = toolchain_rust_target(t);

  if (!*text)
    picker_add(t, generation, !chosen ? "Host ✓" : "Host", "this machine's", "");
  for (int i = 0; targets && targets[i]; i++) {
    if (!*targets[i] || (*text && fuzzy_score(text, targets[i]) < 0))
      continue;
    char *label = g_strdup_printf("%s%s", targets[i], g_strcmp0(chosen, targets[i]) ? "" : " ✓");
    picker_add(t, generation, label, NULL, targets[i]);
    g_free(label);
  }
  if (!targets)
    picker_message(t, generation, "rustup is not installed: only the host target");
  g_free(chosen);
}

static void target_choose(App *t, const char *payload)
{
  toolchain_set(t, "rust-target", payload);
}

void toolchain_pick_target(App *t)
{
  const char *argv[] = { "rustup", "target", "list", "--installed", NULL };
  char *out = NULL;

  g_clear_pointer(&targets, g_strfreev);
  if (g_spawn_sync(NULL, (char **)argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, &out, NULL,
                   NULL, NULL))
    targets = g_strsplit(g_strstrip(out), "\n", -1);
  g_free(out);
  picker_open(t, "Rust target for rust-analyzer, Build, Run and Test", target_query, target_choose);
}
