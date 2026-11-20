/*
 * The status of the project's repositories, run in the background: when a repository changes (its
 * .git/HEAD or .git/index, .hg/dirstate, .svn/wc.db, watched), when an editor saves, and every 10
 * seconds for changes made outside t-code (a build, a shell command). The repositories are git,
 * Mercurial or Subversion ones (vcs.h): the ones holding the project folder, the clones nested in a
 * git one (its git status lists them; submodules are not among them), or, in a folder that is no
 * repository, the ones found in it. Several may hold the project folder (an svn checkout inside a
 * git repository, hg-git's .git next to .hg): the nearest is the project's own unless the user
 * chose another (the session keeps the choice). Source Control shows one of them
 * (t->project.scm_top): its answers go to the panel (scm_update) and the status bar's branch label.
 * Mercurial and Subversion answers are translated into git status's format first (vcs.c).
 */
#pragma once

#include "app.h"

/* A repository of the project, as its last status left it. */
typedef struct {
  char *top;               /* its work tree top */
  VcsKind kind;            /* git, hg or svn; one folder may be two repositories (hg-git) */
  char *parent;            /* the repository whose git status lists it; NULL: holds the project, or found by looking */
  char *branch;            /* NULL until the status answered; "(detached)" off a branch */
  int changes;             /* files changed, staged or not */
  char *oid;               /* the commit (svn: the revision) checked out */
  char *svn_url;           /* svn: the working copy's URL from the repository root ("^/trunk") */
  GHashTable *staged;      /* hg and svn, which have no index: the paths the user staged (t-code keeps them) */
  GHashTable *changelists; /* svn: path -> its changelist's name, from the last status; NULL otherwise */
  gboolean changelists_moved; /* they changed since the panel showed them: its rows are made again */
  guint hash;                 /* of the status's output */
  guint own_hash;             /* of it without the files of the hg and svn repositories nested in it */
  gboolean busy, again;       /* the status runs; asked for again meanwhile */
} GitRepo;

/* Watches the project's repository and starts the 10 second timer. Once, at start. */
void git_status_start(App *t);

/* Finds the repositories holding the project (t->project.root) and in it, and watches the
 * project's own: t->project.repo_top / repo_kind (NULL / VCS_NONE at once, set when known, and
 * staying so outside a repository); call again after the root changes. */
void watch_git(App *t);

/* Runs the status shortly; changes that come together (a commit touches several files) share one. */
void refresh_branch(App *t);

/* The project's repositories (GitRepo): by path, the ones holding the project first. Empty outside
 * any. */
const GPtrArray *git_repos(void);

/* The work tree top of the project's repository holding path (the innermost one), or NULL. */
const char *git_repo_of(const char *path);

/* The git status output (v1 or v2, -z) of the git repository at top without the files of the hg and
 * svn repositories found nested in it, which git lists as its own untracked files. A new reference
 * (to out itself when nothing goes); NULL for NULL. */
GBytes *git_status_of_own(const char *top, GBytes *out);

/* Source Control shows the repository at top of kind (one of git_repos()) from now on. One holding
 * the project folder becomes the project's own too: the Files panel's colors and the diff gutters
 * follow it, and the session keeps the choice. */
void git_select_repo(App *t, const char *top, VcsKind kind);

/* The kind chosen for the project's own repository among several (VCS_NONE: the nearest), and
 * setting it before watch_git() (session.c, from the project's session). */
VcsKind git_chosen_kind(void);
void git_set_chosen_kind(VcsKind kind);

/* The repository Source Control shows (NULL outside any). */
GitRepo *git_shown_repo(App *t);
