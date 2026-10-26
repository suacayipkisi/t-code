/*
 * What the Source Control panel's files share: the panel's state (scm.c), running git (gitrun.c),
 * staging (staging.c), the commit message (message.c), the change lists (changes.c), branches
 * (branches.c) and Commits (commits.c). Only those files include it.
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
/* A command that changes the repository and shows only its failure (failure: the note when git says
 * nothing). */
void scm_run_simple(const char *const *args, const char *failure);

/* staging.c: + and − on a file or a section, ↶ (after asking). */
void scm_stage(const char *path);
void scm_unstage(const Change *c);
void scm_discard(const Change *c);
void scm_stage_all(void);
void scm_unstage_all(void);

/* message.c: the Commit button (and Ctrl+Enter); HEAD moved: the message history is read again at the
 * next Up; the message box's "changed" and key handlers (Ctrl+Enter, Up / Down through the history). */
void scm_commit(void);
void scm_forget_history(void);
void scm_on_message_changed(GtkTextBuffer *buffer, gpointer data);
gboolean scm_on_message_key(GtkEventControllerKey *controller, guint key, guint code, GdkModifierType state,
                            gpointer data);

/* The panel's lists. */
void scm_clear_list(GtkListBox *list);
/* Puts a row's main content in a flat button that activates the row. */
GtkWidget *scm_open_button(GtkWidget *row, GtkWidget *content);
/* The colored status letter on the right of a row (M, A, D, R, U, !). */
GtkWidget *scm_code_label(char code);
/* A file's row: name, its folder dimmed; *box_out takes the buttons and the letter on the right. */
GtkWidget *scm_file_row(const char *path, char code, GtkWidget **box_out);
/* A small flat button with a symbol (+, −, ↶, ⟳), shown on hover. */
GtkWidget *scm_icon_button(const char *text, const char *tooltip);
/* changes.c: a list filled from changes (staged: their buttons unstage), a row's click (its changes
 * side by side) and right-click menu. */
void scm_fill_changes(GtkListBox *list, GPtrArray *changes, gboolean staged);
void scm_on_change_activated(GtkListBox *list, GtkListBoxRow *row, gpointer data);
void scm_on_changes_menu(GtkWidget *list, double x, double y, gpointer data);

/* commits.c: makes scm.commits_list the Commits list (its clicks, tooltips and right-click menu),
 * and lists the last 100 commits of HEAD (scm.head) in it again. */
void commits_attach(void);
void commits_load(void);
