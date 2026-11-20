/*
 * The status of the project's repositories, run in the background (see gitstatus.h). The answers
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
#define FIND_MAX 32  /* and how many are taken from it at most (each runs its status) */

static GPtrArray *repos;                        /* GitRepo */
static gboolean resolving;                      /* git rev-parse (or the look for repositories) has not answered yet */
static int branch_busy;                         /* status commands running */
static guint branch_soon_id;                    /* refresh_branch()'s timeout */
static GFileMonitor *git_monitor, *scm_monitor; /* the project's repository folder; the shown one's */
static char *scm_monitored;                     /* the work tree scm_monitor is for */
static VcsKind scm_monitored_kind;
static VcsKind chosen_kind; /* the user's choice among the repositories holding the project (VCS_NONE: none) */
static gboolean poll_branch(gpointer data);

static void repo_free(GitRepo *r)
{
  g_free(r->top);
  g_free(r->parent);
  g_free(r->branch);
  g_free(r->oid);
  g_free(r->svn_url);
  if (r->staged)
    g_hash_table_unref(r->staged);
  g_free(r);
}

static GitRepo *find_repo(const char *top, VcsKind kind)
{
  for (guint i = 0; top && repos && i < repos->len; i++) {
    GitRepo *r = repos->pdata[i];
    if (r->kind == kind && strcmp(r->top, top) == 0)
      return r;
  }
  return NULL;
}

/* By path (the outer repository holds the nested ones, so it comes first), then git, hg, svn. */
static int compare_repos(gconstpointer a, gconstpointer b)
{
  const GitRepo *x = *(GitRepo *const *)a, *y = *(GitRepo *const *)b;
  int by_path = strcmp(x->top, y->top);

  return by_path ? by_path : (int)x->kind - (int)y->kind;
}

static GitRepo *add_repo(const char *top, const char *parent, VcsKind kind)
{
  GitRepo *r = find_repo(top, kind);

  if (r)
    return r;
  r = g_new0(GitRepo, 1);
  r->top = g_strdup(top);
  r->kind = kind;
  r->parent = g_strdup(parent);
  if (kind != VCS_GIT)
    r->staged = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
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

/* Whether path is top or inside it. */
static gboolean is_under(const char *path, const char *top)
{
  gsize len = strlen(top);
  return path && strncmp(path, top, len) == 0 && (path[len] == '/' || path[len] == '\0');
}

const char *git_repo_of(const char *path)
{
  const char *best = NULL;

  for (guint i = 0; path && repos && i < repos->len; i++) {
    const char *top = ((GitRepo *)repos->pdata[i])->top;
    if (is_under(path, top) && (!best || strlen(top) > strlen(best)))
      best = top;
  }
  return best;
}

GitRepo *git_shown_repo(App *t)
{
  return find_repo(t->project.scm_top, t->project.scm_kind);
}

VcsKind git_chosen_kind(void)
{
  return chosen_kind;
}

void git_set_chosen_kind(VcsKind kind)
{
  chosen_kind = kind;
}

/* The files in a repository folder whose change means the status changed: git's HEAD (a commit, a
 * checkout) and index (staging), hg's dirstate, branch and bookmarks, svn's wc.db. */
static gboolean is_repo_state(GFile *file)
{
  static const char *const names[] = {
    "HEAD", "index", "dirstate", "branch", "bookmarks", "bookmarks.current", "wc.db"
  };
  char *name = file ? g_file_get_basename(file) : NULL;
  gboolean yes = FALSE;

  for (guint i = 0; name && i < G_N_ELEMENTS(names) && !yes; i++)
    yes = strcmp(name, names[i]) == 0;
  g_free(name);
  return yes;
}

/* Something changed in a repository folder. */
static void on_repo_changed(GFileMonitor *monitor G_GNUC_UNUSED, GFile *file, GFile *other,
                            GFileMonitorEvent event G_GNUC_UNUSED, App *t)
{
  /* git writes HEAD.lock and index.lock, then renames them into place (other is the new name). */
  if (is_repo_state(file) || is_repo_state(other))
    refresh_branch(t);
}

static GFileMonitor *monitor_dir(const char *dir, App *t)
{
  GFile *file = g_file_new_for_path(dir);
  GFileMonitor *monitor = g_file_monitor_directory(file, G_FILE_MONITOR_WATCH_MOVES, NULL, NULL);

  if (monitor)
    g_signal_connect(monitor, "changed", G_CALLBACK(on_repo_changed), t);
  g_object_unref(file);
  return monitor;
}

/* The folder of a repository's state: top/.git (a .git file, a linked work tree, leaves it to the 10
 * second timer), top/.hg or top/.svn. NULL when it is no folder. */
static char *state_dir(const char *top, VcsKind kind)
{
  static const char *const names[VCS_KINDS] = { NULL, ".git", ".hg", ".svn" };
  char *dir = kind != VCS_NONE ? g_build_filename(top, names[kind], NULL) : NULL;

  if (dir && !g_file_test(dir, G_FILE_TEST_IS_DIR))
    g_clear_pointer(&dir, g_free);
  return dir;
}

static void stop_monitor(GFileMonitor **monitor)
{
  if (*monitor) {
    g_file_monitor_cancel(*monitor);
    g_clear_object(monitor);
  }
}

/* The shown repository's folder is watched too when it is not the project's own. */
static void watch_shown(App *t)
{
  const char *top = t->project.scm_top;

  if (g_strcmp0(top, scm_monitored) == 0 && t->project.scm_kind == scm_monitored_kind)
    return;
  stop_monitor(&scm_monitor);
  g_set_str(&scm_monitored, top);
  scm_monitored_kind = t->project.scm_kind;
  if (top && (g_strcmp0(top, t->project.repo_top) != 0 || t->project.scm_kind != t->project.repo_kind)) {
    char *dir = state_dir(top, t->project.scm_kind);
    if (dir)
      scm_monitor = monitor_dir(dir, t);
    g_free(dir);
  }
}

/* The project's own repository: among the ones holding the project folder, the kind the user chose,
 * else the nearest (git first in one folder). Its state folder is watched. */
static void choose_own(App *t)
{
  GitRepo *own = NULL;

  for (guint i = 0; i < repos->len; i++) {
    GitRepo *r = repos->pdata[i];
    if (r->parent || !is_under(t->project.root, r->top))
      continue;
    if (chosen_kind != VCS_NONE && r->kind == chosen_kind) {
      own = r;
      break;
    }
    if (!own || strlen(r->top) > strlen(own->top)) /* the nearest; git before hg before svn in one folder */
      own = r;
  }
  if (own && g_strcmp0(own->top, t->project.repo_top) == 0 && own->kind == t->project.repo_kind)
    return;
  stop_monitor(&git_monitor);
  g_set_str(&t->project.repo_top, own ? own->top : NULL);
  t->project.repo_kind = own ? own->kind : VCS_NONE;
  /* The files' gutters and blame look their repository up by path (vcs_kind_of): the choice goes there. */
  if (own && chosen_kind == own->kind)
    vcs_prefer(own->top, own->kind);
  if (own && own->kind != VCS_GIT) { /* git's folder comes from git rev-parse (watch_git) */
    char *dir = state_dir(own->top, own->kind);
    if (dir)
      git_monitor = monitor_dir(dir, t);
    g_free(dir);
  }
}

/* The shown repository is gone (or none was chosen yet): the project's own, else the first. */
static void choose_shown(App *t)
{
  if (git_shown_repo(t))
    return;
  GitRepo *first = repos->len ? repos->pdata[0] : NULL;
  g_set_str(&t->project.scm_top, first ? first->top : NULL);
  t->project.scm_kind = first ? first->kind : VCS_NONE;
  if (t->project.repo_top && find_repo(t->project.repo_top, t->project.repo_kind)) {
    g_set_str(&t->project.scm_top, t->project.repo_top);
    t->project.scm_kind = t->project.repo_kind;
  }
  scm_reset();
  watch_shown(t);
}

void git_select_repo(App *t, const char *top, VcsKind kind)
{
  GitRepo *r = find_repo(top, kind);

  if (!r || (g_strcmp0(top, t->project.scm_top) == 0 && kind == t->project.scm_kind))
    return;
  g_set_str(&t->project.scm_top, top);
  t->project.scm_kind = kind;
  /* One of several holding the project: the user's choice of the project's own. */
  if (!r->parent && is_under(t->project.root, top)) {
    gboolean several = FALSE;
    for (guint i = 0; i < repos->len; i++) {
      GitRepo *other = repos->pdata[i];
      several |= other != r && !other->parent && is_under(t->project.root, other->top);
    }
    if (several) {
      if (t->project.repo_top)
        vcs_prefer(t->project.repo_top, VCS_NONE);
      chosen_kind = kind;
      char *old_top = g_strdup(t->project.repo_top);
      VcsKind old_kind = t->project.repo_kind;
      choose_own(t);
      if (g_strcmp0(old_top, t->project.repo_top) != 0 || old_kind != t->project.repo_kind) {
        files_git_changed(t);
        diff_gutter_head_moved(t); /* the gutters compare with the chosen one's base */
      }
      g_free(old_top);
    }
  }
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
    if (!find_repo(nested->pdata[j], VCS_GIT)) {
      add_repo(nested->pdata[j], parent, VCS_GIT);
      changed = TRUE;
    }
  g_ptr_array_unref(nested);
  if (changed) {
    choose_shown(t);
    refresh_branch(t); /* the new ones' status */
  }
  return changed;
}

/* A repository's status answered, in git status --porcelain=v2 --branch -z's format (hg's and svn's
 * translated): the panel, the branch label, the Files panel and the gutters hear of it. ok: the
 * command worked (out may still be NULL). */
static void got_status(App *t, GitRepo *r, gboolean ok, GBytes *out)
{
  gsize size = 0;
  const char *bytes = out ? g_bytes_get_data(out, &size) : NULL;
  char *branch = NULL, *oid = NULL;
  int changes = 0;
  gboolean listed = FALSE;

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
  if (!seen && ok && r->kind == VCS_GIT && sync_nested(t, r->top, bytes, size))
    listed = TRUE;
  if (listed)
    scm_repos_changed(t);
  /* sync_nested() may have dropped repositories, never this one: r is still good. */
  if (g_strcmp0(r->top, t->project.scm_top) == 0 && r->kind == t->project.scm_kind) {
    scm_update(t, ok ? bytes : NULL, size);
    status_show_branch(t, branch, changes > 0);
  }
  /* The Files panel reads its colors again (its status also lists ignored files). */
  if (hash != r->hash && g_strcmp0(r->top, t->project.repo_top) == 0 && r->kind == t->project.repo_kind)
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
}

/* The repository a status command was started for, unless another project opened meanwhile (or the
 * repository went). */
static GitRepo *asked_repo(App *t, GObject *source)
{
  if (t->closing || g_strcmp0(g_object_get_data(source, "root"), t->project.root) != 0)
    return NULL;
  return find_repo(g_object_get_data(source, "top"), GPOINTER_TO_INT(g_object_get_data(source, "kind")));
}

/* One git repository's git status answered (porcelain v2, -z). */
static void on_branch(GObject *source, GAsyncResult *result, gpointer data)
{
  App *t = data;
  GBytes *out = NULL;
  gboolean ok = g_subprocess_communicate_finish(G_SUBPROCESS(source), result, &out, NULL, NULL) &&
                g_subprocess_get_if_exited(G_SUBPROCESS(source)) &&
                g_subprocess_get_exit_status(G_SUBPROCESS(source)) == 0;
  GitRepo *r = asked_repo(t, source);

  branch_busy--;
  if (r)
    got_status(t, r, ok, out);
  if (out)
    g_bytes_unref(out);
}

static GSubprocess *spawn_in(const char *top, VcsKind kind, const char *const *args);
static void tag_proc(GSubprocess *proc, App *t, GitRepo *r);

/* hg / svn: the head, the status and (during an hg merge) the unresolved files answered: all
 * through vcs_git_status() into git's format. */
static void vcs_status_done(App *t, GitRepo *r, gboolean ok, GBytes *out, const char *head, const char *unresolved)
{
  gsize size = 0, length;
  const char *bytes = out ? g_bytes_get_data(out, &size) : NULL;

  if (r->kind == VCS_SVN && head) {
    g_free(r->svn_url);
    r->svn_url = vcs_svn_url(head);
  }
  char *git = ok ? vcs_git_status(r->kind, bytes, size, head, unresolved, r->staged, FALSE, &length) : NULL;
  GBytes *translated = git ? g_bytes_new_take(git, length) : NULL;
  got_status(t, r, ok && head, translated);
  if (translated)
    g_bytes_unref(translated);
}

/* hg resolve --list answered, after the status. */
static void on_vcs_unresolved(GObject *source, GAsyncResult *result, gpointer data)
{
  App *t = data;
  char *unresolved = NULL;
  gboolean ok = g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result, &unresolved, NULL, NULL);
  GitRepo *r = asked_repo(t, source);

  branch_busy--;
  if (r)
    vcs_status_done(t, r, TRUE, g_object_get_data(source, "status"), g_object_get_data(source, "head"),
                    ok ? unresolved : NULL);
  g_free(unresolved);
}

/* hg / svn: the status answered after the head; during an hg merge hg resolve --list runs next. */
static void on_vcs_status(GObject *source, GAsyncResult *result, gpointer data)
{
  App *t = data;
  GBytes *out = NULL;
  gboolean ok = g_subprocess_communicate_finish(G_SUBPROCESS(source), result, &out, NULL, NULL) &&
                g_subprocess_get_successful(G_SUBPROCESS(source));
  GitRepo *r = asked_repo(t, source);
  const char *head = g_object_get_data(source, "head");
  GSubprocess *proc =
      r && ok && out && vcs_merging(r->kind, r->top) ? spawn_in(r->top, r->kind, vcs_unresolved_args()) : NULL;

  if (proc) {
    tag_proc(proc, t, r);
    g_object_set_data_full(G_OBJECT(proc), "head", g_strdup(head), g_free);
    g_object_set_data_full(G_OBJECT(proc), "status", g_bytes_ref(out), (GDestroyNotify)g_bytes_unref);
    g_subprocess_communicate_utf8_async(proc, NULL, NULL, on_vcs_unresolved, t);
    g_object_unref(proc);
  } else {
    branch_busy--;
    if (r)
      vcs_status_done(t, r, ok, out, head, NULL);
  }
  if (out)
    g_bytes_unref(out);
}

static GSubprocess *spawn_in(const char *top, VcsKind kind, const char *const *args)
{
  GSubprocessLauncher *launcher =
      g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE);
  GPtrArray *argv = g_ptr_array_new();
  GSubprocess *proc;

  g_ptr_array_add(argv, (gpointer)vcs_program(kind));
  for (int i = 0; args[i]; i++)
    g_ptr_array_add(argv, (gpointer)args[i]);
  g_ptr_array_add(argv, NULL);
  g_subprocess_launcher_set_cwd(launcher, top);
  g_subprocess_launcher_setenv(launcher, "HGPLAIN", "1", TRUE); /* hg's output as documented, whatever the hgrc */
  proc = g_subprocess_launcher_spawnv(launcher, (const char *const *)argv->pdata, NULL);
  g_ptr_array_free(argv, TRUE);
  g_object_unref(launcher);
  return proc;
}

static void tag_proc(GSubprocess *proc, App *t, GitRepo *r)
{
  g_object_set_data_full(G_OBJECT(proc), "root", g_strdup(t->project.root), g_free);
  g_object_set_data_full(G_OBJECT(proc), "top", g_strdup(r->top), g_free);
  g_object_set_data(G_OBJECT(proc), "kind", GINT_TO_POINTER(r->kind));
}

/* hg / svn: the head answered (the branch and the revision); the status runs next. */
static void on_vcs_head(GObject *source, GAsyncResult *result, gpointer data)
{
  App *t = data;
  char *head = NULL;
  gboolean ok = g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result, &head, NULL, NULL) &&
                g_subprocess_get_successful(G_SUBPROCESS(source));
  GitRepo *r = asked_repo(t, source);
  GSubprocess *proc = r ? spawn_in(r->top, r->kind, vcs_status_args(r->kind)) : NULL;

  if (proc) {
    tag_proc(proc, t, r);
    g_object_set_data_full(G_OBJECT(proc), "head", ok ? g_steal_pointer(&head) : NULL, g_free);
    g_subprocess_communicate_async(proc, NULL, NULL, on_vcs_status, t);
    g_object_unref(proc);
  } else {
    branch_busy--;
    if (r)
      got_status(t, r, FALSE, NULL);
  }
  g_free(head);
}

/* Outside any repository: the panel and the branch label are empty. */
static void show_no_git(App *t)
{
  scm_update(t, NULL, 0);
  status_show_branch(t, NULL, FALSE);
}

/* Starts the status in each repository, unless one runs there (then it runs again after that one).
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
    GSubprocess *proc;
    if (r->kind == VCS_GIT)
      proc = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, NULL, "git", "-C",
                              r->top, "status", "--porcelain=v2", "--branch", "-z", "--untracked-files=all",
                              "--ignored=matching", NULL);
    else
      proc = spawn_in(r->top, r->kind, vcs_head_args(r->kind));
    if (proc) {
      tag_proc(proc, t, r);
      r->busy = TRUE;
      branch_busy++;
      if (r->kind == VCS_GIT)
        g_subprocess_communicate_async(proc, NULL, NULL, on_branch, t);
      else
        g_subprocess_communicate_utf8_async(proc, NULL, NULL, on_vcs_head, t);
      g_object_unref(proc);
    }
  }
  return G_SOURCE_CONTINUE;
}

/* refresh_branch()'s timeout: one status. */
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

/* The repositories are known: the project's own and the shown one are chosen, and asked. */
static void repos_known(App *t)
{
  resolving = FALSE;
  choose_own(t);
  choose_shown(t);
  scm_repos_changed(t);
  files_git_changed(t); /* its colors need repo_top */
  poll_branch(t);
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
  for (guint i = 0; found && i < found->len; i++) {
    char *git = g_build_filename(found->pdata[i], ".git", NULL);
    VcsKind other = vcs_mark_in(found->pdata[i]);
    if (g_file_test(git, G_FILE_TEST_EXISTS))
      add_repo(found->pdata[i], NULL, VCS_GIT);
    if (other)
      add_repo(found->pdata[i], NULL, other);
    g_free(git);
  }
  if (found)
    g_ptr_array_unref(found);
  repos_known(t);
}

/* No repository holds the project: maybe it is a folder of them. */
static void find_repos_in(App *t)
{
  GTask *task = g_task_new(NULL, NULL, on_found, t);

  g_task_set_task_data(task, g_strdup(t->project.root), g_free);
  g_task_run_in_thread(task, find_in_thread);
  g_object_unref(task);
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
    add_repo(lines[1], NULL, VCS_GIT);
    repos_known(t);
    if (t->project.repo_kind == VCS_GIT && !git_monitor)
      git_monitor = monitor_dir(lines[0], t);
  } else if (repos->len) {
    repos_known(t); /* git failed, but hg or svn holds the project */
  } else {
    find_repos_in(t);
  }
  g_strfreev(lines);
  g_free(out);
}

void watch_git(App *t)
{
  GSubprocess *proc = NULL;
  gboolean git_here = FALSE;

  stop_monitor(&git_monitor);
  stop_monitor(&scm_monitor);
  g_clear_pointer(&scm_monitored, g_free);
  git_repos();
  g_ptr_array_set_size(repos, 0); /* answers still coming for them are dropped by their top */
  if (t->project.repo_top)
    vcs_prefer(t->project.repo_top, VCS_NONE);
  g_clear_pointer(&t->project.repo_top, g_free); /* until the repositories are known */
  g_clear_pointer(&t->project.scm_top, g_free);
  t->project.repo_kind = t->project.scm_kind = VCS_NONE;
  scm_repos_changed(t);
  resolving = TRUE;

  /* hg and svn are found by their folders; git's top comes from git (worktrees, GIT_DIR). */
  GPtrArray *places = vcs_find_up(t->project.root);
  for (guint i = 0; i < places->len; i++) {
    VcsPlace *place = places->pdata[i];
    if (place->kind == VCS_GIT)
      git_here = TRUE;
    else
      add_repo(place->top, NULL, place->kind);
  }
  g_ptr_array_unref(places);
  if (git_here)
    proc = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, NULL, "git", "-C",
                            t->project.root, "rev-parse", "--absolute-git-dir", "--show-toplevel", NULL);
  if (proc) {
    g_object_set_data_full(G_OBJECT(proc), "root", g_strdup(t->project.root), g_free);
    g_subprocess_communicate_utf8_async(proc, NULL, NULL, on_git_dir, t);
    g_object_unref(proc);
  } else if (repos->len) {
    repos_known(t);
  } else {
    find_repos_in(t);
  }
}

void git_status_start(App *t)
{
  watch_git(t);
  g_timeout_add_seconds(10, poll_branch, t);
}
