/*
 * What the Source Control panel's files share (scm.c, branches.c, commits.c): the panel's state,
 * running git, the note under the buttons and the list rows. Only those files include it.
 */
#pragma once

#include "app.h"

typedef struct {
  char *path;     /* relative to the work tree top */
  char *old_path; /* renames: where it was */
  char code;      /* M A D R C U(ntracked) !(conflict) */
} Change;

typedef struct {
  App *app;
  GtkWidget *message; /* GtkTextView */
  GtkWidget *placeholder;
  GtkWidget *commit, *pull, *push;
  GtkLabel *note; /* the last error or result */
  GtkListBox *staged_list, *changes_list, *commits_list;
  GtkLabel *staged_title, *changes_title;
  GtkWidget *staged_box, *changes_box, *commits_box;
  gboolean commits_stale;      /* HEAD moved while Commits was folded: listed once it opens */
  GPtrArray *staged, *changes; /* Change */
  char *head;                  /* the commit HEAD is at: commits are listed again when it moves */
  char *upstream;
  int ahead, behind;
  gboolean initial;    /* no commit yet */
  gboolean busy;       /* a git command that changes the repository runs */
  gboolean committing; /* a commit waits for the editors to save */
  GQueue waiting;      /* such commands that wait for it (GitCall) */
  char *open_commit;   /* the commit whose files are shown */
  /* Up and Down in the message go through the earlier commits' messages, as in a shell. */
  GPtrArray *history; /* their messages, newest first; NULL until the first Up */
  gboolean history_asked;
  int recalled;       /* the one shown, -1 for what was typed */
  int pending;        /* a step asked for while the history loads (+1 Up, -1 Down, 0 none) */
  char *draft;        /* what was typed before the first Up */
  gboolean recalling; /* the message is being replaced by one of them */
} ScmState;

extern ScmState scm;

/* The line under the buttons: the last result ("Committed") or error (in red); NULL hides it. */
void scm_set_note(const char *text, gboolean error);

typedef void (*GitDone)(gboolean ok, const char *out, const char *err, gpointer data);

/* Runs git in the work tree with args (NULL-terminated); input goes to its stdin. Commands that
 * change the repository (not quiet) run one after another; quiet ones (background reads) at once.
 * done gets git's output (err stripped) and owns data; drop releases data when done is never
 * called (no repository, git does not start, the window closes). */
void scm_run_git_owned(const char *const *args, const char *input, gboolean quiet, GitDone done, gpointer data,
                       GDestroyNotify drop);
void scm_run_git(const char *const *args, const char *input, gboolean quiet, GitDone done, gpointer data);

/* The panel's lists. */
void scm_clear_list(GtkListBox *list);
/* Puts a row's main content in a flat button that activates the row. */
GtkWidget *scm_open_button(GtkWidget *row, GtkWidget *content);
/* The colored status letter on the right of a row (M, A, D, R, U, !). */
GtkWidget *scm_code_label(char code);
/* A file's row: name, its folder dimmed; *box_out takes the buttons and the letter on the right. */
GtkWidget *scm_file_row(const char *path, char code, GtkWidget **box_out);

/* commits.c: makes scm.commits_list the Commits list (its clicks, tooltips and right-click menu),
 * and lists the last 100 commits of HEAD (scm.head) in it again. */
void commits_attach(void);
void commits_load(void);
