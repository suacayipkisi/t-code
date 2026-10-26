/*
 * Ctrl+P: open a file by typing part of its name.
 * The file list comes from git when the project is a repository (so .gitignore applies),
 * otherwise from walking the project, skipping hidden folders and build output. It is built
 * on a worker thread; the picker shows the previous list of the same project until then.
 */
#include "popups/quickopen.h"

#include "editor/editor.h"
#include "popups/picker.h"

#include <string.h>

#define MAX_WALK 20000

static GPtrArray *files;  /* paths relative to the project root */
static char *files_root;  /* the root they belong to */
static gboolean listing;  /* a worker is building the list */
static guint list_number; /* drops lists of a project that was left meanwhile */

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

/* PickerQuery: the files that match text, best first (in list order while nothing is typed). */
static void query(App *t, const char *text, guint generation)
{
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

/* PickerChoose: opens the picked file. */
static void choose(App *t, const char *relative)
{
  char *path = g_build_filename(t->project.root, relative, NULL);

  open_file(t, path);
  g_free(path);
}

typedef struct {
  guint number;
  char *root;
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

  if (!from_git(list, l->root))
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

void quick_open_show(App *t)
{
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
    g_task_set_task_data(task, l, (GDestroyNotify)listing_free);
    listing = TRUE;
    g_task_run_in_thread(task, list_files);
    g_object_unref(task);
  }
  picker_open(t, "Go to file by name", query, choose);
}
