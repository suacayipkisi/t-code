/*
 * git status for the project's repositories, run in the background: when the repository changes
 * (.git/HEAD or .git/index, watched), when an editor saves, and every 10 seconds for changes made
 * outside t-code (a build, a shell command). The repositories are the project's own and the clones
 * nested in it (its git status lists them; submodules are not among them), or, in a folder that is
 * no repository, the ones found in it. Source Control shows one of them (t->project.scm_top): its
 * answers go to the panel (scm_update) and the status bar's branch label.
 */
#pragma once

#include "app.h"

/* A repository of the project, as its last git status left it. */
typedef struct {
  char *top;            /* its work tree top */
  char *parent;         /* the repository whose git status lists it; NULL: the project's own, or found by looking */
  char *branch;         /* NULL until git status answered; "(detached)" off a branch */
  int changes;          /* files changed, staged or not */
  char *oid;            /* the commit HEAD is at */
  guint hash;           /* of git status's output */
  gboolean busy, again; /* git status runs; asked for again meanwhile */
} GitRepo;

/* Watches the project's repository and starts the 10 second timer. Once, at start. */
void git_status_start(App *t);

/* Watches the git directory of the project (t->project.root) and finds the top of its work tree
 * (t->project.git_top: NULL at once, set when git answers, and stays NULL outside a repository) and
 * the repositories in the project; call again after the root changes. */
void watch_git(App *t);

/* Runs git status shortly; changes that come together (a commit touches several files) share
 * one. */
void refresh_branch(App *t);

/* The project's repositories (GitRepo): its own first, then by path. Empty outside git. */
const GPtrArray *git_repos(void);

/* The work tree top of the project's repository holding path (the innermost one), or NULL. */
const char *git_repo_of(const char *path);

/* Source Control shows the repository at top (one of git_repos()) from now on. */
void git_select_repo(App *t, const char *top);
