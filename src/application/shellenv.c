/*
 * The login shell's environment (see shellenv.h).
 */
#define _DEFAULT_SOURCE /* kill() under -std=c23 with musl */
#include "application/shellenv.h"

#include <gio/gio.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define START "__TCODE_ENV_START__"
#define END "__TCODE_ENV_END__"
#define SEPARATOR "\036" /* ASCII record separator: before each NAME=value */

/* PATH, then what the toolchains' installers and version managers set for their programs to run:
 * Go (gopls and dlv in $GOBIN or $GOPATH/bin), Rust (rustup's proxies need RUSTUP_HOME when it is
 * not ~/.rustup), Python (pyenv's shims), Node.js, Java (jdtls, Maven, Gradle) and the managers
 * that serve several languages. */
const char *const shell_env_names[] = {
  "PATH",         "GOPATH",           "GOBIN",      "GOROOT",        "CARGO_HOME",    "RUSTUP_HOME",  "PYENV_ROOT",
  "PIPX_BIN_DIR", "NVM_DIR",          "VOLTA_HOME", "PNPM_HOME",     "BUN_INSTALL",   "DENO_INSTALL", "JAVA_HOME",
  "SDKMAN_DIR",   "GRADLE_USER_HOME", "ASDF_DIR",   "ASDF_DATA_DIR", "MISE_DATA_DIR", NULL,
};

/* For each name: the separator and "NAME=", then its value from printenv (nothing when it is not
 * set). printf and printenv work the same in sh, bash, zsh and fish, and printenv prints PATH with
 * colons in every shell (fish keeps it as a list). */
static char *command(void)
{
  GString *text = g_string_new("printf '%s' " START ";");

  for (int i = 0; shell_env_names[i]; i++)
    g_string_append_printf(text, " printf '%%s' '" SEPARATOR "%s='; printenv %s;", shell_env_names[i],
                           shell_env_names[i]);
  g_string_append(text, " printf '%s' " END);
  return g_string_free(text, FALSE);
}

GHashTable *shell_env_parse(const char *output)
{
  const char *start = output ? strstr(output, START) : NULL;
  const char *end = start ? strstr(start + strlen(START), END) : NULL;

  if (!end)
    return NULL;
  GHashTable *env = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  char *inside = g_strndup(start + strlen(START), end - start - strlen(START));
  char **records = g_strsplit(inside, SEPARATOR, -1);

  for (int i = 0; records[i]; i++) {
    char *equals = strchr(records[i], '=');
    if (!equals || equals == records[i])
      continue;
    char *value = g_strstrip(g_strdup(equals + 1));
    if (*value && !strchr(value, '\n'))
      g_hash_table_replace(env, g_strndup(records[i], equals - records[i]), g_steal_pointer(&value));
    g_free(value);
  }
  g_strfreev(records);
  g_free(inside);
  return env;
}

char *shell_env_merge_path(const char *current, const char *path)
{
  char **have = g_strsplit(current ? current : "", ":", -1), **add = g_strsplit(path ? path : "", ":", -1);
  GString *merged = g_string_new(current ? current : "");
  GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);

  for (int i = 0; have[i]; i++)
    g_hash_table_add(seen, have[i]);
  for (int i = 0; add[i]; i++)
    if (*add[i] && !g_hash_table_contains(seen, add[i])) {
      g_hash_table_add(seen, add[i]);
      if (merged->len)
        g_string_append_c(merged, ':');
      g_string_append(merged, add[i]);
    }
  g_hash_table_destroy(seen);
  g_strfreev(add);
  g_strfreev(have);
  return g_string_free(merged, FALSE);
}

/* Takes env into this process's environment; TRUE when something changed, which is described in
 * what (the PATH folders added, the variables set). */
static gboolean apply(GHashTable *env, GString *what)
{
  gboolean changed = FALSE;

  for (int i = 0; shell_env_names[i]; i++) {
    const char *name = shell_env_names[i], *value = g_hash_table_lookup(env, name), *current = g_getenv(name);
    if (!value)
      continue;
    if (strcmp(name, "PATH") == 0) {
      char *merged = shell_env_merge_path(current, value);
      if (g_strcmp0(merged, current) != 0) {
        size_t old = current ? strlen(current) : 0;
        g_string_append_printf(what, "PATH + %s", merged + old + (old > 0 && merged[old] == ':'));
        g_setenv(name, merged, TRUE);
        changed = TRUE;
      }
      g_free(merged);
    } else if (!current || !*current) {
      g_string_append_printf(what, "%s%s=%s", what->len ? "; " : "", name, value);
      g_setenv(name, value, TRUE);
      changed = TRUE;
    }
  }
  return changed;
}

static gboolean loading;

gboolean shell_env_loading(void)
{
  return loading;
}

typedef struct {
  char *shell;
  GSubprocess *process;
  GCancellable *cancel;
  guint timeout_id, timeout_ms;
  gboolean timed_out;
  ShellEnvDone done;
  gpointer data;
} Load;

/* A session of its own: no controlling terminal to read from, and one process group to kill. */
static void new_session(gpointer data G_GNUC_UNUSED)
{
  setsid();
}

static gboolean on_timeout(gpointer data)
{
  Load *load = data;
  const char *pid = g_subprocess_get_identifier(load->process);

  load->timeout_id = 0;
  load->timed_out = TRUE;
  if (pid)
    kill(-(pid_t)atoi(pid), SIGKILL);
  g_cancellable_cancel(load->cancel);
  return G_SOURCE_REMOVE;
}

static void on_output(GObject *source, GAsyncResult *result, gpointer data)
{
  Load *load = data;
  char *output = NULL;
  GHashTable *env = NULL;
  gboolean changed = FALSE;

  if (g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result, &output, NULL, NULL))
    env = shell_env_parse(output);
  /* In the log: a server "not installed" may be one this did not find (or that it did). */
  if (env) {
    GString *what = g_string_new(NULL);
    changed = apply(env, what);
    g_message("t-code: login shell %s: %s", load->shell, changed ? what->str : "nothing new");
    g_string_free(what, TRUE);
    g_hash_table_unref(env);
  } else if (load->timed_out) {
    g_warning("t-code: the login shell %s gave no environment in %u s (its start-up files wait for something): "
              "programs on the PATH they set may not be found",
              load->shell, load->timeout_ms / 1000);
  } else {
    g_warning("t-code: the login shell %s printed no environment (it ended first): programs on the PATH its "
              "start-up files set may not be found",
              load->shell);
  }
  g_clear_handle_id(&load->timeout_id, g_source_remove);
  loading = FALSE;
  load->done(changed, load->data);
  g_free(output);
  g_object_unref(load->process);
  g_object_unref(load->cancel);
  g_free(load->shell);
  g_free(load);
}

static gboolean report_failure(gpointer data)
{
  Load *load = data;

  load->done(FALSE, load->data);
  g_free(load->shell);
  g_free(load);
  return G_SOURCE_REMOVE;
}

void shell_env_load(const char *shell, guint timeout_ms, ShellEnvDone done, gpointer data)
{
  char *script = command();
  const char *argv[] = { shell, "-l", "-i", "-c", script, NULL };
  GSubprocessLauncher *launcher =
      g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE);
  Load *load = g_new0(Load, 1);
  GError *error = NULL;

  load->shell = g_strdup(shell);
  load->done = done;
  load->data = data;
  load->timeout_ms = timeout_ms;
  g_subprocess_launcher_set_child_setup(launcher, new_session, NULL, NULL);
  load->process = shell ? g_subprocess_launcher_spawnv(launcher, argv, &error) : NULL;
  g_object_unref(launcher);
  g_free(script);
  if (!load->process) {
    g_warning("t-code: cannot run the login shell %s for its PATH: %s", shell ? shell : "(none)",
              error ? error->message : "no shell");
    g_clear_error(&error);
    g_idle_add(report_failure, load);
    return;
  }
  loading = TRUE;
  load->cancel = g_cancellable_new();
  load->timeout_id = g_timeout_add(timeout_ms, on_timeout, load);
  g_subprocess_communicate_utf8_async(load->process, NULL, load->cancel, on_output, load);
}
