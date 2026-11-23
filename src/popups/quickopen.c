/*
 * Ctrl+P and the title bar's search: open a file by typing part of its name, or with '#' first a
 * command (window/commands.c's palette).
 * The file list comes from git, hg or svn when the project is a repository (so their ignore rules
 * apply), otherwise from walking the project, skipping hidden folders and build output. It is built
 * on a worker thread; the picker shows the previous list of the same project until then.
 */
#include "popups/quickopen.h"

#include "editor/editor.h"
#include "git/vcs.h"
#include "popups/picker.h"
#include "window/commands.h"

#include <string.h>

#define MAX_WALK 20000

static GPtrArray *files;     /* paths relative to the project root */
static char *files_root;     /* the root they belong to */
static gboolean listing;     /* a worker is building the list */
static guint list_number;    /* drops lists of a project that was left meanwhile */
static gboolean list_wanted; /* opened, and the files not listed yet: the first file query lists them */
static gboolean commands;    /* the rows shown are commands (the text starts with '#') */

static void list_files_start(App *t);

static gboolean from_git(GPtrArray *files, const char *root)
{
  char *argv[] = { "git", "-C", (char *)root, "ls-files", "-z", "-co", "--exclude-standard", NULL };
  char *out = NULL;
  int status;

  if (!g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, &out, NULL, &status,
                    NULL) ||
      !g_spawn_check_wait_status(status, NULL)) {
    g_free(out);
    return FALSE;
  }
  for (const char *p = out; *p; p += strlen(p) + 1)
    g_ptr_array_add(files, g_strdup(p));
  g_free(out);
  return TRUE;
}

/* Without git: the files under dir (relative names start with prefix), hidden folders, build
 * output and linked folders left out, up to MAX_WALK files. */
static void walk(GPtrArray *files, const char *dir, const char *prefix)
{
  GDir *d = g_dir_open(dir, 0, NULL);
  const char *name;

  if (!d)
    return;
  while ((name = g_dir_read_name(d)) && files->len < MAX_WALK) {
    char *path = g_build_filename(dir, name, NULL);
    char *rel = *prefix ? g_strconcat(prefix, "/", name, NULL) : g_strdup(name);
    if (g_file_test(path, G_FILE_TEST_IS_DIR)) {
      /* Linked folders are left out: a link to a parent ("ln -s .. loop") never ends. */
      if (name[0] != '.' && strcmp(name, "build") != 0 && strcmp(name, "node_modules") != 0 &&
          !g_file_test(path, G_FILE_TEST_IS_SYMLINK))
        walk(files, path, rel);
      g_free(rel);
    } else {
      g_ptr_array_add(files, rel);
    }
    g_free(path);
  }
  g_dir_close(d);
}

/* hg / svn: their files under root; svn's folders are left out, its unknown folders walked. */
static gboolean from_vcs(GPtrArray *files, const char *root, VcsKind kind)
{
  const char *const *args = vcs_files_args(kind);
  GSubprocessLauncher *launcher =
      g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE);
  GPtrArray *argv = g_ptr_array_new();
  GSubprocess *proc = NULL;
  GBytes *out = NULL;

  g_ptr_array_add(argv, (gpointer)vcs_program(kind));
  for (int i = 0; args && args[i]; i++)
    g_ptr_array_add(argv, (gpointer)args[i]);
  g_ptr_array_add(argv, NULL);
  g_subprocess_launcher_set_cwd(launcher, root);
  g_subprocess_launcher_setenv(launcher, "HGPLAIN", "1", TRUE);
  if (args)
    proc = g_subprocess_launcher_spawnv(launcher, (const char *const *)argv->pdata, NULL);
  gboolean ok =
      proc && g_subprocess_communicate(proc, NULL, NULL, &out, NULL, NULL) && g_subprocess_get_successful(proc);
  g_ptr_array_free(argv, TRUE);
  g_object_unref(launcher);
  g_clear_object(&proc);
  if (!ok) {
    if (out)
      g_bytes_unref(out);
    return FALSE;
  }
  gsize size;
  const char *data = g_bytes_get_data(out, &size);
  GPtrArray *tracked = g_ptr_array_new_with_free_func(g_free), *unknown = g_ptr_array_new_with_free_func(g_free);
  vcs_files(kind, data, size, tracked, unknown);
  for (guint i = 0; i < tracked->len && files->len < MAX_WALK; i++) {
    char *path = g_build_filename(root, tracked->pdata[i], NULL);
    if (kind == VCS_HG || g_file_test(path, G_FILE_TEST_IS_REGULAR))
      g_ptr_array_add(files, g_strdup(tracked->pdata[i]));
    g_free(path);
  }
  for (guint i = 0; i < unknown->len && files->len < MAX_WALK; i++) {
    char *path = g_build_filename(root, unknown->pdata[i], NULL);
    if (g_file_test(path, G_FILE_TEST_IS_DIR))
      walk(files, path, unknown->pdata[i]);
    else
      g_ptr_array_add(files, g_strdup(unknown->pdata[i]));
    g_free(path);
  }
  g_ptr_array_unref(tracked);
  g_ptr_array_unref(unknown);
  g_bytes_unref(out);
  return TRUE;
}

typedef struct {
  const char *path;
  int score;
} Match;

/* Best match first; equal ones by path. */
static int by_score(gconstpointer a, gconstpointer b)
{
  const Match *x = a, *y = b;

  return x->score != y->score ? y->score - x->score : strcmp(x->path, y->path);
}

/* PickerQuery: the files that match text, best first (in list order while nothing is typed); with
 * '#' first the commands that match the rest. */
static void query(App *t, const char *text, guint generation)
{
  commands = text[0] == '#';
  if (commands) {
    command_palette_query(t, text + 1 + strspn(text + 1, " "), generation);
    return;
  }
  if (list_wanted)
    list_files_start(t);

  GArray *matches = g_array_new(FALSE, FALSE, sizeof(Match));

  for (guint i = 0; i < files->len; i++) {
    const char *path = g_ptr_array_index(files, i);
    int s = *text ? fuzzy_score(text, path) : 0;
    if (s >= 0) {
      Match m = { path, s };
      g_array_append_val(matches, m);
    }
  }
  if (*text)
    g_array_sort(matches, by_score);
  for (guint i = 0; i < matches->len; i++) {
    const char *path = g_array_index(matches, Match, i).path;
    char *name = g_path_get_basename(path);
    char *dir = g_path_get_dirname(path);
    picker_add(t, generation, name, strcmp(dir, ".") == 0 ? "" : dir, path);
    g_free(dir);
    g_free(name);
  }
  if (matches->len == 0)
    picker_message(t, generation, listing ? "Listing files..." : "No matching files");
  g_array_free(matches, TRUE);
}

/* PickerChoose: opens the picked file, or runs the picked command. */
static void choose(App *t, const char *relative)
{
  if (commands) {
    command_palette_choose(t, relative);
    return;
  }
  char *path = g_build_filename(t->project.root, relative, NULL);

  open_file(t, path);
  g_free(path);
}

typedef struct {
  guint number;
  char *root;
  VcsKind kind; /* the project's repository: git, hg or svn lists the files */
} Listing;

/* Frees a Listing. */
static void listing_free(Listing *l)
{
  g_free(l->root);
  g_free(l);
}

/* On a worker thread: the project's files from git, or else from walking the folder. */
static void list_files(GTask *task, gpointer source G_GNUC_UNUSED, gpointer data, GCancellable *cancel G_GNUC_UNUSED)
{
  Listing *l = data;
  GPtrArray *list = g_ptr_array_new_with_free_func(g_free);

  gboolean listed =
      l->kind == VCS_HG || l->kind == VCS_SVN ? from_vcs(list, l->root, l->kind) : from_git(list, l->root);
  if (!listed)
    walk(list, l->root, "");
  g_task_return_pointer(task, list, (GDestroyNotify)g_ptr_array_unref);
}

/* Back on the main thread: the new list replaces the old one, unless another project was opened
 * meanwhile. */
static void on_listed(GObject *source G_GNUC_UNUSED, GAsyncResult *result, gpointer data)
{
  App *t = data;
  GTask *task = G_TASK(result);
  Listing *l = g_task_get_task_data(task);
  GPtrArray *list = g_task_propagate_pointer(task, NULL);

  if (l->number != list_number) {
    g_ptr_array_unref(list);
    return;
  }
  listing = FALSE;
  g_ptr_array_unref(files);
  files = list;
  picker_requery(t, query);
}

/* Lists the project's files on a worker thread (once per opening), unless one is listing them. */
static void list_files_start(App *t)
{
  list_wanted = FALSE;
  if (!files || g_strcmp0(files_root, t->project.root) != 0) {
    g_clear_pointer(&files, g_ptr_array_unref);
    files = g_ptr_array_new_with_free_func(g_free);
    g_set_str(&files_root, t->project.root);
    list_number++;
    listing = FALSE;
  }
  if (!listing) {
    GTask *task = g_task_new(NULL, NULL, on_listed, t);
    Listing *l = g_new(Listing, 1);
    l->number = list_number;
    l->root = g_strdup(t->project.root);
    l->kind = t->project.repo_kind;
    g_task_set_task_data(task, l, (GDestroyNotify)listing_free);
    listing = TRUE;
    g_task_run_in_thread(task, list_files);
    g_object_unref(task);
  }
}

void quick_open_show(App *t)
{
  quick_open_show_text(t, "");
}

void quick_open_show_text(App *t, const char *text)
{
  list_wanted = TRUE;
  picker_open_text(t, "Go to file by name (# for commands)", text, query, choose);
}
