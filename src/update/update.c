/*
 * The new-release check: a few seconds after the window shows (and by "Check for Updates Now"),
 * the latest release's tag on GitHub goes against t-code's own version. A newer one opens a small
 * window ("t-code 0.4 is out"), whose button opens the release page in the browser (GtkUriLauncher,
 * like the Markdown preview's web links).
 *
 * The request goes out through curl (else wget) in a GSubprocess, so no HTTP library shows up as a
 * dependency; and both answer empty when offline, in which case nothing is asked. GitHub's
 * "latest release" API only ever returns a stable release — never a draft or pre-release — so a
 * v0.4-rc1 that is out never asks.
 */
#include "update/update.h"

#include <json-glib/json-glib.h>
#include <string.h>

#ifndef TCODE_VERSION /* set by the Makefile */
#define TCODE_VERSION "unknown"
#endif

#define RELEASES_URL "https://api.github.com/repos/suacayipkisi/t-code/releases/latest"
#define API_HEADER "Accept: application/vnd.github+json"
#define GIVE_UP_SECONDS "8"   /* a curl/wget stuck that long ends the check */
#define START_DELAY_SECONDS 4 /* nothing asks before the window has come up */
#define VERSION_ROUNDS 8      /* versions do not carry more numbers than that */

/* One check's parts: asked for, filled in by the program's answer, shown and freed together. */
typedef struct {
  App *app;
  gboolean silent; /* "Check for Updates Now" says out loud when there is nothing newer */
  char *latest;    /* the release's tag, without its leading v */
  char *url;       /* the release's page */
} Check;

/* Versions ---------------------------------------------------------------------------------- */

/* The next number of a dotted version: "0.3.0", "v0.4", "0.4.1-rc2" (a pre-release counts only by
 * its own numbers). next ends on the rest's first dot or the string's end. */
static long next_number(const char *s, char **next)
{
  char *end;
  long number = strtol(s, &end, 10);

  if (*end) { /* nothing came out ("rc…"), or more of the version follows: to the next dot or the end */
    const char *dot = strchr(end, '.');
    *next = (char *)(dot ? dot + 1 : end + strlen(end));
  } else
    *next = end;
  return number;
}

/* TRUE when theirs is a version above ours ("0.4" above "0.3.0"): number by number, and nothing
 * else — 0.3.0-rc2 sits with 0.3.0. */
gboolean newer_version(const char *ours, const char *theirs)
{
  char *a, *b;

  if (*ours == 'v')
    ours++;
  if (*theirs == 'v')
    theirs++;
  for (int round = 0; round < VERSION_ROUNDS; round++) {
    long ours_number = next_number(ours, &a), theirs_number = next_number(theirs, &b);

    if (ours_number != theirs_number)
      return theirs_number > ours_number;
    if (!*a && !*b)
      break;
    ours = a;
    theirs = b;
  }
  return FALSE;
}

/* The window -------------------------------------------------------------------------------- */

/* The update window while it waits (at most one exists). */
static GtkWidget *update_window;

/* The Check goes with the window it answered: gone with it. */
static void on_update_close(GtkWindow *window G_GNUC_UNUSED, gpointer data)
{
  Check *check = data;

  g_free(check->latest);
  g_free(check->url);
  g_free(check);
}

static void on_uri_launched(GObject *source, GAsyncResult *result, gpointer user_data)
{
  GtkUriLauncher *launcher = GTK_URI_LAUNCHER(source);
  char *url = user_data;
  GError *error = NULL;

  if (!gtk_uri_launcher_launch_finish(launcher, result, &error)) {
    if (url && *url)
      g_app_info_launch_default_for_uri(url, NULL, NULL);
    g_clear_error(&error);
  }
  g_free(url);
}

/* "Open the Release": the system's URI handler takes over (the browser), then the window goes. */
static void on_open_release(GtkButton *button, gpointer data G_GNUC_UNUSED)
{
  const char *url = g_object_get_data(G_OBJECT(button), "release-url");
  GtkWindow *dialog = GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(button)));
  GtkWindow *parent = gtk_window_get_transient_for(dialog);

  if (url && *url) {
    GtkUriLauncher *launcher = gtk_uri_launcher_new(url);
    gtk_uri_launcher_launch(launcher, parent, NULL, on_uri_launched, g_strdup(url));
    g_object_unref(launcher);
  }
  gtk_window_destroy(dialog);
}

static void on_later(GtkButton *button, gpointer data G_GNUC_UNUSED)
{
  gtk_window_destroy(GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(button))));
}

/* The newest release's window: its version, ours, and where the release lives. */
static void show_update(Check *check)
{
  GtkWidget *window, *headline, *body, *buttons, *later, *open;
  char *text;

  window = gtk_window_new();
  gtk_window_set_title(GTK_WINDOW(window), "Update t-code");
  gtk_window_set_transient_for(GTK_WINDOW(window), check->app->window);
  gtk_window_set_default_size(GTK_WINDOW(window), 380, -1);
  g_object_add_weak_pointer(G_OBJECT(window), (gpointer *)&update_window);
  update_window = window;
  g_signal_connect(window, "destroy", G_CALLBACK(on_update_close), check);

  char *escape = g_markup_escape_text(check->latest, -1);
  text = g_strdup_printf("<b>t-code %s is out</b>", escape);
  g_free(escape);
  headline = gtk_label_new(NULL);
  gtk_label_set_markup(GTK_LABEL(headline), text);
  gtk_widget_set_halign(headline, GTK_ALIGN_START);
  g_free(text);
  text = g_strdup_printf("You have t-code %s. Open the release page to download it.", TCODE_VERSION);
  body = gtk_label_new(text);
  gtk_label_set_wrap(GTK_LABEL(body), TRUE);
  gtk_widget_set_halign(body, GTK_ALIGN_START);
  g_free(text);

  open = gtk_button_new_with_label("Open the Release");
  gtk_widget_add_css_class(open, "suggested-action");
  g_object_set_data_full(G_OBJECT(open), "release-url", g_strdup(check->url), g_free);
  g_signal_connect(open, "clicked", G_CALLBACK(on_open_release), NULL);
  later = gtk_button_new_with_label("Not Now");
  g_signal_connect(later, "clicked", G_CALLBACK(on_later), NULL);
  buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign(buttons, GTK_ALIGN_END);
  gtk_box_append(GTK_BOX(buttons), later);
  gtk_box_append(GTK_BOX(buttons), open);

  GtkWidget *pane = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
  gtk_widget_set_margin_top(pane, 20);
  gtk_widget_set_margin_bottom(pane, 20);
  gtk_widget_set_margin_start(pane, 24);
  gtk_widget_set_margin_end(pane, 24);
  gtk_box_append(GTK_BOX(pane), headline);
  gtk_box_append(GTK_BOX(pane), body);
  gtk_box_append(GTK_BOX(pane), buttons);
  gtk_window_set_child(GTK_WINDOW(window), pane);
  gtk_window_present(GTK_WINDOW(window));
}

/* An answer's turn: a newer release's window replaces the one from before, and a same-or-older one
 * says what the newest release is (when the answer came from the palette command). */
static void show_result(Check *check)
{
  if (update_window) /* the previous window: its answer is behind ours */
    gtk_window_destroy(GTK_WINDOW(update_window));
  if (check->latest && check->url && newer_version(TCODE_VERSION, check->latest))
    show_update(check);
  else {
    if (!check->silent) {
      if (check->latest)
        g_printerr("t-code: %s is the newest release\n", check->latest);
      else
        g_printerr("t-code: could not check for updates (no answer)\n");
    }
    on_update_close(NULL, check); /* the window never shows, so it frees the Check itself */
  }
}

/* The answer -------------------------------------------------------------------------------- */

/* The release's tag ("v0.4") and page, only when the JSON is a release object (GitHub answers other
 * things on error, always JSON though). */
static void read_release(JsonParser *parser, Check *check)
{
  JsonNode *root = json_parser_get_root(parser);
  JsonObject *release = root && JSON_NODE_HOLDS_OBJECT(root) ? json_node_get_object(root) : NULL;

  if (release && (json_object_has_member(release, "tag_name") || json_object_has_member(release, "html_url"))) {
    if (json_object_has_member(release, "tag_name"))
      g_set_str(&check->latest, json_object_get_string_member(release, "tag_name"));
    if (json_object_has_member(release, "html_url"))
      g_set_str(&check->url, json_object_get_string_member(release, "html_url"));
  }
}

/* The program answered (or did not): its JSON becomes the newest release's window. */
static void on_fetched(GObject *source, GAsyncResult *result, gpointer data)
{
  Check *check = data;
  char *out = NULL;
  JsonParser *parser;
  GError *error = NULL;

  if (g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result, &out, NULL, NULL) && out && *g_strstrip(out)) {
    parser = json_parser_new();
    if (json_parser_load_from_data(parser, out, -1, &error))
      read_release(parser, check);
    g_clear_error(&error);
    g_object_unref(parser);
  } /* nothing readable: offline, or GitHub is unhappy — the window quietly stays away */
  g_free(out);
  show_result(check);
}

/* Asks GitHub for the latest release, through curl (usually there) or wget. */
static void fetch_latest(Check *check)
{
  const char *curl[] = { "curl", "-fsSL", "--max-time", GIVE_UP_SECONDS, "-H", API_HEADER, RELEASES_URL, NULL };
  const char *wget[] = { "wget", "-q", "-T", GIVE_UP_SECONDS, "--header=" API_HEADER, "-O-", RELEASES_URL, NULL };
  char *found = g_find_program_in_path("curl");
  const char *const *command = found ? curl : wget;
  GSubprocess *process;
  GError *error = NULL;

  g_free(found);
  process = g_subprocess_newv(command, G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, &error);
  if (!process) { /* neither downloader is there: the check quietly stops this time */
    g_clear_error(&error);
    if (!check->silent)
      g_printerr("t-code: no curl or wget to ask about new releases\n");
    on_update_close(NULL, check);
    return;
  }
  g_subprocess_communicate_utf8_async(process, NULL, NULL, on_fetched, check);
  g_object_unref(process);
}

/* Heats the check after the window has come up (updates_start put this one in its place). */
static gboolean start_check(gpointer data)
{
  Check *check = data;

  if (check->app->settings.check_updates)
    fetch_latest(check);
  else
    on_update_close(NULL, check); /* turned off meanwhile: nothing asks */
  return G_SOURCE_REMOVE;
}

/* The check that comes after the window has come up (updates_start put this one in its place). */
static gboolean begin_first_check(gpointer data)
{
  App *t = data;

  /* stays silent: a newer release's window is the only thing they see */
  if (t->settings.check_updates && !update_window)
    g_timeout_add_seconds(START_DELAY_SECONDS, start_check,
                          g_memdup2(&(Check){ .app = t, .silent = TRUE }, sizeof(Check)));
  return G_SOURCE_REMOVE;
}

/* Outside ----------------------------------------------------------------------------------- */

void updates_start(App *t)
{
  static gboolean scheduled; /* at most one by itself: more windows would mean more checks */

  if (scheduled)
    return;
  scheduled = TRUE;
  g_timeout_add_seconds(1, begin_first_check, t);
}

void updates_check_now(App *t)
{
  if (update_window) /* the window from before: ours replaces it */
    gtk_window_destroy(GTK_WINDOW(update_window));
  fetch_latest(g_memdup2(&(Check){ .app = t }, sizeof(Check)));
}
