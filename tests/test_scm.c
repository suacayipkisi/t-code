/* Tests for the Source Control panel (src/git/scm/) and the git status that feeds it
 * (src/git/gitstatus.c, included to reach poll_branch) when projects switch: nothing of the last project may stay in
 * a folder without git, in a repository without commits, or when git answers late. Uses real
 * repositories in a temporary folder; needs a display. */
#include "git/gitstatus.c"

#include "editor/editor.h"
#include "editor/tabs.h"
#include "git/compare.h"
#include "git/scm/state.h"
#include "popups/picker.h"
#include "util.h"
#include "widgets.h"

#include <glib/gstdio.h>

static int head_moves; /* diff_gutter_head_moved() calls */

/* What git/scm/ and gitstatus.c call elsewhere in t-code. */
/* clang-format off */
gboolean any_unsaved(App *t G_GNUC_UNUSED) { return FALSE; }
void save_all(App *t, SavedFunc then, gpointer data) { then(t, data); }
Tab *find_tab(App *t G_GNUC_UNUSED, TabKind kind G_GNUC_UNUSED, const char *path G_GNUC_UNUSED) { return NULL; }
void open_file(App *t G_GNUC_UNUSED, const char *path G_GNUC_UNUSED) {}
void editor_reload(Tab *tab G_GNUC_UNUSED) {}
void diff_gutter_head_moved(App *t G_GNUC_UNUSED) { head_moves++; }
void files_git_changed(App *t G_GNUC_UNUSED) {}
void quit_editor(Tab *tab G_GNUC_UNUSED) {}
void compare_with_head(App *t G_GNUC_UNUSED, const char *path G_GNUC_UNUSED) {}
void compare_with_index(App *t G_GNUC_UNUSED, const char *path G_GNUC_UNUSED) {}
void compare_revisions(App *t G_GNUC_UNUSED, const char *old_rev G_GNUC_UNUSED, const char *old_rel G_GNUC_UNUSED,
                       const char *new_rev G_GNUC_UNUSED, const char *new_rel G_GNUC_UNUSED) {}
void status_show_branch(App *t G_GNUC_UNUSED, const char *branch G_GNUC_UNUSED, gboolean dirty G_GNUC_UNUSED) {}
void show_sidebar_page(App *t G_GNUC_UNUSED, const char *page G_GNUC_UNUSED) {}
void status_say(App *t G_GNUC_UNUSED, const char *message G_GNUC_UNUSED) {}
void files_reveal(App *t G_GNUC_UNUSED, const char *path G_GNUC_UNUSED) {}
static char *previewed; /* what open_preview() was asked for last */
void open_preview(App *t G_GNUC_UNUSED, const char *path) { g_set_str(&previewed, path); }
/* clang-format on */

static App app;

/* The picker, as the hunk choice uses it: the rows of the last query and the callbacks. */
static PickerQuery picker_query;
static PickerChoose picker_choose;
static PickerCancel picker_cancel;
static GPtrArray *picked_rows; /* "label\tdetail\tpayload" */

static void picker_run(App *t, const char *text)
{
  g_ptr_array_set_size(picked_rows, 0);
  picker_query(t, text, 1);
}

void picker_open(App *t, const char *placeholder G_GNUC_UNUSED, PickerQuery query, PickerChoose choose)
{
  if (picker_cancel)
    picker_cancel(t);
  picker_query = query;
  picker_choose = choose;
  picker_cancel = NULL;
  picker_run(t, "");
}

void picker_set_preview(App *t G_GNUC_UNUSED, PickerChoose preview G_GNUC_UNUSED, PickerCancel cancel)
{
  picker_cancel = cancel;
}

void picker_requery(App *t, PickerQuery query)
{
  if (query == picker_query)
    picker_run(t, "");
}

void picker_add(App *t G_GNUC_UNUSED, guint generation G_GNUC_UNUSED, const char *label, const char *detail,
                const char *payload)
{
  g_ptr_array_add(picked_rows, g_strdup_printf("%s\t%s\t%s", label, detail ? detail : "", payload));
}

void picker_message(App *t G_GNUC_UNUSED, guint generation G_GNUC_UNUSED, const char *text G_GNUC_UNUSED)
{
}

int fuzzy_score(const char *query, const char *text)
{
  return strstr(text, query) ? 1 : -1;
}

/* Picks a row: the picker closes, so nothing is left to cancel. */
static void picker_pick(const char *payload)
{
  picker_cancel = NULL;
  picker_choose(&app, payload);
}

static char *tmp, *with_commits, *no_git, *no_commits;
static char *data_home; /* XDG_DATA_HOME for the tests */
static int commits = 2; /* in with_commits */

static void git(const char *dir, const char *const *args)
{
  GPtrArray *argv = g_ptr_array_new();
  int status;

  g_ptr_array_add(argv, "git");
  g_ptr_array_add(argv, "-C");
  g_ptr_array_add(argv, (gpointer)dir);
  for (int i = 0; args[i]; i++)
    g_ptr_array_add(argv, (gpointer)args[i]);
  g_ptr_array_add(argv, NULL);
  g_assert_true(g_spawn_sync(NULL, (char **)argv->pdata, NULL,
                             G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL,
                             NULL, NULL, &status, NULL));
  g_assert_true(g_spawn_check_wait_status(status, NULL));
  g_ptr_array_free(argv, TRUE);
}

static char *make_dir(const char *name, const char *file)
{
  char *dir = g_build_filename(tmp, name, NULL), *path = g_build_filename(dir, file, NULL);

  g_mkdir_with_parents(dir, 0755);
  g_assert_true(g_file_set_contents(path, "one\n", -1, NULL));
  g_free(path);
  return dir;
}

/* The panel's lists, in order: Staged Changes, Changes, Commits. */
static GtkListBox *list(int index)
{
  GtkWidget *found[3] = { NULL };
  int n = 0;
  GPtrArray *queue = g_ptr_array_new();

  g_ptr_array_add(queue, app.scm);
  for (guint i = 0; i < queue->len && n < 3; i++) {
    GtkWidget *w = queue->pdata[i];
    if (GTK_IS_LIST_BOX(w) && strcmp(gtk_widget_get_name(w), "repositories") != 0)
      found[n++] = w;
    else
      for (GtkWidget *c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c))
        g_ptr_array_add(queue, c);
  }
  g_ptr_array_free(queue, TRUE);
  g_assert_cmpint(n, ==, 3);
  return GTK_LIST_BOX(found[index]);
}

static int rows(int index)
{
  int n = 0;

  for (GtkWidget *w = gtk_widget_get_first_child(GTK_WIDGET(list(index))); w; w = gtk_widget_get_next_sibling(w))
    n++;
  return n;
}

static const char *tab_title(void)
{
  return gtk_stack_page_get_title(gtk_stack_get_page(app.dock.scm_stack, app.scm));
}

/* Opens a project the way switch_project (project/project.c) does, then waits for git. */
static void open_project_at(const char *root)
{
  g_set_str(&app.project.root, root);
  scm_reset();
  watch_git(&app); /* repo_top, then git status in 300ms */
}

static void wait_for(int index, int count)
{
  for (gint64 until = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
       rows(index) != count && g_get_monotonic_time() < until;)
    g_main_context_iteration(NULL, FALSE);
  g_assert_cmpint(rows(index), ==, count);
}

static void assert_empty(void)
{
  g_assert_cmpint(rows(0), ==, 0);
  g_assert_cmpint(rows(1), ==, 0);
  g_assert_cmpint(rows(2), ==, 0);
  g_assert_cmpstr(tab_title(), ==, "SOURCE CONTROL");
}

static void open_with_commits(void)
{
  open_project_at(with_commits);
  wait_for(2, commits);
  wait_for(1, 2); /* a.txt changed, b.txt new */
  g_assert_cmpstr(tab_title(), ==, "SOURCE CONTROL  2");
}

/* The button labelled label in a widget tree, or NULL. */
static GtkWidget *button_labelled(GtkWidget *root, const char *label)
{
  if (GTK_IS_BUTTON(root) && g_strcmp0(gtk_button_get_label(GTK_BUTTON(root)), label) == 0)
    return root;
  for (GtkWidget *child = gtk_widget_get_first_child(root); child; child = gtk_widget_get_next_sibling(child)) {
    GtkWidget *found = button_labelled(child, label);
    if (found)
      return found;
  }
  return NULL;
}

/* Presses the button labelled label in an open dialog (a GtkAlertDialog's window). */
static void press_dialog_button(const char *label)
{
  GtkWidget *button = NULL;

  for (gint64 until = g_get_monotonic_time() + 5 * G_USEC_PER_SEC; !button && g_get_monotonic_time() < until;) {
    GListModel *windows = gtk_window_get_toplevels();
    for (guint i = 0; !button && i < g_list_model_get_n_items(windows); i++) {
      GtkWidget *window = g_list_model_get_item(windows, i);
      button = button_labelled(window, label);
      g_object_unref(window);
    }
    g_main_context_iteration(NULL, FALSE);
  }
  g_assert_nonnull(button);
  g_signal_emit_by_name(button, "clicked");
}

/* "Discard changes?" answered after another project opened: nothing is discarded, in either repository
 * (not the new one's file of the same name either). */
static void test_discard_after_switch(void)
{
  char *other = make_dir("discard-other", "a.txt"), *theirs = g_build_filename(other, "a.txt", NULL);
  char *mine = g_build_filename(with_commits, "a.txt", NULL), *text = NULL;
  Change change = { "a.txt", NULL, 'M' };

  git(other, (const char *[]){ "init", "-q", NULL });
  git(other, (const char *[]){ "add", "a.txt", NULL });
  git(other, (const char *[]){ "-c", "user.name=t", "-c", "user.email=t@t", "commit", "-q", "-m", "one", NULL });
  g_assert_true(g_file_set_contents(theirs, "theirs\n", -1, NULL));
  open_with_commits();
  scm_discard(&change);
  open_project_at(other);
  for (gint64 until = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
       g_strcmp0(app.project.scm_top, other) != 0 && g_get_monotonic_time() < until;)
    g_main_context_iteration(NULL, FALSE);
  g_assert_cmpstr(app.project.scm_top, ==, other);
  press_dialog_button("Discard");
  run_for(500);
  g_assert_true(g_file_get_contents(theirs, &text, NULL, NULL));
  g_assert_cmpstr(text, ==, "theirs\n");
  g_free(text);
  g_assert_true(g_file_get_contents(mine, &text, NULL, NULL));
  g_assert_cmpstr(text, ==, "two\n");
  g_free(text);
  g_free(mine);
  g_free(theirs);
  g_free(other);
}

static void test_repository(void)
{
  open_with_commits();
}

static void test_no_git(void)
{
  open_with_commits();
  open_project_at(no_git);
  assert_empty(); /* at once, before git answers */
  g_assert_null(app.project.repo_top);
  run_for(800);
  assert_empty();
  /* git status runs again every 10 seconds: still nothing. */
  poll_branch(&app);
  run_for(300);
  assert_empty();
}

static void test_no_commits(void)
{
  open_with_commits();
  open_project_at(no_commits);
  g_assert_cmpint(rows(2), ==, 0);
  wait_for(1, 1); /* c.txt, untracked */
  run_for(500);
  g_assert_cmpint(rows(2), ==, 0);
  g_assert_cmpstr(tab_title(), ==, "SOURCE CONTROL  1");
}

/* git status runs again (after a save, every 10 seconds): when it says the same, the change rows stay
 * as they are; a new change builds them again. */
static void test_same_status(void)
{
  open_with_commits();
  GtkWidget *first = gtk_widget_get_first_child(GTK_WIDGET(list(1)));
  poll_branch(&app);
  for (gint64 until = g_get_monotonic_time() + 5 * G_USEC_PER_SEC; branch_busy && g_get_monotonic_time() < until;)
    g_main_context_iteration(NULL, FALSE);
  g_assert_false(branch_busy);
  g_assert_true(gtk_widget_get_first_child(GTK_WIDGET(list(1))) == first);
  g_assert_cmpstr(tab_title(), ==, "SOURCE CONTROL  2");

  char *path = g_build_filename(with_commits, "c.txt", NULL);
  g_assert_true(g_file_set_contents(path, "c\n", -1, NULL));
  poll_branch(&app);
  wait_for(1, 3);
  g_assert_cmpstr(tab_title(), ==, "SOURCE CONTROL  3");
  g_assert_true(g_unlink(path) == 0);
  g_free(path);
  poll_branch(&app);
  wait_for(1, 2);
}

/* A git log asked for in the last project answers after the switch: it is not shown. */
static void test_late_log(void)
{
  open_with_commits();
  /* A new commit: the next status lists the commits again. */
  git(with_commits, (const char *[]){ "-c", "user.name=t", "-c", "user.email=t@t", "commit", "-q", "--allow-empty",
                                      "-m", "third", NULL });
  commits++;
  poll_branch(&app);
  for (gint64 until = g_get_monotonic_time() + 5 * G_USEC_PER_SEC; branch_busy && g_get_monotonic_time() < until;)
    g_main_context_iteration(NULL, FALSE); /* status answered: git log runs now */
  g_assert_cmpint(rows(2), ==, commits - 1);
  open_project_at(no_git);
  run_for(800);
  assert_empty();
}

/* A git status started in the last project answers after the switch: it is not shown. */
static void test_late_status(void)
{
  open_with_commits();
  poll_branch(&app); /* runs now */
  g_assert_true(branch_busy);
  open_project_at(no_git);
  for (gint64 until = g_get_monotonic_time() + 5 * G_USEC_PER_SEC; branch_busy && g_get_monotonic_time() < until;)
    g_main_context_iteration(NULL, FALSE);
  assert_empty(); /* the old status came, before the new one */
  run_for(800);
  assert_empty();
  g_assert_false(branch_busy);
}

/* Runs git status until diff_gutter_head_moved() was called more than before times (or 5 s). */
static gboolean head_moved_after(int before)
{
  for (gint64 until = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
       head_moves == before && g_get_monotonic_time() < until;) {
    if (!branch_busy)
      poll_branch(&app);
    run_for(50);
  }
  return head_moves > before;
}

/* A commit on a branch moves the gutters' base; staging a file does not. */
static void test_head_moves(void)
{
  int before;

  open_with_commits();
  run_for(400);
  before = head_moves;
  git(with_commits, (const char *[]){ "add", "b.txt", NULL });
  g_assert_false(head_moved_after(before));
  git(with_commits,
      (const char *[]){ "-c", "user.name=t", "-c", "user.email=t@t", "commit", "-q", "-m", "third", NULL });
  commits++;
  g_assert_true(head_moved_after(before));
}

/* A folded Commits section runs no git log; it lists the commits once it opens. */
static void test_commits_folded(void)
{
  open_project_at(with_commits);
  wait_for(2, commits);
  scm_show_commits(FALSE);
  g_assert_false(scm_commits_shown());
  git(with_commits, (const char *[]){ "-c", "user.name=t", "-c", "user.email=t@t", "commit", "-q", "--allow-empty",
                                      "-m", "folded", NULL });
  commits++;
  poll_branch(&app);
  run_for(800);
  g_assert_cmpint(rows(2), ==, commits - 1); /* the old list, not read again */
  scm_show_commits(TRUE);
  wait_for(2, commits);
}

/* The commit message's text view (the panel's only one). */
static GtkWidget *message_view(GtkWidget *w)
{
  if (GTK_IS_TEXT_VIEW(w))
    return w;
  for (GtkWidget *c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c)) {
    GtkWidget *found = message_view(c);
    if (found)
      return found;
  }
  return NULL;
}

/* Presses key in the message (its key controller's handler), as the window would. */
static gboolean press(guint key)
{
  GListModel *controllers = gtk_widget_observe_controllers(message_view(app.scm));
  gboolean handled = FALSE;

  for (guint i = 0; i < g_list_model_get_n_items(controllers); i++) {
    GObject *controller = g_list_model_get_item(controllers, i);
    if (GTK_IS_EVENT_CONTROLLER_KEY(controller) && !handled)
      g_signal_emit_by_name(controller, "key-pressed", key, 0, 0, &handled);
    g_object_unref(controller);
  }
  g_object_unref(controllers);
  return handled;
}

static void assert_message(const char *expected)
{
  char *message = NULL;

  for (gint64 until = g_get_monotonic_time() + 5 * G_USEC_PER_SEC; g_get_monotonic_time() < until;) {
    g_free(message);
    message = scm_message();
    if (strcmp(message, expected) == 0)
      break;
    g_main_context_iteration(NULL, FALSE);
  }
  g_assert_cmpstr(message, ==, expected);
  g_free(message);
}

/* Up on the message's first line shows the earlier commits' messages, newest first; Down on its last
 * line goes back, to what was typed in the end. Typing makes the shown one what was typed. */
static void test_message_history(void)
{
  char *dir = make_dir("history", "x.txt"), *file = g_build_filename(dir, "x.txt", NULL);
  GtkTextBuffer *buffer;
  const char *const messages[] = { "first", "second", "third\n\nwith a body" };

  git(dir, (const char *[]){ "init", "-q", NULL });
  for (int i = 0; i < 3; i++) {
    char *text = g_strdup_printf("%d\n", i);
    g_assert_true(g_file_set_contents(file, text, -1, NULL));
    git(dir, (const char *[]){ "add", "x.txt", NULL });
    git(dir, (const char *[]){ "-c", "user.name=t", "-c", "user.email=t@example.com", "commit", "-q", "-m", messages[i],
                               NULL });
    g_free(text);
  }
  open_project_at(dir);
  wait_for(2, 3);
  buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(message_view(app.scm)));

  scm_set_message("typed");
  g_assert_false(press(GDK_KEY_Down)); /* nothing recalled: Down is the text's own */
  g_assert_true(press(GDK_KEY_Up));
  assert_message("third\n\nwith a body");
  g_assert_true(press(GDK_KEY_Up)); /* the cursor went to the start: Up goes on */
  assert_message("second");
  g_assert_true(press(GDK_KEY_Up));
  assert_message("first");
  g_assert_true(press(GDK_KEY_Up)); /* the oldest stays */
  assert_message("first");
  g_assert_true(press(GDK_KEY_Down));
  assert_message("second");
  g_assert_true(press(GDK_KEY_Down));
  assert_message("third\n\nwith a body");
  g_assert_true(press(GDK_KEY_Down));
  assert_message("typed");

  g_assert_true(press(GDK_KEY_Up));
  assert_message("third\n\nwith a body");
  gtk_text_buffer_insert_at_cursor(buffer, "x", -1); /* edited: now what was typed */
  g_assert_false(press(GDK_KEY_Down));
  assert_message("xthird\n\nwith a body");
  scm_set_message("");
  g_free(file);
  g_free(dir);
}

/* "Open Commit Details" (a commit's right-click menu) writes its whole message, author, line counts,
 * files and diff into a Markdown file and previews it. */
static void test_commit_details(void)
{
  char *dir = make_dir("detailed", "x.txt"), *file = g_build_filename(dir, "x.txt", NULL), *text = NULL;
  const char *const author[] = { "-c", "user.name=Ada Lovelace", "-c", "user.email=ada@example.com", "commit", "-q" };

  git(dir, (const char *[]){ "init", "-q", NULL });
  git(dir, (const char *[]){ "add", "x.txt", NULL });
  git(dir, (const char *[]){ author[0], author[1], author[2], author[3], author[4], author[5], "-m", "start", NULL });
  g_assert_true(g_file_set_contents(file, "one\nchanged\n", -1, NULL));
  git(dir, (const char *[]){ "add", "x.txt", NULL });
  git(dir, (const char *[]){ author[0], author[1], author[2], author[3], author[4], author[5], "-m",
                             "the subject\n\nbody line one\nbody line two", NULL });
  open_project_at(dir);
  wait_for(2, 2);

  g_clear_pointer(&previewed, g_free);
  scm_open_commit_details(0); /* the newest */
  for (gint64 end = g_get_monotonic_time() + 5 * G_USEC_PER_SEC; !previewed && g_get_monotonic_time() < end;)
    g_main_context_iteration(NULL, FALSE);
  g_assert_nonnull(previewed);
  g_assert_true(g_file_get_contents(previewed, &text, NULL, NULL));
  g_assert_nonnull(strstr(text, "# the subject\n"));
  g_assert_nonnull(strstr(text, "**Ada Lovelace** &lt;ada@example.com&gt;"));
  g_assert_nonnull(strstr(text, "body line one\nbody line two"));
  g_assert_nonnull(strstr(text, "1 file changed, 1 insertion(+)"));
  g_assert_nonnull(strstr(text, "| `x.txt` | 1 | 0 |"));
  g_assert_nonnull(strstr(text, "````diff\n"));
  g_assert_nonnull(strstr(text, "\n+changed\n"));
  g_free(text);
  g_free(file);
  g_free(dir);
}

/* What git prints in dir for args. */
static char *git_out(const char *dir, const char *const *args)
{
  GPtrArray *argv = g_ptr_array_new();
  char *out = NULL;

  g_ptr_array_add(argv, "git");
  g_ptr_array_add(argv, "-C");
  g_ptr_array_add(argv, (gpointer)dir);
  for (int i = 0; args[i]; i++)
    g_ptr_array_add(argv, (gpointer)args[i]);
  g_ptr_array_add(argv, NULL);
  g_assert_true(g_spawn_sync(NULL, (char **)argv->pdata, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL, NULL,
                             NULL, &out, NULL, NULL, NULL));
  g_ptr_array_free(argv, TRUE);
  return out;
}

static void wait_rows(guint count)
{
  for (gint64 end = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
       picked_rows->len != count && g_get_monotonic_time() < end;)
    g_main_context_iteration(NULL, FALSE);
  g_assert_cmpuint(picked_rows->len, ==, count);
}

/* Runs the main loop until git diff --cached in dir is (or 5 s pass). */
static char *wait_cached(const char *dir, gboolean empty)
{
  const char *const args[] = { "diff", "--cached", "--no-color", "-U0", NULL };
  char *out = git_out(dir, args);

  for (gint64 end = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
       (*out == '\0') != empty && g_get_monotonic_time() < end;) {
    run_for(50);
    g_free(out);
    out = git_out(dir, args);
  }
  return out;
}

/* "Stage Hunk…" lists the file's hunks in the picker by their first changed line and "@@" line;
 * the picked one goes into the index, a search narrows the list, and "Unstage Hunk…" takes it back. */
static void test_stage_hunk(void)
{
  char *dir = make_dir("hunks", "h.txt"), *file = g_build_filename(dir, "h.txt", NULL), *cached;
  const char *const commit[] = { "-c", "user.name=t", "-c", "user.email=t@t", "commit", "-q", "-m", "start", NULL };
  GString *text = g_string_new(NULL);

  for (int i = 1; i <= 20; i++)
    g_string_append_printf(text, "line %d\n", i);
  g_assert_true(g_file_set_contents(file, text->str, -1, NULL));
  git(dir, (const char *[]){ "init", "-q", NULL });
  git(dir, (const char *[]){ "add", "h.txt", NULL });
  git(dir, commit);
  g_string_replace(text, "line 2\n", "line two\n", 1);
  g_string_replace(text, "line 18\n", "line eighteen\n", 1);
  g_assert_true(g_file_set_contents(file, text->str, -1, NULL));
  open_project_at(dir);
  wait_for(1, 1);

  picked_rows = g_ptr_array_new_with_free_func(g_free);
  scm_choose_hunk(file, FALSE);
  wait_rows(2);
  g_assert_cmpstr(picked_rows->pdata[0], ==, "- line 2\t@@ -1,5 +1,5 @@\t0");
  g_assert_cmpstr(picked_rows->pdata[1], ==, "- line 18\t@@ -15,6 +15,6 @@ line 14\t1");
  picker_run(&app, "@@ -15");
  g_assert_cmpuint(picked_rows->len, ==, 1);
  picker_pick("1");
  cached = wait_cached(dir, FALSE);
  g_assert_nonnull(strstr(cached, "+line eighteen\n"));
  g_assert_null(strstr(cached, "line two"));
  g_free(cached);

  /* Closed without a pick, then chosen again: still one hunk staged. */
  g_ptr_array_set_size(picked_rows, 0); /* the rows of the last search */
  scm_choose_hunk(file, TRUE);
  wait_rows(1);
  g_assert_cmpstr(picked_rows->pdata[0], ==, "- line 18\t@@ -15,6 +15,6 @@ line 14\t0");
  picker_cancel(&app);
  picker_cancel = NULL;
  g_ptr_array_set_size(picked_rows, 0);
  scm_choose_hunk(file, TRUE);
  wait_rows(1);
  picker_pick("0");
  cached = wait_cached(dir, TRUE);
  g_assert_cmpstr(cached, ==, "");
  g_free(cached);

  g_ptr_array_free(picked_rows, TRUE);
  g_string_free(text, TRUE);
  g_free(file);
  g_free(dir);
}

/* git rev-parse of what in dir (stripped), waiting until it is want (or 5 s pass). */
static char *wait_rev(const char *dir, const char *what, const char *want)
{
  const char *const args[] = { "rev-parse", "--abbrev-ref", what, NULL };
  const char *const full[] = { "rev-parse", what, NULL };
  gboolean name = strcmp(what, "HEAD") == 0;
  char *out = g_strstrip(git_out(dir, name ? args : full));

  for (gint64 end = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
       strcmp(out, want) != 0 && g_get_monotonic_time() < end;) {
    run_for(50);
    g_free(out);
    out = g_strstrip(git_out(dir, name ? args : full));
  }
  return out;
}

/* Branches: the picker lists the local ones, then the remote ones without a local one of their name
 * (and not origin/HEAD); a remote one is checked out tracking it, a typed name is created; fetch and
 * pull bring the remote's new commit. */
static void test_branches(void)
{
  char *origin = make_dir("origin", "o.txt"), *clone = g_build_filename(tmp, "clone", NULL), *rev, *head;
  const char *const commit[] = { "-c", "user.name=t", "-c", "user.email=t@t", "commit", "-q", "-m", "more", NULL };

  git(origin, (const char *[]){ "init", "-q", "-b", "main", NULL });
  git(origin, (const char *[]){ "add", "o.txt", NULL });
  git(origin, commit);
  git(origin, (const char *[]){ "branch", "feature", NULL });
  git(tmp, (const char *[]){ "clone", "-q", origin, clone, NULL });
  open_project_at(clone);
  wait_for(2, 1);

  picked_rows = g_ptr_array_new_with_free_func(g_free);
  scm_switch_branch();
  wait_rows(3);
  g_assert_cmpstr(picked_rows->pdata[0], ==, "+ Create new branch…\t\tnew\t");
  g_assert_true(g_str_has_prefix(picked_rows->pdata[1], "main\tchecked out · "));
  g_assert_true(g_str_has_prefix(picked_rows->pdata[2], "origin/feature\tremote · "));
  g_assert_true(g_str_has_suffix(picked_rows->pdata[2], "\ttrack\torigin/feature"));
  picker_pick("track\torigin/feature");
  head = wait_rev(clone, "HEAD", "feature");
  g_assert_cmpstr(head, ==, "feature");
  g_free(head);

  picker_run(&app, "topic");
  g_assert_cmpstr(picked_rows->pdata[0], ==, "+ Create branch \"topic\"\tfrom what is checked out\tcreate\ttopic");
  picker_pick("create\ttopic");
  head = wait_rev(clone, "HEAD", "topic");
  g_assert_cmpstr(head, ==, "topic");
  g_free(head);
  scm_switch_branch(); /* back to main */
  wait_rows(4);        /* + Create, topic, feature, main (each by its last commit) */
  picker_pick("switch\tmain");
  head = wait_rev(clone, "HEAD", "main");
  g_assert_cmpstr(head, ==, "main");
  g_free(head);

  /* The remote moves on: fetch brings its branch, pull the commit. */
  char *file = g_build_filename(origin, "o.txt", NULL);
  g_file_set_contents(file, "two\n", -1, NULL);
  git(origin, (const char *[]){ "-c", "user.name=t", "-c", "user.email=t@t", "commit", "-q", "-am", "newer", NULL });
  char *newest = g_strstrip(git_out(origin, (const char *const[]){ "rev-parse", "HEAD", NULL }));
  scm_fetch();
  rev = wait_rev(clone, "origin/main", newest);
  g_assert_cmpstr(rev, ==, newest);
  g_free(rev);
  scm_pull();
  rev = wait_rev(clone, "main", newest);
  g_assert_cmpstr(rev, ==, newest);
  g_free(rev);

  g_ptr_array_free(picked_rows, TRUE);
  picked_rows = NULL;
  g_free(newest);
  g_free(file);
  g_free(clone);
  g_free(origin);
}

/* Runs the main loop until path holds text (or 5 s pass); TRUE when it does (NULL: until it is gone). */
static gboolean wait_file(const char *path, const char *text)
{
  for (gint64 end = g_get_monotonic_time() + 5 * G_USEC_PER_SEC; g_get_monotonic_time() < end; run_for(50)) {
    char *now = NULL;
    gboolean there = g_file_get_contents(path, &now, NULL, NULL), same = there && g_strcmp0(now, text) == 0;
    g_free(now);
    if (text ? same : !there)
      return TRUE;
  }
  return FALSE;
}

static void test_stash(void)
{
  char *dir = make_dir("stash", "s.txt"), *file = g_build_filename(dir, "s.txt", NULL);
  char *untracked = g_build_filename(dir, "new.txt", NULL);
  const char *const list[] = { "stash", "list", "--format=%gs", NULL };

  git(dir, (const char *[]){ "init", "-q", "-b", "main", NULL });
  git(dir, (const char *[]){ "add", "s.txt", NULL });
  git(dir, (const char *[]){ "-c", "user.name=t", "-c", "user.email=t@t", "commit", "-q", "-m", "first", NULL });
  open_project_at(dir);
  wait_for(2, 1);
  picked_rows = g_ptr_array_new_with_free_func(g_free);

  /* Stash with a message: the change goes; pop: it comes back and the stash goes. */
  g_file_set_contents(file, "two\n", -1, NULL);
  scm_stash();
  picker_run(&app, "half done");
  g_assert_cmpuint(picked_rows->len, ==, 2);
  g_assert_cmpstr(picked_rows->pdata[0], ==, "Stash\thalf done\tpush\thalf done");
  picker_pick("push\thalf done");
  g_assert_true(wait_file(file, "one\n"));
  scm_pick_stash(STASH_POP);
  wait_rows(1);
  g_assert_true(g_str_has_prefix(picked_rows->pdata[0], "On main: half done\tstash@{0} · "));
  g_assert_true(g_str_has_suffix(picked_rows->pdata[0], " ago\tstash@{0}"));
  picker_pick("stash@{0}");
  g_assert_true(wait_file(file, "two\n"));
  char *left = git_out(dir, list);
  for (gint64 end = g_get_monotonic_time() + 5 * G_USEC_PER_SEC; *left && g_get_monotonic_time() < end;) {
    run_for(50);
    g_free(left);
    left = git_out(dir, list);
  }
  g_assert_cmpstr(left, ==, "");
  g_free(left);

  /* Untracked files too, no message; apply keeps the stash. */
  g_file_set_contents(untracked, "new\n", -1, NULL);
  scm_stash();
  picker_pick("push-u\t");
  g_assert_true(wait_file(untracked, NULL));
  g_assert_true(wait_file(file, "one\n"));
  scm_pick_stash(STASH_APPLY);
  wait_rows(1);
  g_assert_true(g_str_has_prefix(picked_rows->pdata[0], "WIP on main: "));
  picker_pick("stash@{0}");
  g_assert_true(wait_file(untracked, "new\n"));
  g_assert_true(wait_file(file, "two\n"));
  left = git_out(dir, list);
  g_assert_true(g_str_has_prefix(left, "WIP on main: "));
  g_free(left);

  g_ptr_array_free(picked_rows, TRUE);
  picked_rows = NULL;
  g_free(untracked);
  g_free(file);
  g_free(dir);
}

/* The rows of Repositories: each one's name, and the current one's. */
static int repo_rows(char **current)
{
  GtkWidget *list = NULL;
  int n = 0;
  GPtrArray *queue = g_ptr_array_new();

  g_ptr_array_add(queue, app.scm);
  for (guint i = 0; i < queue->len && !list; i++) {
    GtkWidget *w = queue->pdata[i];
    if (GTK_IS_LIST_BOX(w) && strcmp(gtk_widget_get_name(w), "repositories") == 0)
      list = w;
    for (GtkWidget *c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c))
      g_ptr_array_add(queue, c);
  }
  g_ptr_array_free(queue, TRUE);
  g_assert_nonnull(list);
  if (!gtk_widget_get_visible(gtk_widget_get_parent(list)))
    return 0;
  for (GtkWidget *row = gtk_widget_get_first_child(list); row; row = gtk_widget_get_next_sibling(row), n++)
    if (current && gtk_widget_has_css_class(row, "current"))
      *current = g_object_get_data(G_OBJECT(row), "top");
  return n;
}

static void wait_repo_rows(int count)
{
  for (gint64 until = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
       repo_rows(NULL) != count && g_get_monotonic_time() < until;)
    g_main_context_iteration(NULL, FALSE);
  g_assert_cmpint(repo_rows(NULL), ==, count);
}

static void commit_all(const char *dir)
{
  git(dir, (const char *[]){ "add", "-A", NULL });
  git(dir, (const char *[]){ "-c", "user.name=t", "-c", "user.email=t@t", "commit", "-q", "-m", "first", NULL });
}

/* Clones nested in the project's repository show in Repositories (also an ignored one; a submodule
 * does not), apart from
 * the project's own changes; a click shows one in the panel, and staging works in it. */
static void test_nested_repos(void)
{
  char *top = make_dir("nested", "top.txt"), *lib = make_dir("nested-lib", "lib.txt");
  char *inner = g_build_filename(top, "inner", NULL), *inner_file = g_build_filename(inner, "in.txt", NULL);
  char *current = NULL, *staged;

  char *kept = g_build_filename(top, "kept", NULL), *ignore = g_build_filename(top, ".gitignore", NULL);

  git(lib, (const char *[]){ "init", "-q", NULL });
  commit_all(lib);
  g_file_set_contents(ignore, "kept/\n", -1, NULL);
  git(top, (const char *[]){ "init", "-q", NULL });
  commit_all(top);
  g_mkdir_with_parents(kept, 0755);
  git(kept, (const char *[]){ "init", "-q", NULL }); /* a clone the project's .gitignore leaves out */
  git(top, (const char *[]){ "-c", "protocol.file.allow=always", "submodule", "add", "-q", lib, "sub", NULL });
  git(top, (const char *[]){ "-c", "user.name=t", "-c", "user.email=t@t", "commit", "-q", "-m", "sub", NULL });
  g_mkdir_with_parents(inner, 0755);
  g_file_set_contents(inner_file, "one\n", -1, NULL);
  git(inner, (const char *[]){ "init", "-q", NULL });
  commit_all(inner);
  g_file_set_contents(inner_file, "two\n", -1, NULL);

  open_project_at(top);
  wait_repo_rows(3); /* the project's own, inner and kept, not sub */
  repo_rows(&current);
  g_assert_cmpstr(current, ==, top);
  wait_for(2, 2);
  g_assert_cmpint(rows(1), ==, 0); /* inner/ is no change of the project's own */

  git_select_repo(&app, inner, VCS_GIT);
  g_assert_cmpstr(app.project.scm_top, ==, inner);
  wait_for(1, 1); /* in.txt changed */
  wait_for(2, 1);
  repo_rows(&current);
  g_assert_cmpstr(current, ==, inner);
  scm_stage("in.txt");
  wait_for(0, 1);
  staged = git_out(inner, (const char *[]){ "diff", "--cached", "--name-only", NULL });
  g_assert_cmpstr(staged, ==, "in.txt\n");
  g_free(staged);

  git_select_repo(&app, top, VCS_GIT);
  wait_for(2, 2);
  g_assert_cmpint(rows(0), ==, 0);

  /* A folder that is no repository, with repositories in it: they show, the first one in the panel. */
  char *folder = g_build_filename(tmp, "folder-of-repos", NULL), *one = g_build_filename(folder, "one", NULL),
       *two = g_build_filename(folder, "two", NULL), *file = g_build_filename(one, "f.txt", NULL);
  g_mkdir_with_parents(one, 0755);
  g_mkdir_with_parents(two, 0755);
  g_file_set_contents(file, "f\n", -1, NULL);
  git(one, (const char *[]){ "init", "-q", NULL });
  git(two, (const char *[]){ "init", "-q", NULL });
  open_project_at(folder);
  wait_repo_rows(2);
  g_assert_cmpstr(app.project.scm_top, ==, one);
  wait_for(1, 1); /* f.txt, untracked */

  open_project_at(no_git);
  run_for(800);
  g_assert_cmpint(repo_rows(NULL), ==, 0);
  g_assert_null(app.project.scm_top);
  g_free(file);
  g_free(two);
  g_free(one);
  g_free(folder);
  g_free(inner_file);
  g_free(inner);
  g_free(ignore);
  g_free(kept);
  g_free(lib);
  g_free(top);
}

/* Mercurial and Subversion ---------------------------------------------------------------------- */

static gboolean have(const char *program)
{
  char *path = g_find_program_in_path(program);
  g_free(path);
  return path != NULL;
}

/* Runs program in dir with args; the test fails when it does. */
static void tool(const char *dir, const char *program, const char *const *args)
{
  GPtrArray *argv = g_ptr_array_new();
  int status;

  g_ptr_array_add(argv, (gpointer)program);
  for (int i = 0; args[i]; i++)
    g_ptr_array_add(argv, (gpointer)args[i]);
  g_ptr_array_add(argv, NULL);
  g_assert_true(g_spawn_sync(dir, (char **)argv->pdata, NULL,
                             G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL,
                             NULL, NULL, &status, NULL));
  g_assert_true(g_spawn_check_wait_status(status, NULL));
  g_ptr_array_free(argv, TRUE);
}

static GtkWidget *panel_button(const char *label)
{
  GPtrArray *queue = g_ptr_array_new();
  GtkWidget *found = NULL;

  g_ptr_array_add(queue, app.scm);
  for (guint i = 0; i < queue->len && !found; i++) {
    GtkWidget *w = queue->pdata[i];
    if (GTK_IS_BUTTON(w) && g_strcmp0(gtk_button_get_label(GTK_BUTTON(w)), label) == 0)
      found = w;
    for (GtkWidget *c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c))
      g_ptr_array_add(queue, c);
  }
  g_ptr_array_free(queue, TRUE);
  return found;
}

/* A Mercurial repository: its changes, staging (an unknown file is added), committing the staged
 * file and then everything, the Commits list and a commit's files. */
static void test_hg(void)
{
  if (!have("hg")) {
    g_test_skip("no hg");
    return;
  }
  char *top = make_dir("hg", "a.txt"), *a = g_build_filename(top, "a.txt", NULL),
       *b = g_build_filename(top, "b.txt", NULL);
  tool(top, "hg", (const char *[]){ "init", NULL });
  tool(top, "hg", (const char *[]){ "commit", "-A", "-m", "first", NULL });
  g_file_set_contents(a, "two\n", -1, NULL);
  g_file_set_contents(b, "new\n", -1, NULL);

  open_project_at(top);
  wait_for(2, 1);
  wait_for(1, 2); /* a.txt changed, b.txt unknown */
  g_assert_cmpint(app.project.repo_kind, ==, VCS_HG);
  g_assert_cmpstr(app.project.repo_top, ==, top);
  g_assert_cmpstr(tab_title(), ==, "SOURCE CONTROL  2");
  g_assert_true(gtk_widget_get_visible(panel_button("Push")));
  g_assert_nonnull(panel_button("Pull"));

  scm_stage("a.txt");
  wait_for(0, 1);
  g_assert_cmpint(rows(1), ==, 1);
  scm_set_message("Change a");
  scm_commit();
  wait_for(2, 2);
  wait_for(0, 0);
  g_assert_cmpint(rows(1), ==, 1); /* b.txt was not staged: it stays */

  scm_stage("b.txt"); /* hg add */
  wait_for(0, 1);
  GitRepo *r = git_shown_repo(&app);
  g_assert_true(g_hash_table_contains(r->staged, "b.txt"));
  scm_unstage(g_ptr_array_index(scm.staged, 0));
  wait_for(1, 1); /* still added, but not staged */
  g_assert_cmpint(rows(0), ==, 0);
  scm_set_message("Everything");
  scm_commit(); /* nothing staged: every change */
  wait_for(2, 3);
  wait_for(1, 0);

  /* A commit's files under it. */
  GtkListBoxRow *last = gtk_list_box_get_row_at_index(list(2), 0);
  g_signal_emit_by_name(list(2), "row-activated", last);
  wait_for(2, 4);
  g_free(b);
  g_free(a);
  g_free(top);
}

/* What program prints in dir, stripped. */
static char *tool_out(const char *dir, const char *const *argv)
{
  char *out = NULL;

  g_spawn_sync(dir, (char **)argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, &out, NULL, NULL,
               NULL);
  return g_strstrip(out ? out : g_strdup(""));
}

/* Runs the main loop until argv prints want in dir (or 5 s pass); returns what it printed last. */
static char *wait_out(const char *dir, const char *const *argv, const char *want)
{
  char *out = tool_out(dir, argv);

  for (gint64 end = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
       strcmp(out, want) != 0 && g_get_monotonic_time() < end;) {
    run_for(50);
    g_free(out);
    out = tool_out(dir, argv);
  }
  return out;
}

/* Mercurial's branches: its named branches and bookmarks to switch to (hg update), a new one made
 * as a bookmark. */
static void test_hg_branches(void)
{
  if (!have("hg")) {
    g_test_skip("no hg");
    return;
  }
  const char *const where[] = { "hg", "log", "-r", ".", "-T", "{activebookmark}|{branch}", NULL };
  char *top = make_dir("hg-branches", "a.txt"), *a = g_build_filename(top, "a.txt", NULL), *now;
  tool(top, "hg", (const char *[]){ "init", NULL });
  tool(top, "hg", (const char *[]){ "commit", "-A", "-m", "one", NULL });
  tool(top, "hg", (const char *[]){ "branch", "other", NULL });
  g_file_set_contents(a, "two\n", -1, NULL);
  tool(top, "hg", (const char *[]){ "commit", "-m", "two", NULL });
  tool(top, "hg", (const char *[]){ "update", "-q", "default", NULL });
  open_project_at(top);
  wait_for(2, 2);

  picked_rows = g_ptr_array_new_with_free_func(g_free);
  scm_switch_branch();
  wait_rows(3);
  g_assert_cmpstr(picked_rows->pdata[0], ==, "+ Create new branch…\t\tnew\t");
  g_assert_true(g_str_has_prefix(picked_rows->pdata[1], "other\t"));
  g_assert_true(g_str_has_suffix(picked_rows->pdata[1], "· two\tswitch\tother"));
  g_assert_true(g_str_has_prefix(picked_rows->pdata[2], "default\tchecked out · "));
  picker_pick("switch\tother");
  now = wait_out(top, where, "|other");
  g_assert_cmpstr(now, ==, "|other");
  g_free(now);

  picker_run(&app, "feat");
  picker_pick("create\tfeat"); /* a bookmark, made active */
  now = wait_out(top, where, "feat|other");
  g_assert_cmpstr(now, ==, "feat|other");
  g_free(now);
  scm_switch_branch();
  wait_rows(4); /* + Create, feat, other, default */
  g_assert_true(g_str_has_prefix(picked_rows->pdata[1], "feat\t"));
  picker_pick("switch\tdefault");
  now = wait_out(top, where, "|default");
  g_assert_cmpstr(now, ==, "|default");
  g_free(now);
  g_ptr_array_free(picked_rows, TRUE);
  picked_rows = NULL;
  g_free(a);
  g_free(top);
}

/* The repository at top of kind among git_repos(), or NULL. */
static const GitRepo *find_repo_of(const char *top, VcsKind kind)
{
  const GPtrArray *all = git_repos();
  for (guint i = 0; i < all->len; i++) {
    const GitRepo *r = all->pdata[i];
    if (r->kind == kind && strcmp(r->top, top) == 0)
      return r;
  }
  return NULL;
}

/* An hg repository and an svn working copy (in a folder .gitignore leaves out) inside a git project:
 * both show in Repositories with their kind, their files are no changes of the project's own, and
 * each shows its own changes when picked. */
static void test_nested_others(void)
{
  if (!have("hg") || !have("svn") || !have("svnadmin")) {
    g_test_skip("no hg or svn");
    return;
  }
  char *top = make_dir("nested-others", "top.txt"), *ignore = g_build_filename(top, ".gitignore", NULL);
  char *hg = g_build_filename(top, "hgsub", NULL), *hg_file = g_build_filename(hg, "h.txt", NULL);
  char *repo = g_build_filename(tmp, "nested-svnrepo", NULL), *url = g_strconcat("file://", repo, NULL);
  char *wc = g_build_filename(top, "wc", NULL), *wc_file = g_build_filename(wc, "w.txt", NULL);
  g_file_set_contents(ignore, "wc/\n", -1, NULL);
  git(top, (const char *[]){ "init", "-q", NULL });
  commit_all(top);
  g_mkdir_with_parents(hg, 0755);
  g_file_set_contents(hg_file, "one\n", -1, NULL);
  tool(hg, "hg", (const char *[]){ "init", NULL });
  tool(hg, "hg", (const char *[]){ "commit", "-A", "-m", "first", NULL });
  g_file_set_contents(hg_file, "two\n", -1, NULL);
  tool(tmp, "svnadmin", (const char *[]){ "create", repo, NULL });
  tool(tmp, "svn", (const char *[]){ "checkout", "-q", url, wc, NULL });
  g_file_set_contents(wc_file, "one\n", -1, NULL);
  tool(wc, "svn", (const char *[]){ "add", "-q", "w.txt", NULL });
  tool(wc, "svn", (const char *[]){ "commit", "-q", "-m", "first", NULL });
  g_file_set_contents(wc_file, "two\n", -1, NULL);

  open_project_at(top);
  wait_repo_rows(3); /* the project's own, hgsub, wc */
  g_assert_cmpstr(app.project.scm_top, ==, top);
  wait_for(2, 1);
  for (gint64 end = g_get_monotonic_time() + G_USEC_PER_SEC; g_get_monotonic_time() < end;)
    run_for(50);
  g_assert_cmpint(rows(1), ==, 0); /* hgsub's files are not the project's untracked files */
  g_assert_nonnull(find_repo_of(hg, VCS_HG));
  g_assert_nonnull(find_repo_of(wc, VCS_SVN));

  git_select_repo(&app, hg, VCS_HG);
  g_assert_cmpint(app.project.scm_kind, ==, VCS_HG);
  wait_for(1, 1); /* h.txt changed */
  git_select_repo(&app, wc, VCS_SVN);
  wait_for(1, 1); /* w.txt changed */
  git_select_repo(&app, top, VCS_GIT);
  wait_for(1, 0);
  g_free(wc_file);
  g_free(wc);
  g_free(url);
  g_free(repo);
  g_free(hg_file);
  g_free(hg);
  g_free(ignore);
  g_free(top);
}

/* Whether a row of the list holds a label reading text. */
static gboolean row_has_label(GtkListBox *box, const char *text)
{
  GPtrArray *queue = g_ptr_array_new();
  gboolean found = FALSE;

  g_ptr_array_add(queue, box);
  for (guint i = 0; i < queue->len && !found; i++) {
    GtkWidget *w = queue->pdata[i];
    found = GTK_IS_LABEL(w) && strcmp(gtk_label_get_text(GTK_LABEL(w)), text) == 0;
    for (GtkWidget *c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c))
      g_ptr_array_add(queue, c);
  }
  g_ptr_array_free(queue, TRUE);
  return found;
}

/* Subversion's changelists: a file's one shows on its row; picking a changelist stages its files,
 * a typed name puts the staged files in a new one, they can be taken out again, and the commit takes
 * only the staged files. */
static void test_svn_changelists(void)
{
  if (!have("svn") || !have("svnadmin")) {
    g_test_skip("no svn");
    return;
  }
  char *repo = g_build_filename(tmp, "svnrepo-cl", NULL), *top = g_build_filename(tmp, "svn-cl", NULL);
  char *url = g_strconcat("file://", repo, NULL), *a = g_build_filename(top, "a.txt", NULL),
       *b = g_build_filename(top, "b.txt", NULL), *out;
  const char *const later[] = { "svn", "status", "--changelist", "later", "-q", NULL };
  tool(tmp, "svnadmin", (const char *[]){ "create", repo, NULL });
  tool(tmp, "svn", (const char *[]){ "checkout", "-q", url, top, NULL });
  g_file_set_contents(a, "one\n", -1, NULL);
  g_file_set_contents(b, "one\n", -1, NULL);
  tool(top, "svn", (const char *[]){ "add", "-q", "a.txt", "b.txt", NULL });
  tool(top, "svn", (const char *[]){ "commit", "-q", "-m", "first", NULL });
  g_file_set_contents(a, "two\n", -1, NULL);
  g_file_set_contents(b, "two\n", -1, NULL);
  tool(top, "svn", (const char *[]){ "changelist", "-q", "fixes", "a.txt", NULL });

  open_project_at(top);
  wait_for(1, 2);
  g_assert_true(row_has_label(list(1), "fixes"));

  picked_rows = g_ptr_array_new_with_free_func(g_free);
  scm_changelists();
  g_assert_cmpuint(picked_rows->len, ==, 1);
  g_assert_cmpstr(picked_rows->pdata[0], ==, "fixes\tstage its 1 files\tstage\tfixes");
  picker_pick("stage\tfixes");
  wait_for(0, 1);
  wait_for(1, 1); /* b.txt, in no changelist, stays */

  picker_run(&app, "later");
  g_assert_cmpstr(picked_rows->pdata[0], ==, "Put the staged files in \"later\"\ta new changelist\tmove\tlater");
  picker_pick("move\tlater");
  out = wait_out(top, later, "M       a.txt");
  g_assert_true(g_str_has_suffix(out, "M       a.txt"));
  g_free(out);
  for (gint64 end = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
       !row_has_label(list(0), "later") && g_get_monotonic_time() < end;)
    run_for(50);
  g_assert_true(row_has_label(list(0), "later"));

  scm_changelists();
  picker_pick("remove\t");
  out = wait_out(top, later, "");
  g_assert_cmpstr(out, ==, "");
  g_free(out);
  for (gint64 end = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
       strcmp(gtk_label_get_text(scm.note), "Removed from the changelists") != 0 && g_get_monotonic_time() < end;)
    run_for(50); /* the panel is done with it: a commit now is not skipped as busy */

  scm_set_message("Only a");
  scm_commit();
  wait_for(0, 0);
  wait_for(1, 1); /* b.txt */
  g_ptr_array_free(picked_rows, TRUE);
  picked_rows = NULL;
  g_free(b);
  g_free(a);
  g_free(url);
  g_free(top);
  g_free(repo);
}

/* Mercurial's stash is hg shelve: a shelve with a message, popped by its name (hg unshelve); one with
 * the unknown files, applied (it stays); stashing nothing says so. */
static void test_hg_shelve(void)
{
  if (!have("hg")) {
    g_test_skip("no hg");
    return;
  }
  const char *const list[] = { "hg", "shelve", "--list", NULL };
  char *top = make_dir("hg-shelve", "s.txt"), *file = g_build_filename(top, "s.txt", NULL),
       *unknown = g_build_filename(top, "new.txt", NULL), *left;
  tool(top, "hg", (const char *[]){ "init", NULL });
  tool(top, "hg", (const char *[]){ "commit", "-A", "-m", "first", NULL });
  open_project_at(top);
  wait_for(2, 1);
  picked_rows = g_ptr_array_new_with_free_func(g_free);

  g_file_set_contents(file, "two\n", -1, NULL);
  scm_stash();
  picker_pick("push\thalf done");
  g_assert_true(wait_file(file, "one\n"));
  scm_pick_stash(STASH_POP);
  wait_rows(1);
  g_assert_true(g_str_has_prefix(picked_rows->pdata[0], "half done\tdefault · "));
  g_assert_true(g_str_has_suffix(picked_rows->pdata[0], "\tdefault"));
  picker_pick("default");
  g_assert_true(wait_file(file, "two\n"));
  left = wait_out(top, list, "");
  g_assert_cmpstr(left, ==, "");
  g_free(left);

  g_file_set_contents(unknown, "new\n", -1, NULL);
  scm_stash();
  picker_pick("push-u\t");
  g_assert_true(wait_file(unknown, NULL));
  g_assert_true(wait_file(file, "one\n"));
  scm_pick_stash(STASH_APPLY);
  wait_rows(1);
  picker_pick("default");
  g_assert_true(wait_file(unknown, "new\n"));
  g_assert_true(wait_file(file, "two\n"));
  left = tool_out(top, list);
  g_assert_true(g_str_has_prefix(left, "default "));
  g_free(left);

  tool(top, "hg", (const char *[]){ "revert", "--all", "--no-backup", NULL });
  g_unlink(unknown);
  run_for(300);
  scm_stash();
  picker_pick("push\t");
  for (gint64 end = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
       g_strcmp0(gtk_label_get_text(scm.note), "No local changes to stash") != 0 && g_get_monotonic_time() < end;)
    run_for(50);
  g_assert_cmpstr(gtk_label_get_text(scm.note), ==, "No local changes to stash");

  g_ptr_array_free(picked_rows, TRUE);
  picked_rows = NULL;
  g_free(unknown);
  g_free(file);
  g_free(top);
}

/* Subversion's branches: trunk and the folders in branches/; a new one is copied on the server and
 * switched to (svn switch), and switching back works. */
static void test_svn_branches(void)
{
  if (!have("svn") || !have("svnadmin")) {
    g_test_skip("no svn");
    return;
  }
  const char *const where[] = { "svn", "info", "--show-item", "relative-url", NULL };
  char *repo = g_build_filename(tmp, "svnrepo-branches", NULL), *top = g_build_filename(tmp, "svn-branches", NULL);
  char *root = g_strconcat("file://", repo, NULL), *trunk = g_strconcat(root, "/trunk", NULL),
       *folder = g_strconcat(root, "/branches", NULL), *a = g_build_filename(top, "a.txt", NULL), *now;
  tool(tmp, "svnadmin", (const char *[]){ "create", repo, NULL });
  tool(tmp, "svn", (const char *[]){ "mkdir", "-q", "-m", "layout", trunk, folder, NULL });
  tool(tmp, "svn", (const char *[]){ "checkout", "-q", trunk, top, NULL });
  g_file_set_contents(a, "one\n", -1, NULL);
  tool(top, "svn", (const char *[]){ "add", "-q", "a.txt", NULL });
  tool(top, "svn", (const char *[]){ "commit", "-q", "-m", "first", NULL });
  tool(top, "svn", (const char *[]){ "update", "-q", NULL });
  open_project_at(top);
  wait_for(2, 2);

  picked_rows = g_ptr_array_new_with_free_func(g_free);
  scm_switch_branch();
  wait_rows(2);
  g_assert_true(g_str_has_prefix(picked_rows->pdata[1], "trunk\tchecked out · "));
  picker_run(&app, "fix");
  picker_pick("create\tfix");
  now = wait_out(top, where, "^/branches/fix");
  g_assert_cmpstr(now, ==, "^/branches/fix");
  g_free(now);
  scm_switch_branch();
  wait_rows(3); /* + Create, fix, trunk */
  g_assert_true(g_str_has_prefix(picked_rows->pdata[1], "fix\t"));
  picker_pick("switch\ttrunk");
  now = wait_out(top, where, "^/trunk");
  g_assert_cmpstr(now, ==, "^/trunk");
  g_free(now);
  g_ptr_array_free(picked_rows, TRUE);
  picked_rows = NULL;
  g_free(a);
  g_free(folder);
  g_free(trunk);
  g_free(root);
  g_free(top);
  g_free(repo);
}

/* An hg merge with a conflict: the conflict shows (hg status calls it modified), staging it marks it
 * resolved, and the commit takes the whole merge without the .orig copy hg left. */
static void test_hg_merge(void)
{
  if (!have("hg")) {
    g_test_skip("no hg");
    return;
  }
  char *top = make_dir("hg-merge", "f.txt"), *f = g_build_filename(top, "f.txt", NULL), *orig = NULL;
  tool(top, "hg", (const char *[]){ "init", NULL });
  g_file_set_contents(f, "a\n", -1, NULL);
  tool(top, "hg", (const char *[]){ "commit", "-A", "-m", "one", NULL });
  g_file_set_contents(f, "b\n", -1, NULL);
  tool(top, "hg", (const char *[]){ "commit", "-m", "two", NULL });
  tool(top, "hg", (const char *[]){ "update", "-q", "0", NULL });
  g_file_set_contents(f, "c\n", -1, NULL);
  tool(top, "hg", (const char *[]){ "commit", "-m", "three", NULL });
  g_spawn_sync(top, (char *[]){ "hg", "merge", "--tool", "internal:merge", NULL }, NULL,
               G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, NULL, NULL,
               NULL, NULL); /* exits 1: the conflict */
  orig = g_build_filename(top, "f.txt.orig", NULL);

  open_project_at(top);
  wait_for(2, 3);
  int changes = g_file_test(orig, G_FILE_TEST_EXISTS) ? 2 : 1; /* f.txt, and f.txt.orig when hg kept one */
  wait_for(1, changes);
  gboolean conflict = FALSE;
  for (guint i = 0; i < scm.changes->len; i++) {
    const Change *c = scm.changes->pdata[i];
    conflict |= strcmp(c->path, "f.txt") == 0 && c->code == '!';
  }
  g_assert_true(conflict);

  g_file_set_contents(f, "b and c\n", -1, NULL);
  scm_stage("f.txt"); /* hg resolve --mark */
  wait_for(0, 1);
  wait_for(1, changes - 1);
  scm_set_message("Merge");
  scm_commit();
  wait_for(2, 4);
  wait_for(0, 0);
  wait_for(1, changes - 1); /* the .orig copy was not added */
  g_free(orig);
  g_free(f);
  g_free(top);
}

/* A Subversion working copy: no Push, Update for Pull; a commit of the staged file and of everything
 * (a new folder added on the way). */
static void test_svn(void)
{
  if (!have("svn") || !have("svnadmin")) {
    g_test_skip("no svn");
    return;
  }
  char *repo = g_build_filename(tmp, "svnrepo", NULL), *top = g_build_filename(tmp, "svn", NULL);
  char *url = g_strconcat("file://", repo, "/trunk", NULL), *a = g_build_filename(top, "a.txt", NULL);
  char *n = g_build_filename(top, "d", "n.txt", NULL), *d = g_build_filename(top, "d", NULL);
  tool(tmp, "svnadmin", (const char *[]){ "create", repo, NULL });
  tool(tmp, "svn", (const char *[]){ "mkdir", "-q", "-m", "init", url, NULL });
  tool(tmp, "svn", (const char *[]){ "checkout", "-q", url, top, NULL });
  g_file_set_contents(a, "one\n", -1, NULL);
  tool(top, "svn", (const char *[]){ "add", "-q", "a.txt", NULL });
  tool(top, "svn", (const char *[]){ "commit", "-q", "-m", "first", NULL });
  tool(top, "svn", (const char *[]){ "update", "-q", NULL });
  g_file_set_contents(a, "two\n", -1, NULL);
  g_mkdir_with_parents(d, 0755);
  g_file_set_contents(n, "new\n", -1, NULL);

  open_project_at(top);
  wait_for(2, 2); /* init, first */
  wait_for(1, 2); /* a.txt changed, d unknown */
  g_assert_cmpint(app.project.repo_kind, ==, VCS_SVN);
  g_assert_false(gtk_widget_get_visible(panel_button("Push")));
  g_assert_nonnull(panel_button("Update"));

  scm_stage("a.txt");
  wait_for(0, 1);
  scm_set_message("Change a");
  scm_commit();
  wait_for(0, 0);
  wait_for(1, 1);
  scm_set_message("Everything");
  scm_commit(); /* svn add of d, then the commit */
  wait_for(1, 0);
  tool(top, "svn", (const char *[]){ "update", "-q", NULL });
  refresh_branch(&app);
  wait_for(2, 4);
  g_free(d);
  g_free(n);
  g_free(a);
  g_free(url);
  g_free(top);
  g_free(repo);
}

/* An svn update that conflicts: the conflict shows, staging it accepts the file as it is
 * (svn resolve --accept working) and the commit takes it. */
static void test_svn_conflict(void)
{
  if (!have("svn") || !have("svnadmin")) {
    g_test_skip("no svn");
    return;
  }
  char *repo = g_build_filename(tmp, "svnrepo-conflict", NULL), *top = g_build_filename(tmp, "svn-conflict", NULL),
       *other = g_build_filename(tmp, "svn-conflict-other", NULL);
  char *url = g_strconcat("file://", repo, "/trunk", NULL), *a = g_build_filename(top, "a.txt", NULL),
       *b = g_build_filename(other, "a.txt", NULL);
  tool(tmp, "svnadmin", (const char *[]){ "create", repo, NULL });
  tool(tmp, "svn", (const char *[]){ "mkdir", "-q", "-m", "init", url, NULL });
  tool(tmp, "svn", (const char *[]){ "checkout", "-q", url, top, NULL });
  g_file_set_contents(a, "one\n", -1, NULL);
  tool(top, "svn", (const char *[]){ "add", "-q", "a.txt", NULL });
  tool(top, "svn", (const char *[]){ "commit", "-q", "-m", "first", NULL });
  tool(tmp, "svn", (const char *[]){ "checkout", "-q", url, other, NULL });
  g_file_set_contents(b, "theirs\n", -1, NULL);
  tool(other, "svn", (const char *[]){ "commit", "-q", "-m", "theirs", NULL });
  g_file_set_contents(a, "mine\n", -1, NULL);
  tool(top, "svn", (const char *[]){ "update", "-q", "--accept", "postpone", NULL });

  open_project_at(top);
  wait_for(2, 3);
  gboolean conflict = FALSE;
  for (gint64 until = g_get_monotonic_time() + 5 * G_USEC_PER_SEC; !conflict && g_get_monotonic_time() < until;) {
    g_main_context_iteration(NULL, FALSE);
    for (guint i = 0; i < scm.changes->len; i++) {
      const Change *c = scm.changes->pdata[i];
      conflict |= strcmp(c->path, "a.txt") == 0 && c->code == '!';
    }
  }
  g_assert_true(conflict);

  g_file_set_contents(a, "mine and theirs\n", -1, NULL);
  scm_stage("a.txt"); /* svn resolve --accept working */
  wait_for(0, 1);
  scm_set_message("Both");
  scm_commit();
  wait_for(0, 0);
  tool(top, "svn", (const char *[]){ "update", "-q", NULL });
  refresh_branch(&app);
  wait_for(2, 4);
  g_free(b);
  g_free(a);
  g_free(url);
  g_free(other);
  g_free(top);
  g_free(repo);
}

static void wait_kind(VcsKind kind)
{
  for (gint64 until = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;
       app.project.repo_kind != kind && g_get_monotonic_time() < until;)
    g_main_context_iteration(NULL, FALSE);
  g_assert_cmpint(app.project.repo_kind, ==, kind);
}

/* Initialize Repository with Subversion: no URL makes a local repository (in the data folder, with a
 * trunk) and checks the folder out from it, its files kept; a URL checks out from there. */
static void test_svn_init(void)
{
  if (!have("svn") || !have("svnadmin")) {
    g_test_skip("no svn");
    return;
  }
  char *local = make_dir("svn-local", "l.txt"), *served = make_dir("svn-served", "s.txt");
  char *repository = g_build_filename(g_get_user_data_dir(), "t-code", "svn", "svn-local", NULL);

  open_project_at(local);
  run_for(500);
  scm_init_repository(VCS_SVN, "  ");
  wait_kind(VCS_SVN);
  g_assert_true(g_file_test(repository, G_FILE_TEST_IS_DIR));
  g_assert_cmpstr(app.project.repo_top, ==, local);
  wait_for(1, 1); /* l.txt stays, unknown */
  wait_for(2, 1); /* Create trunk */

  /* The same repository again, from its URL: a working copy of it in another folder. */
  char *uri = g_filename_to_uri(repository, NULL, NULL), *trunk = g_strconcat(uri, "/trunk", NULL);
  open_project_at(served);
  run_for(500);
  scm_init_repository(VCS_SVN, trunk);
  wait_kind(VCS_SVN);
  g_assert_cmpstr(app.project.repo_top, ==, served);
  wait_for(1, 1); /* s.txt */

  open_project_at(no_git);
  run_for(300);
  g_free(trunk);
  g_free(uri);
  g_free(repository);
  g_free(served);
  g_free(local);
}

/* A folder that is a git and a Mercurial repository (hg-git): both show, git is the project's own
 * until Mercurial is chosen. A folder in none offers to make one, of the kind picked. */
static void test_choice(void)
{
  if (!have("hg")) {
    g_test_skip("no hg");
    return;
  }
  char *top = make_dir("both", "x.txt"), *current = NULL;
  git(top, (const char *[]){ "init", "-q", NULL });
  tool(top, "hg", (const char *[]){ "init", NULL });

  git_set_chosen_kind(VCS_NONE);
  open_project_at(top);
  wait_repo_rows(2);
  g_assert_cmpint(app.project.repo_kind, ==, VCS_GIT);
  git_select_repo(&app, top, VCS_HG);
  g_assert_cmpint(app.project.repo_kind, ==, VCS_HG);
  g_assert_cmpint(app.project.scm_kind, ==, VCS_HG);
  g_assert_cmpint(git_chosen_kind(), ==, VCS_HG);
  repo_rows(&current);
  g_assert_cmpstr(current, ==, top);
  /* The choice comes back with the project (session.c sets it before watch_git). */
  open_project_at(no_git);
  run_for(500);
  open_project_at(top);
  wait_repo_rows(2);
  g_assert_cmpint(app.project.repo_kind, ==, VCS_HG);
  git_set_chosen_kind(VCS_NONE);

  char *empty = make_dir("empty", "e.txt"), *hg = g_build_filename(empty, ".hg", NULL);
  open_project_at(empty);
  run_for(500);
  g_assert_true(gtk_widget_get_visible(scm.init_box));
  scm_init_repository(VCS_HG, NULL);
  for (gint64 until = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
       app.project.repo_kind != VCS_HG && g_get_monotonic_time() < until;)
    g_main_context_iteration(NULL, FALSE);
  g_assert_true(g_file_test(hg, G_FILE_TEST_IS_DIR));
  g_assert_cmpint(app.project.repo_kind, ==, VCS_HG);
  wait_for(1, 1); /* e.txt, unknown */
  g_assert_false(gtk_widget_get_visible(scm.init_box));
  g_free(hg);
  g_free(empty);
  g_free(top);
}

int main(int argc, char **argv)
{
  int status;

  g_test_init(&argc, &argv, NULL);
  ignore_gtk_noise();
  g_setenv("HGUSER", "t <t@t>", TRUE); /* hg commits without asking who */
  /* Local Subversion repositories go there, not in the user's data folder (set before GTK reads it). */
  data_home = g_dir_make_tmp("t-code-scm-data-XXXXXX", NULL);
  g_setenv("XDG_DATA_HOME", data_home, TRUE);
  if (!gtk_init_check()) {
    g_test_skip("no display");
    return g_test_run();
  }
  tmp = g_dir_make_tmp("t-code-scm-XXXXXX", NULL);
  with_commits = make_dir("with-commits", "a.txt");
  git(with_commits, (const char *[]){ "init", "-q", NULL });
  git(with_commits, (const char *[]){ "-c", "user.name=t", "-c", "user.email=t@t", "commit", "-q", "--allow-empty",
                                      "-m", "first", NULL });
  git(with_commits, (const char *[]){ "add", "a.txt", NULL });
  git(with_commits,
      (const char *[]){ "-c", "user.name=t", "-c", "user.email=t@t", "commit", "-q", "-m", "second", NULL });
  char *changed = g_build_filename(with_commits, "a.txt", NULL), *added = g_build_filename(with_commits, "b.txt", NULL);
  g_file_set_contents(changed, "two\n", -1, NULL);
  g_file_set_contents(added, "new\n", -1, NULL);
  g_free(changed);
  g_free(added);
  no_git = make_dir("no-git", "x.txt");
  no_commits = make_dir("no-commits", "c.txt");
  git(no_commits, (const char *[]){ "init", "-q", NULL });

  app.project.root = g_strdup(tmp);
  app.paths.temp = g_build_filename(tmp, "temp", NULL);
  app.dock.scm_stack = GTK_STACK(gtk_stack_new());
  app.scm = scm_panel(&app);
  gtk_stack_add_titled(app.dock.scm_stack, app.scm, "scm", "SOURCE CONTROL");

  g_test_add_func("/scm/repository", test_repository);
  g_test_add_func("/scm/discard-after-switch", test_discard_after_switch);
  g_test_add_func("/scm/no-git", test_no_git);
  g_test_add_func("/scm/no-commits", test_no_commits);
  g_test_add_func("/scm/same-status", test_same_status);
  g_test_add_func("/scm/late-log", test_late_log);
  g_test_add_func("/scm/late-status", test_late_status);
  g_test_add_func("/scm/head-moves", test_head_moves);
  g_test_add_func("/scm/commits-folded", test_commits_folded);
  g_test_add_func("/scm/commit-details", test_commit_details);
  g_test_add_func("/scm/message-history", test_message_history);
  g_test_add_func("/scm/stage-hunk", test_stage_hunk);
  g_test_add_func("/scm/branches", test_branches);
  g_test_add_func("/scm/stash", test_stash);
  g_test_add_func("/scm/nested-repos", test_nested_repos);
  g_test_add_func("/scm/hg", test_hg);
  g_test_add_func("/scm/hg-merge", test_hg_merge);
  g_test_add_func("/scm/hg-branches", test_hg_branches);
  g_test_add_func("/scm/svn-branches", test_svn_branches);
  g_test_add_func("/scm/hg-shelve", test_hg_shelve);
  g_test_add_func("/scm/nested-others", test_nested_others);
  g_test_add_func("/scm/svn-changelists", test_svn_changelists);
  g_test_add_func("/scm/svn-conflict", test_svn_conflict);
  g_test_add_func("/scm/svn", test_svn);
  g_test_add_func("/scm/svn-init", test_svn_init);
  g_test_add_func("/scm/choice", test_choice);
  status = g_test_run();

  app.closing = TRUE;
  char *rm[] = { "rm", "-rf", tmp, data_home, NULL };
  g_spawn_sync(NULL, rm, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, NULL, NULL);
  return status;
}
