/*
 * Comparing a file with an older version from git, side by side in one tab:
 *   left   the working file; its gutter marks what changed since that version
 *   right  the old version, read-only; its gutter marks what changed since then
 * Ctrl+Alt+D compares with the last commit (HEAD); Ctrl+Alt+H lists the file's commits to pick
 * one. Closing the tab closes both; "Git: Stop Comparing" keeps the working file, its gutter
 * following HEAD again.
 */
#include "git/compare.h"

#include "editor/gutter/diffgutter.h"
#include "editor/editor.h"
#include "editor/tabs.h"
#include "popups/picker.h"

#include <glib/gstdio.h>
#include <stdarg.h>
#include <string.h>

/* The file the comparison commands work on: the current editor's, or for an old version, the
 * working file it is compared with. */
const char *comparable_path(App *t)
{
  Tab *tab = current_tab(t);

  if (!tab || tab->kind != TAB_EDITOR)
    return NULL;
  return tab->compare.with ? tab->compare.with : tab->path;
}

/* A message box over the window ("No git history", "Nothing to compare with"). */
static void show_message(App *t, const char *message, const char *detail)
{
  GtkAlertDialog *dialog = gtk_alert_dialog_new("%s", message);

  gtk_alert_dialog_set_detail(dialog, detail);
  gtk_alert_dialog_show(dialog, t->window);
  g_object_unref(dialog);
}

/* git in the background ---------------------------------------------------------------------- */

/* Called with each command's output in order (NULL: it failed, or was skipped). Not called once t-code
 * closes. */
typedef void (*GitDone)(App *t, char **outs, gpointer data);

typedef struct {
  App *t;
  char *dir;
  GPtrArray *commands; /* GStrv each; an empty one is skipped */
  guint next;
  char **outs;
  GitDone done;
  gpointer data;
  GDestroyNotify free_data;
} GitRun;

static void run_next(GitRun *r);

static void on_git_out(GObject *source, GAsyncResult *result, gpointer data)
{
  GitRun *r = data;
  char *out = NULL;

  if (g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result, &out, NULL, NULL) &&
      g_subprocess_get_successful(G_SUBPROCESS(source)))
    r->outs[r->next] = g_steal_pointer(&out);
  g_free(out);
  r->next++;
  run_next(r);
}

/* Starts the next command, or after the last one calls done and frees everything. */
static void run_next(GitRun *r)
{
  for (; r->next < r->commands->len; r->next++) {
    char **args = g_ptr_array_index(r->commands, r->next);
    if (!args[0])
      continue;
    GPtrArray *argv = g_ptr_array_new();
    g_ptr_array_add(argv, "git");
    g_ptr_array_add(argv, "-C");
    g_ptr_array_add(argv, r->dir);
    for (int i = 0; args[i]; i++)
      g_ptr_array_add(argv, args[i]);
    g_ptr_array_add(argv, NULL);
    GSubprocess *proc = g_subprocess_newv((const char *const *)argv->pdata,
                                          G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, NULL);
    g_ptr_array_free(argv, TRUE);
    if (proc) {
      g_subprocess_communicate_utf8_async(proc, NULL, NULL, on_git_out, r);
      g_object_unref(proc);
      return;
    }
  }
  if (!r->t->closing)
    r->done(r->t, r->outs, r->data);
  if (r->free_data)
    r->free_data(r->data);
  for (guint i = 0; i < r->commands->len; i++)
    g_free(r->outs[i]);
  g_free(r->outs);
  g_ptr_array_free(r->commands, TRUE);
  g_free(r->dir);
  g_free(r);
}

/* Runs git in dir with each NULL-ended argument list in turn (an empty one is skipped), without
 * waiting for it: a long history or a big file never stops the window. Then done(t, outputs, data).
 * The lists end with a NULL too. */
static void git_run(App *t, const char *dir, GitDone done, gpointer data, GDestroyNotify free_data, ...)
{
  GitRun *r = g_new0(GitRun, 1);
  va_list ap;

  r->t = t;
  r->dir = g_strdup(dir);
  r->commands = g_ptr_array_new_with_free_func((GDestroyNotify)g_strfreev);
  r->done = done;
  r->data = data;
  r->free_data = free_data;
  va_start(ap, free_data);
  for (const char *const *args; (args = va_arg(ap, const char *const *));)
    g_ptr_array_add(r->commands, g_strdupv((char **)args));
  va_end(ap);
  r->outs = g_new0(char *, r->commands->len + 1);
  run_next(r);
}

/* Comparing ---------------------------------------------------------------------------------- */

/* Where an old version of path goes under the temp folder's subfolder: by its place in the work
 * tree (or the project, or else by its full path), so two files with one name never share it. */
static char *temp_path(App *t, const char *folder, const char *path)
{
  GFile *top = g_file_new_for_path(t->project.git_top ? t->project.git_top : t->project.root),
        *file = g_file_new_for_path(path);
  char *relative = g_file_get_relative_path(top, file);
  char *result = g_build_filename(t->paths.temp, folder, relative ? relative : path, NULL);

  g_free(relative);
  g_object_unref(file);
  g_object_unref(top);
  return result;
}

/* Writes content to path, making its folders first. */
static void store(const char *path, const char *content)
{
  char *dir = g_path_get_dirname(path);

  g_mkdir_with_parents(dir, 0700);
  g_file_set_contents(path, content, -1, NULL);
  g_free(dir);
}

/* Writes content as the old version of path under the temp folder's subfolder; returns its path. */
static char *write_old_version(App *t, const char *path, const char *content, const char *folder)
{
  char *old_path = temp_path(t, folder, path);

  store(old_path, content);
  return old_path;
}

static void on_vscroll(GtkAdjustment *src, GtkScrollable *dst)
{
  GtkAdjustment *target = gtk_scrollable_get_vadjustment(dst);
  double val = gtk_adjustment_get_value(src);
  double diff = target ? gtk_adjustment_get_value(target) - val : 0.0;

  if (target && (diff > 0.5 || diff < -0.5))
    gtk_adjustment_set_value(target, val);
}

static void on_hscroll(GtkAdjustment *src, GtkScrollable *dst)
{
  GtkAdjustment *target = gtk_scrollable_get_hadjustment(dst);
  double val = gtk_adjustment_get_value(src);
  double diff = target ? gtk_adjustment_get_value(target) - val : 0.0;

  if (target && (diff > 0.5 || diff < -0.5))
    gtk_adjustment_set_value(target, val);
}

/* Once laid out, the right side starts where the left one is (it opens at the top). */
static void on_right_mapped(GtkWidget *right, GtkScrollable *left)
{
  GtkAdjustment *from = gtk_scrollable_get_vadjustment(left);

  g_signal_handlers_disconnect_by_func(right, on_right_mapped, left);
  if (from)
    on_vscroll(from, GTK_SCROLLABLE(right));
}

/* Keeps a comparison's two sides scrolling together vertically and horizontally. */
static void sync_scroll(Tab *left, Tab *right)
{
  GtkScrollable *a = GTK_SCROLLABLE(left->source), *b = GTK_SCROLLABLE(right->source);

  g_signal_connect_object(right->source, "map", G_CALLBACK(on_right_mapped), a, G_CONNECT_AFTER);
  g_signal_connect_object(gtk_scrollable_get_vadjustment(a), "value-changed", G_CALLBACK(on_vscroll), b, 0);
  g_signal_connect_object(gtk_scrollable_get_vadjustment(b), "value-changed", G_CALLBACK(on_vscroll), a, 0);
  g_signal_connect_object(gtk_scrollable_get_hadjustment(a), "value-changed", G_CALLBACK(on_hscroll), b, 0);
  g_signal_connect_object(gtk_scrollable_get_hadjustment(b), "value-changed", G_CALLBACK(on_hscroll), a, 0);
}

void unsync_scroll(Tab *left)
{
  Tab *sides[] = { left, left->compare.old_side };

  for (gsize i = 0; i < G_N_ELEMENTS(sides); i++) {
    if (!sides[i] || !sides[i]->source)
      continue;
    GtkAdjustment *adjustments[] = { gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(sides[i]->source)),
                                     gtk_scrollable_get_hadjustment(GTK_SCROLLABLE(sides[i]->source)) };
    for (gsize k = 0; k < G_N_ELEMENTS(adjustments); k++) {
      if (!adjustments[k])
        continue;
      g_signal_handlers_disconnect_matched(adjustments[k], G_SIGNAL_MATCH_FUNC, 0, 0, NULL, on_vscroll, NULL);
      g_signal_handlers_disconnect_matched(adjustments[k], G_SIGNAL_MATCH_FUNC, 0, 0, NULL, on_hscroll, NULL);
    }
  }
}

/* Opens path (an old version, read-only) in a tab of the active group titled "name @ label", compared
 * with working; a tab showing it already goes first (the file may have been written again). */
static Tab *open_version(App *t, const char *path, const char *label, const char *working)
{
  char *name = g_path_get_basename(working);
  Tab *tab;

  for (int i = tab_count(t) - 1; i >= 0; i--) {
    Tab *other = tab_nth(t, i);
    if (g_strcmp0(other->path, path) == 0)
      remove_tab(other); /* an old version: nothing to save */
    else if (other->compare.old_side && g_strcmp0(other->compare.old_side->path, path) == 0)
      stop_comparing_tab(other);
  }
  tab = open_editor(t, path, 0, 0, TRUE);
  tab->compare.title = g_strdup_printf("%s @ %s", name, label);
  tab->compare.with = g_strdup(working);
  update_tab_label(tab);
  g_free(name);
  return tab;
}

/* left and right in one tab, each gutter marking what differs from the other, scrolling together. */
static void show_side_by_side(Tab *left, Tab *right)
{
  join_tabs(left, right);
  diff_gutter_pair(left, right);
  sync_scroll(left, right);
  select_tab(left);
}

/* The working file next to old_path (read-only, "name @ label") in one tab: the working file's
 * gutter compared with the commit base_rev (by name; it follows new commits when that is HEAD) or
 * else with old_path, the old version's with the working file. */
static void compare_working(App *t, const char *path, const char *old_path, const char *label, const char *base_rev)
{
  Tab *work = find_tab(t, TAB_EDITOR, path), *old;

  if (!work)
    work = open_editor(t, path, 0, 0, FALSE);
  stop_comparing_tab(work); /* one comparison per file at a time */
  keep_tab(work);           /* a click in Files must not replace it while it is compared */
  t->editors.active = tab_group(work);
  old = open_version(t, old_path, label, path);
  if (base_rev && strcmp(base_rev, "HEAD") != 0)
    diff_gutter_compare_rev(work, base_rev);
  else if (base_rev)
    diff_gutter_follow_head(work);
  else
    diff_gutter_compare_file(work, old_path);
  diff_gutter_compare_file(old, path);
  show_side_by_side(work, old);
}

/* git answered compare_with(): outs = { the commit, the file in it }; data is the file's path. */
static void compared_with(App *t, char **outs, gpointer data)
{
  const char *path = data;
  char *name = g_path_get_basename(path);

  if (!outs[0]) {
    show_message(t, "No git history", "This file is not in a git repository with commits.");
  } else {
    char *commit = g_strstrip(outs[0]), *short_commit = g_strndup(commit, 7);
    if (!outs[1]) {
      char *detail = g_strdup_printf("%s is not in commit %s (it may be new).", name, short_commit);
      show_message(t, "Nothing to compare with", detail);
      g_free(detail);
    } else {
      char *old_path = write_old_version(t, path, outs[1], short_commit);
      compare_working(t, path, old_path, short_commit, commit);
      g_free(old_path);
    }
    g_free(short_commit);
  }
  g_free(name);
}

/* path next to its version in rev (a commit, "HEAD"): compare_with_head() and the File History
 * picker. */
static void compare_with(App *t, const char *path, const char *rev)
{
  char *dir = g_path_get_dirname(path);
  char *name = g_path_get_basename(path);
  char *spec = g_strconcat(rev, ":./", name, NULL);

  git_run(t, dir, compared_with, g_strdup(path), g_free,
          (const char *const[]){ "rev-parse", "--verify", "--quiet", rev, NULL },
          (const char *const[]){ "show", spec, NULL }, NULL);
  g_free(spec);
  g_free(name);
  g_free(dir);
}

void compare_with_head(App *t, const char *path)
{
  compare_with(t, path, "HEAD");
}

/* git answered compare_with_index(): outs = { the staged file }. */
static void compared_with_index(App *t, char **outs, gpointer data)
{
  const char *path = data;

  if (outs[0]) {
    char *old_path = write_old_version(t, path, outs[0], "index");
    compare_working(t, path, old_path, "Index", NULL);
    g_free(old_path);
  } else {
    show_message(t, "Nothing to compare with", "This file is not staged.");
  }
}

/* The working file next to its staged version (the index): what is not staged yet. */
void compare_with_index(App *t, const char *path)
{
  char *dir = g_path_get_dirname(path);
  char *name = g_path_get_basename(path);
  char *spec = g_strconcat(":./", name, NULL);

  git_run(t, dir, compared_with_index, g_strdup(path), g_free, (const char *const[]){ "show", spec, NULL }, NULL);
  g_free(spec);
  g_free(name);
  g_free(dir);
}

void compare_with_disk(App *t, const char *path)
{
  char *content = NULL;

  if (!g_file_get_contents(path, &content, NULL, NULL)) {
    show_message(t, "Nothing to compare with", "The file is no longer on disk.");
    return;
  }
  char *old_path = write_old_version(t, path, content, "disk");
  compare_working(t, path, old_path, "Disk", NULL);
  g_free(old_path);
  g_free(content);
}

/* Two versions from git ---------------------------------------------------------------------- */

/* One side of compare_revisions(): rel (relative to the work tree top) in rev (INDEX_REV: as staged). */
typedef struct {
  char *rev, *rel; /* NULL: the side is missing */
} Side;

typedef struct {
  char *top; /* the work tree it was asked in */
  Side old, new;
} Revisions;

static void revisions_free(gpointer data)
{
  Revisions *r = data;

  g_free(r->top);
  g_free(r->old.rev);
  g_free(r->old.rel);
  g_free(r->new.rev);
  g_free(r->new.rel);
  g_free(r);
}

/* Writes side's file from git's outs = { short commit, content } under the temp folder; returns its
 * path, and the short commit (or "Index") in *label. NULL when git has no such version. */
static char *write_revision(App *t, const Side *side, char **outs, char **label)
{
  gboolean index = side->rev && strcmp(side->rev, INDEX_REV) == 0;
  char *path;

  if (!side->rev || !outs[1] || (!index && !outs[0]))
    return NULL;
  *label = index ? g_strdup("Index") : g_strdup(g_strstrip(outs[0]));
  path = g_build_filename(t->paths.temp, index ? "index" : *label, side->rel, NULL);
  store(path, outs[1]);
  return path;
}

/* The two git commands that read one side, left empty (skipped) when it is missing; spec is kept by
 * the caller until they are copied. */
static void side_commands(const Side *side, const char *short_args[6], const char *show_args[3], char **spec)
{
  *spec = side->rev ? g_strconcat(side->rev, ":", side->rel, NULL) : NULL;
  if (!side->rev)
    return;
  if (strcmp(side->rev, INDEX_REV) != 0) {
    const char *args[] = { "rev-parse", "--short", "--verify", "--quiet", side->rev, NULL };
    memcpy(short_args, args, sizeof args);
  }
  show_args[0] = "show";
  show_args[1] = *spec;
}

static void show_revisions(App *t, char **outs, gpointer data);

/* Shows a file in two versions side by side in one tab: new_rev's on the left, old_rev's on the
 * right, each gutter marking what differs from the other. Paths are relative to the work tree top
 * (they differ after a rename); a NULL rev or path is a missing side (the file was added or
 * deleted): the other one shows alone. */
void compare_revisions(App *t, const char *old_rev, const char *old_rel, const char *new_rev, const char *new_rel)
{
  Revisions *r;
  const char *old_short[6] = { NULL }, *old_show[3] = { NULL }, *new_short[6] = { NULL }, *new_show[3] = { NULL };
  char *old_spec, *new_spec;

  if (!t->project.git_top) {
    show_message(t, "Nothing to compare", "This folder is not in a git repository.");
    return;
  }
  r = g_new0(Revisions, 1);
  r->top = g_strdup(t->project.git_top);
  if (old_rev && old_rel)
    r->old = (Side){ g_strdup(old_rev), g_strdup(old_rel) };
  if (new_rev && new_rel)
    r->new = (Side){ g_strdup(new_rev), g_strdup(new_rel) };
  side_commands(&r->old, old_short, old_show, &old_spec);
  side_commands(&r->new, new_short, new_show, &new_spec);
  git_run(t, r->top, show_revisions, r, revisions_free, old_short, old_show, new_short, new_show, NULL);
  g_free(old_spec);
  g_free(new_spec);
}

/* git answered compare_revisions(): outs = { old short commit, old file, new short commit, new file }. */
static void show_revisions(App *t, char **outs, gpointer data)
{
  Revisions *r = data;
  char *old_label = NULL, *new_label = NULL;
  char *old_path = write_revision(t, &r->old, outs, &old_label);
  char *new_path = write_revision(t, &r->new, outs + 2, &new_label);
  char *working = g_build_filename(r->top, r->new.rel ? r->new.rel : r->old.rel, NULL);
  Tab *left = NULL, *right = NULL;

  if (!old_path && !new_path) {
    show_message(t, "Nothing to compare", "git has no such version of this file.");
  } else {
    if (new_path)
      left = open_version(t, new_path, new_label, working);
    if (old_path)
      right = open_version(t, old_path, old_label, working);
    if (left)
      diff_gutter_compare_file(left, old_path); /* NULL: the other side is missing, no marks */
    if (right)
      diff_gutter_compare_file(right, new_path);
    if (left && right)
      show_side_by_side(left, right);
    else
      select_tab(left ? left : right);
  }
  g_free(working);
  g_free(new_path);
  g_free(old_path);
  g_free(new_label);
  g_free(old_label);
}

void stop_comparing_tab(Tab *tab)
{
  if (!tab->compare.old_side)
    return;
  part_tabs(tab);
  if (!tab->compare.with) { /* the working file: its gutter follows HEAD again */
    diff_gutter_pair(tab, NULL);
    diff_gutter_follow_head(tab);
  }
}

/* Ends the current tab's comparison: an old version closes, the working file stays. */
void stop_comparing(App *t)
{
  Tab *tab = current_tab(t);

  if (!tab || tab->kind != TAB_EDITOR)
    return;
  if (tab->compare.with) /* two old versions, or one: nothing to keep */
    remove_tab(tab);
  else
    stop_comparing_tab(tab);
}

void toggle_unchanged_lines(App *t)
{
  Tab *tab = current_tab(t);

  if (tab && tab->kind == TAB_EDITOR)
    diff_gutter_toggle_unchanged(tab);
}

/* File history ------------------------------------------------------------------------------- */

static char *history_path;
static GPtrArray *history; /* "hash \x1f short \x1f date \x1f author \x1f subject" */

static void history_query(App *t, const char *text, guint generation)
{
  int shown = 0;

  for (guint i = 0; i < history->len; i++) {
    char **f = g_strsplit(g_ptr_array_index(history, i), "\x1f", 5);
    if (g_strv_length(f) == 5) {
      char *haystack = g_strjoin(" ", f[4], f[1], f[3], f[2], NULL);
      if (!*text || fuzzy_score(text, haystack) >= 0) {
        char *detail = g_strdup_printf("%s · %s · %s", f[1], f[2], f[3]);
        picker_add(t, generation, f[4], detail, f[0]);
        g_free(detail);
        shown++;
      }
      g_free(haystack);
    }
    g_strfreev(f);
  }
  if (!shown)
    picker_message(t, generation, history->len ? "No matching commits" : "This file has no commits yet");
}

/* A commit was picked in File History: compare the file with it. */
static void history_choose(App *t, const char *commit)
{
  compare_with(t, history_path, commit);
}

/* git answered file_history(): outs = { the log }. */
static void show_history(App *t, char **outs, gpointer data)
{
  const char *path = data;
  char *name = g_path_get_basename(path);
  char *out = outs[0];
  char *title;

  if (!out) {
    show_message(t, "No git history", "This file is not in a git repository with commits.");
    g_free(name);
    return;
  }
  g_free(history_path);
  history_path = g_strdup(path);
  if (!history)
    history = g_ptr_array_new_with_free_func(g_free);
  g_ptr_array_set_size(history, 0);
  char **lines = g_strsplit(out, "\n", -1);
  for (int i = 0; lines[i]; i++)
    if (*lines[i])
      g_ptr_array_add(history, g_strdup(lines[i]));
  g_strfreev(lines);

  title = g_strdup_printf("History of %s: pick a commit to compare with", name);
  picker_open(t, title, history_query, history_choose);
  g_free(title);
  g_free(name);
}

void file_history(App *t, const char *path)
{
  char *dir = g_path_get_dirname(path);
  char *name = g_path_get_basename(path);

  git_run(t, dir, show_history, g_strdup(path), g_free,
          (const char *const[]){ "log", "--follow", "-n", "500", "--date=format:%Y-%m-%d %H:%M",
                                 "--format=%H%x1f%h%x1f%ad%x1f%an%x1f%s", "--", name, NULL },
          NULL);
  g_free(name);
  g_free(dir);
}

/* Temp files --------------------------------------------------------------------------------- */

static void delete_tree(const char *path)
{
  GDir *dir = g_dir_open(path, 0, NULL);
  const char *name;

  while (dir && (name = g_dir_read_name(dir))) {
    char *child = g_build_filename(path, name, NULL);
    if (g_file_test(child, G_FILE_TEST_IS_DIR) && !g_file_test(child, G_FILE_TEST_IS_SYMLINK))
      delete_tree(child);
    else
      g_remove(child);
    g_free(child);
  }
  if (dir)
    g_dir_close(dir);
  g_rmdir(path);
}

void remove_temp_dir(App *t)
{
  if (t->paths.temp)
    delete_tree(t->paths.temp);
}
