/* Tests for Mercurial and Subversion in git's formats (src/git/vcs.c): finding the repositories, then
 * recorded answers, then real repositories built here (each skipped without hg or svn). */
#include "git/vcs.h"

#include "util.h"

#include <gio/gio.h>

#include <string.h>

static char *tmp;

static gsize last_length; /* run()'s output's length: it may hold NULs */

/* Runs program in dir with args (and a second list's); returns its stdout (its length in
 * last_length), NULL when it fails. */
static char *run(const char *dir, const char *program, const char *const *args, const char *const *more)
{
  GPtrArray *argv = g_ptr_array_new();
  GSubprocessLauncher *launcher =
      g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE);
  GBytes *bytes = NULL;
  char *out = NULL;

  g_ptr_array_add(argv, (gpointer)program);
  for (int i = 0; args && args[i]; i++)
    g_ptr_array_add(argv, (gpointer)args[i]);
  for (int i = 0; more && more[i]; i++)
    g_ptr_array_add(argv, (gpointer)more[i]);
  g_ptr_array_add(argv, NULL);
  g_subprocess_launcher_set_cwd(launcher, dir);
  g_subprocess_launcher_setenv(launcher, "HGPLAIN", "1", TRUE);
  g_subprocess_launcher_setenv(launcher, "HGUSER", "Ada <ada@x>", TRUE);
  g_subprocess_launcher_setenv(launcher, "LC_ALL", "C", TRUE);
  GSubprocess *proc = g_subprocess_launcher_spawnv(launcher, (const char *const *)argv->pdata, NULL);
  if (proc && g_subprocess_communicate(proc, NULL, NULL, &bytes, NULL, NULL) && g_subprocess_get_successful(proc)) {
    gsize size;
    const char *data = g_bytes_get_data(bytes, &size);
    out = g_malloc(size + 1);
    if (size)
      memcpy(out, data, size);
    out[size] = '\0';
    last_length = size;
  }
  if (bytes)
    g_bytes_unref(bytes);
  g_clear_object(&proc);
  g_object_unref(launcher);
  g_ptr_array_free(argv, TRUE);
  return out;
}

static gboolean have(const char *program)
{
  char *path = g_find_program_in_path(program);
  g_free(path);
  return path != NULL;
}

static void must(const char *dir, const char *program, const char *const *args)
{
  char *out = run(dir, program, args, NULL);
  g_assert_nonnull(out);
  g_free(out);
}

/* What the args (without the program) print, run in dir. */
static char *ask(const char *dir, VcsKind kind, char **args)
{
  char *out = run(dir, vcs_program(kind), (const char *const *)args, NULL);
  g_strfreev(args);
  return out;
}

/* The records of a -z output, joined by "|" (easier to compare). */
static char *records(const char *out, gsize length)
{
  char *copy = g_memdup2(out, length + 1);
  for (gsize i = 0; i < length; i++)
    if (copy[i] == '\0')
      copy[i] = '|';
  copy[length] = '\0';
  return copy;
}

static void test_find(void)
{
  char *dir = g_build_filename(tmp, "find", "inner", "deep", NULL), *outer = g_build_filename(tmp, "find", NULL);
  char *inner = g_build_filename(tmp, "find", "inner", NULL), *top = NULL;
  char *marks[] = { g_build_filename(outer, ".svn", NULL), g_build_filename(inner, ".hg", NULL),
                    g_build_filename(inner, ".git", NULL) };

  g_mkdir_with_parents(dir, 0755);
  g_assert_cmpint(vcs_kind_of(dir, &top), ==, VCS_NONE);
  g_assert_null(top);
  for (guint i = 0; i < G_N_ELEMENTS(marks); i++)
    g_mkdir_with_parents(marks[i], 0755);

  GPtrArray *found = vcs_find_up(dir);
  g_assert_cmpuint(found->len, ==, 3);
  g_assert_cmpint(((VcsPlace *)found->pdata[0])->kind, ==, VCS_GIT); /* hg-git: both in one folder, git first */
  g_assert_cmpstr(((VcsPlace *)found->pdata[0])->top, ==, inner);
  g_assert_cmpint(((VcsPlace *)found->pdata[1])->kind, ==, VCS_HG);
  g_assert_cmpint(((VcsPlace *)found->pdata[2])->kind, ==, VCS_SVN);
  g_assert_cmpstr(((VcsPlace *)found->pdata[2])->top, ==, outer);
  g_ptr_array_unref(found);

  g_assert_cmpint(vcs_kind_of(dir, &top), ==, VCS_GIT);
  g_assert_cmpstr(top, ==, inner);
  g_clear_pointer(&top, g_free);
  /* The user's choice wins, also over a nearer repository. */
  vcs_prefer(outer, VCS_SVN);
  g_assert_cmpint(vcs_kind_of(dir, &top), ==, VCS_SVN);
  g_assert_cmpstr(top, ==, outer);
  g_clear_pointer(&top, g_free);
  vcs_prefer(outer, VCS_NONE);
  vcs_prefer(inner, VCS_HG);
  g_assert_cmpint(vcs_kind_of(dir, NULL), ==, VCS_HG);
  vcs_prefer(inner, VCS_NONE);
  g_assert_cmpint(vcs_kind_of(dir, NULL), ==, VCS_GIT);

  g_assert_cmpint(vcs_mark_in(inner), ==, VCS_HG);
  g_assert_cmpint(vcs_mark_in(outer), ==, VCS_SVN);
  g_assert_cmpint(vcs_mark_in(dir), ==, VCS_NONE);
  g_assert_cmpint(vcs_kind_named("hg"), ==, VCS_HG);
  g_assert_cmpstr(vcs_title(VCS_SVN), ==, "Subversion");
  for (guint i = 0; i < G_N_ELEMENTS(marks); i++)
    g_free(marks[i]);
  g_free(dir);
  g_free(inner);
  g_free(outer);
}

/* hg status --print0 --copies as hg 7.2 gives it: a move, a removal, a copy, an unknown file. */
static void test_hg_status_recorded(void)
{
  static const char status[] = "M f.txt\0A added.txt\0A d/k.txt\0  d/m.txt\0A copy.txt\0  f.txt\0R d/m.txt\0"
                               "R g.txt\0! gone.txt\0? new.txt\0";
  GHashTable *staged = g_hash_table_new(g_str_hash, g_str_equal);
  gsize length;

  g_hash_table_add(staged, "f.txt");
  g_hash_table_add(staged, "d/k.txt");
  char *out = vcs_git_status(VCS_HG, status, sizeof status - 1,
                             "9c65c4ef\x1f"
                             "default\x1f",
                             staged, FALSE, &length);
  char *joined = records(out, length);
  g_assert_cmpstr(joined, ==,
                  "# branch.oid 9c65c4ef|# branch.head default|# branch.upstream default|"
                  "1 M. N... 000000 000000 000000 0 0 f.txt|"
                  "1 .A N... 000000 000000 000000 0 0 added.txt|"
                  "2 R. N... 000000 000000 000000 0 0 R100 d/k.txt|d/m.txt|"
                  "2 .C N... 000000 000000 000000 0 0 C100 copy.txt|f.txt|"
                  "1 .D N... 000000 000000 000000 0 0 g.txt|"
                  "1 .D N... 000000 000000 000000 0 0 gone.txt|"
                  "? new.txt|");
  g_free(joined);
  g_free(out);

  /* The bookmark is the branch shown; no commit yet. */
  out = vcs_git_status(VCS_HG, "", 0,
                       "0000000000\x1f"
                       "default\x1f"
                       "feature",
                       NULL, FALSE, &length);
  joined = records(out, length);
  g_assert_cmpstr(joined, ==, "# branch.oid (initial)|# branch.head feature|# branch.upstream default|");
  g_free(joined);
  g_free(out);

  out = vcs_git_status(VCS_HG, status, sizeof status - 1, NULL, staged, TRUE, &length);
  joined = records(out, length);
  g_assert_cmpstr(joined, ==,
                  "M  f.txt| A added.txt|R  d/k.txt|d/m.txt| C copy.txt|f.txt| D g.txt| D gone.txt|?? new.txt|");
  g_free(joined);
  g_free(out);
  g_hash_table_unref(staged);
}

/* svn status --xml and svn info --xml as svn 1.15 gives them (shortened). */
static const char svn_status[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<status>\n<target path=\".\">\n"
    "<entry path=\".\"><wc-status item=\"normal\" revision=\"1\" props=\"modified\"></wc-status></entry>\n"
    "<entry path=\"added.txt\"><wc-status item=\"added\" revision=\"-1\" props=\"none\"></wc-status></entry>\n"
    "<entry path=\"build\"><wc-status props=\"none\" item=\"ignored\"></wc-status></entry>\n"
    "<entry path=\"d/k.txt\"><wc-status props=\"none\" copied=\"true\" moved-from=\"d/m.txt\" item=\"added\">"
    "<commit revision=\"2\"><author>ada</author><date>2026-11-18T19:23:59.494070Z</date></commit></wc-status></entry>\n"
    "<entry path=\"d/m.txt\"><wc-status moved-to=\"d/k.txt\" item=\"deleted\" revision=\"2\" props=\"none\"></wc-status>"
    "</entry>\n"
    "<entry path=\"f.txt\"><wc-status item=\"modified\" revision=\"2\" props=\"none\"></wc-status></entry>\n"
    "<entry path=\"c.txt\"><wc-status item=\"conflicted\" revision=\"2\" props=\"none\"></wc-status></entry>\n"
    "<entry path=\"p.txt\"><wc-status item=\"normal\" revision=\"2\" props=\"modified\"></wc-status></entry>\n"
    "<entry path=\"new.txt\"><wc-status item=\"unversioned\" props=\"none\"></wc-status></entry>\n"
    "</target>\n</status>\n";
static const char svn_info[] = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<info>\n"
                               "<entry kind=\"dir\" path=\".\" revision=\"7\">\n<url>file:///r/branches/fix</url>\n"
                               "<relative-url>^/branches/fix</relative-url>\n</entry>\n</info>\n";

static void test_svn_status_recorded(void)
{
  GHashTable *staged = g_hash_table_new(g_str_hash, g_str_equal);
  gsize length;

  g_hash_table_add(staged, "added.txt");
  char *out = vcs_git_status(VCS_SVN, svn_status, sizeof svn_status - 1, svn_info, staged, FALSE, &length);
  char *joined = records(out, length);
  g_assert_cmpstr(joined, ==,
                  "# branch.oid 7|# branch.head fix|# branch.upstream server|"
                  "1 A. N... 000000 000000 000000 0 0 added.txt|"
                  "! build|"
                  "2 .R N... 000000 000000 000000 0 0 R100 d/k.txt|d/m.txt|"
                  "1 .M N... 000000 000000 000000 0 0 f.txt|"
                  "u UU N... 000000 000000 000000 000000 0 0 0 c.txt|"
                  "1 .M N... 000000 000000 000000 0 0 p.txt|"
                  "? new.txt|");
  g_free(joined);
  g_free(out);
  g_hash_table_unref(staged);

  char *url = vcs_svn_url(svn_info);
  g_assert_cmpstr(url, ==, "^/branches/fix");
  g_free(url);
  out =
      vcs_git_status(VCS_SVN, "", 0, "<info><entry revision=\"0\"><relative-url>^/trunk</relative-url></entry></info>",
                     NULL, FALSE, &length);
  joined = records(out, length);
  g_assert_cmpstr(joined, ==, "# branch.oid (initial)|# branch.head trunk|# branch.upstream server|");
  g_free(joined);
  g_free(out);
}

static void test_revisions(void)
{
  char *rev = vcs_rev(VCS_SVN, "12^");
  g_assert_cmpstr(rev, ==, "11");
  g_free(rev);
  rev = vcs_rev(VCS_SVN, "HEAD");
  g_assert_cmpstr(rev, ==, "BASE");
  g_free(rev);
  rev = vcs_rev(VCS_HG, "HEAD");
  g_assert_cmpstr(rev, ==, ".");
  g_free(rev);
  rev = vcs_rev(VCS_HG, "abc^");
  g_assert_cmpstr(rev, ==, "abc^");
  g_free(rev);

  char **args = vcs_cat_args(VCS_SVN, "5", "d/m.txt", "^/trunk");
  char *joined = g_strjoinv(" ", args);
  g_assert_cmpstr(joined, ==, "cat --non-interactive ^/trunk/d/m.txt@5");
  g_free(joined);
  g_strfreev(args);
  args = vcs_cat_args(VCS_GIT, "HEAD", "a.c", NULL);
  joined = g_strjoinv(" ", args);
  g_assert_cmpstr(joined, ==, "show HEAD:./a.c");
  g_free(joined);
  g_strfreev(args);
}

static void test_recorded_logs(void)
{
  static const char log[] = "<?xml version=\"1.0\"?>\n<log>\n<logentry revision=\"2\"><author>ada</author>"
                            "<date>2026-11-18T19:23:59.494070Z</date><msg>First line\nbody</msg></logentry>\n"
                            "<logentry revision=\"1\"><author>bo</author><date>2026-11-17T10:00:00.000000Z</date>"
                            "<msg>init</msg></logentry>\n</log>\n";
  char *out = vcs_git_log(VCS_SVN, log);
  char **commits = g_strsplit(out, "\x1e", -1);
  g_assert_cmpuint(g_strv_length(commits), ==, 3);
  char **f = g_strsplit(commits[1], "\x1f", 9);
  g_assert_cmpuint(g_strv_length(f), ==, 9);
  g_assert_cmpstr(f[0], ==, "2");
  g_assert_cmpstr(f[1], ==, "r2");
  g_assert_cmpstr(f[2], ==, "First line");
  g_assert_cmpstr(f[3], ==, "ada");
  g_assert_true(g_str_has_suffix(f[5], " ago"));
  g_assert_true(g_str_has_prefix(f[8], "First line\nbody\x1d"));
  g_strfreev(f);
  g_strfreev(commits);
  g_free(out);

  out = vcs_git_messages(VCS_SVN, log);
  g_assert_cmpstr(out, ==, "First line\nbody\x1einit\x1e");
  g_free(out);

  static const char files[] = "<log><logentry revision=\"3\"><paths>"
                              "<path action=\"A\" kind=\"dir\">/trunk/d</path>"
                              "<path action=\"M\" kind=\"file\">/trunk/f.txt</path>"
                              "<path action=\"A\" kind=\"file\" copyfrom-path=\"/trunk/d/m.txt\" copyfrom-rev=\"2\">"
                              "/trunk/d/k.txt</path>"
                              "<path action=\"D\" kind=\"file\">/trunk/d/m.txt</path>"
                              "<path action=\"M\" kind=\"file\">/branches/x/f.txt</path>"
                              "</paths></logentry></log>";
  out = vcs_git_commit_files(VCS_SVN, files, "^/trunk");
  g_assert_cmpstr(out, ==, "M\tf.txt\nC100\td/m.txt\td/k.txt\nD\td/m.txt\n");
  g_free(out);

  out = vcs_git_show("diff --git a/f.txt b/f.txt\n--- a/f.txt\n+++ b/f.txt\n@@ -1,2 +1,2 @@\n a\n-b\n+c\n+d\n"
                     "diff --git a/g b/g\nnew file mode 100644\n");
  g_assert_true(g_str_has_prefix(out, "2\t1\tf.txt\n0\t0\tg\ndiff --git a/f.txt"));
  g_free(out);

  static const char blame[] = "<blame><target path=\"f.txt\"><entry line-number=\"1\"><commit revision=\"2\">"
                              "<author>ada</author><date>2026-11-18T19:23:59.494070Z</date></commit></entry>"
                              "<entry line-number=\"2\"></entry></target></blame>";
  out = vcs_git_blame(VCS_SVN, blame, 0);
  g_assert_cmpstr(out, ==, "2 1 1 1\nauthor ada\nauthor-time 1795029839\nsummary r2\n\t\n");
  g_free(out);
  out = vcs_git_blame(VCS_SVN, blame, 1);
  g_assert_true(g_str_has_prefix(out, "0000000000000000000000000000000000000000 "));
  g_free(out);
  g_assert_null(vcs_git_blame(VCS_SVN, blame, 2));
  out = vcs_git_blame(VCS_HG,
                      "abc\x1f"
                      "Ada <ada@x>\x1f"
                      "1795029823 -10800\x1f"
                      "First line\n",
                      0);
  g_assert_cmpstr(out, ==, "abc 1 1 1\nauthor Ada\nauthor-time 1795029823\nsummary First line\n\t\n");
  g_free(out);
}

/* Real repositories ------------------------------------------------------------------------------ */

/* status + head of the repository at top, through vcs.c's arguments, in git's format. */
static char *status_of(VcsKind kind, const char *top, GHashTable *staged)
{
  char *status = run(top, vcs_program(kind), vcs_status_args(kind), NULL);
  gsize size = last_length, length;
  char *head = run(top, vcs_program(kind), vcs_head_args(kind), NULL);
  g_assert_nonnull(status);
  char *out = vcs_git_status(kind, status, size, head, staged, FALSE, &length);
  char *joined = records(out, length);
  g_free(out);
  g_free(status);
  g_free(head);
  return joined;
}

static void test_hg_real(void)
{
  if (!have("hg")) {
    g_test_skip("no hg");
    return;
  }
  char *top = g_build_filename(tmp, "hg", NULL), *sub = g_build_filename(top, "d", NULL);
  g_mkdir_with_parents(sub, 0755);
  must(top, "hg", (const char *[]){ "init", NULL });
  g_free(write_at(top, "f.txt", "a\nb\n", -1, 0));
  g_free(write_at(top, "d/m.txt", "m\n", -1, 0));
  char *joined = status_of(VCS_HG, top, NULL);
  g_assert_true(g_str_has_prefix(joined, "# branch.oid (initial)|# branch.head default|"));
  g_assert_nonnull(strstr(joined, "|? d/m.txt|? f.txt|"));
  g_free(joined);

  must(top, "hg", (const char *[]){ "commit", "-A", "-m", "First line\n\nbody", NULL });
  g_free(write_at(top, "f.txt", "a\nB\nc\n", -1, 0));
  must(top, "hg", (const char *[]){ "mv", "d/m.txt", "d/k.txt", NULL });
  GHashTable *staged = g_hash_table_new(g_str_hash, g_str_equal);
  g_hash_table_add(staged, "d/k.txt");
  joined = status_of(VCS_HG, top, staged);
  g_assert_nonnull(strstr(joined, "|1 .M N... 000000 000000 000000 0 0 f.txt|"));
  g_assert_nonnull(strstr(joined, "|2 R. N... 000000 000000 000000 0 0 R100 d/k.txt|d/m.txt|"));
  g_assert_null(strstr(joined, "# branch.oid (initial)"));
  g_free(joined);
  g_hash_table_unref(staged);

  /* The base and the log, as the gutter, the comparisons and the Commits list ask for them. */
  char *base = ask(top, VCS_HG, vcs_cat_args(VCS_HG, "HEAD", "f.txt", NULL));
  g_assert_cmpstr(base, ==, "a\nb\n");
  g_free(base);
  char *id = ask(top, VCS_HG, vcs_id_args(VCS_HG, "HEAD", TRUE, NULL));
  g_assert_cmpuint(strlen(id), ==, 12);
  char *out = ask(top, VCS_HG, vcs_log_args(VCS_HG, 10)), *log = vcs_git_log(VCS_HG, out);
  char **commits = g_strsplit(log, "\x1e", -1);
  g_assert_cmpuint(g_strv_length(commits), ==, 2);
  char **f = g_strsplit(commits[1], "\x1f", 9);
  g_assert_cmpuint(g_strv_length(f), ==, 9);
  g_assert_cmpstr(f[1], ==, id);
  g_assert_cmpstr(f[2], ==, "First line");
  g_assert_cmpstr(f[3], ==, "Ada");
  g_assert_cmpstr(f[4], ==, "ada@x");
  g_assert_true(g_str_has_prefix(f[8], "First line\n\nbody\x1d"));
  char *files = ask(top, VCS_HG, vcs_commit_files_args(VCS_HG, f[0])),
       *named = vcs_git_commit_files(VCS_HG, files, NULL);
  g_assert_cmpstr(named, ==, "A\td/m.txt\nA\tf.txt\n");
  char *patch = ask(top, VCS_HG, vcs_show_args(VCS_HG, f[0])), *show = vcs_git_show(patch);
  g_assert_true(g_str_has_prefix(show, "1\t0\td/m.txt\n2\t0\tf.txt\ndiff --git a/d/m.txt b/d/m.txt"));
  g_free(show);
  g_free(patch);
  g_free(named);
  g_free(files);
  g_strfreev(f);
  g_strfreev(commits);
  g_free(log);
  g_free(out);

  out = ask(top, VCS_HG, vcs_file_log_args(VCS_HG, 10, "f.txt"));
  log = vcs_git_file_log(VCS_HG, out);
  f = g_strsplit(log, "\x1f", 5);
  g_assert_cmpuint(g_strv_length(f), ==, 5);
  g_assert_cmpstr(f[1], ==, id);
  g_assert_cmpstr(f[4], ==, "First line\n");
  g_strfreev(f);
  g_free(log);
  g_free(out);
  out = ask(top, VCS_HG, vcs_messages_args(VCS_HG, 10));
  g_assert_cmpstr(out, ==, "First line\n\nbody\x1e");
  g_free(out);

  /* Blame of the saved file: the changed line is not committed yet. */
  out = ask(top, VCS_HG, vcs_blame_args(VCS_HG, "f.txt"));
  char *line = vcs_git_blame(VCS_HG, out, 0);
  g_assert_true(g_str_has_suffix(line, "author Ada\nauthor-time 0\nsummary First line\n\t\n") ||
                strstr(line, "summary First line\n"));
  g_free(line);
  line = vcs_git_blame(VCS_HG, out, 1);
  g_assert_true(g_str_has_prefix(line, "0000000000000000000000000000000000000000 "));
  g_free(line);
  g_free(out);
  g_free(id);
  g_free(sub);
  g_free(top);
}

static void test_svn_real(void)
{
  if (!have("svn") || !have("svnadmin")) {
    g_test_skip("no svn");
    return;
  }
  char *repo = g_build_filename(tmp, "svnrepo", NULL), *top = g_build_filename(tmp, "svn", NULL);
  char *url = g_strconcat("file://", repo, NULL), *trunk = g_strconcat(url, "/trunk", NULL);
  must(tmp, "svnadmin", (const char *[]){ "create", repo, NULL });
  must(tmp, "svn", (const char *[]){ "mkdir", "-q", "-m", "init", trunk, NULL });
  must(tmp, "svn", (const char *[]){ "checkout", "-q", trunk, top, NULL });
  g_free(write_at(top, "f.txt", "a\nb\n", -1, 0));
  g_free(write_at(top, "d/m.txt", "m\n", -1, 0));
  must(top, "svn", (const char *[]){ "add", "-q", "f.txt", "d", NULL });
  must(top, "svn", (const char *[]){ "commit", "-q", "--username", "ada", "-m", "First line\nbody", NULL });
  must(top, "svn", (const char *[]){ "update", "-q", NULL });
  g_free(write_at(top, "f.txt", "a\nB\nc\n", -1, 0));
  g_free(write_at(top, "new.txt", "n\n", -1, 0));
  must(top, "svn", (const char *[]){ "mv", "-q", "d/m.txt", "d/k.txt", NULL });

  char *joined = status_of(VCS_SVN, top, NULL);
  g_assert_cmpstr(joined, ==,
                  "# branch.oid 2|# branch.head trunk|# branch.upstream server|"
                  "2 .R N... 000000 000000 000000 0 0 R100 d/k.txt|d/m.txt|"
                  "1 .M N... 000000 000000 000000 0 0 f.txt|"
                  "? new.txt|");
  g_free(joined);

  char *info = run(top, "svn", vcs_head_args(VCS_SVN), NULL), *wc = vcs_svn_url(info);
  g_assert_cmpstr(wc, ==, "^/trunk");
  char *base = ask(top, VCS_SVN, vcs_cat_args(VCS_SVN, "HEAD", "f.txt", wc));
  g_assert_cmpstr(base, ==, "a\nb\n");
  g_free(base);
  base = ask(top, VCS_SVN, vcs_cat_args(VCS_SVN, "2", "d/m.txt", wc)); /* moved away in the working copy */
  g_assert_cmpstr(base, ==, "m\n");
  g_free(base);
  char *id = ask(top, VCS_SVN, vcs_id_args(VCS_SVN, "HEAD", TRUE, "f.txt"));
  g_assert_cmpstr(g_strstrip(id), ==, "2");
  g_free(id);

  char *out = ask(top, VCS_SVN, vcs_log_args(VCS_SVN, 10)), *log = vcs_git_log(VCS_SVN, out);
  char **commits = g_strsplit(log, "\x1e", -1);
  g_assert_cmpuint(g_strv_length(commits), ==, 3);
  g_assert_true(g_str_has_prefix(commits[1], "2\x1fr2\x1f"
                                             "First line\x1f"
                                             "ada\x1f"));
  g_strfreev(commits);
  g_free(log);
  g_free(out);
  out = ask(top, VCS_SVN, vcs_commit_files_args(VCS_SVN, "2"));
  char *named = vcs_git_commit_files(VCS_SVN, out, wc);
  g_assert_cmpstr(named, ==, "A\td/m.txt\nA\tf.txt\n");
  g_free(named);
  g_free(out);
  out = ask(top, VCS_SVN, vcs_show_args(VCS_SVN, "2"));
  char *show = vcs_git_show(out);
  g_assert_nonnull(strstr(show, "2\t0\tf.txt\n"));
  g_free(show);
  g_free(out);

  out = ask(top, VCS_SVN, vcs_blame_args(VCS_SVN, "f.txt"));
  char *line = vcs_git_blame(VCS_SVN, out, 0);
  g_assert_true(g_str_has_prefix(line, "2 1 1 1\nauthor ada\n"));
  g_free(line);
  line = vcs_git_blame(VCS_SVN, out, 1);
  g_assert_true(g_str_has_prefix(line, "0000000000000000000000000000000000000000 "));
  g_free(line);
  g_free(out);
  g_free(wc);
  g_free(info);
  g_free(trunk);
  g_free(url);
  g_free(top);
  g_free(repo);
}

int main(int argc, char **argv)
{
  int status;

  g_test_init(&argc, &argv, NULL);
  tmp = g_dir_make_tmp("t-code-vcs-XXXXXX", NULL);
  g_test_add_func("/vcs/find", test_find);
  g_test_add_func("/vcs/hg-status-recorded", test_hg_status_recorded);
  g_test_add_func("/vcs/svn-status-recorded", test_svn_status_recorded);
  g_test_add_func("/vcs/revisions", test_revisions);
  g_test_add_func("/vcs/recorded-logs", test_recorded_logs);
  g_test_add_func("/vcs/hg", test_hg_real);
  g_test_add_func("/vcs/svn", test_svn_real);
  status = g_test_run();
  char *argv_rm[] = { "rm", "-rf", tmp, NULL };
  g_spawn_sync(NULL, argv_rm, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, NULL, NULL);
  g_free(tmp);
  return status;
}
