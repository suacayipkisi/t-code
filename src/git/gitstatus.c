/*
 * git status for the project's repositories, run in the background (see gitstatus.h). The answers
 * fill the Source Control panel and the branch in the status bar (the repository it shows), tell the
 * Files panel when the project's own repository changed, and the diff gutters when a HEAD moved.
 */
#include "git/gitstatus.h"

#include "editor/gutter/diffgutter.h"
#include "files/files.h"
#include "git/repos.h"
#include "git/scm/scm.h"
#include "window/statusbar.h"

#include <string.h>

#define FIND_DEPTH 3 /* how far down a folder that is no repository is looked into for repositories */
#define FIND_MAX 32  /* and how many are taken from it at most (each runs git status) */

static GPtrArray *repos;                        /* GitRepo */
static gboolean resolving;                      /* git rev-parse (or the look for repositories) has not answered yet */
static int branch_busy;                         /* git status commands running */
static guint branch_soon_id;                    /* refresh_branch()'s timeout */
static GFileMonitor *git_monitor, *scm_monitor; /* the project's git directory; the shown one's */
static char *scm_monitored;                     /* the work tree scm_monitor is for */
static gboolean poll_branch(gpointer data);

static void repo_free(GitRepo *r)
{
  g_free(r->top);
  g_free(r->parent);
  g_free(r->branch);
  g_free(r->oid);
  g_free(r);
}

static GitRepo *find_repo(const char *top)
{
  for (guint i = 0; repos && i < repos->len; i++) {
    GitRepo *r = repos->pdata[i];
    if (strcmp(r->top, top) == 0)
      return r;
  }
  return NULL;
}

/* By path: the project's own repository holds the nested ones, so it comes first. */
static int compare_repos(gconstpointer a, gconstpointer b)
{
  return strcmp((*(GitRepo *const *)a)->top, (*(GitRepo *const *)b)->top);
}

static GitRepo *add_repo(const char *top, const char *parent)
{
  GitRepo *r = g_new0(GitRepo, 1);

  r->top = g_strdup(top);
  r->parent = g_strdup(parent);
  g_ptr_array_add(repos, r);
  g_ptr_array_sort(repos, compare_repos);
  return r;
}

const GPtrArray *git_repos(void)
{
  if (!repos)
    repos = g_ptr_array_new_with_free_func((GDestroyNotify)repo_free);
  return repos;
}

const char *git_repo_of(const char *path)
{
  const char *best = NULL;

  for (guint i = 0; path && repos && i < repos->len; i++) {
    const char *top = ((GitRepo *)repos->pdata[i])->top;
    gsize len = strlen(top);
    if (strncmp(path, top, len) == 0 && (path[len] == '/' || path[len] == '\0') && (!best || len > strlen(best)))
      best = top;
  }
  return best;
}

/* .git/HEAD (a commit, a checkout) or .git/index (staging) changed. */
static gboolean is_git_state(GFile *file)
{
  char *name = file ? g_file_get_basename(file) : NULL;
  gboolean yes = name && (strcmp(name, "HEAD") == 0 || strcmp(name, "index") == 0);

  g_free(name);
  return yes;
}

/* Something changed in a git directory. */
static void on_git_changed(GFileMonitor *monitor G_GNUC_UNUSED, GFile *file, GFile *other,
                           GFileMonitorEvent event G_GNUC_UNUSED, App *t)
{
  /* git writes HEAD.lock and index.lock, then renames them into place (other is the new name). */
  if (is_git_state(file) || is_git_state(other))
    refresh_branch(t);
}

static GFileMonitor *monitor_dir(const char *dir, App *t)
{
  GFile *file = g_file_new_for_path(dir);
  GFileMonitor *monitor = g_file_monitor_directory(file, G_FILE_MONITOR_WATCH_MOVES, NULL, NULL);

  if (monitor)
    g_signal_connect(monitor, "changed", G_CALLBACK(on_git_changed), t);
  g_object_unref(file);
  return monitor;
}

static void stop_monitor(GFileMonitor **monitor)
{
  if (*monitor) {
    g_file_monitor_cancel(*monitor);
    g_clear_object(monitor);
  }
}

/* The shown repository's .git folder is watched too when it is not the project's own (a .git file,
 * a linked work tree, leaves it to the 10 second timer). */
static void watch_shown(App *t)
{
  const char *top = t->project.scm_top;

  if (g_strcmp0(top, scm_monitored) == 0)
    return;
  stop_monitor(&scm_monitor);
  g_set_str(&scm_monitored, top);
  if (top && g_strcmp0(top, t->project.git_top) != 0) {
    char *dir = g_build_filename(top, ".git", NULL);
    if (g_file_test(dir, G_FILE_TEST_IS_DIR))
      scm_monitor = monitor_dir(dir, t);
    g_free(dir);
  }
}

/* The shown repository is gone (or none was chosen yet): the project's own, else the first. */
static void choose_shown(App *t)
{
  if (t->project.scm_top && find_repo(t->project.scm_top))
    return;
  g_free(t->project.scm_top);
  t->project.scm_top = repos->len ? g_strdup(((GitRepo *)repos->pdata[0])->top) : NULL;
  if (t->project.git_top && find_repo(t->project.git_top))
    g_set_str(&t->project.scm_top, t->project.git_top);
  scm_reset();
  watch_shown(t);
}

void git_select_repo(App *t, const char *top)
{
  if (!find_repo(top) || g_strcmp0(top, t->project.scm_top) == 0)
    return;
  g_set_str(&t->project.scm_top, top);
  scm_reset();
  watch_shown(t);
  scm_repos_changed(t);
  poll_branch(t);
}

/* The repositories nested in parent are the ones its git status lists now: new ones are added (and
 * asked for their status), gone ones dropped. Returns whether the list changed. */
static gboolean sync_nested(App *t, const char *parent, const char *out, gsize size)
{
  GPtrArray *nested = git_nested_repos(parent, out, size);
  gboolean changed = FALSE;

  for (guint i = repos->len; i-- > 0;) {
    GitRepo *r = repos->pdata[i];
    gboolean listed = FALSE;
    if (g_strcmp0(r->parent, parent) != 0)
      continue;
    for (guint j = 0; j < nested->len && !listed; j++)
      listed = strcmp(nested->pdata[j], r->top) == 0;
    if (!listed) {
      g_ptr_array_remove_index(repos, i);
      changed = TRUE;
    }
  }
  for (guint j = 0; j < nested->len; j++)
    if (!find_repo(nested->pdata[j])) {
      add_repo(nested->pdata[j], parent);
      changed = TRUE;
    }
  g_ptr_array_unref(nested);
  if (changed) {
    choose_shown(t);
    refresh_branch(t); /* the new ones' status */
  }
  return changed;
}

/* One repository's git status answered (porcelain v2, -z). */
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
  int changes = 0;
  gboolean listed = FALSE;
  /* Started before another project opened (or the repository went): not shown. */
  GitRepo *r = g_strcmp0(g_object_get_data(source, "root"), t->project.root) == 0
                   ? find_repo(g_object_get_data(source, "top"))
                   : NULL;

  branch_busy--;
  if (!r || t->closing) {
    if (out)
      g_bytes_unref(out);
    return;
  }
  r->busy = FALSE;
  if (r->again) {
    r->again = FALSE;
    refresh_branch(t);
  }
  if (ok && bytes) {
    for (const char *p = bytes, *end = bytes + size; p < end;) {
      const char *nul = memchr(p, '\0', end - p);
      gsize len = (nul ? nul : end) - p;
      if (len > 14 && strncmp(p, "# branch.head ", 14) == 0)
        branch = g_strndup(p + 14, len - 14);
      else if (len > 13 && strncmp(p, "# branch.oid ", 13) == 0)
        oid = g_strndup(p + 13, len - 13);
      else if (len && *p != '#' && *p != '!' && !git_is_nested_record(p, len))
        changes++;                                    /* not an ignored file, nor a nested repository */
      if (len && *p == '2' && nul && nul + 1 < end) { /* a rename: its old path follows */
        const char *old = memchr(nul + 1, '\0', end - nul - 1);
        p = old ? old + 1 : end;
        continue;
      }
      p += len + 1;
    }
  }
  guint hash = out ? g_bytes_hash(out) : 0;
  gboolean seen = hash == r->hash && r->branch;
  if (g_strcmp0(branch, r->branch) != 0 || changes != r->changes) {
    g_set_str(&r->branch, branch);
    r->changes = changes;
    listed = TRUE;
  }
  if (!seen && ok && sync_nested(t, r->top, bytes, size))
    listed = TRUE;
  if (listed)
    scm_repos_changed(t);
  /* sync_nested() may have dropped repositories, never this one: r is still good. */
  if (g_strcmp0(r->top, t->project.scm_top) == 0) {
    scm_update(t, ok ? bytes : NULL, size);
    status_show_branch(t, branch, changes > 0);
  }
  /* The Files panel reads its git colors again (its git status also lists ignored files). */
  if (hash != r->hash && g_strcmp0(r->top, t->project.git_top) == 0)
    files_git_changed(t);
  r->hash = hash;
  /* A new commit (also on a branch, where .git/HEAD itself stays the same) or a checkout: the diff
   * gutters compare with the new HEAD. Staging does not move it, and asks nothing. */
  if (oid && r->oid && strcmp(oid, r->oid) != 0)
    diff_gutter_head_moved(t);
  if (ok)
    g_set_str(&r->oid, oid);
  g_free(oid);
  g_free(branch);
  if (out)
    g_bytes_unref(out);
}

/* Outside any repository: the panel and the branch label are empty. */
static void show_no_git(App *t)
{
  scm_update(t, NULL, 0);
  status_show_branch(t, NULL, FALSE);
}

/* Starts git status in each repository, unless one runs there (then it runs again after that one).
 * Also the 10 second timer, which keeps going. */
static gboolean poll_branch(gpointer data)
{
  App *t = data;

  if (t->closing)
    return G_SOURCE_REMOVE;
  if (resolving)
    return G_SOURCE_CONTINUE; /* watch_git() asks once it knows the repositories */
  git_repos();
  if (repos->len == 0)
    show_no_git(t);
  for (guint i = 0; i < repos->len; i++) {
    GitRepo *r = repos->pdata[i];
    if (r->busy) {
      r->again = TRUE; /* the running one may have missed the change */
      continue;
    }
    GSubprocess *proc = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, NULL,
                                         "git", "-C", r->top, "status", "--porcelain=v2", "--branch", "-z",
                                         "--untracked-files=all", "--ignored=matching", NULL);
    if (proc) {
      g_object_set_data_full(G_OBJECT(proc), "root", g_strdup(t->project.root), g_free);
      g_object_set_data_full(G_OBJECT(proc), "top", g_strdup(r->top), g_free);
      r->busy = TRUE;
      branch_busy++;
      g_subprocess_communicate_async(proc, NULL, NULL, on_branch, t);
      g_object_unref(proc);
    }
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

/* The repositories found in a folder that is none (git_find_repos() in a thread). */
static void find_in_thread(GTask *task, gpointer source G_GNUC_UNUSED, gpointer data,
                           GCancellable *cancel G_GNUC_UNUSED)
{
  g_task_return_pointer(task, git_find_repos(data, FIND_DEPTH, FIND_MAX), (GDestroyNotify)g_ptr_array_unref);
}

static void on_found(GObject *source G_GNUC_UNUSED, GAsyncResult *result, gpointer data)
{
  App *t = data;
  GPtrArray *found = g_task_propagate_pointer(G_TASK(result), NULL);
  const char *root = g_task_get_task_data(G_TASK(result));

  if (t->closing || g_strcmp0(root, t->project.root) != 0) {
    if (found)
      g_ptr_array_unref(found);
    return;
  }
  resolving = FALSE;
  for (guint i = 0; found && i < found->len; i++)
    add_repo(found->pdata[i], NULL);
  if (found)
    g_ptr_array_unref(found);
  choose_shown(t);
  scm_repos_changed(t);
  poll_branch(t);
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
  char **lines = ok && out ? g_strsplit(out, "\n", 3) : NULL;
  if (lines && lines[1] && *lines[1]) {
    g_free(t->project.git_top);
    t->project.git_top = g_strdup(lines[1]);
    git_monitor = monitor_dir(lines[0], t);
    add_repo(t->project.git_top, NULL);
    resolving = FALSE;
    choose_shown(t);
    scm_repos_changed(t);
    files_git_changed(t); /* its git colors need git_top */
    poll_branch(t);
  } else {
    /* No repository here: maybe a folder of them. */
    GTask *task = g_task_new(NULL, NULL, on_found, t);
    g_task_set_task_data(task, g_strdup(t->project.root), g_free);
    g_task_run_in_thread(task, find_in_thread);
    g_object_unref(task);
  }
  g_strfreev(lines);
  g_free(out);
}

void watch_git(App *t)
{
  GSubprocess *proc;

  stop_monitor(&git_monitor);
  stop_monitor(&scm_monitor);
  g_clear_pointer(&scm_monitored, g_free);
  git_repos();
  g_ptr_array_set_size(repos, 0);               /* answers still coming for them are dropped by their top */
  g_clear_pointer(&t->project.git_top, g_free); /* until git answers for the new root */
  g_clear_pointer(&t->project.scm_top, g_free);
  scm_repos_changed(t);
  resolving = TRUE;
  proc = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, NULL, "git", "-C",
                          t->project.root, "rev-parse", "--absolute-git-dir", "--show-toplevel", NULL);
  if (!proc) {
    resolving = FALSE;
    show_no_git(t);
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
