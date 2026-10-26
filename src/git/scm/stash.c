/*
 * git stash for the Source Control panel (see scm.h): stashing the changes (with a message typed in
 * the picker), and popping, applying or dropping a stash picked from the list. The editors are saved
 * first, as git works on the files on disk; the panel's note says how it went.
 */
#include "git/scm/scm.h"

#include "editor/editor.h"
#include "git/scm/state.h"
#include "popups/picker.h"
#include "window/window.h"

#include <string.h>

static StashAction stash_action; /* what picking a stash does */
static GPtrArray *stashes;       /* "stash@{0}\t2 hours ago\tOn main: message", newest first; NULL while read */

/* git answered a stash command: its own words when it did nothing ("No local changes to save"). */
static void on_stashed(gboolean ok, const char *out, const char *err, gpointer data)
{
  if (!ok) {
    scm_set_note(*err ? err : "git stash failed", TRUE); /* a pop that conflicts: the stash stays */
    show_sidebar_page(scm.app, "scm");
  } else
    scm_set_note(g_str_has_prefix(out, "No local changes") ? "No local changes to stash" : data, FALSE);
}

/* The editors are saved: git stash runs as payload says ("push\tmessage", "push-u\tmessage" with the
 * untracked files, "pop\tstash@{0}", "apply\t...", "drop\t..."). */
static void on_saved_for_stash(App *t G_GNUC_UNUSED, gpointer data)
{
  char *verb = data, *arg = strchr(verb, '\t');
  GPtrArray *args = g_ptr_array_new();
  const char *done = "Stashed";

  *arg++ = '\0';
  g_ptr_array_add(args, "stash");
  if (g_str_has_prefix(verb, "push")) {
    g_ptr_array_add(args, "push");
    if (strcmp(verb, "push-u") == 0)
      g_ptr_array_add(args, "--include-untracked");
    if (*arg) { /* else git's own message: "WIP on main: ..." */
      g_ptr_array_add(args, "-m");
      g_ptr_array_add(args, arg);
    }
  } else {
    g_ptr_array_add(args, verb);
    g_ptr_array_add(args, arg);
    done = strcmp(verb, "pop") == 0     ? "Popped the stash"
           : strcmp(verb, "apply") == 0 ? "Applied the stash"
                                        : "Dropped the stash";
  }
  g_ptr_array_add(args, NULL);
  scm_set_note(NULL, FALSE);
  scm_run_git((const char *const *)args->pdata, NULL, FALSE, on_stashed, (gpointer)done);
  g_ptr_array_free(args, TRUE);
  g_free(verb);
}

/* "Git: Stash…": the typed text is the stash's message. */
static void stash_query(App *t, const char *text, guint generation)
{
  char *message = g_strstrip(g_strdup(text)), *payload = g_strconcat("push\t", message, NULL);
  char *untracked = g_strconcat("push-u\t", message, NULL);
  const char *detail = *message ? message : "type a message, or none";

  picker_add(t, generation, "Stash", detail, payload);
  picker_add(t, generation, "Stash, untracked files too", detail, untracked);
  g_free(untracked);
  g_free(payload);
  g_free(message);
}

static void stash_choose(App *t, const char *payload)
{
  save_all(t, on_saved_for_stash, g_strdup(payload));
}

void scm_stash(void)
{
  if (!scm.app || !scm.app->project.git_top)
    return;
  picker_open(scm.app, "Message for the stash (optional)", stash_query, stash_choose);
}

/* The stashes ------------------------------------------------------------------------------- */

static void stashes_query(App *t, const char *text, guint generation)
{
  if (!stashes) {
    picker_message(t, generation, "Reading the stashes…");
    return;
  }
  if (stashes->len == 0)
    picker_message(t, generation, "No stashes");
  for (guint i = 0; i < stashes->len; i++) {
    char **f = g_strsplit(stashes->pdata[i], "\t", 3);
    if (g_strv_length(f) == 3 && (!*text || fuzzy_score(text, f[2]) >= 0)) {
      char *detail = g_strdup_printf("%s · %s", f[0], f[1]);
      picker_add(t, generation, f[2], detail, f[0]);
      g_free(detail);
    }
    g_strfreev(f);
  }
}

/* "git stash list" answered. */
static void on_stashes(gboolean ok, const char *out, const char *err G_GNUC_UNUSED, gpointer data G_GNUC_UNUSED)
{
  char **lines = g_strsplit(ok ? out : "", "\n", -1);

  if (stashes)
    g_ptr_array_unref(stashes);
  stashes = g_ptr_array_new_with_free_func(g_free);
  for (int i = 0; lines[i]; i++)
    if (*lines[i])
      g_ptr_array_add(stashes, g_strdup(lines[i]));
  g_strfreev(lines);
  picker_requery(scm.app, stashes_query);
}

static void on_drop_answer(GObject *source, GAsyncResult *result, gpointer data)
{
  char *payload = data;

  if (gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(source), result, NULL) == 1)
    save_all(scm.app, on_saved_for_stash, payload);
  else
    g_free(payload);
}

static void stashes_choose(App *t, const char *ref)
{
  static const char *const verbs[] = { "pop", "apply", "drop" };
  char *payload = g_strconcat(verbs[stash_action], "\t", ref, NULL);

  if (stash_action != STASH_DROP) {
    save_all(t, on_saved_for_stash, payload);
    return;
  }
  GtkAlertDialog *dialog = gtk_alert_dialog_new("Drop %s?", ref);
  const char *buttons[] = { "Cancel", "Drop", NULL };
  gtk_alert_dialog_set_detail(dialog, "Its changes are lost.");
  gtk_alert_dialog_set_buttons(dialog, buttons);
  gtk_alert_dialog_set_cancel_button(dialog, 0);
  gtk_alert_dialog_set_default_button(dialog, 0);
  gtk_alert_dialog_choose(dialog, t->window, NULL, on_drop_answer, payload);
  g_object_unref(dialog);
}

void scm_pick_stash(StashAction action)
{
  static const char *const prompts[] = { "Pick a stash to pop", "Pick a stash to apply", "Pick a stash to drop" };
  const char *args[] = { "stash", "list", "--format=%gd%x09%cr%x09%gs", NULL };

  if (!scm.app || !scm.app->project.git_top)
    return;
  stash_action = action;
  g_clear_pointer(&stashes, g_ptr_array_unref);
  scm_run_git(args, NULL, TRUE, on_stashes, NULL);
  picker_open(scm.app, prompts[action], stashes_query, stashes_choose);
}
