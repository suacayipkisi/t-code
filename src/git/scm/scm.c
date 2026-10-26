/*
 * The Source Control panel, a tab next to the file manager (Ctrl+Shift+G):
 *   a commit message and Commit / Pull / Push buttons (message.c, branches.c)
 *   Staged Changes and Changes: click a file to see its changes side by side; + stages, − unstages,
 *     ↶ discards (after asking) (changes.c, staging.c)
 *   Commits (commits.c)
 * This file holds its state, builds it and fills it from the git status that gitstatus.c runs
 * anyway (scm_update); git commands run in the background (gitrun.c), and the git directory monitor
 * in gitstatus.c refreshes everything afterwards.
 */
#include "git/scm/scm.h"

#include "git/gitstatus.h"
#include "git/scm/state.h"
#include "popups/contextmenu.h"
#include "window/palette.h"

#include <string.h>

ScmState scm = { .recalled = -1 };

static const char *const css =
    ".tcode-scm { background: var(--tc-panel); }"
    ".tcode-scm textview, .tcode-scm textview text { background: var(--tc-input); color: var(--tc-fg); }"
    ".tcode-scm .message { padding: 4px; background: var(--tc-input); }"
    ".tcode-scm .message { border: 1px solid var(--tc-border-strong); margin: 0 10px; }"
    ".tcode-scm .message:focus-within { border-color: var(--tc-accent); }"
    ".tcode-scm .placeholder { color: var(--tc-fg-faint); margin: 4px 6px; }"
    ".tcode-scm .actions { margin: 6px 10px; }"
    ".tcode-scm .actions button { min-height: 24px; padding: 0 10px; }"
    ".tcode-scm .primary { background: var(--tc-accent); color: var(--tc-accent-fg); }"
    ".tcode-scm .primary:hover { background: var(--tc-accent-hover); }"
    ".tcode-scm .note { color: var(--tc-fg-dim); font-size: 0.85em; margin: 0 10px 4px; }"
    ".tcode-scm .note.error { color: var(--tc-error); }"
    ".tcode-scm .section { font-size: 0.8em; font-weight: bold; color: var(--tc-fg); padding: 2px 10px 2px 2px; }"
    ".tcode-scm .section .chevron { -gtk-icon-size: 12px; margin-right: 4px; color: var(--tc-fg); }"
    ".tcode-scm list { background: transparent; }"
    ".tcode-scm row { padding: 0 6px 0 18px; margin: 0 4px; min-height: 22px; border-radius: 6px; color: var(--tc-fg); }"
    ".tcode-scm row:hover { background: var(--tc-hover); }"
    ".tcode-scm row:selected { background: var(--tc-selection); }"
    ".tcode-scm row .dim { color: var(--tc-fg-faint); font-size: 0.9em; }"
    ".tcode-scm button.tool { min-height: 0; min-width: 0; padding: 0 5px; opacity: 0; }"
    ".tcode-scm row:hover button.tool, .tcode-scm .section:hover button.tool { opacity: 1; }"
    ".tcode-scm button.open { min-height: 0; padding: 0; background: none; box-shadow: none; }"
    ".tcode-scm .code { font-family: 'Cascadia Code', monospace; font-weight: bold; min-width: 14px; }"
    ".tcode-scm .code-M { color: var(--tc-git-modified); } .tcode-scm .code-A, .tcode-scm .code-U { color: var(--tc-git-untracked); }"
    ".tcode-scm .code-D { color: var(--tc-git-deleted); } .tcode-scm .code-R, .tcode-scm .code-C { color: var(--tc-git-untracked); }"
    ".tcode-scm .code-x { color: var(--tc-git-conflict); }"
    ".tcode-scm .file-row { padding-left: 34px; }";

/* Frees one entry of scm.staged / scm.changes. */
static void change_free(Change *c)
{
  g_free(c->path);
  g_free(c->old_path);
  g_free(c);
}

/* The line under the buttons: the last result ("Committed") or error (in red); NULL hides it. */
void scm_set_note(const char *text, gboolean error)
{
  gtk_label_set_text(scm.note, text ? text : "");
  gtk_widget_set_visible(GTK_WIDGET(scm.note), text && *text);
  if (error)
    gtk_widget_add_css_class(GTK_WIDGET(scm.note), "error");
  else
    gtk_widget_remove_css_class(GTK_WIDGET(scm.note), "error");
}

/* Status ------------------------------------------------------------------------------------- */

static void add_change(GPtrArray *list, const char *path, const char *old_path, char code)
{
  Change *c = g_new0(Change, 1);

  c->path = g_strdup(path);
  c->old_path = g_strdup(old_path);
  c->code = code;
  g_ptr_array_add(list, c);
}

/* Sorts changes by path. */
static int compare_changes(gconstpointer a, gconstpointer b)
{
  return g_strcmp0((*(Change **)a)->path, (*(Change **)b)->path);
}

/* The buttons' labels: "Publish Branch" / "Push ↑2 ↓1", and "Commit All" when nothing is staged. */
static void update_buttons(void)
{
  char *text;

  if (!scm.upstream)
    text = g_strdup("Publish Branch");
  else if (scm.behind)
    text = g_strdup_printf("Push ↑%d ↓%d", scm.ahead, scm.behind);
  else if (scm.ahead)
    text = g_strdup_printf("Push ↑%d", scm.ahead);
  else
    text = g_strdup("Push");
  gtk_button_set_label(GTK_BUTTON(scm.push), text);
  g_free(text);
  gtk_button_set_label(GTK_BUTTON(scm.commit),
                       scm.staged->len == 0 && scm.changes->len > 0 ? "✓ Commit All" : "✓ Commit");
}

/* Takes the output of "git status --porcelain=v2 --branch -z --untracked-files=all" (NULL: not a
 * repository, or git failed). */
static void show_changes(App *t);

void scm_update(App *t, const char *out, gsize size)
{
  char *head = NULL;

  if (!scm.app || t->closing)
    return;
  g_ptr_array_set_size(scm.staged, 0);
  g_ptr_array_set_size(scm.changes, 0);
  g_clear_pointer(&scm.upstream, g_free);
  scm.ahead = scm.behind = 0;
  scm.initial = FALSE;

  for (const char *p = out, *end = out + size; out && p < end;) {
    const char *nul = memchr(p, '\0', end - p);
    gsize len = (nul ? nul : end) - p;
    char *record = g_strndup(p, len);
    p += len + 1;

    if (g_str_has_prefix(record, "# branch.oid ")) {
      head = g_strdup(record + 13);
      scm.initial = strcmp(head, "(initial)") == 0;
    } else if (g_str_has_prefix(record, "# branch.upstream ")) {
      scm.upstream = g_strdup(record + 18);
    } else if (g_str_has_prefix(record, "# branch.ab ")) {
      sscanf(record + 12, "+%d -%d", &scm.ahead, &scm.behind);
    } else if (record[0] == '?' && len > 2) {
      add_change(scm.changes, record + 2, NULL, 'U');
    } else if ((record[0] == '1' || record[0] == '2' || record[0] == 'u') && len > 4) {
      /* "1 XY sub mH mI mW hH hI path", "2 ... Xscore path\0orig", "u XY sub m1 m2 m3 mW h1 h2 h3 path" */
      int skip = record[0] == '1' ? 8 : record[0] == '2' ? 9 : 10;
      char *path = record;
      for (int i = 0; i < skip && path; i++)
        path = strchr(path, ' ') ? strchr(path, ' ') + 1 : NULL;
      char *orig = NULL;
      if (record[0] == '2' && p < end) {
        const char *onul = memchr(p, '\0', end - p);
        gsize olen = (onul ? onul : end) - p;
        orig = g_strndup(p, olen);
        p += olen + 1;
      }
      if (path && record[0] == 'u') {
        add_change(scm.changes, path, NULL, '!');
      } else if (path) {
        char x = record[2], y = record[3];
        if (x != '.')
          add_change(scm.staged, path, orig, x == 'T' ? 'M' : x);
        if (y != '.')
          add_change(scm.changes, path, NULL, y == 'T' ? 'M' : y);
      }
      g_free(orig);
    }
    g_free(record);
  }
  g_ptr_array_sort(scm.staged, compare_changes);
  g_ptr_array_sort(scm.changes, compare_changes);
  show_changes(t);
  gtk_widget_set_sensitive(scm.commit, out != NULL);
  gtk_widget_set_sensitive(scm.push, out != NULL && !scm.initial);
  gtk_widget_set_sensitive(scm.pull, out != NULL && scm.upstream != NULL);
  if (!out)
    scm_set_note(t->project.git_top ? NULL : "This folder is not in a git repository.", FALSE);

  /* Commits are listed again after a scm_commit, a checkout or another project; outside a
     * repository (no HEAD) and before the first commit there are none. */
  if (!head || scm.initial || g_strcmp0(head, scm.head) != 0) {
    g_free(scm.head);
    scm.head = head;
    scm_forget_history(); /* a commit of its own or a checkout: other earlier messages */
    scm.commits_stale = FALSE;
    if (head && !scm.initial && !scm_commits_shown())
      scm.commits_stale = TRUE; /* folded: no git log until it opens */
    else if (head && !scm.initial)
      commits_load();
    else
      gtk_list_box_remove_all(scm.commits_list);
  } else {
    g_free(head);
  }
}

/* Fills the Staged Changes and Changes lists, their counts and the tab's badge, and the buttons
 * from scm.staged / scm.changes. */
static void show_changes(App *t)
{
  char *title;

  scm_fill_changes(scm.staged_list, scm.staged, TRUE);
  scm_fill_changes(scm.changes_list, scm.changes, FALSE);
  gtk_widget_set_visible(scm.staged_box, scm.staged->len > 0);
  title = g_strdup_printf("CHANGES  %u", scm.changes->len);
  gtk_label_set_text(scm.changes_title, title);
  g_free(title);
  title = g_strdup_printf("STAGED CHANGES  %u", scm.staged->len);
  gtk_label_set_text(scm.staged_title, title);
  g_free(title);
  update_buttons();
  /* The tab shows how many files changed. */
  title = scm.staged->len + scm.changes->len ? g_strdup_printf("SOURCE CONTROL  %u", scm.staged->len + scm.changes->len)
                                             : g_strdup("SOURCE CONTROL");
  gtk_stack_page_set_title(gtk_stack_get_page(t->dock.scm_stack, t->scm), title);
  g_free(title);
}

/* Building the panel ------------------------------------------------------------------------- */

/* A section opens or folds; Commits lists what it missed while folded. */
static void set_section_open(GtkWidget *box, gboolean open)
{
  GtkWidget *head = gtk_widget_get_first_child(box), *list = gtk_widget_get_next_sibling(head);

  gtk_widget_set_visible(list, open);
  gtk_image_set_from_icon_name(GTK_IMAGE(gtk_widget_get_first_child(head)),
                               open ? "pan-down-symbolic" : "pan-end-symbolic");
  if (open && box == scm.commits_box && scm.commits_stale) {
    scm.commits_stale = FALSE;
    commits_load();
  }
}

/* A click on a section's heading (not on its buttons) opens or folds it. */
static void on_section_click(GtkGestureClick *click, int n G_GNUC_UNUSED, double x G_GNUC_UNUSED,
                             double y G_GNUC_UNUSED, GtkWidget *box)
{
  GtkWidget *list = gtk_widget_get_next_sibling(gtk_widget_get_first_child(box));

  gtk_gesture_set_state(GTK_GESTURE(click), GTK_EVENT_SEQUENCE_CLAIMED);
  set_section_open(box, !gtk_widget_get_visible(list));
}

gboolean scm_commits_shown(void)
{
  return !scm.commits_box ||
         gtk_widget_get_visible(gtk_widget_get_next_sibling(gtk_widget_get_first_child(scm.commits_box)));
}

void scm_show_commits(gboolean shown)
{
  if (scm.commits_box)
    set_section_open(scm.commits_box, shown);
}

/* A section heading (a fold arrow, the title, buttons on its right); the list goes below it in the
 * returned box. */
static GtkWidget *section(GtkLabel **title, const char *text, GtkListBox **list, GtkWidget *buttons[], int n)
{
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *label = gtk_label_new(text);
  GtkWidget *chevron = gtk_image_new_from_icon_name("pan-down-symbolic");
  GtkGesture *click = gtk_gesture_click_new();

  gtk_widget_add_css_class(head, "section");
  gtk_widget_add_css_class(chevron, "chevron");
  gtk_box_append(GTK_BOX(head), chevron);
  g_signal_connect(click, "released", G_CALLBACK(on_section_click), box);
  gtk_widget_add_controller(head, GTK_EVENT_CONTROLLER(click));
  gtk_label_set_xalign(GTK_LABEL(label), 0);
  gtk_widget_set_hexpand(label, TRUE);
  gtk_box_append(GTK_BOX(head), label);
  for (int i = 0; i < n; i++)
    gtk_box_append(GTK_BOX(head), buttons[i]);
  gtk_box_append(GTK_BOX(box), head);
  *list = GTK_LIST_BOX(gtk_list_box_new());
  gtk_list_box_set_selection_mode(*list, GTK_SELECTION_NONE);
  gtk_box_append(GTK_BOX(box), GTK_WIDGET(*list));
  if (title)
    *title = GTK_LABEL(label);
  return box;
}

GtkWidget *scm_panel(App *t)
{
  GtkWidget *panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  GtkWidget *scroll = gtk_scrolled_window_new();
  GtkWidget *overlay = gtk_overlay_new();
  GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  GtkEventController *keys = gtk_event_controller_key_new();
  GtkWidget *unstage_all_button = scm_icon_button("−", "Unstage All");
  GtkWidget *stage_all_button = scm_icon_button("+", "Stage All");
  GtkWidget *refresh = scm_icon_button("⟳", "Refresh");
  GtkWidget *staged_buttons[] = { unstage_all_button };
  GtkWidget *changes_buttons[] = { stage_all_button };
  GtkWidget *commits_buttons[] = { refresh };
  GtkTextBuffer *buffer;

  palette_add_css(css);

  scm.app = t;
  g_queue_init(&scm.waiting);
  scm.staged = g_ptr_array_new_with_free_func((GDestroyNotify)change_free);
  scm.changes = g_ptr_array_new_with_free_func((GDestroyNotify)change_free);
  gtk_widget_add_css_class(panel, "tcode-scm");

  gtk_widget_set_margin_top(content, 8);

  /* The commit message, with a hint while it is empty. */
  scm.message = gtk_text_view_new();
  gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(scm.message), GTK_WRAP_WORD_CHAR);
  gtk_text_view_set_left_margin(GTK_TEXT_VIEW(scm.message), 6);
  gtk_text_view_set_top_margin(GTK_TEXT_VIEW(scm.message), 4);
  gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(scm.message), 4);
  gtk_widget_set_size_request(scm.message, -1, 56);
  buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(scm.message));
  g_signal_connect(buffer, "changed", G_CALLBACK(scm_on_message_changed), NULL);
  g_signal_connect(keys, "key-pressed", G_CALLBACK(scm_on_message_key), NULL);
  gtk_widget_add_controller(scm.message, keys);
  scm.placeholder = gtk_label_new("Message (Ctrl+Enter to scm_commit)");
  gtk_widget_add_css_class(scm.placeholder, "placeholder");
  gtk_widget_set_halign(scm.placeholder, GTK_ALIGN_START);
  gtk_widget_set_valign(scm.placeholder, GTK_ALIGN_START);
  gtk_widget_set_can_target(scm.placeholder, FALSE);
  gtk_overlay_set_child(GTK_OVERLAY(overlay), scm.message);
  gtk_overlay_add_overlay(GTK_OVERLAY(overlay), scm.placeholder);
  gtk_widget_add_css_class(overlay, "message");
  gtk_widget_set_overflow(overlay, GTK_OVERFLOW_HIDDEN); /* the text view inside follows its rounded corners */
  gtk_box_append(GTK_BOX(content), overlay);

  scm.commit = gtk_button_new_with_label("✓ Commit");
  gtk_widget_add_css_class(scm.commit, "primary");
  gtk_widget_set_hexpand(scm.commit, TRUE);
  gtk_widget_set_tooltip_text(scm.commit, "Commit the staged changes (all changes when none are staged)");
  g_signal_connect_swapped(scm.commit, "clicked", G_CALLBACK(scm_commit), NULL);
  scm.push = gtk_button_new_with_label("Push");
  gtk_widget_set_tooltip_text(scm.push, "Push the commits to the remote");
  g_signal_connect_swapped(scm.push, "clicked", G_CALLBACK(scm_push), NULL);
  scm.pull = gtk_button_new_with_label("Pull");
  gtk_widget_set_tooltip_text(scm.pull, "Pull the remote's commits (git pull: merge or rebase as you set it up)");
  g_signal_connect_swapped(scm.pull, "clicked", G_CALLBACK(scm_pull), NULL);
  gtk_widget_add_css_class(actions, "actions");
  gtk_box_append(GTK_BOX(actions), scm.commit);
  gtk_box_append(GTK_BOX(actions), scm.pull);
  gtk_box_append(GTK_BOX(actions), scm.push);
  gtk_box_append(GTK_BOX(content), actions);

  scm.note = GTK_LABEL(gtk_label_new(NULL));
  gtk_widget_add_css_class(GTK_WIDGET(scm.note), "note");
  gtk_label_set_wrap(scm.note, TRUE);
  gtk_label_set_xalign(scm.note, 0);
  gtk_label_set_selectable(scm.note, TRUE);
  gtk_widget_set_visible(GTK_WIDGET(scm.note), FALSE);
  gtk_box_append(GTK_BOX(content), GTK_WIDGET(scm.note));

  g_signal_connect_swapped(unstage_all_button, "clicked", G_CALLBACK(scm_unstage_all), NULL);
  g_signal_connect_swapped(stage_all_button, "clicked", G_CALLBACK(scm_stage_all), NULL);
  g_signal_connect_swapped(refresh, "clicked", G_CALLBACK(refresh_branch), t);
  scm.staged_box = section(&scm.staged_title, "STAGED CHANGES", &scm.staged_list, staged_buttons, 1);
  scm.changes_box = section(&scm.changes_title, "CHANGES", &scm.changes_list, changes_buttons, 1);
  gtk_box_append(GTK_BOX(content), scm.staged_box);
  gtk_box_append(GTK_BOX(content), scm.changes_box);
  scm.commits_box = section(NULL, "COMMITS", &scm.commits_list, commits_buttons, 1);
  gtk_box_append(GTK_BOX(content), scm.commits_box);
  gtk_list_box_set_activate_on_single_click(scm.staged_list, TRUE);
  gtk_list_box_set_activate_on_single_click(scm.changes_list, TRUE);
  g_signal_connect(scm.staged_list, "row-activated", G_CALLBACK(scm_on_change_activated), NULL);
  g_signal_connect(scm.changes_list, "row-activated", G_CALLBACK(scm_on_change_activated), NULL);
  commits_attach();
  context_menu_attach(GTK_WIDGET(scm.staged_list), scm_on_changes_menu, NULL);
  context_menu_attach(GTK_WIDGET(scm.changes_list), scm_on_changes_menu, NULL);
  gtk_widget_set_visible(scm.staged_box, FALSE);

  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), content);
  gtk_widget_set_vexpand(scroll, TRUE);
  gtk_box_append(GTK_BOX(panel), scroll);
  return panel;
}

/* Another project: nothing of the last one stays while git status runs for the new one, and its
 * commits are listed afresh. */
void scm_reset(void)
{
  g_clear_pointer(&scm.head, g_free);
  g_clear_pointer(&scm.open_commit, g_free);
  scm_forget_history();
  if (!scm.app)
    return;
  scm_set_note(NULL, FALSE);
  g_ptr_array_set_size(scm.staged, 0);
  g_ptr_array_set_size(scm.changes, 0);
  g_clear_pointer(&scm.upstream, g_free);
  scm.ahead = scm.behind = 0;
  scm.initial = FALSE;
  show_changes(scm.app);
  gtk_list_box_remove_all(scm.commits_list);
}
