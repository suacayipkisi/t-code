/*
 * The Source Control panel's Commits list: click a commit to list its files, click a file to see
 * what that commit changed; the pointer on a commit shows its whole message, author, date, refs and
 * line counts, and its right-click menu opens all that with its diff in a tab ("Open Commit Details").
 */
#include "git/scm/scm.h"

#include "editor/editor.h"
#include "editor/preview.h"
#include "git/compare.h"
#include "git/scm/state.h"
#include "popups/contextmenu.h"
#include "window/palette.h"

#include <string.h>

#define DETAIL_DIFF_LINES 4000 /* a bigger diff is cut there */

static const char *const css =
    ".tcode-commit-tip { padding: 4px 2px; }"
    ".tcode-commit-tip .rule { min-height: 1px; background: var(--tc-border-strong); margin: 4px 0; }"
    ".tcode-commit-tip .ref { padding: 0 7px; border-radius: 9px; font-size: 0.85em;"
    "  border: 1px solid var(--tc-border-strong); color: var(--tc-fg-bright); }"
    ".tcode-commit-tip .hash { font-family: 'Cascadia Code', monospace; color: var(--tc-fg-dim); font-size: 0.9em; }";

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

static CommitInfo *commit_info_copy(const CommitInfo *c)
{
  CommitInfo *copy = g_new(CommitInfo, 1);

  *copy = (CommitInfo){ g_strdup(c->hash),   g_strdup(c->short_hash), g_strdup(c->subject), g_strdup(c->message),
                        g_strdup(c->author), g_strdup(c->email),      g_strdup(c->ago),     g_strdup(c->date),
                        g_strdup(c->refs),   g_strdup(c->stat) };
  return copy;
}

/* A commit's files ---------------------------------------------------------------------------- */

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
      GtkWidget *box, *row = scm_file_row(path, code, &box);
      gtk_widget_add_css_class(row, "file-row");
      gtk_box_append(GTK_BOX(box), scm_code_label(code));
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
  g_set_str(&scm.open_commit, commit);
  /* A merge commit lists what it brought to its first parent (by default git shows a combined
   * diff, which has no file list). */
  const char *args[] = { "-c", "core.quotePath=false",       "show",      "--name-status",
                         "-M", "--diff-merges=first-parent", "--format=", commit,
                         NULL };
  scm_run_git(args, NULL, TRUE, on_commit_files, g_strdup(commit));
}

/* The tooltip ------------------------------------------------------------------------------------ */

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

/* Appends a wrapped label with markup to box. */
static void tip_label(GtkWidget *box, const char *markup, const char *css_class)
{
  GtkWidget *label = gtk_label_new(NULL);

  gtk_label_set_markup(GTK_LABEL(label), markup);
  gtk_label_set_xalign(GTK_LABEL(label), 0);
  gtk_label_set_wrap(GTK_LABEL(label), TRUE);
  gtk_label_set_max_width_chars(GTK_LABEL(label), 72);
  if (css_class)
    gtk_widget_add_css_class(label, css_class);
  gtk_box_append(GTK_BOX(box), label);
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
  tip_label(box, markup, NULL);
  g_free(markup);
  markup = g_markup_escape_text(c->message, -1);
  tip_label(box, markup, NULL);
  g_free(markup);
  if (*c->stat) {
    GtkWidget *rule = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(rule, "rule");
    gtk_box_append(GTK_BOX(box), rule);
    markup = stat_markup(c->stat);
    tip_label(box, markup, NULL);
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
  tip_label(box, markup, "hash");
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

/* What git show gave for a commit: "--numstat" lines, then the patch. Written as Markdown into the
 * temp folder and opened as a preview. */
static void on_commit_details(gboolean ok, const char *out, const char *err, gpointer data)
{
  CommitInfo *c = data;
  App *t = scm.app;

  if (!ok) {
    scm_set_note(*err ? err : "git show failed", TRUE);
    commit_info_free(c);
    return;
  }

  GString *md = g_string_new(NULL);
  const char *patch = strstr(out, "diff --git ");
  char *numstat = g_strndup(out, patch ? (gsize)(patch - out) : strlen(out));
  char **refs = ref_names(c->refs);
  g_string_append_printf(md, "# %s\n\n**%s** &lt;%s&gt; · %s (%s)\n\n`%s`", c->subject, c->author, c->email, c->date,
                         c->ago, c->hash);
  for (int i = 0; refs[i] && *refs[i]; i++)
    g_string_append_printf(md, " · `%s`", refs[i]);
  g_strfreev(refs);
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
  g_free(numstat);
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
    scm_set_note(error->message, TRUE);
  g_clear_error(&error);
  g_free(path);
  g_free(name);
  g_string_free(md, TRUE);
  commit_info_free(c);
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

  scm_run_git(args, NULL, TRUE, on_commit_details, commit_info_copy(c));
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

/* The right-click menu ------------------------------------------------------------------------- */

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
  const char *path = g_object_get_data(G_OBJECT(data), "file-new");
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

/* The list ------------------------------------------------------------------------------------- */

static GtkWidget *commit_row(CommitInfo *c)
{
  GtkWidget *row = gtk_list_box_row_new();
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *subject = gtk_label_new(c->subject);
  char *info = g_strdup_printf("%s · %s", c->short_hash, c->ago);
  GtkWidget *detail = gtk_label_new(info);

  g_free(info);
  gtk_label_set_ellipsize(GTK_LABEL(subject), PANGO_ELLIPSIZE_END);
  gtk_label_set_xalign(GTK_LABEL(subject), 0);
  gtk_widget_set_hexpand(subject, TRUE);
  gtk_widget_add_css_class(detail, "dim");
  gtk_box_append(GTK_BOX(box), subject);
  gtk_box_append(GTK_BOX(box), detail);
  gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), scm_open_button(row, box));
  gtk_widget_set_has_tooltip(row, TRUE);
  g_signal_connect(row, "query-tooltip", G_CALLBACK(on_commit_tooltip), NULL);
  g_object_set_data_full(G_OBJECT(row), "commit", g_strdup(c->hash), g_free);
  g_object_set_data_full(G_OBJECT(row), "commit-info", c, (GDestroyNotify)commit_info_free);
  return row;
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
  scm_clear_list(scm.commits_list);
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
      gtk_list_box_append(scm.commits_list, commit_row(c));
    }
    g_strfreev(f);
  }
  g_strfreev(records);
}

void commits_load(void)
{
  const char *args[] = { "log",
                         "-n",
                         "100",
                         "--shortstat",
                         "--date=format:%Y-%m-%d %H:%M", /* the same in every locale */
                         "--format=%x1e%H%x1f%h%x1f%s%x1f%an%x1f%ae%x1f%ar%x1f%ad%x1f%D%x1f%B%x1d",
                         NULL };

  scm_run_git(args, NULL, TRUE, on_log, g_strdup(scm.head));
}

void commits_attach(void)
{
  palette_add_css(css);
  gtk_list_box_set_activate_on_single_click(scm.commits_list, TRUE);
  g_signal_connect(scm.commits_list, "row-activated", G_CALLBACK(on_commit_activated), NULL);
  context_menu_attach(GTK_WIDGET(scm.commits_list), on_commits_menu, NULL);
}
