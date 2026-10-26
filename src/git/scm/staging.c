/*
 * Staging in the Source Control panel (see state.h): a file or everything staged and unstaged,
 * one hunk staged or unstaged through the picker ("Stage Hunk…"), changes discarded after asking.
 */
#include "git/scm/scm.h"
#include "git/scm/state.h"

#include "editor/editor.h"
#include "editor/tabs.h"
#include "git/gitstatus.h"
#include "git/patch.h"
#include "popups/picker.h"

#include <string.h>

/* Staging ------------------------------------------------------------------------------------ */

/* git add takes the files from disk: what the editors hold unsaved is saved first. */
static void on_saved_for_add(App *t, gpointer data)
{
  char *path = data; /* NULL: everything */
  const char *one[] = { "add", "-A", "--", path, NULL };
  const char *all[] = { "add", "-A", NULL };

  if (!t->closing)
    scm_run_simple(path ? one : all, path ? "Could not stage the file" : "Could not stage the changes");
  g_free(path);
}

/* The + button: stages one file (after saving the editors). */
void scm_stage(const char *path)
{
  save_all(scm.app, on_saved_for_add, g_strdup(path));
}

/* The − button: takes one file out of the staged changes. */
void scm_unstage(const Change *c)
{
  /* Before the first commit there is no HEAD to restore from. A rename is staged as its old
     * path's deletion too: both come back. */
  const char *first[] = { "rm", "--cached", "-r", "-q", "--", c->path, NULL };
  const char *args[] = { "restore", "--staged", "--", c->path, c->old_path, NULL };
  scm_run_simple(scm.initial ? first : args, "Could not unstage the file");
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
      scm_set_note(*err ? err : (choice->request->staged ? "Could not unstage this hunk" : "Could not stage this hunk"),
                   TRUE);
    else
      scm_set_note(choice->request->staged ? "Hunk unstaged" : "Hunk staged", FALSE);
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

/* A hunk was picked: git apply its patch to the index (reversed to scm_unstage). */
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
  scm_run_git_owned(choice->request->staged ? unapply : apply, g_ptr_array_index(choice->patches, chosen), FALSE,
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
    scm_set_note(*err ? err : "Could not read file hunks", TRUE);
    hunk_request_free(request);
    return;
  }
  patches = git_patch_hunks(out);
  if (!patches->len) {
    scm_set_note("No text hunks to stage or unstage", FALSE);
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

  scm_run_git_owned(request->staged ? staged : unstaged, NULL, FALSE, on_hunks_read, request,
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
void scm_stage_all(void)
{
  save_all(scm.app, on_saved_for_add, NULL);
}

/* Unstage All. */
void scm_unstage_all(void)
{
  const char *first[] = { "rm", "--cached", "-r", "-q", ".", NULL };
  const char *args[] = { "reset", "-q", NULL };
  scm_run_simple(scm.initial ? first : args, "Could not unstage the changes");
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
    scm_set_note(*err ? err : "Could not discard the changes", TRUE);
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
        scm_set_note(error->message, TRUE);
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
      scm_set_note(NULL, FALSE);
      scm_run_git(args, NULL, FALSE, on_discarded, g_build_filename(scm.app->project.git_top, d->path, NULL));
    }
  }
  g_free(d->path);
  g_free(d);
}

/* The ↶ button: asks first, then discards the file's unstaged changes. */
void scm_discard(const Change *c)
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
