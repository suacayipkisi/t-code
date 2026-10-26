/*
 * git commands for the Source Control panel (see state.h): those that change the repository run one
 * after another (two at once would fight over .git/index.lock), background reads at once; the
 * panel's note shows a failure.
 */
#include "git/scm/state.h"

#include "git/gitstatus.h"

/* Running git -------------------------------------------------------------------------------- */

typedef struct {
  char **argv; /* git and its arguments */
  char *input; /* for its stdin, or NULL */
  GitDone done;
  gpointer data;
  GDestroyNotify drop; /* releases data if the call is canceled or git cannot start */
  gboolean quiet;      /* a background read: runs at once and does not hold the buttons */
} GitCall;

/* Frees a GitCall. */
static void git_call_free(GitCall *call)
{
  if (call->drop)
    call->drop(call->data);
  g_strfreev(call->argv);
  g_free(call->input);
  g_free(call);
}

static gboolean start_git(GitCall *call);

/* The next command that changes the repository, once none runs: two at once would fight over
 * .git/index.lock. */
static void start_waiting(void)
{
  GitCall *call;

  while (!scm.busy && (call = g_queue_pop_head(&scm.waiting)))
    if (!scm.app->closing && scm.app->project.git_top)
      start_git(call);
    else
      git_call_free(call);
}

/* git finished: its output goes to the call's done function, then the next waiting command starts. */
static void on_git_done(GObject *source, GAsyncResult *result, gpointer user)
{
  GitCall *call = user;
  char *out = NULL, *err = NULL;
  gboolean ok = g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result, &out, &err, NULL) &&
                g_subprocess_get_if_exited(G_SUBPROCESS(source)) &&
                g_subprocess_get_exit_status(G_SUBPROCESS(source)) == 0;

  if (!call->quiet)
    scm.busy = FALSE;
  if (scm.app && !scm.app->closing) {
    if (call->done) {
      call->drop = NULL; /* the callback now owns data */
      call->done(ok, out ? out : "", err ? g_strstrip(err) : "", call->data);
    }
    if (!call->quiet)
      refresh_branch(scm.app);
  }
  if (!call->quiet)
    start_waiting(); /* unless done started the next step already (a commit after staging) */
  g_free(out);
  g_free(err);
  git_call_free(call);
}

/* Starts the call, or frees it and returns FALSE when git does not start. */
static gboolean start_git(GitCall *call)
{
  GSubprocessLauncher *launcher;
  GError *error = NULL;
  GSubprocess *proc;

  launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE |
                                       (call->input ? G_SUBPROCESS_FLAGS_STDIN_PIPE : 0));
  g_subprocess_launcher_set_cwd(launcher, scm.app->project.git_top);
  /* No terminal to answer a password prompt in: fail instead of waiting forever. */
  g_subprocess_launcher_setenv(launcher, "GIT_TERMINAL_PROMPT", "0", TRUE);
  /* Nor an editor for a pull's merge message: git's own one is taken. */
  g_subprocess_launcher_setenv(launcher, "GIT_MERGE_AUTOEDIT", "no", TRUE);
  proc = g_subprocess_launcher_spawnv(launcher, (const char *const *)call->argv, &error);
  g_object_unref(launcher);
  if (!proc) {
    scm_set_note(error->message, TRUE);
    g_error_free(error);
    git_call_free(call);
    return FALSE;
  }
  if (!call->quiet)
    scm.busy = TRUE;
  g_subprocess_communicate_utf8_async(proc, call->input, NULL, on_git_done, call);
  g_object_unref(proc);
  return TRUE;
}

/* Runs git in the work tree with args (NULL-terminated); input goes to its stdin. Commands that
 * change the repository (not quiet) run one after another. */
void scm_run_git_owned(const char *const *args, const char *input, gboolean quiet, GitDone done, gpointer data,
                       GDestroyNotify drop)
{
  GPtrArray *argv = g_ptr_array_new();
  GitCall *call;

  if (!scm.app->project.git_top) {
    if (drop)
      drop(data);
    g_ptr_array_free(argv, TRUE);
    return;
  }
  g_ptr_array_add(argv, g_strdup("git"));
  for (int i = 0; args[i]; i++)
    g_ptr_array_add(argv, g_strdup(args[i]));
  g_ptr_array_add(argv, NULL);
  call = g_new0(GitCall, 1);
  call->argv = (char **)g_ptr_array_free(argv, FALSE);
  call->input = g_strdup(input);
  call->done = done;
  call->data = data;
  call->drop = drop;
  call->quiet = quiet;
  if (!quiet && scm.busy)
    g_queue_push_tail(&scm.waiting, call);
  else
    start_git(call);
}

void scm_run_git(const char *const *args, const char *input, gboolean quiet, GitDone done, gpointer data)
{
  scm_run_git_owned(args, input, quiet, done, data, NULL);
}

/* Shows git's complaint when a command fails. */
static void report_failure(gboolean ok, const char *out G_GNUC_UNUSED, const char *err, gpointer data)
{
  if (!ok)
    scm_set_note(*err ? err : data, TRUE);
}

/* A command that changes the repository and shows only its failure (scm_stage, scm_unstage). */
void scm_run_simple(const char *const *args, const char *failure)
{
  scm_set_note(NULL, FALSE);
  scm_run_git(args, NULL, FALSE, report_failure, (gpointer)failure);
}
