/*
 * Staging in the Source Control panel (see state.h): a file or everything staged and unstaged,
 * one hunk staged or unstaged through the picker ("Stage Hunk…"), changes discarded after asking.
 * hg and svn have no index: their staged files are a list t-code keeps (GitRepo.staged), which the
 * commit takes; a file they do not know yet is added (hg add, svn add) when staged, a conflict is
 * marked resolved (hg resolve --mark, svn resolve --accept working), as git add does.
 */
#include "git/scm/scm.h"
#include "git/scm/state.h"

#include "editor/editor.h"
#include "editor/tabs.h"
#include "git/gitstatus.h"
#include "git/patch.h"
#include "popups/picker.h"
#include "window/statusbar.h"

#include <string.h>

/* Staging ------------------------------------------------------------------------------------ */

/* hg / svn: the change at path (and a rename's old path) goes into t-code's staged list. */
static void stage_in_list(GHashTable *staged, const Change *c)
{
  g_hash_table_add(staged, g_strdup(c->path));
  if (c->old_path)
    g_hash_table_add(staged, g_strdup(c->old_path));
}

/* hg / svn: the files added (hg add, svn add) or marked resolved are staged once that worked. data:
 * their paths. */
static void on_added(gboolean ok, const char *out G_GNUC_UNUSED, const char *err, gpointer data)
{
  GPtrArray *paths = data;
  GitRepo *r = git_shown_repo(scm.app);

  if (!ok)
    scm_set_note(*err ? err : "Could not stage the files", TRUE);
  for (guint i = 0; ok && r && i < paths->len; i++)
    g_hash_table_add(r->staged, g_strdup(paths->pdata[i]));
  g_ptr_array_unref(paths);
}

/* hg / svn: runs args (add or resolve), then the paths are staged; args ends with "--". */
static void run_then_stage(GPtrArray *args, GPtrArray *paths)
{
  for (guint i = 0; i < paths->len; i++)
    g_ptr_array_add(args, paths->pdata[i]);
  g_ptr_array_add(args, NULL);
  scm_run_git((const char *const *)args->pdata, NULL, FALSE, on_added, g_ptr_array_ref(paths));
}

/* hg / svn: stages path (NULL: every change); unknown files are added first, conflicts marked
 * resolved. */
static void stage_without_index(const char *path)
{
  GitRepo *r = git_shown_repo(scm.app);
  GPtrArray *unknown = g_ptr_array_new_with_free_func(g_free), *conflicts = g_ptr_array_new_with_free_func(g_free);
  gboolean svn = scm_kind() == VCS_SVN;

  if (!r) {
    g_ptr_array_unref(unknown);
    g_ptr_array_unref(conflicts);
    return;
  }
  for (guint i = 0; i < scm.changes->len; i++) {
    const Change *c = scm.changes->pdata[i];
    if (path && strcmp(c->path, path) != 0)
      continue;
    if (c->code == 'U')
      g_ptr_array_add(unknown, g_strdup(c->path));
    else if (c->code == '!')
      g_ptr_array_add(conflicts, g_strdup(c->path));
    else
      stage_in_list(r->staged, c);
  }
  scm_set_note(NULL, FALSE);
  if (conflicts->len) { /* the file as it is now is the resolution */
    GPtrArray *args = g_ptr_array_new();
    g_ptr_array_add(args, "resolve");
    g_ptr_array_add(args, svn ? "--accept" : "--mark");
    if (svn)
      g_ptr_array_add(args, "working");
    g_ptr_array_add(args, "--");
    run_then_stage(args, conflicts);
    g_ptr_array_free(args, TRUE);
  }
  if (unknown->len) {
    GPtrArray *args = g_ptr_array_new();
    g_ptr_array_add(args, "add");
    if (svn)
      g_ptr_array_add(args, "--parents"); /* its folders too */
    g_ptr_array_add(args, "--");
    run_then_stage(args, unknown);
    g_ptr_array_free(args, TRUE);
  }
  if (!conflicts->len && !unknown->len)
    refresh_branch(scm.app);
  g_ptr_array_unref(unknown);
  g_ptr_array_unref(conflicts);
}

/* git add takes the files from disk: what the editors hold unsaved is saved first. */
static void on_saved_for_add(App *t, gpointer data)
{
  char *path = data; /* NULL: everything */
  const char *one[] = { "add", "-A", "--", path, NULL };
  const char *all[] = { "add", "-A", NULL };

  if (!t->closing && scm_kind() != VCS_GIT)
    stage_without_index(path);
  else if (!t->closing)
    scm_run_simple(path ? one : all, path ? "Could not stage the file" : "Could not stage the changes");
  g_free(path);
}

/* Subversion's changelists --------------------------------------------------------------------- */

static int by_name(gconstpointer a, gconstpointer b)
{
  return g_utf8_collate(*(const char *const *)a, *(const char *const *)b);
}

/* The changelists' names in the shown working copy, sorted (char *, owned by the table). */
static GPtrArray *changelist_names(GitRepo *r)
{
  GPtrArray *names = g_ptr_array_new();
  GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);
  GHashTableIter iter;
  gpointer name;

  if (r && r->changelists) {
    g_hash_table_iter_init(&iter, r->changelists);
    while (g_hash_table_iter_next(&iter, NULL, &name))
      if (g_hash_table_add(seen, name))
        g_ptr_array_add(names, name);
  }
  g_hash_table_unref(seen);
  g_ptr_array_sort(names, by_name);
  return names;
}

/* How many of the shown working copy's changes are in the changelist called name. */
static int changelist_count(GitRepo *r, const char *name)
{
  int n = 0;
  if (!r->changelists)
    return 0;
  for (guint i = 0; i < scm.changes->len; i++) {
    const char *in = g_hash_table_lookup(r->changelists, ((Change *)scm.changes->pdata[i])->path);
    n += g_strcmp0(in, name) == 0;
  }
  for (guint i = 0; i < scm.staged->len; i++) {
    const char *in = g_hash_table_lookup(r->changelists, ((Change *)scm.staged->pdata[i])->path);
    n += g_strcmp0(in, name) == 0;
  }
  return n;
}

static void changelists_query(App *t, const char *text, guint generation)
{
  GitRepo *r = git_shown_repo(scm.app);
  GPtrArray *names = changelist_names(r);
  char *typed = g_strstrip(g_strdup(text));
  gboolean listed = FALSE, in_any = FALSE;

  for (guint i = 0; i < scm.staged->len && r && r->changelists; i++)
    in_any |= g_hash_table_contains(r->changelists, ((Change *)scm.staged->pdata[i])->path);
  for (guint i = 0; i < names->len; i++) {
    const char *name = names->pdata[i];
    if (*typed && fuzzy_score(typed, name) < 0)
      continue;
    listed |= strcmp(name, typed) == 0;
    char *payload = g_strconcat("stage\t", name, NULL),
         *detail = g_strdup_printf("stage its %d files", changelist_count(r, name));
    picker_add(t, generation, name, detail, payload);
    g_free(detail);
    g_free(payload);
  }
  if (scm.staged->len && (*typed || names->len)) {
    const char *target = *typed ? typed : NULL;
    for (guint i = 0; !target && i < names->len; i++)
      target = names->pdata[i]; /* nothing typed: the first one */
    char *label = g_strdup_printf("Put the staged files in \"%s\"", target),
         *payload = g_strconcat("move\t", target, NULL);
    picker_add(t, generation, label, listed || !*typed ? NULL : "a new changelist", payload);
    g_free(payload);
    g_free(label);
  }
  if (in_any && !*typed)
    picker_add(t, generation, "Remove the staged files from their changelists", NULL, "remove\t");
  if (names->len == 0 && !*typed)
    picker_message(t, generation,
                   scm.staged->len ? "No changelists: type a name for the staged files' one"
                                   : "No changelists: stage files, then type a name to make one");
  g_ptr_array_free(names, TRUE);
  g_free(typed);
}

/* svn changelist answered: the status shows the changelists again. */
static void on_changelist(gboolean ok, const char *out G_GNUC_UNUSED, const char *err, gpointer data)
{
  if (ok)
    scm_set_note(data, FALSE);
  else
    scm_set_note(*err ? err : "The changelist could not be changed", TRUE);
  refresh_branch(scm.app);
}

static void changelists_choose(App *t G_GNUC_UNUSED, const char *payload)
{
  GitRepo *r = git_shown_repo(scm.app);
  const char *name = strchr(payload, '\t') + 1;

  if (!r)
    return;
  if (g_str_has_prefix(payload, "stage\t") && r->changelists) {
    for (guint i = 0; i < scm.changes->len; i++) {
      const Change *c = scm.changes->pdata[i];
      if (g_strcmp0(g_hash_table_lookup(r->changelists, c->path), name) == 0)
        stage_in_list(r->staged, c);
    }
    refresh_branch(scm.app);
    return;
  }
  GPtrArray *args = g_ptr_array_new();
  g_ptr_array_add(args, "changelist");
  if (g_str_has_prefix(payload, "remove\t"))
    g_ptr_array_add(args, "--remove");
  g_ptr_array_add(args, "--quiet");
  g_ptr_array_add(args, "--");
  if (g_str_has_prefix(payload, "move\t"))
    g_ptr_array_add(args, (gpointer)name);
  for (guint i = 0; i < scm.staged->len; i++) {
    const Change *c = scm.staged->pdata[i];
    if (c->code != 'D') /* a deleted file is in no changelist */
      g_ptr_array_add(args, c->path);
  }
  g_ptr_array_add(args, NULL);
  scm_set_note(NULL, FALSE);
  scm_run_git(
      (const char *const *)args->pdata, NULL, FALSE, on_changelist,
      (gpointer)(g_str_has_prefix(payload, "move\t") ? "Put in the changelist" : "Removed from the changelists"));
  g_ptr_array_free(args, TRUE);
}

void scm_changelists(void)
{
  if (!scm.app || !scm.app->project.scm_top)
    return;
  if (scm_kind() != VCS_SVN) {
    scm_set_note("Changelists are Subversion's: this repository has none.", TRUE);
    status_say(scm.app, "Changelists are Subversion's: this repository has none.");
    return;
  }
  picker_open(scm.app, "Stage a changelist, or type a name for the staged files' one", changelists_query,
              changelists_choose);
}

/* The + button: stages one file (after saving the editors). */
void scm_stage(const char *path)
{
  save_all(scm.app, on_saved_for_add, g_strdup(path));
}

/* The − button: takes one file out of the staged changes. */
void scm_unstage(const Change *c)
{
  GitRepo *r = git_shown_repo(scm.app);
  if (scm_kind() != VCS_GIT) { /* out of t-code's list; an added file stays added */
    if (r) {
      g_hash_table_remove(r->staged, c->path);
      if (c->old_path)
        g_hash_table_remove(r->staged, c->old_path);
      refresh_branch(scm.app);
    }
    return;
  }
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

  if (!scm.app->closing && g_strcmp0(scm.app->project.scm_top, choice->request->root) == 0) {
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
      g_strcmp0(t->project.scm_top, choice->request->root) != 0) {
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

  if (scm.app->closing || g_strcmp0(scm.app->project.scm_top, request->root) != 0) {
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

  if (!t->closing && g_strcmp0(t->project.scm_top, request->root) == 0)
    read_hunks(request);
  else
    hunk_request_free(request);
}

void scm_choose_hunk(const char *path, gboolean staged)
{
  if (scm_git_only(staged ? "Unstage Hunk" : "Stage Hunk"))
    return;
  HunkRequest *request = g_new0(HunkRequest, 1);

  request->path = g_strdup(path);
  request->root = g_strdup(scm.app->project.scm_top);
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
  GitRepo *r = git_shown_repo(scm.app);
  if (scm_kind() != VCS_GIT) {
    if (r) {
      g_hash_table_remove_all(r->staged);
      refresh_branch(scm.app);
    }
    return;
  }
  const char *first[] = { "rm", "--cached", "-r", "-q", ".", NULL };
  const char *args[] = { "reset", "-q", NULL };
  scm_run_simple(scm.initial ? first : args, "Could not unstage the changes");
}

typedef struct {
  char *path, *old_path;
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
      char *full = g_build_filename(scm.app->project.scm_top, d->path, NULL);
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
      /* hg and svn put the last commit's version back (a rename's old path with it). */
      const char *git[] = { "restore", "--", d->path, NULL };
      const char *hg[] = { "revert", "--no-backup", "--", d->path, d->old_path, NULL };
      const char *svn[] = { "revert", "--", d->path, d->old_path, NULL };
      scm_set_note(NULL, FALSE);
      scm_run_git(scm_kind() == VCS_HG    ? hg
                  : scm_kind() == VCS_SVN ? svn
                                          : git,
                  NULL, FALSE, on_discarded, g_build_filename(scm.app->project.scm_top, d->path, NULL));
    }
  }
  g_free(d->path);
  g_free(d->old_path);
  g_free(d);
}

/* The ↶ button: asks first, then discards the file's unstaged changes. */
void scm_discard(const Change *c)
{
  Discard *d = g_new0(Discard, 1);
  GtkAlertDialog *dialog = gtk_alert_dialog_new("Discard changes in %s?", c->path);
  const char *buttons[] = { "Cancel", "Discard", NULL };

  d->path = g_strdup(c->path);
  d->old_path = g_strdup(c->old_path);
  d->untracked = c->code == 'U';
  gtk_alert_dialog_set_detail(dialog, d->untracked ? "The file is not in the repository; it goes to the trash."
                                      : scm_kind() != VCS_GIT ? "Its changes are lost."
                                                              : "Its unstaged changes are lost.");
  gtk_alert_dialog_set_buttons(dialog, buttons);
  gtk_alert_dialog_set_cancel_button(dialog, 0);
  gtk_alert_dialog_set_default_button(dialog, 0);
  gtk_alert_dialog_choose(dialog, scm.app->window, NULL, on_discard_answer, d);
  g_object_unref(dialog);
}
