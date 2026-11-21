/*
 * The login shell's environment (GLib/GIO-only). A t-code started from the desktop's menu has the
 * session's environment, without what the user's shell start-up files add: PATH folders (nvm, a
 * Node.js unpacked in the home folder, pyenv, asdf, mise, ...) and the toolchains' variables (GOPATH,
 * CARGO_HOME, JAVA_HOME, ...). Language servers, debuggers and test runners installed there were
 * "not installed". At start-up the shell runs once as a login, interactive shell and prints them:
 * the PATH folders t-code lacks are added at its end, so every program found before is still the
 * one found, and a variable is set only where t-code has none.
 */
#pragma once

#include <glib.h>

/* The variables taken from the shell, PATH first; NULL-terminated. */
extern const char *const shell_env_names[];

/* name -> value of what the shell printed between the markers of the command shell_env_load runs
 * (start-up files may print anything around it); unset, empty and multi-line values left out. NULL
 * when the markers are not both there (the shell was killed or never ran the command). */
GHashTable *shell_env_parse(const char *output);

/* current's folders, then those of path that current lacks, in path's order (empty ones left out).
 * current may be NULL. */
char *shell_env_merge_path(const char *current, const char *path);

/* TRUE from shell_env_load until its done runs: programs that keep running (plugins) wait. */
gboolean shell_env_loading(void);

/* changed: this process's environment got something new. */
typedef void (*ShellEnvDone)(gboolean changed, gpointer data);

/* Runs `shell -l -i -c ...` in a new session, without a terminal, and takes what it prints into this
 * process's environment (g_setenv) as described above. Gives up after timeout_ms (the shell and what
 * it started get SIGKILL). done runs in the main loop either way. */
void shell_env_load(const char *shell, guint timeout_ms, ShellEnvDone done, gpointer data);
