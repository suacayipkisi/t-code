/* Tests for the Source Control panel (src/git/scm.c) and the git status that feeds it
 * (src/git/gitstatus.c, included to reach poll_branch) when projects switch: nothing of the last project may stay in
 * a folder without git, in a repository without commits, or when git answers late. Uses real
 * repositories in a temporary folder; needs a display. */
#include "git/gitstatus.c"

#include "editor/editor.h"
#include "editor/tabs.h"
#include "git/compare.h"
#include "popups/picker.h"

#include <glib/gstdio.h>

static int head_moves; /* diff_gutter_head_moved() calls */

/* What scm.c and gitstatus.c call elsewhere in t-code. */
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

static void runs_for(int ms)
{
  for (gint64 until = g_get_monotonic_time() + ms * 1000; g_get_monotonic_time() < until;)
    g_main_context_iteration(NULL, FALSE);
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
    if (GTK_IS_LIST_BOX(w))
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
  g_free(app.project.root);
  app.project.root = g_strdup(root);
  scm_reset();
  watch_git(&app); /* git_top, then git status in 300ms */
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

static void test_repository(void)
{
  open_with_commits();
}

static void test_no_git(void)
{
  open_with_commits();
  open_project_at(no_git);
  assert_empty(); /* at once, before git answers */
  g_assert_null(app.project.git_top);
  runs_for(800);
  assert_empty();
  /* git status runs again every 10 seconds: still nothing. */
  poll_branch(&app);
  runs_for(300);
  assert_empty();
}

static void test_no_commits(void)
{
  open_with_commits();
  open_project_at(no_commits);
  g_assert_cmpint(rows(2), ==, 0);
  wait_for(1, 1); /* c.txt, untracked */
  runs_for(500);
  g_assert_cmpint(rows(2), ==, 0);
  g_assert_cmpstr(tab_title(), ==, "SOURCE CONTROL  1");
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
  runs_for(800);
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
  runs_for(800);
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
    runs_for(50);
  }
  return head_moves > before;
}

/* A commit on a branch moves the gutters' base; staging a file does not. */
static void test_head_moves(void)
{
  int before;

  open_with_commits();
  runs_for(400);
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
  runs_for(800);
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
    runs_for(50);
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

int main(int argc, char **argv)
{
  int status;

  g_test_init(&argc, &argv, NULL);
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
  g_test_add_func("/scm/no-git", test_no_git);
  g_test_add_func("/scm/no-commits", test_no_commits);
  g_test_add_func("/scm/late-log", test_late_log);
  g_test_add_func("/scm/late-status", test_late_status);
  g_test_add_func("/scm/head-moves", test_head_moves);
  g_test_add_func("/scm/commits-folded", test_commits_folded);
  g_test_add_func("/scm/commit-details", test_commit_details);
  g_test_add_func("/scm/message-history", test_message_history);
  g_test_add_func("/scm/stage-hunk", test_stage_hunk);
  status = g_test_run();

  app.closing = TRUE;
  char *rm[] = { "rm", "-rf", tmp, NULL };
  g_spawn_sync(NULL, rm, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, NULL, NULL);
  return status;
}
