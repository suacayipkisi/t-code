/* A folder's entries as a list model, GIO-only: see folder.h. */
#include "files/folder.h"

#define ATTRIBUTES G_FILE_ATTRIBUTE_STANDARD_NAME "," G_FILE_ATTRIBUTE_STANDARD_TYPE

typedef struct {
  GListStore *store; /* not a reference: the folder lives as long as the store */
  GFile *dir;
  gboolean watch, loaded;
  GFileMonitor *monitor;
  GCancellable *reading;
  guint timer;
} Folder;

static Folder *folder_of(GListModel *list)
{
  return g_object_get_data(G_OBJECT(list), "folder");
}

static void folder_free(Folder *f)
{
  g_cancellable_cancel(f->reading); /* its answer is dropped (see on_read) */
  g_clear_object(&f->reading);
  g_clear_handle_id(&f->timer, g_source_remove);
  if (f->monitor)
    g_file_monitor_cancel(f->monitor);
  g_clear_object(&f->monitor);
  g_object_unref(f->dir);
  g_free(f);
}

/* Reading ------------------------------------------------------------------------------------ */

/* In a thread: the folder's entries, each with its GFile. */
static void read_entries(GTask *task, gpointer source G_GNUC_UNUSED, gpointer data, GCancellable *cancel)
{
  GFile *dir = data;
  GFileEnumerator *entries = g_file_enumerate_children(dir, ATTRIBUTES, G_FILE_QUERY_INFO_NONE, cancel, NULL);
  GPtrArray *infos = g_ptr_array_new_with_free_func(g_object_unref);
  GFileInfo *info;

  while (entries && (info = g_file_enumerator_next_file(entries, cancel, NULL))) {
    GFile *child = g_file_enumerator_get_child(entries, info);
    g_file_info_set_attribute_object(info, "standard::file", G_OBJECT(child));
    g_object_unref(child);
    g_ptr_array_add(infos, info);
  }
  if (entries)
    g_object_unref(entries);
  g_task_return_pointer(task, infos, (GDestroyNotify)g_ptr_array_unref);
}

static gboolean same_entry(GFileInfo *a, GFileInfo *b)
{
  return g_file_info_get_file_type(a) == g_file_info_get_file_type(b) &&
         g_strcmp0(g_file_info_get_name(a), g_file_info_get_name(b)) == 0;
}

/* Back in the main thread: entries that are gone leave the list, new ones join it; the rest stay. */
static void on_read(GObject *source G_GNUC_UNUSED, GAsyncResult *result, gpointer data)
{
  GTask *task = G_TASK(result);
  Folder *f = data;
  GPtrArray *infos;
  GHashTable *now;

  if (g_cancellable_is_cancelled(g_task_get_cancellable(task)))
    return; /* f may be freed */
  infos = g_task_propagate_pointer(task, NULL);
  g_clear_object(&f->reading);
  now = g_hash_table_new(g_str_hash, g_str_equal); /* name -> info */
  for (guint i = 0; i < infos->len; i++)
    g_hash_table_insert(now, (gpointer)g_file_info_get_name(infos->pdata[i]), infos->pdata[i]);

  for (guint i = g_list_model_get_n_items(G_LIST_MODEL(f->store)); i-- > 0;) {
    GFileInfo *old = g_list_model_get_item(G_LIST_MODEL(f->store), i);
    GFileInfo *same = g_hash_table_lookup(now, g_file_info_get_name(old));
    if (same && same_entry(old, same))
      g_hash_table_remove(now, g_file_info_get_name(old)); /* stays */
    else
      g_list_store_remove(f->store, i);
    g_object_unref(old);
  }
  GPtrArray *added = g_ptr_array_new();
  GHashTableIter iter;
  gpointer info;
  g_hash_table_iter_init(&iter, now);
  while (g_hash_table_iter_next(&iter, NULL, &info))
    g_ptr_array_add(added, info);
  g_list_store_splice(f->store, g_list_model_get_n_items(G_LIST_MODEL(f->store)), 0, added->pdata, added->len);
  g_ptr_array_unref(added);
  g_hash_table_unref(now);
  g_ptr_array_unref(infos);
}

static void read_folder(Folder *f)
{
  GTask *task;

  g_cancellable_cancel(f->reading); /* an older reading: this one is newer */
  g_clear_object(&f->reading);
  f->reading = g_cancellable_new();
  task = g_task_new(NULL, f->reading, on_read, f);
  g_task_set_task_data(task, g_object_ref(f->dir), g_object_unref);
  g_task_run_in_thread(task, read_entries);
  g_object_unref(task);
}

/* Watching ----------------------------------------------------------------------------------- */

static gboolean on_timer(gpointer data)
{
  Folder *f = data;

  f->timer = 0;
  read_folder(f);
  return G_SOURCE_REMOVE;
}

/* An entry came, went or was renamed: the folder is read again once the burst is over. A file
 * written in place changes no entry. */
static void on_changed(GFileMonitor *monitor G_GNUC_UNUSED, GFile *file G_GNUC_UNUSED, GFile *other G_GNUC_UNUSED,
                       GFileMonitorEvent event, gpointer data)
{
  Folder *f = data;

  if (event != G_FILE_MONITOR_EVENT_CREATED && event != G_FILE_MONITOR_EVENT_DELETED &&
      event != G_FILE_MONITOR_EVENT_MOVED_IN && event != G_FILE_MONITOR_EVENT_MOVED_OUT &&
      event != G_FILE_MONITOR_EVENT_RENAMED)
    return;
  g_clear_handle_id(&f->timer, g_source_remove);
  f->timer = g_timeout_add(FOLDER_DELAY_MS, on_timer, f);
}

/* The model ---------------------------------------------------------------------------------- */

GListModel *folder_new(const char *path, gboolean watch)
{
  GListStore *store = g_list_store_new(G_TYPE_FILE_INFO);
  Folder *f = g_new0(Folder, 1);

  f->store = store;
  f->dir = g_file_new_for_path(path);
  f->watch = watch;
  g_object_set_data_full(G_OBJECT(store), "folder", f, (GDestroyNotify)folder_free);
  return G_LIST_MODEL(store);
}

void folder_load(GListModel *list)
{
  Folder *f = folder_of(list);

  if (f->loaded)
    return;
  f->loaded = TRUE;
  if (f->watch) {
    f->monitor = g_file_monitor_directory(f->dir, G_FILE_MONITOR_WATCH_MOVES, NULL, NULL);
    if (f->monitor)
      g_signal_connect(f->monitor, "changed", G_CALLBACK(on_changed), f);
  }
  read_folder(f);
}

gboolean folder_loaded(GListModel *list)
{
  return folder_of(list)->loaded;
}

gboolean folder_watched(GListModel *list)
{
  return folder_of(list)->monitor != NULL;
}

const char *folder_path(GListModel *list)
{
  return g_file_peek_path(folder_of(list)->dir);
}
