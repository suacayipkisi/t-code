/*
 * The Source Control panel's commit message (see state.h): Up and Down go through the earlier
 * commits' messages, as in a shell; Commit (Ctrl+Enter) saves the editors first, and with nothing
 * staged stages everything.
 */
#include "git/scm/scm.h"
#include "git/scm/state.h"

#include "editor/editor.h"
#include "editor/tabs.h"

/* Commit and push ---------------------------------------------------------------------------- */

static char *message_text(void)
{
  GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(scm.message));
  GtkTextIter start, end;

  gtk_text_buffer_get_bounds(buffer, &start, &end);
  return g_strstrip(gtk_text_buffer_get_text(buffer, &start, &end, FALSE));
}

char *scm_message(void)
{
  return scm.app ? message_text() : g_strdup("");
}

void scm_set_message(const char *message)
{
  if (scm.app)
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(scm.message)), message ? message : "", -1);
}

/* Message history ---------------------------------------------------------------------------- */

/* HEAD moved or another project opened: the history is asked for again at the next Up. */
void scm_forget_history(void)
{
  g_clear_pointer(&scm.history, g_ptr_array_unref);
  scm.history_asked = FALSE;
  scm.pending = 0;
}

/* Shows the step-th message from the one shown (+1 older, -1 newer); past the newest, what was
 * typed comes back. The cursor goes to the start going up and to the end going down, so the next
 * press goes on. */
static void step_history(int step);

/* data: the HEAD it was asked for (a later HEAD asks again). */
static void on_history(gboolean ok G_GNUC_UNUSED, const char *out, const char *err G_GNUC_UNUSED, gpointer data)
{
  int pending = scm.pending;
  gboolean stale = g_strcmp0(data, scm.head) != 0 || !scm.history_asked || scm.history;

  g_free(data);
  if (stale)
    return;
  scm.history = g_ptr_array_new_with_free_func(g_free);
  if (out) {
    char **records = g_strsplit(out, "\x1e", -1);
    for (int i = 0; records[i]; i++) {
      char *message = g_strstrip(records[i]);
      if (*message)
        g_ptr_array_add(scm.history, g_strdup(message));
    }
    g_strfreev(records);
  }
  scm.pending = 0;
  if (pending)
    step_history(pending);
}

static void step_history(int step)
{
  GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(scm.message));
  GtkTextIter start, end;
  int next = scm.recalled + step;

  if (!scm.history) {
    const char *args[] = { "log", "-n", "200", "--format=%B%x1e", NULL };
    scm.pending = step;
    if (!scm.history_asked && scm.head) {
      scm.history_asked = TRUE;
      scm_run_git_owned(args, NULL, TRUE, on_history, g_strdup(scm.head), g_free);
    }
    return;
  }
  if (next < -1 || next >= (int)scm.history->len)
    return;
  gtk_text_buffer_get_bounds(buffer, &start, &end);
  if (scm.recalled < 0) {
    g_free(scm.draft);
    scm.draft = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
  }
  scm.recalling = TRUE;
  gtk_text_buffer_set_text(buffer, next < 0 ? scm.draft : g_ptr_array_index(scm.history, next), -1);
  scm.recalling = FALSE;
  scm.recalled = next;
  gtk_text_buffer_get_bounds(buffer, &start, &end);
  gtk_text_buffer_place_cursor(buffer, step > 0 ? &start : &end);
}

/* git commit finished: the message box empties, or the error shows. */
static void on_committed(gboolean ok, const char *out, const char *err, gpointer data G_GNUC_UNUSED)
{
  if (!ok) {
    /* "nothing to commit" comes on stdout */
    char *said = g_strstrip(g_strdup(out));
    scm_set_note(*err ? err : *said ? said : "The commit failed", TRUE);
    g_free(said);
    return;
  }
  gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(scm.message)), "", -1);
  scm_set_note("Committed", FALSE);
}

/* With nothing staged, the commit takes every change. */
static void on_staged_all(gboolean ok, const char *out G_GNUC_UNUSED, const char *err, gpointer data)
{
  const char *args[] = { "commit", "-q", "-F", "-", NULL };

  if (!ok) {
    scm_set_note(*err ? err : "Could not stage the changes", TRUE);
    g_free(data);
    return;
  }
  scm_run_git(args, data, FALSE, on_committed, NULL);
  g_free(data);
}

/* The editors are saved: scm_commit (with nothing staged, everything is staged first). */
static void on_saved_for_commit(App *t, gpointer data)
{
  char *message = data;

  scm.committing = FALSE;
  if (!t->closing) {
    if (scm.staged->len == 0) {
      const char *add[] = { "add", "-A", NULL };
      scm_run_git(add, NULL, FALSE, on_staged_all, g_strdup(message));
    } else {
      const char *args[] = { "commit", "-q", "-F", "-", NULL };
      scm_run_git(args, message, FALSE, on_committed, NULL);
    }
  }
  g_free(message);
}

/* The Commit button (or Ctrl+Enter in the message): checks there is a message and something to
 * scm_commit, saves the editors, then commits. */
void scm_commit(void)
{
  char *message = message_text();

  if (scm.busy || scm.committing) {
    g_free(message);
    return;
  }
  if (!*message) {
    scm_set_note("Type a commit message first", TRUE);
    gtk_widget_grab_focus(scm.message);
  } else if (scm.staged->len == 0 && scm.changes->len == 0 && !any_unsaved(scm.app)) {
    scm_set_note("Nothing to commit", TRUE);
  } else {
    /* git commits what is on disk: unsaved edits are saved first. */
    scm_set_note(NULL, FALSE);
    scm.committing = TRUE;
    save_all(scm.app, on_saved_for_commit, message);
    return;
  }
  g_free(message);
}

void scm_on_message_changed(GtkTextBuffer *buffer, gpointer data G_GNUC_UNUSED)
{
  gtk_widget_set_visible(scm.placeholder, gtk_text_buffer_get_char_count(buffer) == 0);
  if (!scm.recalling) { /* typed (or a recalled message edited): that is what was typed now */
    scm.recalled = -1;
    g_clear_pointer(&scm.draft, g_free);
  }
}

/* Ctrl+Enter in the commit message commits. */
gboolean scm_on_message_key(GtkEventControllerKey *controller G_GNUC_UNUSED, guint key, guint code G_GNUC_UNUSED,
                            GdkModifierType state, gpointer data G_GNUC_UNUSED)
{
  if ((key == GDK_KEY_Return || key == GDK_KEY_KP_Enter) && (state & GDK_CONTROL_MASK)) {
    scm_commit();
    return TRUE;
  }
  if ((key == GDK_KEY_Up || key == GDK_KEY_Down) && !(state & (GDK_CONTROL_MASK | GDK_SHIFT_MASK | GDK_ALT_MASK))) {
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(scm.message));
    GtkTextIter cursor;

    gtk_text_buffer_get_iter_at_mark(buffer, &cursor, gtk_text_buffer_get_insert(buffer));
    if (key == GDK_KEY_Up && gtk_text_iter_get_line(&cursor) == 0) {
      step_history(1);
      return TRUE;
    }
    if (key == GDK_KEY_Down && scm.recalled >= 0 &&
        gtk_text_iter_get_line(&cursor) == gtk_text_buffer_get_line_count(buffer) - 1) {
      step_history(-1);
      return TRUE;
    }
  }
  return FALSE;
}
