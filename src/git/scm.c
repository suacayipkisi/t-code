/*
 * The Source Control panel, a tab next to the file manager (Ctrl+Shift+G):
 *   a commit message and Commit / Push buttons
 *   Staged Changes and Changes: click a file to see its changes side by side; + stages, − unstages,
 *     ↶ discards (after asking)
 *   Commits: click one to list its files, click a file to see what that commit changed; the pointer
 *     on one shows its whole message, author, date, refs and line counts, and its right-click menu
 *     opens all that with its diff in a tab ("Open Commit Details")
 * The lists come from the git status that gitstatus.c runs anyway (scm_update); git commands run in
 * the background, and the git directory monitor in gitstatus.c refreshes everything afterwards.
 */
#include "git/scm.h"

#include "editor/editor.h"
#include "editor/tabs.h"
#include "git/compare.h"
#include "git/gitstatus.h"
#include "git/patch.h"
#include "editor/preview.h"
#include "files/files.h"
#include "popups/contextmenu.h"
#include "popups/picker.h"
#include "window/window.h"
#include "window/palette.h"

#include <string.h>

typedef struct {
  char *path;     /* relative to the work tree top */
  char *old_path; /* renames: where it was */
  char code;      /* M A D R C U(ntracked) !(conflict) */
} Change;

static struct {
  App *app;
  GtkWidget *message; /* GtkTextView */
  GtkWidget *placeholder;
  GtkWidget *commit, *push;
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
} scm;

static const char *const css =
    ".tcode-scm { background: var(--tc-panel); }"
    ".tcode-scm textview, .tcode-scm textview text { background: var(--tc-input); color: var(--tc-fg); }"
    ".tcode-scm .message { padding: 4px; background: var(--tc-input); }"
    ".tcode-scm .message { border: 1px solid var(--tc-border-strong); margin: 0 10px; }"
    ".tcode-scm .message:focus-within { border-color: var(--tc-accent); }"
    ".tcode-scm .placeholder { color: var(--tc-fg-faint); margin: 4px 6px; }"
    ".tcode-scm .actions { margin: 6px 10px; }"
    ".tcode-scm .actions button { min-height: 24px; padding: 0 10px; }"
    ".tcode-scm .primary { background: var(--tc-accent); color: var(--tc-accent-fg); }"
    ".tcode-scm .primary:hover { background: var(--tc-accent-hover); }"
    ".tcode-scm .note { color: var(--tc-fg-dim); font-size: 0.85em; margin: 0 10px 4px; }"
    ".tcode-scm .note.error { color: var(--tc-error); }"
    ".tcode-scm .section { font-size: 0.8em; font-weight: bold; color: var(--tc-fg); padding: 2px 10px 2px 2px; }"
    ".tcode-scm .section .chevron { -gtk-icon-size: 12px; margin-right: 4px; color: var(--tc-fg); }"
    ".tcode-scm list { background: transparent; }"
    ".tcode-scm row { padding: 0 6px 0 18px; margin: 0 4px; min-height: 22px; border-radius: 6px; color: var(--tc-fg); }"
    ".tcode-scm row:hover { background: var(--tc-hover); }"
    ".tcode-scm row:selected { background: var(--tc-selection); }"
    ".tcode-scm row .dim { color: var(--tc-fg-faint); font-size: 0.9em; }"
    ".tcode-scm button.tool { min-height: 0; min-width: 0; padding: 0 5px; opacity: 0; }"
    ".tcode-scm row:hover button.tool, .tcode-scm .section:hover button.tool { opacity: 1; }"
    ".tcode-scm button.open { min-height: 0; padding: 0; background: none; box-shadow: none; }"
    ".tcode-scm .code { font-family: 'Cascadia Code', monospace; font-weight: bold; min-width: 14px; }"
    ".tcode-scm .code-M { color: var(--tc-git-modified); } .tcode-scm .code-A, .tcode-scm .code-U { color: var(--tc-git-untracked); }"
    ".tcode-scm .code-D { color: var(--tc-git-deleted); } .tcode-scm .code-R, .tcode-scm .code-C { color: var(--tc-git-untracked); }"
    ".tcode-scm .code-x { color: var(--tc-git-conflict); }"
    ".tcode-scm .file-row { padding-left: 34px; }"
    ".tcode-commit-tip { padding: 4px 2px; }"
    ".tcode-commit-tip .rule { min-height: 1px; background: var(--tc-border-strong); margin: 4px 0; }"
    ".tcode-commit-tip .ref { padding: 0 7px; border-radius: 9px; font-size: 0.85em;"
    "  border: 1px solid var(--tc-border-strong); color: var(--tc-fg-bright); }"
    ".tcode-commit-tip .hash { font-family: 'Cascadia Code', monospace; color: var(--tc-fg-dim); font-size: 0.9em; }";

/* Frees one entry of scm.staged / scm.changes. */
static void change_free(Change *c)
{
  g_free(c->path);
  g_free(c->old_path);
  g_free(c);
}

/* The line under the buttons: the last result ("Committed") or error (in red); NULL hides it. */
static void set_note(const char *text, gboolean error)
{
  gtk_label_set_text(scm.note, text ? text : "");
  gtk_widget_set_visible(GTK_WIDGET(scm.note), text && *text);
  if (error)
    gtk_widget_add_css_class(GTK_WIDGET(scm.note), "error");
  else
    gtk_widget_remove_css_class(GTK_WIDGET(scm.note), "error");
}

/* Running git -------------------------------------------------------------------------------- */

typedef void (*GitDone)(gboolean ok, const char *out, const char *err, gpointer data);

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
  proc = g_subprocess_launcher_spawnv(launcher, (const char *const *)call->argv, &error);
  g_object_unref(launcher);
  if (!proc) {
    set_note(error->message, TRUE);
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
static void run_git_owned(const char *const *args, const char *input, gboolean quiet, GitDone done, gpointer data,
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

static void run_git(const char *const *args, const char *input, gboolean quiet, GitDone done, gpointer data)
{
  run_git_owned(args, input, quiet, done, data, NULL);
}

/* Shows git's complaint when a command fails. */
static void report_failure(gboolean ok, const char *out G_GNUC_UNUSED, const char *err, gpointer data)
{
  if (!ok)
    set_note(*err ? err : data, TRUE);
}

/* A command that changes the repository and shows only its failure (stage, unstage). */
static void run_simple(const char *const *args, const char *failure)
{
  set_note(NULL, FALSE);
  run_git(args, NULL, FALSE, report_failure, (gpointer)failure);
}

/* Staging ------------------------------------------------------------------------------------ */

/* git add takes the files from disk: what the editors hold unsaved is saved first. */
static void on_saved_for_add(App *t, gpointer data)
{
  char *path = data; /* NULL: everything */
  const char *one[] = { "add", "-A", "--", path, NULL };
  const char *all[] = { "add", "-A", NULL };

  if (!t->closing)
    run_simple(path ? one : all, path ? "Could not stage the file" : "Could not stage the changes");
  g_free(path);
}

/* The + button: stages one file (after saving the editors). */
static void stage(const char *path)
{
  save_all(scm.app, on_saved_for_add, g_strdup(path));
}

/* The − button: takes one file out of the staged changes. */
static void unstage(const Change *c)
{
  /* Before the first commit there is no HEAD to restore from. A rename is staged as its old
     * path's deletion too: both come back. */
  const char *first[] = { "rm", "--cached", "-r", "-q", "--", c->path, NULL };
  const char *args[] = { "restore", "--staged", "--", c->path, c->old_path, NULL };
  run_simple(scm.initial ? first : args, "Could not unstage the file");
}

typedef struct {
  char *path;
  char *root;
  gboolean staged;
} HunkRequest;

typedef struct {
  HunkRequest *request;
  GPtrArray *patches;
} HunkChoice;

static HunkChoice *choosing; /* the hunks the picker lists; one picker at a time */

static void hunk_request_free(HunkRequest *request)
{
  g_free(request->path);
  g_free(request->root);
  g_free(request);
}

static void hunk_choice_free(HunkChoice *choice)
{
  if (!choice)
    return;
  hunk_request_free(choice->request);
  g_ptr_array_free(choice->patches, TRUE);
  g_free(choice);
}

static void on_hunk_applied(gboolean ok, const char *out G_GNUC_UNUSED, const char *err, gpointer data)
{
  HunkChoice *choice = data;

  if (!scm.app->closing && g_strcmp0(scm.app->project.git_top, choice->request->root) == 0) {
    if (!ok)
      set_note(*err ? err : (choice->request->staged ? "Could not unstage this hunk" : "Could not stage this hunk"),
               TRUE);
    else
      set_note(choice->request->staged ? "Hunk unstaged" : "Hunk staged", FALSE);
  }
  hunk_choice_free(choice);
}

/* A patch's "@@ -a,b +c,d @@ context" line, and its first added or removed line (sign kept,
 * NULL when there is none). Free both. */
static void hunk_lines(const char *patch, char **header, char **first)
{
  const char *at = strstr(patch, "\n@@ ");

  *header = *first = NULL;
  if (!at)
    return;
  for (const char *line = at + 1; *line;) {
    const char *end = strchr(line, '\n');
    int length = end ? (int)(end - line) : (int)strlen(line);
    if (!*header)
      *header = g_strndup(line, length);
    else if (*line == '+' || *line == '-') {
      char *text = g_strndup(line + 1, length - 1);
      *first = g_strdup_printf("%c %s", *line, g_strstrip(text));
      g_free(text);
      break;
    }
    if (!end)
      break;
    line = end + 1;
  }
}

static void hunk_query(App *t, const char *text, guint generation)
{
  int shown = 0;

  if (!choosing) /* picker_open()'s first query, before the hunks are handed over */
    return;
  for (guint i = 0; i < choosing->patches->len; i++) {
    char *header, *first, *haystack, *payload;
    hunk_lines(g_ptr_array_index(choosing->patches, i), &header, &first);
    char *label = first ? g_strdup(first) : g_strdup_printf("Hunk %u", i + 1);
    haystack = g_strjoin(" ", label, header ? header : "", NULL);
    if (!*text || fuzzy_score(text, haystack) >= 0) {
      payload = g_strdup_printf("%u", i);
      picker_add(t, generation, label, header, payload);
      g_free(payload);
      shown++;
    }
    g_free(haystack);
    g_free(label);
    g_free(header);
    g_free(first);
  }
  if (!shown)
    picker_message(t, generation, "No matching hunks");
}

/* A hunk was picked: git apply its patch to the index (reversed to unstage). */
static void hunk_choose(App *t, const char *payload)
{
  HunkChoice *choice = g_steal_pointer(&choosing);
  guint64 chosen;

  if (!choice)
    return;
  if (!g_ascii_string_to_unsigned(payload, 10, 0, choice->patches->len - 1, &chosen, NULL) || t->closing ||
      g_strcmp0(t->project.git_top, choice->request->root) != 0) {
    hunk_choice_free(choice);
    return;
  }
  const char *apply[] = { "apply", "--cached", NULL };
  const char *unapply[] = { "apply", "--cached", "--reverse", NULL };
  run_git_owned(choice->request->staged ? unapply : apply, g_ptr_array_index(choice->patches, chosen), FALSE,
                on_hunk_applied, choice, (GDestroyNotify)hunk_choice_free);
}

static void hunk_cancel(App *t G_GNUC_UNUSED)
{
  g_clear_pointer(&choosing, hunk_choice_free);
}

static void on_hunks_read(gboolean ok, const char *out, const char *err, gpointer data)
{
  HunkRequest *request = data;
  GPtrArray *patches;
  char *name, *title;

  if (scm.app->closing || g_strcmp0(scm.app->project.git_top, request->root) != 0) {
    hunk_request_free(request);
    return;
  }
  if (!ok) {
    set_note(*err ? err : "Could not read file hunks", TRUE);
    hunk_request_free(request);
    return;
  }
  patches = git_patch_hunks(out);
  if (!patches->len) {
    set_note("No text hunks to stage or unstage", FALSE);
    g_ptr_array_free(patches, TRUE);
    hunk_request_free(request);
    return;
  }
  name = g_path_get_basename(request->path);
  title = g_strdup_printf("%s a hunk of %s", request->staged ? "Unstage" : "Stage", name);
  picker_open(scm.app, title, hunk_query, hunk_choose); /* cancels a hunk picker still open */
  choosing = g_new0(HunkChoice, 1);
  choosing->request = request;
  choosing->patches = patches;
  picker_set_preview(scm.app, NULL, hunk_cancel);
  picker_requery(scm.app, hunk_query);
  g_free(title);
  g_free(name);
}

static void read_hunks(HunkRequest *request)
{
  const char *unstaged[] = { "diff",        "--no-color", "--no-ext-diff", "--no-textconv",
                             "--unified=3", "--",         request->path,   NULL };
  const char *staged[] = { "diff", "--cached",    "--no-color", "--no-ext-diff", "--no-textconv", "--unified=3",
                           "--",   request->path, NULL };

  run_git_owned(request->staged ? staged : unstaged, NULL, FALSE, on_hunks_read, request,
                (GDestroyNotify)hunk_request_free);
}

static void on_saved_for_stage_hunk(App *t, gpointer data)
{
  HunkRequest *request = data;

  if (!t->closing && g_strcmp0(t->project.git_top, request->root) == 0)
    read_hunks(request);
  else
    hunk_request_free(request);
}

void scm_choose_hunk(const char *path, gboolean staged)
{
  HunkRequest *request = g_new0(HunkRequest, 1);

  request->path = g_strdup(path);
  request->root = g_strdup(scm.app->project.git_top);
  request->staged = staged;
  if (staged)
    read_hunks(request);
  else
    save_all(scm.app, on_saved_for_stage_hunk, request);
}

/* Stage All: every change (after saving the editors). */
static void stage_all(void)
{
  save_all(scm.app, on_saved_for_add, NULL);
}

/* Unstage All. */
static void unstage_all(void)
{
  const char *first[] = { "rm", "--cached", "-r", "-q", ".", NULL };
  const char *args[] = { "reset", "-q", NULL };
  run_simple(scm.initial ? first : args, "Could not unstage the changes");
}

typedef struct {
  char *path;
  gboolean untracked;
} Discard;

/* An editor on the file still holds the discarded text, and would save it back. */
static void on_discarded(gboolean ok, const char *out G_GNUC_UNUSED, const char *err, gpointer data)
{
  char *full = data;
  Tab *tab = find_tab(scm.app, TAB_EDITOR, full);

  if (!ok)
    set_note(*err ? err : "Could not discard the changes", TRUE);
  else if (tab)
    editor_reload(tab);
  g_free(full);
}

/* The user answered "Discard changes?": untracked files go to the trash, others are restored from
 * the index. */
static void on_discard_answer(GObject *dialog, GAsyncResult *result, gpointer data)
{
  Discard *d = data;

  if (gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(dialog), result, NULL) == 1) {
    if (d->untracked) {
      /* Never in git: to the trash, where it can still be found. */
      char *full = g_build_filename(scm.app->project.git_top, d->path, NULL);
      GFile *file = g_file_new_for_path(full);
      GError *error = NULL;
      if (!g_file_trash(file, NULL, &error)) {
        set_note(error->message, TRUE);
        g_error_free(error);
      } else {
        Tab *tab = find_tab(scm.app, TAB_EDITOR, full);
        if (tab)
          quit_editor(tab); /* the file is gone */
      }
      g_object_unref(file);
      g_free(full);
      refresh_branch(scm.app);
    } else {
      const char *args[] = { "restore", "--", d->path, NULL };
      set_note(NULL, FALSE);
      run_git(args, NULL, FALSE, on_discarded, g_build_filename(scm.app->project.git_top, d->path, NULL));
    }
  }
  g_free(d->path);
  g_free(d);
}

/* The ↶ button: asks first, then discards the file's unstaged changes. */
static void discard(const Change *c)
{
  Discard *d = g_new0(Discard, 1);
  GtkAlertDialog *dialog = gtk_alert_dialog_new("Discard changes in %s?", c->path);
  const char *buttons[] = { "Cancel", "Discard", NULL };

  d->path = g_strdup(c->path);
  d->untracked = c->code == 'U';
  gtk_alert_dialog_set_detail(dialog, d->untracked ? "The file is not in git; it goes to the trash."
                                                   : "Its unstaged changes are lost.");
  gtk_alert_dialog_set_buttons(dialog, buttons);
  gtk_alert_dialog_set_cancel_button(dialog, 0);
  gtk_alert_dialog_set_default_button(dialog, 0);
  gtk_alert_dialog_choose(dialog, scm.app->window, NULL, on_discard_answer, d);
  g_object_unref(dialog);
}

/* Commit and push ---------------------------------------------------------------------------- */

static char *message_text(void)
{
  GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(scm.message));
  GtkTextIter start, end;

  gtk_text_buffer_get_bounds(buffer, &start, &end);
  return g_strstrip(gtk_text_buffer_get_text(buffer, &start, &end, FALSE));
}

char *scm_message(void)
{
  return scm.app ? message_text() : g_strdup("");
}

void scm_set_message(const char *message)
{
  if (scm.app)
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(scm.message)), message ? message : "", -1);
}

/* git commit finished: the message box empties, or the error shows. */
static void on_committed(gboolean ok, const char *out, const char *err, gpointer data G_GNUC_UNUSED)
{
  if (!ok) {
    /* "nothing to commit" comes on stdout */
    char *said = g_strstrip(g_strdup(out));
    set_note(*err ? err : *said ? said : "The commit failed", TRUE);
    g_free(said);
    return;
  }
  gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(scm.message)), "", -1);
  set_note("Committed", FALSE);
}

/* With nothing staged, the commit takes every change. */
static void on_staged_all(gboolean ok, const char *out G_GNUC_UNUSED, const char *err, gpointer data)
{
  const char *args[] = { "commit", "-q", "-F", "-", NULL };

  if (!ok) {
    set_note(*err ? err : "Could not stage the changes", TRUE);
    g_free(data);
    return;
  }
  run_git(args, data, FALSE, on_committed, NULL);
  g_free(data);
}

/* The editors are saved: commit (with nothing staged, everything is staged first). */
static void on_saved_for_commit(App *t, gpointer data)
{
  char *message = data;

  scm.committing = FALSE;
  if (!t->closing) {
    if (scm.staged->len == 0) {
      const char *add[] = { "add", "-A", NULL };
      run_git(add, NULL, FALSE, on_staged_all, g_strdup(message));
    } else {
      const char *args[] = { "commit", "-q", "-F", "-", NULL };
      run_git(args, message, FALSE, on_committed, NULL);
    }
  }
  g_free(message);
}

/* The Commit button (or Ctrl+Enter in the message): checks there is a message and something to
 * commit, saves the editors, then commits. */
static void commit(void)
{
  char *message = message_text();

  if (scm.busy || scm.committing) {
    g_free(message);
    return;
  }
  if (!*message) {
    set_note("Type a commit message first", TRUE);
    gtk_widget_grab_focus(scm.message);
  } else if (scm.staged->len == 0 && scm.changes->len == 0 && !any_unsaved(scm.app)) {
    set_note("Nothing to commit", TRUE);
  } else {
    /* git commits what is on disk: unsaved edits are saved first. */
    set_note(NULL, FALSE);
    scm.committing = TRUE;
    save_all(scm.app, on_saved_for_commit, message);
    return;
  }
  g_free(message);
}

/* git push finished: "Pushed", or why not (a password prompt it could not show gets its own hint). */
static void on_pushed(gboolean ok, const char *out G_GNUC_UNUSED, const char *err, gpointer data G_GNUC_UNUSED)
{
  if (ok) {
    set_note("Pushed", FALSE);
    return;
  }
  if (strstr(err, "terminal prompts disabled") || strstr(err, "could not read Username"))
    set_note("git needs a password or token, and there is no terminal to ask in. Push once from the "
             "shell, or set up a credential helper (e.g. gh auth setup-git) or an SSH key.",
             TRUE);
  else
    set_note(*err ? err : "The push failed", TRUE);
}

/* The Push button. */
static void push(void)
{
  /* A branch with no upstream yet goes to origin under its own name. */
  const char *args[] = { "push", NULL };
  const char *first[] = { "push", "-u", "origin", "HEAD", NULL };

  if (scm.busy)
    return;
  set_note("Pushing…", FALSE);
  run_git(scm.upstream ? args : first, NULL, FALSE, on_pushed, NULL);
}

/* Lists -------------------------------------------------------------------------------------- */

static void clear_list(GtkListBox *list)
{
  GtkListBoxRow *row;

  while ((row = gtk_list_box_get_row_at_index(list, 0)))
    gtk_list_box_remove(list, GTK_WIDGET(row));
}

/* A small flat button with a symbol (+, −, ↶, ⟳), shown on hover. */
static GtkWidget *icon_button(const char *text, const char *tooltip)
{
  GtkWidget *button = gtk_button_new_with_label(text);

  gtk_button_set_has_frame(GTK_BUTTON(button), FALSE);
  gtk_widget_set_focusable(button, FALSE);
  gtk_widget_set_tooltip_text(button, tooltip);
  gtk_widget_add_css_class(button, "tool");
  return button;
}

/* Puts a row's main content in a flat button that activates the row: a click, Enter or the
 * accessibility "click" action all open it. */
static GtkWidget *open_button(GtkWidget *row, GtkWidget *content)
{
  GtkWidget *button = gtk_button_new();

  gtk_button_set_has_frame(GTK_BUTTON(button), FALSE);
  gtk_widget_add_css_class(button, "open");
  gtk_widget_set_hexpand(button, TRUE);
  gtk_button_set_child(GTK_BUTTON(button), content);
  g_signal_connect_swapped(button, "clicked", G_CALLBACK(gtk_widget_activate), row);
  return button;
}

/* The colored status letter on the right of a row (M, A, D, R, U, !). */
static GtkWidget *code_label(char code)
{
  char text[2] = { code, 0 };
  char *class = g_strdup_printf("code-%c", code == '!' ? 'x' : code);
  GtkWidget *label = gtk_label_new(text);

  gtk_widget_add_css_class(label, "code");
  gtk_widget_add_css_class(label, class);
  g_free(class);
  return label;
}

/* A row: name, its folder dimmed, then the buttons and the status letter on the right. */
static GtkWidget *file_row(const char *path, char code, GtkWidget **box_out)
{
  GtkWidget *row = gtk_list_box_row_new();
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *names = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  char *name = g_path_get_basename(path), *dir = g_path_get_dirname(path);
  GtkWidget *label = gtk_label_new(name);
  GtkWidget *folder = gtk_label_new(strcmp(dir, ".") == 0 ? "" : dir);

  gtk_label_set_ellipsize(GTK_LABEL(folder), PANGO_ELLIPSIZE_START);
  gtk_widget_add_css_class(folder, "dim");
  gtk_widget_set_hexpand(folder, TRUE);
  gtk_label_set_xalign(GTK_LABEL(folder), 0);
  gtk_box_append(GTK_BOX(names), label);
  gtk_box_append(GTK_BOX(names), folder);
  gtk_box_append(GTK_BOX(box), open_button(row, names));
  gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
  gtk_widget_set_tooltip_text(row, path);
  *box_out = box;
  g_object_set_data(G_OBJECT(row), "code", GINT_TO_POINTER(code));
  g_free(dir);
  g_free(name);
  return row;
}

/* clang-format off */
static void on_stage_clicked(GtkButton *button G_GNUC_UNUSED, Change *c) { stage(c->path); }
static void on_unstage_clicked(GtkButton *button G_GNUC_UNUSED, Change *c) { unstage(c); }
static void on_discard_clicked(GtkButton *button G_GNUC_UNUSED, Change *c) { discard(c); }
/* clang-format on */

/* Fills a list from scm.staged or scm.changes, with the buttons each row needs. */
static void fill_changes(GtkListBox *list, GPtrArray *changes, gboolean staged)
{
  clear_list(list);
  for (guint i = 0; i < changes->len; i++) {
    Change *c = g_ptr_array_index(changes, i);
    GtkWidget *box, *row = file_row(c->path, c->code, &box);
    GtkWidget *button;

    g_object_set_data(G_OBJECT(row), "change", c);
    if (staged) {
      button = icon_button("−", "Unstage");
      g_signal_connect(button, "clicked", G_CALLBACK(on_unstage_clicked), c);
      gtk_box_append(GTK_BOX(box), button);
    } else {
      button = icon_button("↶", "Discard changes");
      g_signal_connect(button, "clicked", G_CALLBACK(on_discard_clicked), c);
      gtk_box_append(GTK_BOX(box), button);
      button = icon_button("+", "Stage");
      g_signal_connect(button, "clicked", G_CALLBACK(on_stage_clicked), c);
      gtk_box_append(GTK_BOX(box), button);
    }
    gtk_box_append(GTK_BOX(box), code_label(c->code));
    gtk_list_box_append(list, row);
  }
}

/* Whether path has staged changes too. */
static gboolean has_staged(const char *path)
{
  for (guint i = 0; i < scm.staged->len; i++)
    if (strcmp(((Change *)g_ptr_array_index(scm.staged, i))->path, path) == 0)
      return TRUE;
  return FALSE;
}

/* A staged change: the staged version (the index) next to the last commit's. */
static void show_staged(App *t, const Change *c)
{
  if (c->code == 'A')
    compare_revisions(t, NULL, NULL, INDEX_REV, c->path); /* new: only the staged version */
  else if (c->code == 'D')
    compare_revisions(t, "HEAD", c->path, NULL, NULL); /* only the old version is left */
  else                                                 /* renames and copies: next to where the file came from */
    compare_revisions(t, "HEAD", c->old_path ? c->old_path : c->path, INDEX_REV, c->path);
}

/* An unstaged change: the working file next to its staged version when it has staged changes too,
 * else next to the last commit (the same then, and the gutter follows new commits). */
static void show_unstaged(App *t, const Change *c)
{
  char *full = g_build_filename(t->project.git_top, c->path, NULL);

  if (c->code == 'D')
    compare_revisions(t, INDEX_REV, c->path, NULL, NULL); /* only the staged version is left */
  else if (c->code != 'M')
    open_file(t, full); /* untracked, conflicted: nothing to compare with */
  else if (has_staged(c->path) || scm.initial)
    compare_with_index(t, full);
  else
    compare_with_head(t, full);
  g_free(full);
}

/* A file in Staged Changes or Changes was clicked: show its changes side by side. */
static void on_change_activated(GtkListBox *list, GtkListBoxRow *row, gpointer data G_GNUC_UNUSED)
{
  Change *c = g_object_get_data(G_OBJECT(row), "change");

  if (!c || !scm.app->project.git_top)
    return;
  if (list == scm.staged_list)
    show_staged(scm.app, c);
  else
    show_unstaged(scm.app, c);
}

/* A changed file's right-click menu: a copy of its change (the lists are made again at every git
 * status), and whether it is staged. */
typedef struct {
  Change change;
  gboolean staged;
} MenuChange;

static void menu_change_free(MenuChange *m)
{
  g_free(m->change.path);
  g_free(m->change.old_path);
  g_free(m);
}

/* The file's whole path, or NULL outside a repository. */
static char *change_full_path(const Change *c)
{
  return scm.app->project.git_top ? g_build_filename(scm.app->project.git_top, c->path, NULL) : NULL;
}

static void menu_open_changes(gpointer data)
{
  MenuChange *m = data;

  if (!scm.app->project.git_top)
    return;
  if (m->staged)
    show_staged(scm.app, &m->change);
  else
    show_unstaged(scm.app, &m->change);
}

static void menu_open_file(gpointer data)
{
  char *full = change_full_path(&((MenuChange *)data)->change);

  if (full && g_file_test(full, G_FILE_TEST_IS_REGULAR))
    open_file(scm.app, full);
  g_free(full);
}

/* clang-format off */
static void menu_stage(gpointer data) { stage(((MenuChange *)data)->change.path); }
static void menu_unstage(gpointer data) { unstage(&((MenuChange *)data)->change); }
static void menu_stage_hunk(gpointer data) { scm_choose_hunk(((MenuChange *)data)->change.path, FALSE); }
static void menu_unstage_hunk(gpointer data) { scm_choose_hunk(((MenuChange *)data)->change.path, TRUE); }
static void menu_discard(gpointer data) { discard(&((MenuChange *)data)->change); }
static void menu_copy_relative(gpointer data) { copy_to_clipboard(((MenuChange *)data)->change.path); }
/* clang-format on */

static void menu_copy_path(gpointer data)
{
  char *full = change_full_path(&((MenuChange *)data)->change);

  copy_to_clipboard(full);
  g_free(full);
}

static void menu_reveal(gpointer data)
{
  char *full = change_full_path(&((MenuChange *)data)->change);

  if (full) {
    show_sidebar_page(scm.app, "files");
    files_reveal(scm.app, full);
  }
  g_free(full);
}

/* A right click in Staged Changes or Changes: the file's menu. */
static void on_changes_menu(GtkWidget *list, double x, double y, gpointer data G_GNUC_UNUSED)
{
  GtkListBoxRow *row = gtk_list_box_get_row_at_y(GTK_LIST_BOX(list), (int)y);
  Change *c = row ? g_object_get_data(G_OBJECT(row), "change") : NULL;
  MenuItem items[12];
  int n = 0;

  if (!c)
    return;
  MenuChange *m = g_new0(MenuChange, 1);
  m->change = (Change){ g_strdup(c->path), g_strdup(c->old_path), c->code };
  m->staged = list == GTK_WIDGET(scm.staged_list);
  items[n++] = (MenuItem){ "Open Changes", menu_open_changes };
  if (c->code != 'D')
    items[n++] = (MenuItem){ "Open File", menu_open_file };
  items[n++] = (MenuItem){ NULL, NULL };
  if (m->staged) {
    items[n++] = (MenuItem){ "Unstage Changes", menu_unstage };
    if (c->code == 'M')
      items[n++] = (MenuItem){ "Unstage Hunk…", menu_unstage_hunk };
  } else {
    items[n++] = (MenuItem){ "Stage Changes", menu_stage };
    if (c->code == 'M')
      items[n++] = (MenuItem){ "Stage Hunk…", menu_stage_hunk };
    items[n++] = (MenuItem){ "Discard Changes…", menu_discard };
  }
  items[n++] = (MenuItem){ NULL, NULL };
  items[n++] = (MenuItem){ "Copy Path", menu_copy_path };
  items[n++] = (MenuItem){ "Copy Relative Path", menu_copy_relative };
  if (c->code != 'D')
    items[n++] = (MenuItem){ "Reveal in Files", menu_reveal };
  context_menu(list, x, y, items, n, m, (GDestroyNotify)menu_change_free);
}

/* Commits ------------------------------------------------------------------------------------ */

static void on_commit_files(gboolean ok, const char *out, const char *err G_GNUC_UNUSED, gpointer data)
{
  char *commit = data;
  GtkListBoxRow *after = NULL;
  int n = 0;

  /* The commit's row: its files go right below it. */
  for (GtkWidget *w = gtk_widget_get_first_child(GTK_WIDGET(scm.commits_list)); w; w = gtk_widget_get_next_sibling(w))
    if (g_strcmp0(g_object_get_data(G_OBJECT(w), "commit"), commit) == 0)
      after = GTK_LIST_BOX_ROW(w);
  if (!ok || !after || g_strcmp0(scm.open_commit, commit) != 0) {
    g_free(commit);
    return;
  }
  int position = gtk_list_box_row_get_index(after) + 1;
  /* --name-status: "M<tab>path", renames and copies "R100<tab>old<tab>new". */
  char **lines = g_strsplit(out, "\n", -1);
  for (int i = 0; lines[i]; i++) {
    char **f = g_strsplit(lines[i], "\t", 3);
    if (g_strv_length(f) >= 2 && *f[0]) {
      char code = f[0][0];
      const char *path = g_strv_length(f) == 3 ? f[2] : f[1];
      const char *old_path = f[1];
      GtkWidget *box, *row = file_row(path, code, &box);
      gtk_widget_add_css_class(row, "file-row");
      gtk_box_append(GTK_BOX(box), code_label(code));
      g_object_set_data_full(G_OBJECT(row), "file-commit", g_strdup(commit), g_free);
      g_object_set_data_full(G_OBJECT(row), "file-new", code == 'D' ? NULL : g_strdup(path), g_free);
      g_object_set_data_full(G_OBJECT(row), "file-old", code == 'A' ? NULL : g_strdup(old_path), g_free);
      gtk_list_box_insert(scm.commits_list, row, position + n++);
    }
    g_strfreev(f);
  }
  g_strfreev(lines);
  g_free(commit);
}

/* Folds the open commit: removes its file rows from the Commits list. */
static void remove_commit_files(void)
{
  GtkWidget *w = gtk_widget_get_first_child(GTK_WIDGET(scm.commits_list));

  while (w) {
    GtkWidget *next = gtk_widget_get_next_sibling(w);
    if (g_object_get_data(G_OBJECT(w), "file-commit"))
      gtk_list_box_remove(scm.commits_list, w);
    w = next;
  }
}

/* A click in Commits: a commit unfolds (or folds) its files; a file shows what that commit changed
 * in it. */
static void on_commit_activated(GtkListBox *list G_GNUC_UNUSED, GtkListBoxRow *row, gpointer data G_GNUC_UNUSED)
{
  const char *commit = g_object_get_data(G_OBJECT(row), "commit");
  const char *file_commit = g_object_get_data(G_OBJECT(row), "file-commit");

  if (file_commit) {
    /* What the commit did to the file: its own version next to its parent's (a first commit
         * has no parent, and that side stays empty). */
    char *parent = g_strconcat(file_commit, "^", NULL);
    compare_revisions(scm.app, parent, g_object_get_data(G_OBJECT(row), "file-old"), file_commit,
                      g_object_get_data(G_OBJECT(row), "file-new"));
    g_free(parent);
    return;
  }
  if (!commit)
    return;
  remove_commit_files();
  if (g_strcmp0(scm.open_commit, commit) == 0) { /* clicked again: fold it */
    g_clear_pointer(&scm.open_commit, g_free);
    return;
  }
  g_free(scm.open_commit);
  scm.open_commit = g_strdup(commit);
  /* A merge commit lists what it brought to its first parent (by default git shows a combined
     * diff, which has no file list). */
  const char *args[] = { "-c", "core.quotePath=false",       "show",      "--name-status",
                         "-M", "--diff-merges=first-parent", "--format=", commit,
                         NULL };
  run_git(args, NULL, TRUE, on_commit_files, g_strdup(commit));
}

/* A commit in the Commits list, from git log. */
typedef struct {
  char *hash, *short_hash, *subject, *message, *author, *email, *ago, *date, *refs, *stat;
} CommitInfo;

static void commit_info_free(CommitInfo *c)
{
  g_free(c->hash);
  g_free(c->short_hash);
  g_free(c->subject);
  g_free(c->message);
  g_free(c->author);
  g_free(c->email);
  g_free(c->ago);
  g_free(c->date);
  g_free(c->refs);
  g_free(c->stat);
  g_free(c);
}

/* "6 files changed, 246 insertions(+), 10 deletions(-)" as markup: insertions green, deletions red. */
static char *stat_markup(const char *stat)
{
  GString *out = g_string_new(NULL);
  char **parts = g_strsplit(stat, ", ", -1);

  for (int i = 0; parts[i]; i++) {
    char *escaped = g_markup_escape_text(parts[i], -1);
    const char *color = strstr(parts[i], "insertion")  ? palette_hex(PAL_GIT_ADDED)
                        : strstr(parts[i], "deletion") ? palette_hex(PAL_GIT_DELETED)
                                                       : NULL;
    if (i)
      g_string_append(out, ", ");
    if (color)
      g_string_append_printf(out, "<span foreground=\"%s\">%s</span>", color, escaped);
    else
      g_string_append(out, escaped);
    g_free(escaped);
  }
  g_strfreev(parts);
  return g_string_free(out, FALSE);
}

/* The refs git log gives ("HEAD -> main, origin/main, tag: v1") one by one: "main", "origin/main",
 * "v1". */
static char **ref_names(const char *refs)
{
  char **names = g_strsplit(refs ? refs : "", ", ", -1);

  for (int i = 0; names[i]; i++) {
    const char *name = names[i];
    if (g_str_has_prefix(name, "HEAD -> "))
      name += strlen("HEAD -> ");
    else if (g_str_has_prefix(name, "tag: "))
      name += strlen("tag: ");
    memmove(names[i], name, strlen(name) + 1);
  }
  return names;
}

static GtkWidget *tip_label(const char *markup, const char *css_class)
{
  GtkWidget *label = gtk_label_new(NULL);

  gtk_label_set_markup(GTK_LABEL(label), markup);
  gtk_label_set_xalign(GTK_LABEL(label), 0);
  gtk_label_set_wrap(GTK_LABEL(label), TRUE);
  gtk_label_set_max_width_chars(GTK_LABEL(label), 72);
  if (css_class)
    gtk_widget_add_css_class(label, css_class);
  return label;
}

/* What shows when the pointer rests on a commit: its author, when, whole message, line counts,
 * refs and hash. */
static GtkWidget *commit_tip(const CommitInfo *c)
{
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  char *markup;

  gtk_widget_add_css_class(box, "tcode-commit-tip");
  markup = g_markup_printf_escaped("<b><span foreground=\"%s\">%s</span></b>, %s (%s)", palette_hex(PAL_LINK),
                                   c->author, c->ago, c->date);
  gtk_box_append(GTK_BOX(box), tip_label(markup, NULL));
  g_free(markup);
  markup = g_markup_escape_text(c->message, -1);
  gtk_box_append(GTK_BOX(box), tip_label(markup, NULL));
  g_free(markup);
  if (*c->stat) {
    GtkWidget *rule = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(rule, "rule");
    gtk_box_append(GTK_BOX(box), rule);
    markup = stat_markup(c->stat);
    gtk_box_append(GTK_BOX(box), tip_label(markup, NULL));
    g_free(markup);
  }
  char **refs = ref_names(c->refs);
  if (refs[0] && *refs[0]) {
    GtkWidget *line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    for (int i = 0; refs[i]; i++) {
      GtkWidget *ref = gtk_label_new(refs[i]);
      gtk_widget_add_css_class(ref, "ref");
      gtk_box_append(GTK_BOX(line), ref);
    }
    gtk_box_append(GTK_BOX(box), line);
  }
  g_strfreev(refs);
  markup = g_markup_printf_escaped("◆ %s", c->short_hash);
  gtk_box_append(GTK_BOX(box), tip_label(markup, "hash"));
  g_free(markup);
  return box;
}

static gboolean on_commit_tooltip(GtkWidget *row, int x G_GNUC_UNUSED, int y G_GNUC_UNUSED,
                                  gboolean keyboard G_GNUC_UNUSED, GtkTooltip *tooltip, gpointer data G_GNUC_UNUSED)
{
  CommitInfo *c = g_object_get_data(G_OBJECT(row), "commit-info");

  if (c)
    gtk_tooltip_set_custom(tooltip, commit_tip(c));
  return c != NULL;
}

/* Commit details in a tab ---------------------------------------------------------------------- */

#define DETAIL_DIFF_LINES 4000 /* a bigger diff is cut there */

/* What git show gave for a commit: "--numstat" lines, then the patch. Written as Markdown into the
 * temp folder and opened as a preview. */
static void on_commit_details(gboolean ok, const char *out, const char *err, gpointer data)
{
  CommitInfo *c = data;
  App *t = scm.app;
  GString *md = g_string_new(NULL);
  const char *patch = out ? strstr(out, "diff --git ") : NULL;
  char *numstat = out ? g_strndup(out, patch ? (gsize)(patch - out) : strlen(out)) : g_strdup("");
  char **refs = ref_names(c->refs);

  if (!ok) {
    set_note(err && *err ? err : "git show failed", TRUE);
    commit_info_free(c);
    g_free(numstat);
    g_strfreev(refs);
    g_string_free(md, TRUE);
    return;
  }
  g_string_append_printf(md, "# %s\n\n**%s** &lt;%s&gt; · %s (%s)\n\n`%s`", c->subject, c->author, c->email, c->date,
                         c->ago, c->hash);
  for (int i = 0; refs[i] && *refs[i]; i++)
    g_string_append_printf(md, " · `%s`", refs[i]);
  g_string_append(md, "\n\n");
  const char *below = strchr(c->message, '\n'); /* the message below its subject */
  char *body = g_strstrip(g_strdup(below ? below : ""));
  if (*body)
    g_string_append_printf(md, "%s\n\n", body);
  g_free(body);
  if (*c->stat)
    g_string_append_printf(md, "---\n\n%s\n\n", c->stat);

  /* --numstat: "added<tab>deleted<tab>path" ("-" for binary files) */
  char **lines = g_strsplit(numstat, "\n", -1);
  gboolean any = FALSE;
  for (int i = 0; lines[i]; i++) {
    char **f = g_strsplit(lines[i], "\t", 3);
    if (g_strv_length(f) == 3) {
      if (!any)
        g_string_append(md, "| File | + | − |\n|---|---:|---:|\n");
      any = TRUE;
      g_string_append_printf(md, "| `%s` | %s | %s |\n", f[2], f[0], f[1]);
    }
    g_strfreev(f);
  }
  g_strfreev(lines);
  if (patch) {
    char **diff = g_strsplit(patch, "\n", DETAIL_DIFF_LINES + 1);
    guint n = g_strv_length(diff);
    g_string_append(md, "\n````diff\n");
    for (guint i = 0; i < MIN(n, DETAIL_DIFF_LINES); i++)
      g_string_append_printf(md, "%s\n", diff[i]);
    g_string_append(md, "````\n");
    if (n > DETAIL_DIFF_LINES)
      g_string_append_printf(md, "\n*The diff goes on: `git show %s` shows all of it.*\n", c->short_hash);
    g_strfreev(diff);
  }

  char *name = g_strdup_printf("Commit %s.md", c->short_hash);
  char *path = g_build_filename(t->paths.temp, name, NULL);
  GError *error = NULL;
  g_mkdir_with_parents(t->paths.temp, 0700);
  if (g_file_set_contents(path, md->str, -1, &error))
    open_preview(t, path);
  else
    set_note(error->message, TRUE);
  g_clear_error(&error);
  g_free(path);
  g_free(name);
  g_strfreev(refs);
  g_free(numstat);
  g_string_free(md, TRUE);
  commit_info_free(c);
}

static CommitInfo *commit_info_copy(const CommitInfo *c)
{
  CommitInfo *copy = g_new(CommitInfo, 1);

  *copy = (CommitInfo){ g_strdup(c->hash),   g_strdup(c->short_hash), g_strdup(c->subject), g_strdup(c->message),
                        g_strdup(c->author), g_strdup(c->email),      g_strdup(c->ago),     g_strdup(c->date),
                        g_strdup(c->refs),   g_strdup(c->stat) };
  return copy;
}

/* "Open Commit Details": the commit's message, author, line counts and diff, in a tab. */
static void open_commit_details(gpointer data)
{
  const CommitInfo *c = data;
  const char *args[] = { "-c",
                         "core.quotePath=false",
                         "show",
                         "-M",
                         "--diff-merges=first-parent",
                         "--format=",
                         "--numstat",
                         "--patch",
                         c->hash,
                         NULL };

  run_git(args, NULL, TRUE, on_commit_details, commit_info_copy(c));
}

static void copy_commit_hash(gpointer data)
{
  copy_to_clipboard(((CommitInfo *)data)->hash);
}

static void copy_commit_message(gpointer data)
{
  copy_to_clipboard(((CommitInfo *)data)->message);
}

/* A file under an open commit: what its row's click does, its path. */
static void open_commit_file(gpointer data)
{
  gtk_widget_activate(data);
}

static void open_commit_file_now(gpointer data)
{
  GtkWidget *row = data;
  const char *path = g_object_get_data(G_OBJECT(row), "file-new");
  char *full = path && scm.app->project.git_top ? g_build_filename(scm.app->project.git_top, path, NULL) : NULL;

  if (full && g_file_test(full, G_FILE_TEST_IS_REGULAR))
    open_file(scm.app, full);
  g_free(full);
}

static void copy_commit_file_path(gpointer data)
{
  const char *path = g_object_get_data(G_OBJECT(data), "file-new");

  copy_to_clipboard(path ? path : g_object_get_data(G_OBJECT(data), "file-old"));
}

/* The right-click menu of a row in Commits: a commit's, or a file's under it. */
static void on_commits_menu(GtkWidget *list, double x, double y, gpointer data G_GNUC_UNUSED)
{
  static const MenuItem commit_items[] = {
    { "Open Commit Details", open_commit_details },
    { NULL, NULL },
    { "Copy Commit Hash", copy_commit_hash },
    { "Copy Commit Message", copy_commit_message },
  };
  static const MenuItem file_items[] = {
    { "Open Changes", open_commit_file },
    { "Open File", open_commit_file_now },
    { NULL, NULL },
    { "Copy Path", copy_commit_file_path },
  };
  GtkListBoxRow *row = gtk_list_box_get_row_at_y(GTK_LIST_BOX(list), (int)y);
  CommitInfo *c = row ? g_object_get_data(G_OBJECT(row), "commit-info") : NULL;

  if (c)
    context_menu(list, x, y, commit_items, G_N_ELEMENTS(commit_items), commit_info_copy(c),
                 (GDestroyNotify)commit_info_free);
  else if (row && g_object_get_data(G_OBJECT(row), "file-commit"))
    context_menu(list, x, y, file_items, G_N_ELEMENTS(file_items), g_object_ref(row), g_object_unref);
}

void scm_open_commit_details(int n)
{
  int i = 0;

  for (GtkWidget *row = gtk_widget_get_first_child(GTK_WIDGET(scm.commits_list)); row;
       row = gtk_widget_get_next_sibling(row)) {
    CommitInfo *c = g_object_get_data(G_OBJECT(row), "commit-info");
    if (c && i++ == n) {
      open_commit_details(c);
      return;
    }
  }
}

/* data: the HEAD the log was asked for. A log that comes after another project opened, or after
 * HEAD moved again, is not shown. */
static void on_log(gboolean ok, const char *out, const char *err G_GNUC_UNUSED, gpointer data)
{
  char **records;
  gboolean stale = g_strcmp0(data, scm.head) != 0;

  g_free(data);
  if (stale)
    return;
  clear_list(scm.commits_list);
  g_clear_pointer(&scm.open_commit, g_free);
  if (!ok)
    return;
  /* each commit: \x1e, then its fields between \x1f, its message up to \x1d, then --shortstat */
  records = g_strsplit(out, "\x1e", -1);
  for (int i = 0; records[i]; i++) {
    char **f = g_strsplit(records[i], "\x1f", 9);
    if (g_strv_length(f) == 9) {
      CommitInfo *c = g_new0(CommitInfo, 1);
      char *end = strchr(f[8], '\x1d');
      c->hash = g_strdup(f[0]);
      c->short_hash = g_strdup(f[1]);
      c->subject = g_strdup(f[2]);
      c->author = g_strdup(f[3]);
      c->email = g_strdup(f[4]);
      c->ago = g_strdup(f[5]);
      c->date = g_strdup(f[6]);
      c->refs = g_strdup(f[7]);
      c->message = g_strstrip(g_strndup(f[8], end ? (gsize)(end - f[8]) : strlen(f[8])));
      c->stat = g_strstrip(g_strdup(end ? end + 1 : ""));

      GtkWidget *row = gtk_list_box_row_new();
      GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
      GtkWidget *subject = gtk_label_new(c->subject);
      char *info = g_strdup_printf("%s · %s", c->short_hash, c->ago);
      GtkWidget *detail = gtk_label_new(info);

      gtk_label_set_ellipsize(GTK_LABEL(subject), PANGO_ELLIPSIZE_END);
      gtk_label_set_xalign(GTK_LABEL(subject), 0);
      gtk_widget_set_hexpand(subject, TRUE);
      gtk_widget_add_css_class(detail, "dim");
      gtk_box_append(GTK_BOX(box), subject);
      gtk_box_append(GTK_BOX(box), detail);
      gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), open_button(row, box));
      gtk_widget_set_has_tooltip(row, TRUE);
      g_signal_connect(row, "query-tooltip", G_CALLBACK(on_commit_tooltip), NULL);
      g_object_set_data_full(G_OBJECT(row), "commit", g_strdup(c->hash), g_free);
      g_object_set_data_full(G_OBJECT(row), "commit-info", c, (GDestroyNotify)commit_info_free);
      gtk_list_box_append(scm.commits_list, row);
      g_free(info);
    }
    g_strfreev(f);
  }
  g_strfreev(records);
}

/* Lists the last 100 commits (on_log shows them). */
static void load_commits(void)
{
  const char *args[] = { "log",
                         "-n",
                         "100",
                         "--shortstat",
                         "--date=format:%Y-%m-%d %H:%M", /* the same in every locale */
                         "--format=%x1e%H%x1f%h%x1f%s%x1f%an%x1f%ae%x1f%ar%x1f%ad%x1f%D%x1f%B%x1d",
                         NULL };

  run_git(args, NULL, TRUE, on_log, g_strdup(scm.head));
}

/* Status ------------------------------------------------------------------------------------- */

static void add_change(GPtrArray *list, const char *path, const char *old_path, char code)
{
  Change *c = g_new0(Change, 1);

  c->path = g_strdup(path);
  c->old_path = g_strdup(old_path);
  c->code = code;
  g_ptr_array_add(list, c);
}

/* Sorts changes by path. */
static int compare_changes(gconstpointer a, gconstpointer b)
{
  return g_strcmp0((*(Change **)a)->path, (*(Change **)b)->path);
}

/* The buttons' labels: "Publish Branch" / "Push ↑2 ↓1", and "Commit All" when nothing is staged. */
static void update_buttons(void)
{
  char *text;

  if (!scm.upstream)
    text = g_strdup("Publish Branch");
  else if (scm.behind)
    text = g_strdup_printf("Push ↑%d ↓%d", scm.ahead, scm.behind);
  else if (scm.ahead)
    text = g_strdup_printf("Push ↑%d", scm.ahead);
  else
    text = g_strdup("Push");
  gtk_button_set_label(GTK_BUTTON(scm.push), text);
  g_free(text);
  gtk_button_set_label(GTK_BUTTON(scm.commit),
                       scm.staged->len == 0 && scm.changes->len > 0 ? "✓ Commit All" : "✓ Commit");
}

/* Takes the output of "git status --porcelain=v2 --branch -z --untracked-files=all" (NULL: not a
 * repository, or git failed). */
static void show_changes(App *t);

void scm_update(App *t, const char *out, gsize size)
{
  char *head = NULL;

  if (!scm.app || t->closing)
    return;
  g_ptr_array_set_size(scm.staged, 0);
  g_ptr_array_set_size(scm.changes, 0);
  g_clear_pointer(&scm.upstream, g_free);
  scm.ahead = scm.behind = 0;
  scm.initial = FALSE;

  for (const char *p = out, *end = out + size; out && p < end;) {
    const char *nul = memchr(p, '\0', end - p);
    gsize len = (nul ? nul : end) - p;
    char *record = g_strndup(p, len);
    p += len + 1;

    if (g_str_has_prefix(record, "# branch.oid ")) {
      head = g_strdup(record + 13);
      scm.initial = strcmp(head, "(initial)") == 0;
    } else if (g_str_has_prefix(record, "# branch.upstream ")) {
      scm.upstream = g_strdup(record + 18);
    } else if (g_str_has_prefix(record, "# branch.ab ")) {
      sscanf(record + 12, "+%d -%d", &scm.ahead, &scm.behind);
    } else if (record[0] == '?' && len > 2) {
      add_change(scm.changes, record + 2, NULL, 'U');
    } else if ((record[0] == '1' || record[0] == '2' || record[0] == 'u') && len > 4) {
      /* "1 XY sub mH mI mW hH hI path", "2 ... Xscore path\0orig", "u XY sub m1 m2 m3 mW h1 h2 h3 path" */
      int skip = record[0] == '1' ? 8 : record[0] == '2' ? 9 : 10;
      char *path = record;
      for (int i = 0; i < skip && path; i++)
        path = strchr(path, ' ') ? strchr(path, ' ') + 1 : NULL;
      char *orig = NULL;
      if (record[0] == '2' && p < end) {
        const char *onul = memchr(p, '\0', end - p);
        gsize olen = (onul ? onul : end) - p;
        orig = g_strndup(p, olen);
        p += olen + 1;
      }
      if (path && record[0] == 'u') {
        add_change(scm.changes, path, NULL, '!');
      } else if (path) {
        char x = record[2], y = record[3];
        if (x != '.')
          add_change(scm.staged, path, orig, x == 'T' ? 'M' : x);
        if (y != '.')
          add_change(scm.changes, path, NULL, y == 'T' ? 'M' : y);
      }
      g_free(orig);
    }
    g_free(record);
  }
  g_ptr_array_sort(scm.staged, compare_changes);
  g_ptr_array_sort(scm.changes, compare_changes);
  show_changes(t);
  gtk_widget_set_sensitive(scm.commit, out != NULL);
  gtk_widget_set_sensitive(scm.push, out != NULL && !scm.initial);
  if (!out)
    set_note(t->project.git_top ? NULL : "This folder is not in a git repository.", FALSE);

  /* Commits are listed again after a commit, a checkout or another project; outside a
     * repository (no HEAD) and before the first commit there are none. */
  if (!head || scm.initial || g_strcmp0(head, scm.head) != 0) {
    g_free(scm.head);
    scm.head = head;
    scm.commits_stale = FALSE;
    if (head && !scm.initial && !scm_commits_shown())
      scm.commits_stale = TRUE; /* folded: no git log until it opens */
    else if (head && !scm.initial)
      load_commits();
    else
      clear_list(scm.commits_list);
  } else {
    g_free(head);
  }
}

/* Fills the Staged Changes and Changes lists, their counts and the tab's badge, and the buttons
 * from scm.staged / scm.changes. */
static void show_changes(App *t)
{
  char *title;

  fill_changes(scm.staged_list, scm.staged, TRUE);
  fill_changes(scm.changes_list, scm.changes, FALSE);
  gtk_widget_set_visible(scm.staged_box, scm.staged->len > 0);
  title = g_strdup_printf("CHANGES  %u", scm.changes->len);
  gtk_label_set_text(scm.changes_title, title);
  g_free(title);
  title = g_strdup_printf("STAGED CHANGES  %u", scm.staged->len);
  gtk_label_set_text(scm.staged_title, title);
  g_free(title);
  update_buttons();
  /* The tab shows how many files changed. */
  title = scm.staged->len + scm.changes->len ? g_strdup_printf("SOURCE CONTROL  %u", scm.staged->len + scm.changes->len)
                                             : g_strdup("SOURCE CONTROL");
  gtk_stack_page_set_title(gtk_stack_get_page(t->dock.scm_stack, t->scm), title);
  g_free(title);
}

/* Building the panel ------------------------------------------------------------------------- */

static void on_message_changed(GtkTextBuffer *buffer, gpointer data G_GNUC_UNUSED)
{
  gtk_widget_set_visible(scm.placeholder, gtk_text_buffer_get_char_count(buffer) == 0);
}

/* Ctrl+Enter in the commit message commits. */
static gboolean on_message_key(GtkEventControllerKey *controller G_GNUC_UNUSED, guint key, guint code G_GNUC_UNUSED,
                               GdkModifierType state, gpointer data G_GNUC_UNUSED)
{
  if ((key == GDK_KEY_Return || key == GDK_KEY_KP_Enter) && (state & GDK_CONTROL_MASK)) {
    commit();
    return TRUE;
  }
  return FALSE;
}

/* A section opens or folds; Commits lists what it missed while folded. */
static void set_section_open(GtkWidget *box, gboolean open)
{
  GtkWidget *head = gtk_widget_get_first_child(box), *list = gtk_widget_get_next_sibling(head);

  gtk_widget_set_visible(list, open);
  gtk_image_set_from_icon_name(GTK_IMAGE(gtk_widget_get_first_child(head)),
                               open ? "pan-down-symbolic" : "pan-end-symbolic");
  if (open && box == scm.commits_box && scm.commits_stale) {
    scm.commits_stale = FALSE;
    load_commits();
  }
}

/* A click on a section's heading (not on its buttons) opens or folds it. */
static void on_section_click(GtkGestureClick *click, int n G_GNUC_UNUSED, double x G_GNUC_UNUSED,
                             double y G_GNUC_UNUSED, GtkWidget *box)
{
  GtkWidget *list = gtk_widget_get_next_sibling(gtk_widget_get_first_child(box));

  gtk_gesture_set_state(GTK_GESTURE(click), GTK_EVENT_SEQUENCE_CLAIMED);
  set_section_open(box, !gtk_widget_get_visible(list));
}

gboolean scm_commits_shown(void)
{
  return !scm.commits_box ||
         gtk_widget_get_visible(gtk_widget_get_next_sibling(gtk_widget_get_first_child(scm.commits_box)));
}

void scm_show_commits(gboolean shown)
{
  if (scm.commits_box)
    set_section_open(scm.commits_box, shown);
}

/* A section heading (a fold arrow, the title, buttons on its right); the list goes below it in the
 * returned box. */
static GtkWidget *section(GtkLabel **title, const char *text, GtkListBox **list, GtkWidget *buttons[], int n)
{
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *label = gtk_label_new(text);
  GtkWidget *chevron = gtk_image_new_from_icon_name("pan-down-symbolic");
  GtkGesture *click = gtk_gesture_click_new();

  gtk_widget_add_css_class(head, "section");
  gtk_widget_add_css_class(chevron, "chevron");
  gtk_box_append(GTK_BOX(head), chevron);
  g_signal_connect(click, "released", G_CALLBACK(on_section_click), box);
  gtk_widget_add_controller(head, GTK_EVENT_CONTROLLER(click));
  gtk_label_set_xalign(GTK_LABEL(label), 0);
  gtk_widget_set_hexpand(label, TRUE);
  gtk_box_append(GTK_BOX(head), label);
  for (int i = 0; i < n; i++)
    gtk_box_append(GTK_BOX(head), buttons[i]);
  gtk_box_append(GTK_BOX(box), head);
  *list = GTK_LIST_BOX(gtk_list_box_new());
  gtk_list_box_set_selection_mode(*list, GTK_SELECTION_NONE);
  gtk_box_append(GTK_BOX(box), GTK_WIDGET(*list));
  if (title)
    *title = GTK_LABEL(label);
  return box;
}

GtkWidget *scm_panel(App *t)
{
  GtkCssProvider *provider = gtk_css_provider_new();
  GtkWidget *panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  GtkWidget *scroll = gtk_scrolled_window_new();
  GtkWidget *overlay = gtk_overlay_new();
  GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  GtkEventController *keys = gtk_event_controller_key_new();
  GtkWidget *unstage_all_button = icon_button("−", "Unstage All");
  GtkWidget *stage_all_button = icon_button("+", "Stage All");
  GtkWidget *refresh = icon_button("⟳", "Refresh");
  GtkWidget *staged_buttons[] = { unstage_all_button };
  GtkWidget *changes_buttons[] = { stage_all_button };
  GtkWidget *commits_buttons[] = { refresh };
  GtkTextBuffer *buffer;

  gtk_css_provider_load_from_string(provider, css);
  gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(provider),
                                             GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref(provider);

  scm.app = t;
  g_queue_init(&scm.waiting);
  scm.staged = g_ptr_array_new_with_free_func((GDestroyNotify)change_free);
  scm.changes = g_ptr_array_new_with_free_func((GDestroyNotify)change_free);
  gtk_widget_add_css_class(panel, "tcode-scm");

  gtk_widget_set_margin_top(content, 8);

  /* The commit message, with a hint while it is empty. */
  scm.message = gtk_text_view_new();
  gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(scm.message), GTK_WRAP_WORD_CHAR);
  gtk_text_view_set_left_margin(GTK_TEXT_VIEW(scm.message), 6);
  gtk_text_view_set_top_margin(GTK_TEXT_VIEW(scm.message), 4);
  gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(scm.message), 4);
  gtk_widget_set_size_request(scm.message, -1, 56);
  buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(scm.message));
  g_signal_connect(buffer, "changed", G_CALLBACK(on_message_changed), NULL);
  g_signal_connect(keys, "key-pressed", G_CALLBACK(on_message_key), NULL);
  gtk_widget_add_controller(scm.message, keys);
  scm.placeholder = gtk_label_new("Message (Ctrl+Enter to commit)");
  gtk_widget_add_css_class(scm.placeholder, "placeholder");
  gtk_widget_set_halign(scm.placeholder, GTK_ALIGN_START);
  gtk_widget_set_valign(scm.placeholder, GTK_ALIGN_START);
  gtk_widget_set_can_target(scm.placeholder, FALSE);
  gtk_overlay_set_child(GTK_OVERLAY(overlay), scm.message);
  gtk_overlay_add_overlay(GTK_OVERLAY(overlay), scm.placeholder);
  gtk_widget_add_css_class(overlay, "message");
  gtk_widget_set_overflow(overlay, GTK_OVERFLOW_HIDDEN); /* the text view inside follows its rounded corners */
  gtk_box_append(GTK_BOX(content), overlay);

  scm.commit = gtk_button_new_with_label("✓ Commit");
  gtk_widget_add_css_class(scm.commit, "primary");
  gtk_widget_set_hexpand(scm.commit, TRUE);
  gtk_widget_set_tooltip_text(scm.commit, "Commit the staged changes (all changes when none are staged)");
  g_signal_connect_swapped(scm.commit, "clicked", G_CALLBACK(commit), NULL);
  scm.push = gtk_button_new_with_label("Push");
  gtk_widget_set_tooltip_text(scm.push, "Push the commits to the remote");
  g_signal_connect_swapped(scm.push, "clicked", G_CALLBACK(push), NULL);
  gtk_widget_add_css_class(actions, "actions");
  gtk_box_append(GTK_BOX(actions), scm.commit);
  gtk_box_append(GTK_BOX(actions), scm.push);
  gtk_box_append(GTK_BOX(content), actions);

  scm.note = GTK_LABEL(gtk_label_new(NULL));
  gtk_widget_add_css_class(GTK_WIDGET(scm.note), "note");
  gtk_label_set_wrap(scm.note, TRUE);
  gtk_label_set_xalign(scm.note, 0);
  gtk_label_set_selectable(scm.note, TRUE);
  gtk_widget_set_visible(GTK_WIDGET(scm.note), FALSE);
  gtk_box_append(GTK_BOX(content), GTK_WIDGET(scm.note));

  g_signal_connect_swapped(unstage_all_button, "clicked", G_CALLBACK(unstage_all), NULL);
  g_signal_connect_swapped(stage_all_button, "clicked", G_CALLBACK(stage_all), NULL);
  g_signal_connect_swapped(refresh, "clicked", G_CALLBACK(refresh_branch), t);
  scm.staged_box = section(&scm.staged_title, "STAGED CHANGES", &scm.staged_list, staged_buttons, 1);
  scm.changes_box = section(&scm.changes_title, "CHANGES", &scm.changes_list, changes_buttons, 1);
  gtk_box_append(GTK_BOX(content), scm.staged_box);
  gtk_box_append(GTK_BOX(content), scm.changes_box);
  scm.commits_box = section(NULL, "COMMITS", &scm.commits_list, commits_buttons, 1);
  gtk_box_append(GTK_BOX(content), scm.commits_box);
  gtk_list_box_set_activate_on_single_click(scm.staged_list, TRUE);
  gtk_list_box_set_activate_on_single_click(scm.changes_list, TRUE);
  gtk_list_box_set_activate_on_single_click(scm.commits_list, TRUE);
  g_signal_connect(scm.staged_list, "row-activated", G_CALLBACK(on_change_activated), NULL);
  g_signal_connect(scm.changes_list, "row-activated", G_CALLBACK(on_change_activated), NULL);
  g_signal_connect(scm.commits_list, "row-activated", G_CALLBACK(on_commit_activated), NULL);
  context_menu_attach(GTK_WIDGET(scm.commits_list), on_commits_menu, NULL);
  context_menu_attach(GTK_WIDGET(scm.staged_list), on_changes_menu, NULL);
  context_menu_attach(GTK_WIDGET(scm.changes_list), on_changes_menu, NULL);
  gtk_widget_set_visible(scm.staged_box, FALSE);

  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), content);
  gtk_widget_set_vexpand(scroll, TRUE);
  gtk_box_append(GTK_BOX(panel), scroll);
  return panel;
}

/* Another project: nothing of the last one stays while git status runs for the new one, and its
 * commits are listed afresh. */
void scm_reset(void)
{
  g_clear_pointer(&scm.head, g_free);
  g_clear_pointer(&scm.open_commit, g_free);
  if (!scm.app)
    return;
  set_note(NULL, FALSE);
  g_ptr_array_set_size(scm.staged, 0);
  g_ptr_array_set_size(scm.changes, 0);
  g_clear_pointer(&scm.upstream, g_free);
  scm.ahead = scm.behind = 0;
  scm.initial = FALSE;
  show_changes(scm.app);
  clear_list(scm.commits_list);
}
