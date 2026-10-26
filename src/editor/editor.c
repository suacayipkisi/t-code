/*
 * Editors (see editor.h): opening a file into a GtkSourceView tab, loading it in the background,
 * saving (Ctrl+S, autosave, before Build and Commit), loading it again when it changes on disk, and
 * closing, which asks about unsaved changes. The page itself is built in editorview.c.
 */
#include "editor/editor.h"

#include "editor/breadcrumbs.h"
#include "editor/breakpoints.h"
#include "editor/detect.h"
#include "editor/diffgutter.h"
#include "editor/editorview.h"
#include "editor/folding.h"
#include "editor/multicursor.h"
#include "editor/preview.h"
#include "editor/tabs.h"
#include "editor/recovery.h"
#include "git/compare.h"
#include "git/gitstatus.h"
#include "lsp/client.h"
#include "lsp/completion.h"
#include "lsp/diagnostics.h"
#include "project/session.h"
#include "window/statusbar.h"
#include "ipc/socket.h"

#define AUTOSAVE_MS 1000  /* after the last change */
#define RELOAD_MS 200     /* a burst of changes on disk is looked at once */
#define SNIFF_BYTES 65536 /* the start of a file, enough to tell its language and indentation */

/* A message box over the tab's window ("Could not save main.c"). */
static void show_error(Tab *tab, const char *message, const char *detail)
{
  GtkAlertDialog *dialog = gtk_alert_dialog_new("%s", message);

  gtk_alert_dialog_set_detail(dialog, detail);
  gtk_alert_dialog_show(dialog, tab_window(tab));
  g_object_unref(dialog);
}

/* The file's identity on disk now (changes with every write), or NULL when it is gone. */
static char *disk_etag(const char *path)
{
  GFile *file = g_file_new_for_path(path);
  GFileInfo *info = g_file_query_info(file, G_FILE_ATTRIBUTE_ETAG_VALUE, G_FILE_QUERY_INFO_NONE, NULL, NULL);
  char *etag = info ? g_strdup(g_file_info_get_etag(info)) : NULL;

  g_clear_object(&info);
  g_object_unref(file);
  return etag;
}

/* The save conflict ---------------------------------------------------------------------------- */

/* The file changed on disk while the editor has changes of its own: neither side is thrown away
 * until the user says which (Overwrite / Reload / Compare). */
static void hide_conflict(Tab *tab)
{
  if (tab->view->conflict)
    gtk_widget_set_visible(tab->view->conflict, FALSE);
}

static void on_overwrite(GtkButton *button G_GNUC_UNUSED, Tab *tab)
{
  hide_conflict(tab);
  tab->view->overwrite = TRUE;
  save_editor(tab);
}

static void on_reload(GtkButton *button G_GNUC_UNUSED, Tab *tab)
{
  hide_conflict(tab);
  editor_reload(tab);
}

static void on_compare(GtkButton *button G_GNUC_UNUSED, Tab *tab)
{
  compare_with_disk(tab->app, tab->path);
}

static void add_button(GtkWidget *bar, const char *label, GCallback clicked, Tab *tab)
{
  GtkWidget *button = gtk_button_new_with_label(label);

  g_signal_connect(button, "clicked", clicked, tab);
  gtk_box_append(GTK_BOX(bar), button);
}

/* Shows the bar over the editor (made the first time). */
static void show_conflict(Tab *tab)
{
  EditorView *v = tab->view;

  if (!v->conflict) {
    char *name = g_path_get_basename(tab->path);
    char *text = g_strdup_printf("%s changed on disk. Your changes are not saved.", name);
    GtkWidget *label = gtk_label_new(text);
    v->conflict = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class(v->conflict, "tcode-conflict");
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_box_append(GTK_BOX(v->conflict), label);
    add_button(v->conflict, "Compare", G_CALLBACK(on_compare), tab);
    add_button(v->conflict, "Reload", G_CALLBACK(on_reload), tab);
    add_button(v->conflict, "Overwrite", G_CALLBACK(on_overwrite), tab);
    gtk_box_prepend(GTK_BOX(v->column), v->conflict);
    g_free(text);
    g_free(name);
  }
  gtk_widget_set_visible(v->conflict, TRUE);
}

gboolean editor_in_conflict(Tab *tab)
{
  return tab->kind == TAB_EDITOR && tab->view->conflict && gtk_widget_get_visible(tab->view->conflict);
}

/* Following the cursor and the changes ------------------------------------------------------- */

/* The cursor's line and column for the status bar (the column with tabs expanded). */
static void update_position(Tab *tab)
{
  GtkTextBuffer *buffer = GTK_TEXT_BUFFER(tab->buffer);
  GtkTextIter iter;

  gtk_text_buffer_get_iter_at_mark(buffer, &iter, gtk_text_buffer_get_insert(buffer));
  tab->cursor.line = gtk_text_iter_get_line(&iter) + 1;
  tab->cursor.column = (int)gtk_source_view_get_visual_column(tab->source, &iter) + 1;
  if (tab == current_tab(tab->app))
    status_show_position(tab->app);
}

static void on_mark_set(GtkTextBuffer *buffer, const GtkTextIter *where G_GNUC_UNUSED, GtkTextMark *mark, Tab *tab)
{
  if (mark == gtk_text_buffer_get_insert(buffer) && !tab->view->loading)
    update_position(tab);
}

/* Saved or changed: the "●" in the tab's label. An edited temporary tab is kept. */
static void on_modified_changed(GtkTextBuffer *buffer, Tab *tab)
{
  tab->modified = gtk_text_buffer_get_modified(buffer);
  if (tab->modified && !tab->view->loading) /* typed, not the file coming in */
    tab->temporary = FALSE;
  if (!tab->modified) /* saved, undone to the saved text, or loaded again */
    recovery_forget(tab);
  update_tab_label(tab);
}

static gboolean autosave_now(gpointer data)
{
  Tab *tab = data;

  tab->view->autosave_id = 0;
  if (tab->modified)
    save_editor(tab);
  return G_SOURCE_REMOVE;
}

/* Typed, pasted, undone: with autosave on, the file is saved a second after the last change. */
static void on_changed(GtkTextBuffer *buffer G_GNUC_UNUSED, Tab *tab)
{
  EditorView *v = tab->view;

  v->changes++;
  if (v->loading)
    return;
  update_position(tab);
  if (!v->readonly && !editor_in_conflict(tab))
    recovery_schedule_snapshot(tab);
  if (!tab->app->settings.autosave || v->readonly || editor_in_conflict(tab)) /* the user decides first */
    return;
  g_clear_handle_id(&v->autosave_id, g_source_remove);
  v->autosave_id = g_timeout_add(AUTOSAVE_MS, autosave_now, tab);
}

/* Scrolls the cursor into view (editor_view_reveal_cursor). A view not on the screen yet (a hidden
 * tab, a new one) has no size to scroll in: it scrolls once it is shown and laid out. */
static gboolean scroll_to_cursor(gpointer data)
{
  Tab *tab = data;

  tab->view->scroll_id = 0;
  if (!gtk_widget_get_mapped(GTK_WIDGET(tab->source))) {
    tab->view->scroll_pending = TRUE;
    return G_SOURCE_REMOVE;
  }
  if (gtk_widget_get_width(GTK_WIDGET(tab->source)) <= 1) { /* shown but not laid out yet: in a moment */
    tab->view->scroll_id = g_timeout_add(50, scroll_to_cursor, tab);
    return G_SOURCE_REMOVE;
  }
  tab->view->scroll_pending = FALSE;
  editor_view_reveal_cursor(tab);
  return G_SOURCE_REMOVE;
}

/* Shown: the language server gets the file now (clangd keeps hundreds of KB to MB per open
 * file: a restored session's hidden tabs would cost it all), and a scroll that waited for it comes,
 * after the layout (idles come after it). */
static void on_view_mapped(GtkWidget *view G_GNUC_UNUSED, Tab *tab)
{
  if (tab->view->lsp_pending) {
    tab->view->lsp_pending = FALSE;
    lsp_open(tab);
  } else {
    lsp_touch(tab);
  }
  if (tab->view->scroll_pending && !tab->view->scroll_id)
    tab->view->scroll_id = g_idle_add(scroll_to_cursor, tab);
}

/* Saving ------------------------------------------------------------------------------------- */

typedef struct {
  Tab *tab; /* gone when cancel is cancelled */
  GCancellable *cancel;
  GArray *waiters; /* SaveWaiter */
  guint changes;   /* the buffer's change count when its text was taken */
  char *text;
} SaveJob;

static void start_save(Tab *tab, GArray *waiters);

/* Tells a save's callers how it went, and frees the list. */
static void call_waiters(GArray *waiters, gboolean ok)
{
  for (guint i = 0; i < waiters->len; i++) {
    SaveWaiter *w = &g_array_index(waiters, SaveWaiter, i);
    w->func(w->data, ok);
  }
  g_array_free(waiters, TRUE);
}

/* The file was written (or not). Changes typed meanwhile keep the tab unsaved, for the next save. */
static void on_saved(GObject *source, GAsyncResult *result, gpointer data)
{
  SaveJob *job = data;
  GError *error = NULL;
  char *etag = NULL;
  gboolean ok = g_file_replace_contents_finish(G_FILE(source), result, &etag, &error);

  if (!g_cancellable_is_cancelled(job->cancel)) {
    Tab *tab = job->tab;
    EditorView *v = tab->view;
    v->saving = FALSE;
    if (ok) {
      hide_conflict(tab);
      g_free(v->etag);
      v->etag = g_steal_pointer(&etag);
      v->save_failed = FALSE;
      if (v->changes == job->changes)
        gtk_text_buffer_set_modified(GTK_TEXT_BUFFER(tab->buffer), FALSE);
      refresh_branch(tab->app); /* the work tree changed */
      lsp_saved(tab);
      socket_did_save(tab->app, tab->path);
    } else if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_WRONG_ETAG)) { /* changed on disk meanwhile */
      show_conflict(tab);
    } else if (!v->save_failed) { /* autosave would say it every second */
      char *name = g_path_get_basename(tab->path), *message = g_strdup_printf("Could not save %s", name);
      v->save_failed = TRUE;
      show_error(tab, message, error->message);
      g_free(message);
      g_free(name);
    }
    if (v->save_again) {
      GArray *next = v->next_waiters;
      v->save_again = FALSE;
      v->next_waiters = g_array_new(FALSE, FALSE, sizeof(SaveWaiter));
      start_save(tab, next);
    }
  }
  call_waiters(job->waiters, ok);
  g_clear_error(&error);
  g_free(etag);
  g_free(job->text);
  g_object_unref(job->cancel);
  g_free(job);
}

/* Writes the buffer's text to the file (replacing it whole, so a crash never leaves half a file). A
 * save that comes while one is in flight follows it. */
static void start_save(Tab *tab, GArray *waiters)
{
  EditorView *v = tab->view;
  GtkTextBuffer *buffer = GTK_TEXT_BUFFER(tab->buffer);
  SaveJob *job;
  GtkTextIter start, end;
  GFile *file;

  if (v->saving || v->loading) {
    g_array_append_vals(v->next_waiters, waiters->data, waiters->len);
    g_array_free(waiters, TRUE);
    v->save_again = TRUE;
    return;
  }
  gtk_text_buffer_get_bounds(buffer, &start, &end);
  job = g_new0(SaveJob, 1);
  job->tab = tab;
  job->cancel = g_object_ref(v->cancel);
  job->waiters = waiters;
  job->changes = v->changes;
  job->text = gtk_text_buffer_get_text(buffer, &start, &end, TRUE); /* the file exactly (see editorview.c) */
  v->saving = TRUE;
  file = g_file_new_for_path(tab->path);
  /* Only over the file as it was loaded or last saved: one changed on disk since then (git, another
   * program) fails with G_IO_ERROR_WRONG_ETAG, and the user chooses. Not cancelled when the tab
   * goes: a save that started finishes. */
  g_file_replace_contents_async(file, job->text, strlen(job->text), v->overwrite ? NULL : v->etag, FALSE,
                                G_FILE_CREATE_NONE, NULL, on_saved, job);
  v->overwrite = FALSE;
  g_object_unref(file);
}

/* Saves an editor and tells func(data, ok) when it is done. */
static void save_then(Tab *tab, EditorSavedFunc func, gpointer data)
{
  GArray *waiters = g_array_new(FALSE, FALSE, sizeof(SaveWaiter));

  if (func) {
    SaveWaiter w = { func, data };
    g_array_append_val(waiters, w);
  }
  if (tab->view->autosave_id) { /* this save covers it */
    g_source_remove(tab->view->autosave_id);
    tab->view->autosave_id = 0;
  }
  start_save(tab, waiters);
}

void save_editor(Tab *tab)
{
  if (tab->kind == TAB_EDITOR && !tab->view->readonly)
    save_then(tab, NULL, NULL);
}

typedef struct {
  App *app;
  SavedFunc then;
  gpointer data;
  int left; /* saves not done yet, plus one while they are being started */
} PendingSave;

/* One of save_all()'s saves is done; after the last one, its callback. */
static void one_saved(gpointer data, gboolean ok G_GNUC_UNUSED)
{
  PendingSave *pending = data;

  if (--pending->left > 0)
    return;
  pending->then(pending->app, pending->data);
  g_free(pending);
}

void save_all(App *t, SavedFunc then, gpointer data)
{
  PendingSave *pending = g_new0(PendingSave, 1);
  int n = tab_count(t);

  pending->app = t;
  pending->then = then;
  pending->data = data;
  pending->left = 1;
  for (int i = 0; i < n; i++) {
    Tab *tab = tab_nth(t, i);
    if (tab->kind == TAB_EDITOR && tab->modified && !tab->view->readonly) {
      pending->left++;
      save_then(tab, one_saved, pending);
    }
  }
  one_saved(pending, TRUE);
}

/* Loading ------------------------------------------------------------------------------------ */

static void watch_file(Tab *tab);

typedef struct {
  Tab *tab; /* gone when cancel is cancelled */
  GCancellable *cancel;
} LoadJob;

/* The text from the buffer's start, up to max bytes (about). */
static char *buffer_start(GtkTextBuffer *buffer, int max)
{
  GtkTextIter start, end;

  gtk_text_buffer_get_start_iter(buffer, &start);
  gtk_text_buffer_get_iter_at_offset(buffer, &end, max);
  return gtk_text_buffer_get_slice(buffer, &start, &end, TRUE);
}

/* The file is in the buffer: its language and indentation, the cursor where it was asked for. */
static void on_loaded(GObject *source, GAsyncResult *result, gpointer data)
{
  LoadJob *job = data;
  GError *error = NULL;
  gboolean ok = gtk_source_file_loader_load_finish(GTK_SOURCE_FILE_LOADER(source), result, &error);

  if (!g_cancellable_is_cancelled(job->cancel)) {
    Tab *tab = job->tab;
    EditorView *v = tab->view;
    GtkTextBuffer *buffer = GTK_TEXT_BUFFER(tab->buffer);
    char *text = buffer_start(buffer, SNIFF_BYTES);

    v->loading = FALSE;
    hide_conflict(tab); /* the file as it is on disk now */
    gtk_text_view_set_editable(GTK_TEXT_VIEW(tab->source), !v->readonly);
    if (!ok && !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND)) { /* not found: a new file */
      char *name = g_path_get_basename(tab->path), *message = g_strdup_printf("Could not open %s", name);
      show_error(tab, message, error->message);
      g_free(message);
      g_free(name);
    }
    gtk_source_buffer_set_language(tab->buffer, editor_language(tab->path, text));
    v->own_indent = FALSE;
    if (tab->app->settings.detect_indent) {
      v->own_tabs = !tab->app->settings.tabs_to_spaces;
      v->own_width = tab->app->settings.tab_size;
      v->own_indent = detect_indent(text, -1, &v->own_tabs, &v->own_width);
    }
    v->config = (IndentStyle){ -1, 0 };
    /* Old versions are in a temporary folder, away from the project's files. */
    v->config_from = v->readonly ? NULL : indent_from_config(tab->path, &v->config);
    editor_view_apply_settings(tab);
    gtk_text_buffer_set_modified(buffer, FALSE);
    g_free(v->etag);
    v->etag = disk_etag(tab->path);
    editor_goto(tab, MAX(v->goto_line, 1), MAX(v->goto_column, 1));
    watch_file(tab);
    diff_gutter_loaded(tab);
    breakpoints_loaded(tab);
    breadcrumbs_update(tab);
    if (!lsp_wants(tab) || is_markdown(tab->path)) /* prose: its words, beside the server's links */
      completion_use_words(tab);
    if (lsp_wants(tab)) {
      if (gtk_widget_get_mapped(GTK_WIDGET(tab->source)) || lsp_has(tab)) /* loaded again */
        lsp_open(tab);
      else
        v->lsp_pending = TRUE; /* a hidden tab: once it is shown */
    }
    if (v->save_again) { /* asked for while it loaded: the buffer is what the file holds now */
      GArray *waiting = v->next_waiters;
      v->save_again = FALSE;
      v->next_waiters = g_array_new(FALSE, FALSE, sizeof(SaveWaiter));
      call_waiters(waiting, TRUE);
    }
    if (ok || g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
      socket_did_open(tab->app, tab->path);
    g_free(text);
    recovery_check_and_prompt(tab);
  }
  g_clear_error(&error);
  g_object_unref(job->cancel);
  g_free(job);
  g_object_unref(source);
}

/* Reads the file into the buffer in the background; the cursor then goes to line and column. */
static void load(Tab *tab, int line, int column)
{
  EditorView *v = tab->view;
  GFile *file = g_file_new_for_path(tab->path);
  GtkSourceFileLoader *loader;
  LoadJob *job = g_new0(LoadJob, 1);

  gtk_source_file_set_location(v->file, file);
  loader = gtk_source_file_loader_new(tab->buffer, v->file);
  job->tab = tab;
  job->cancel = g_object_ref(v->cancel);
  v->goto_line = line;
  v->goto_column = column;
  v->loading = TRUE;
  folding_reset(tab); /* the folded lines may not be there any more */
  multicursor_clear(tab);
  /* The loader replaces the whole text: the old problems' marks would end up around all of it. The
   * server sends the new ones once it has the file again. */
  diagnostics_clear(tab);
  tab->problems.errors = tab->problems.warnings = 0;
  editor_view_show_problems(tab);
  update_status(tab->app);
  gtk_text_view_set_editable(GTK_TEXT_VIEW(tab->source), FALSE); /* nothing typed is lost meanwhile */
  gtk_source_file_loader_load_async(loader, G_PRIORITY_DEFAULT, v->cancel, NULL, NULL, NULL, on_loaded, job);
  g_object_unref(file);
}

void editor_reload(Tab *tab)
{
  if (tab->kind == TAB_EDITOR && !tab->view->loading)
    load(tab, tab->cursor.line, tab->cursor.column);
}

/* A while after the file changed on disk: unless the change was our own save, or the editor has
 * unsaved changes (they would be lost), it is loaded again. */
static gboolean check_disk(gpointer data)
{
  Tab *tab = data;
  EditorView *v = tab->view;
  char *etag = disk_etag(tab->path);

  v->reload_id = 0;
  if (etag && g_strcmp0(etag, v->etag) != 0 && !v->saving && !v->loading) {
    if (!tab->modified)
      load(tab, tab->cursor.line, tab->cursor.column);
    else if (!v->readonly) /* told now, not only when it is saved */
      show_conflict(tab);
  }
  g_free(etag);
  return G_SOURCE_REMOVE;
}

static void on_file_changed(GFileMonitor *monitor G_GNUC_UNUSED, GFile *file G_GNUC_UNUSED, GFile *other G_GNUC_UNUSED,
                            GFileMonitorEvent event G_GNUC_UNUSED, Tab *tab)
{
  if (!tab->view->reload_id)
    tab->view->reload_id = g_timeout_add(RELOAD_MS, check_disk, tab);
}

/* Watches the file for changes made outside the editor (git, other programs). */
static void watch_file(Tab *tab)
{
  EditorView *v = tab->view;
  GFile *file;

  if (v->monitor)
    return;
  file = g_file_new_for_path(tab->path);
  v->monitor = g_file_monitor_file(file, G_FILE_MONITOR_WATCH_MOVES, NULL, NULL);
  if (v->monitor)
    g_signal_connect(v->monitor, "changed", G_CALLBACK(on_file_changed), tab);
  g_object_unref(file);
}

/* Opening and closing ------------------------------------------------------------------------ */

Tab *open_editor(App *t, const char *path, int line, int column, gboolean readonly)
{
  Tab *tab = add_tab(t, TAB_EDITOR, editor_page(), path);

  editor_view_init(tab, readonly);
  g_signal_connect(tab->buffer, "modified-changed", G_CALLBACK(on_modified_changed), tab);
  g_signal_connect(tab->buffer, "changed", G_CALLBACK(on_changed), tab);
  g_signal_connect(tab->buffer, "mark-set", G_CALLBACK(on_mark_set), tab);
  g_signal_connect(tab->source, "map", G_CALLBACK(on_view_mapped), tab);
  load(tab, line, column);
  focus_current_tab(t);
  return tab;
}

void editor_goto(Tab *tab, int line, int column)
{
  GtkTextBuffer *buffer = GTK_TEXT_BUFFER(tab->buffer);
  GtkTextIter iter;

  if (tab->view->loading) { /* once it is loaded */
    tab->view->goto_line = line;
    tab->view->goto_column = column;
    return;
  }
  gtk_text_buffer_get_iter_at_line(buffer, &iter, MAX(line, 1) - 1);
  if (column > 1) {
    int length = gtk_text_iter_get_chars_in_line(&iter);
    gtk_text_iter_set_line_offset(&iter, MIN(column - 1, MAX(length - 1, 0)));
  }
  gtk_text_buffer_place_cursor(buffer, &iter);
  if (!tab->view->scroll_id) /* once laid out */
    tab->view->scroll_id = g_idle_add(scroll_to_cursor, tab);
  update_position(tab);
}

/* An open file is not opened twice: its tab is shown and its cursor moved. */
void open_file_at(App *t, const char *path, int line, int column)
{
  Tab *existing = find_tab(t, TAB_EDITOR, path);

  if (!existing) {
    open_editor(t, path, line, column, FALSE);
    return;
  }
  select_tab(existing);
  if (line > 0)
    editor_goto(existing, line, column);
}

void open_file(App *t, const char *path)
{
  Tab *existing = find_tab(t, TAB_EDITOR, path);

  if (existing)
    keep_tab(existing); /* opened on purpose: no longer temporary */
  open_file_at(t, path, 0, 0);
}

void open_file_temporary(App *t, const char *path)
{
  int group = t->editors.active;
  Tab *existing = find_tab(t, TAB_EDITOR, path), *old = temporary_tab(t, group), *tab;
  int position = -1;

  if (existing) {
    select_tab(existing);
    return;
  }
  if (old) { /* replaced, in its place */
    position = tab_position(old);
    remove_tab(old);           /* temporary tabs have nothing unsaved: an edit keeps them */
    t->editors.active = group; /* removing it focused another tab, maybe in the other group */
  }
  tab = open_editor(t, path, 0, 0, FALSE);
  tab->temporary = TRUE;
  update_tab_label(tab);
  if (position >= 0)
    set_tab_position(tab, position);
}

void open_to_side(App *t, const char *path)
{
  Tab *existing = find_tab(t, TAB_EDITOR, path);

  set_split(t, TRUE);
  t->editors.active = 1;
  if (existing) {
    move_tab(existing, 1);
    select_tab(existing);
  } else {
    open_file(t, path);
  }
}

/* The tab being closed, for as long as it is there (the user may switch projects meanwhile). */
typedef struct {
  GtkWidget *page;
} Closing;

static void closing_free(Closing *c)
{
  if (c->page)
    g_object_remove_weak_pointer(G_OBJECT(c->page), (gpointer *)&c->page);
  g_free(c);
}

/* Saved for closing: the tab goes (it stays when the save failed). */
static void close_after_save(gpointer data, gboolean ok)
{
  Closing *c = data;

  if (ok && c->page)
    remove_tab(tab_of(c->page));
  closing_free(c);
}

/* The answer to "Save changes?": 0 Cancel, 1 Don't Save, 2 Save. */
static void on_close_answer(GObject *dialog, GAsyncResult *result, gpointer data)
{
  Closing *c = data;
  int answer = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(dialog), result, NULL);

  if (c->page && answer == 2) {
    save_then(tab_of(c->page), close_after_save, c);
    return;
  }
  if (c->page && answer == 1) {
    recovery_forget(tab_of(c->page)); /* the changes are thrown away on purpose */
    remove_tab(tab_of(c->page));
  }
  closing_free(c);
}

void quit_editor(Tab *tab)
{
  if (!tab->modified || tab->view->readonly) {
    remove_tab(tab);
    return;
  }

  char *name = g_path_get_basename(tab->path);
  GtkAlertDialog *dialog = gtk_alert_dialog_new("Save the changes to %s?", name);
  const char *buttons[] = { "Cancel", "Don't Save", "Save", NULL };
  Closing *c = g_new0(Closing, 1);

  c->page = tab->page;
  g_object_add_weak_pointer(G_OBJECT(c->page), (gpointer *)&c->page);
  gtk_alert_dialog_set_detail(dialog, "Your changes are lost if you don't save them.");
  gtk_alert_dialog_set_buttons(dialog, buttons);
  gtk_alert_dialog_set_cancel_button(dialog, 0);
  gtk_alert_dialog_set_default_button(dialog, 2);
  gtk_alert_dialog_choose(dialog, tab_window(tab), NULL, on_close_answer, c);
  g_object_unref(dialog);
  g_free(name);
}

/* Turns code coloring off or on in every open editor; new ones start the same way. */
void toggle_syntax(App *t)
{
  int n = tab_count(t);

  t->settings.plain = !t->settings.plain;
  session_save_settings(t);
  for (int i = 0; i < n; i++)
    if (tab_nth(t, i)->kind == TAB_EDITOR)
      editor_view_apply_settings(tab_nth(t, i));
}
