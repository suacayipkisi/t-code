/*
 * git status for the project, run in the background (see gitstatus.h). Its output fills the
 * branch in the status bar and the Source Control panel, and tells the Files panel when it changed.
 */
#include "git/gitstatus.h"

#include "editor/gutter/diffgutter.h"
#include "files/files.h"
#include "git/scm/scm.h"
#include "window/statusbar.h"

#include <string.h>

static gboolean branch_busy;  /* a git status runs */
static gboolean branch_again; /* asked for again while it ran: run once more when it is done */
static guint branch_soon_id;  /* refresh_branch()'s timeout */
static GFileMonitor *git_monitor;
static guint last_status;      /* hash of git status's output */
static char *head, *head_root; /* the commit HEAD was at, and in which project */
static gboolean poll_branch(gpointer data);

/* The output (porcelain v2, -z) also fills the Source Control panel (scm.c). */
static void on_branch(GObject *source, GAsyncResult *result, gpointer data)
{
  App *t = data;
  GBytes *out = NULL;
  gboolean ok = g_subprocess_communicate_finish(G_SUBPROCESS(source), result, &out, NULL, NULL) &&
                g_subprocess_get_if_exited(G_SUBPROCESS(source)) &&
                g_subprocess_get_exit_status(G_SUBPROCESS(source)) == 0;
  gsize size = 0;
  const char *bytes = out ? g_bytes_get_data(out, &size) : NULL;
  char *branch = NULL, *oid = NULL;
  gboolean dirty = FALSE;
  /* Started before another project opened: that project's status, not this one's. */
  gboolean stale = g_strcmp0(g_object_get_data(source, "root"), t->project.root) != 0;

  branch_busy = FALSE;
  if ((branch_again || stale) && !t->closing) {
    branch_again = FALSE;
    refresh_branch(t);
  }
  if (stale) {
    if (out)
      g_bytes_unref(out);
    return;
  }
  if (ok && bytes && !t->closing) {
    /* Records end with NUL; only the header lines matter here. */
    for (const char *p = bytes, *end = bytes + size; p < end;) {
      const char *nul = memchr(p, '\0', end - p);
      gsize len = (nul ? nul : end) - p;
      if (len > 14 && strncmp(p, "# branch.head ", 14) == 0)
        branch = g_strndup(p + 14, len - 14);
      else if (len > 13 && strncmp(p, "# branch.oid ", 13) == 0)
        oid = g_strndup(p + 13, len - 13);
      else if (len && *p != '#')
        dirty = TRUE;
      p += len + 1;
    }
  }
  if (!t->closing)
    scm_update(t, ok ? bytes : NULL, size);
  /* The Files panel reads its git colors again (its git status also lists ignored files). */
  guint hash = out ? g_bytes_hash(out) : 0;
  if (hash != last_status && !t->closing)
    files_git_changed(t);
  last_status = hash;
  if (!t->closing)
    status_show_branch(t, branch, dirty);
  /* A new commit (also on a branch, where .git/HEAD itself stays the same) or a checkout: the diff
   * gutters compare with the new HEAD. Staging does not move it, and asks nothing. */
  if (oid && head && g_strcmp0(head_root, t->project.root) == 0 && strcmp(oid, head) != 0 && !t->closing)
    diff_gutter_head_moved(t);
  if (ok) {
    g_free(head);
    head = g_steal_pointer(&oid);
    g_set_str(&head_root, t->project.root);
  }
  g_free(oid);
  g_free(branch);
  if (out)
    g_bytes_unref(out);
}

/* Starts git status in the project, unless one runs (then it runs again after that one). Also
 * the 10 second timer, which keeps going. */
static gboolean poll_branch(gpointer data)
{
  App *t = data;
  GSubprocess *proc;

  if (t->closing)
    return G_SOURCE_REMOVE;
  if (branch_busy) {
    branch_again = TRUE; /* the running one may have missed the change */
    return G_SOURCE_CONTINUE;
  }
  proc = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, NULL, "git", "-C",
                          t->project.root, "status", "--porcelain=v2", "--branch", "-z", "--untracked-files=all", NULL);
  if (proc) {
    g_object_set_data_full(G_OBJECT(proc), "root", g_strdup(t->project.root), g_free);
    branch_busy = TRUE;
    g_subprocess_communicate_async(proc, NULL, NULL, on_branch, t);
    g_object_unref(proc);
  }
  return G_SOURCE_CONTINUE;
}

/* refresh_branch()'s timeout: one git status. */
static gboolean poll_branch_once(gpointer data)
{
  branch_soon_id = 0;
  poll_branch(data);
  return G_SOURCE_REMOVE;
}

void refresh_branch(App *t)
{
  if (!branch_soon_id)
    branch_soon_id = g_timeout_add(300, poll_branch_once, t);
}

/* .git/HEAD (a commit, a checkout) or .git/index (staging) changed. */
static gboolean is_git_state(GFile *file)
{
  char *name = file ? g_file_get_basename(file) : NULL;
  gboolean yes = name && (strcmp(name, "HEAD") == 0 || strcmp(name, "index") == 0);

  g_free(name);
  return yes;
}

/* Something changed in the git directory. */
static void on_git_changed(GFileMonitor *monitor G_GNUC_UNUSED, GFile *file, GFile *other,
                           GFileMonitorEvent event G_GNUC_UNUSED, App *t)
{
  /* git writes HEAD.lock and index.lock, then renames them into place (other is the new name). */
  if (is_git_state(file) || is_git_state(other))
    refresh_branch(t);
}

/* git rev-parse answered watch_git(): the git directory and the work tree top, one per line. */
static void on_git_dir(GObject *source, GAsyncResult *result, gpointer data)
{
  App *t = data;
  char *out = NULL;
  gboolean ok = g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result, &out, NULL, NULL) &&
                g_subprocess_get_successful(G_SUBPROCESS(source));

  /* Another project opened meanwhile: its own rev-parse answers for it. */
  if (t->closing || g_strcmp0(g_object_get_data(source, "root"), t->project.root) != 0) {
    g_free(out);
    return;
  }
  if (ok && out) {
    char **lines = g_strsplit(out, "\n", 3);
    GFile *dir = g_file_new_for_path(lines[0]);
    g_free(t->project.git_top);
    t->project.git_top = lines[1] && *lines[1] ? g_strdup(lines[1]) : NULL;
    g_strfreev(lines);
    git_monitor = g_file_monitor_directory(dir, G_FILE_MONITOR_WATCH_MOVES, NULL, NULL);
    if (git_monitor)
      g_signal_connect(git_monitor, "changed", G_CALLBACK(on_git_changed), t);
    g_object_unref(dir);
    files_git_changed(t); /* its git colors need git_top */
  }
  g_free(out);
  refresh_branch(t);
}

void watch_git(App *t)
{
  GSubprocess *proc;

  if (git_monitor) {
    g_file_monitor_cancel(git_monitor);
    g_clear_object(&git_monitor);
  }
  g_clear_pointer(&t->project.git_top, g_free); /* until git answers for the new root */
  proc = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, NULL, "git", "-C",
                          t->project.root, "rev-parse", "--absolute-git-dir", "--show-toplevel", NULL);
  if (!proc) {
    refresh_branch(t);
    return;
  }
  g_object_set_data_full(G_OBJECT(proc), "root", g_strdup(t->project.root), g_free);
  g_subprocess_communicate_utf8_async(proc, NULL, NULL, on_git_dir, t);
  g_object_unref(proc);
}

void git_status_start(App *t)
{
  watch_git(t);
  g_timeout_add_seconds(10, poll_branch, t);
}
