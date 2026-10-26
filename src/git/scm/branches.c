/*
 * The Source Control panel's talk with the remote (push, pull, fetch) and the branches: switching and
 * creating them (see scm.h). The panel's note says how it went.
 */
#include "git/scm/scm.h"

#include "editor/editor.h"
#include "git/gitstatus.h"
#include "git/scm/state.h"
#include "popups/picker.h"
#include "window/statusbar.h"
#include "window/window.h"

#include <string.h>

/* The remote ---------------------------------------------------------------------------------- */

/* What talks to the remote: the note when it worked ("Pushed"), else why not (a password prompt it
 * could not show gets its own hint). The branch's ahead / behind counts are read again. */
typedef struct {
  const char *done, *failed;
} RemoteNotes;

static void on_remote_done(gboolean ok, const char *out G_GNUC_UNUSED, const char *err, gpointer data)
{
  const RemoteNotes *notes = data;

  refresh_branch(scm.app); /* a fetch moves only the remote branches, which nothing watches */
  if (ok) {
    scm_set_note(notes->done, FALSE);
    return;
  }
  if (strstr(err, "terminal prompts disabled") || strstr(err, "could not read Username"))
    scm_set_note("git needs a password or token, and there is no terminal to ask in. Run it once from the "
                 "shell, or set up a credential helper (e.g. gh auth setup-git) or an SSH key.",
                 TRUE);
  else
    scm_set_note(*err ? err : notes->failed, TRUE);
}

static void run_remote(const char *const *args, const char *running, const RemoteNotes *notes)
{
  if (!scm.app || !scm.app->project.git_top)
    return;
  scm_set_note(running, FALSE);
  scm_run_git(args, NULL, FALSE, on_remote_done, (gpointer)notes);
}

void scm_push(void)
{
  static const RemoteNotes notes = { "Pushed", "The push failed" };
  /* A branch with no upstream yet goes to origin under its own name. */
  const char *args[] = { "push", NULL };
  const char *first[] = { "push", "-u", "origin", "HEAD", NULL };

  if (!scm.busy)
    run_remote(scm.upstream ? args : first, "Pushing…", &notes);
}

/* git pull as the user set it up (merge or rebase, pull.rebase); a merge takes git's own message. */
void scm_pull(void)
{
  static const RemoteNotes notes = { "Pulled", "The pull failed" };
  const char *args[] = { "pull", NULL };

  run_remote(args, "Pulling…", &notes);
}

void scm_fetch(void)
{
  static const RemoteNotes notes = { "Fetched", "The fetch failed" };
  const char *args[] = { "fetch", "--prune", NULL };

  run_remote(args, "Fetching…", &notes);
}

/* Branches ----------------------------------------------------------------------------------- */

typedef struct {
  char *name;   /* "main", "origin/fix" */
  char *detail; /* "2 days ago · the last commit's subject" */
  gboolean remote, current;
} Branch;

static GPtrArray *branches;      /* Branch, newest commit first; NULL while they are read */
static gboolean branch_new_only; /* "Git: Create Branch…": only the typed name to create */

static void branch_free(Branch *b)
{
  g_free(b->name);
  g_free(b->detail);
  g_free(b);
}

static gboolean branch_exists(const char *name)
{
  for (guint i = 0; branches && i < branches->len; i++)
    if (strcmp(((Branch *)branches->pdata[i])->name, name) == 0)
      return TRUE;
  return FALSE;
}

static void branches_query(App *t, const char *text, guint generation)
{
  char *name = g_strstrip(g_strdup(text));

  if (*name && !branch_exists(name)) {
    char *label = g_strdup_printf("+ Create branch \"%s\"", name), *payload = g_strconcat("create\t", name, NULL);
    picker_add(t, generation, label, "from what is checked out", payload);
    g_free(payload);
    g_free(label);
  }
  if (branch_new_only) {
    if (!*name)
      picker_message(t, generation, "Type the new branch's name");
    g_free(name);
    return;
  }
  if (!*name)
    picker_add(t, generation, "+ Create new branch…", NULL, "new\t");
  if (!branches) {
    picker_message(t, generation, "Reading the branches…");
    g_free(name);
    return;
  }
  for (guint i = 0; i < branches->len; i++) {
    Branch *b = branches->pdata[i];
    if (*name && fuzzy_score(name, b->name) < 0)
      continue;
    char *payload = g_strconcat(b->current ? "current\t" : b->remote ? "track\t" : "switch\t", b->name, NULL);
    char *detail = b->current ? g_strconcat("checked out · ", b->detail, NULL) : g_strdup(b->detail);
    picker_add(t, generation, b->name, detail, payload);
    g_free(detail);
    g_free(payload);
  }
  g_free(name);
}

/* "git for-each-ref" answered: one branch a line, "*|space TAB refname TAB short TAB date TAB subject". */
static void on_branches(gboolean ok, const char *out, const char *err G_GNUC_UNUSED, gpointer data G_GNUC_UNUSED)
{
  char **lines = g_strsplit(ok && out ? out : "", "\n", -1);

  if (branches)
    g_ptr_array_unref(branches);
  branches = g_ptr_array_new_with_free_func((GDestroyNotify)branch_free);
  for (int i = 0; lines[i]; i++) {
    char **f = g_strsplit(lines[i], "\t", 5);
    if (g_strv_length(f) == 5 && !g_str_has_suffix(f[1], "/HEAD")) {
      Branch *b = g_new0(Branch, 1);
      b->current = f[0][0] == '*';
      b->remote = g_str_has_prefix(f[1], "refs/remotes/");
      b->name = g_strdup(f[2]);
      b->detail = g_strdup_printf("%s%s · %s", b->remote ? "remote · " : "", f[3], f[4]);
      g_ptr_array_add(branches, b);
    }
    g_strfreev(f);
  }
  g_strfreev(lines);
  /* A remote branch with a local one of its name is that one: only the local is listed. */
  for (guint i = branches->len; i-- > 0;) {
    Branch *b = branches->pdata[i];
    const char *slash = b->remote ? strchr(b->name, '/') : NULL;
    if (slash && branch_exists(slash + 1))
      g_ptr_array_remove_index(branches, i);
  }
  picker_requery(scm.app, branches_query);
}

static void read_branches(void)
{
  const char *args[] = { "for-each-ref",
                         "--sort=-committerdate",
                         "--format=%(HEAD)%09%(refname)%09%(refname:short)%09%(committerdate:relative)%09%(subject)",
                         "refs/heads",
                         "refs/remotes",
                         NULL };

  g_clear_pointer(&branches, g_ptr_array_unref);
  scm_run_git(args, NULL, TRUE, on_branches, NULL);
}

/* git switch answered. */
static void on_switched(gboolean ok, const char *out G_GNUC_UNUSED, const char *err, gpointer data)
{
  char *note = data;

  if (ok) {
    status_say(scm.app, note);
    scm_set_note(note, FALSE);
  } else { /* local changes in the way, a bad name: git says which */
    scm_set_note(*err ? err : "The branch could not be checked out", TRUE);
    show_sidebar_page(scm.app, "scm");
  }
  refresh_branch(scm.app);
  g_free(note);
}

/* The editors are saved (git switch takes the files on disk along): the switch runs. */
static void on_saved_for_switch(App *t G_GNUC_UNUSED, gpointer data)
{
  char *payload = data, *tab = strchr(payload, '\t');
  const char *name = tab ? tab + 1 : "";
  const char *switch_to[] = { "switch", name, NULL };
  const char *track[] = { "switch", "--track", name, NULL };
  const char *create[] = { "switch", "-c", name, NULL };
  const char *const *args = g_str_has_prefix(payload, "track\t")    ? track
                            : g_str_has_prefix(payload, "create\t") ? create
                                                                    : switch_to;
  char *note = g_strdup_printf(args == create ? "Created and switched to %s" : "Switched to %s",
                               args == track && strchr(name, '/') ? strchr(name, '/') + 1 : name);

  scm_set_note(NULL, FALSE);
  scm_run_git_owned(args, NULL, FALSE, on_switched, note, g_free);
  g_free(payload);
}

static void branches_choose(App *t, const char *payload)
{
  if (g_str_has_prefix(payload, "new\t")) {
    scm_create_branch();
    return;
  }
  if (g_str_has_prefix(payload, "current\t"))
    return; /* already there */
  save_all(t, on_saved_for_switch, g_strdup(payload));
}

void scm_switch_branch(void)
{
  if (!scm.app || !scm.app->project.git_top) {
    if (scm.app)
      status_say(scm.app, "This folder is not in a git repository");
    return;
  }
  branch_new_only = FALSE;
  read_branches();
  picker_open(scm.app, "Switch to a branch, or type a new one's name", branches_query, branches_choose);
}

void scm_create_branch(void)
{
  if (!scm.app || !scm.app->project.git_top)
    return;
  branch_new_only = TRUE;
  read_branches(); /* to say when the name is taken */
  picker_open(scm.app, "Name of the new branch", branches_query, branches_choose);
}
