/*
 * The bar offering to trust the project's folder (see trust.h). One window per process: its state
 * is file-wide.
 */
#include "project/trust.h"

#include "app.h"
#include "lsp/client/client.h"
#include "testing/testing.h"
#include "window/statusbar.h"

#include <string.h>

static struct {
  GtkWidget *bar;
  GtkLabel *label;
  GPtrArray *what; /* what was not run, each once, in the order asked */
  char *dismissed; /* the project "Not Now" was answered for */
} trust;

static void update_label(void)
{
  GString *text = g_string_new("t-code did not run this folder's own programs (");

  for (guint i = 0; i < trust.what->len; i++)
    g_string_append_printf(text, "%s%s", i ? ", " : "", (const char *)trust.what->pdata[i]);
  g_string_append(text, "). Trust it only if you trust its authors.");
  gtk_label_set_text(trust.label, text->str);
  g_string_free(text, TRUE);
}

static void on_trust(GtkButton *button G_GNUC_UNUSED, gpointer data)
{
  App *t = data;
  GError *error = NULL;

  if (!t->project.root)
    return;
  if (!trust_add(t->project.root, &error)) {
    char *message = g_strdup_printf("Could not trust the folder: %s", error->message);
    g_warning("t-code: %s", message);
    status_say(t, message);
    g_free(message);
    g_error_free(error);
    return;
  }
  g_message("Trusted the folder %s", t->project.root);
  trust_reset(t);
  lsp_restart(t); /* with the project's own Python, TypeScript and build scripts now */
  testing_reset(t);
}

static void on_not_now(GtkButton *button G_GNUC_UNUSED, gpointer data)
{
  App *t = data;

  g_set_str(&trust.dismissed, t->project.root);
  gtk_widget_set_visible(trust.bar, FALSE);
}

static void add_button(const char *label, GCallback clicked, App *t)
{
  GtkWidget *button = gtk_button_new_with_label(label);

  gtk_widget_set_focusable(button, FALSE);
  g_signal_connect(button, "clicked", clicked, t);
  gtk_box_append(GTK_BOX(trust.bar), button);
}

GtkWidget *trust_bar_new(App *t)
{
  GtkWidget *label = gtk_label_new(NULL);

  trust.bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  trust.label = GTK_LABEL(label);
  if (!trust.what)
    trust.what = g_ptr_array_new_with_free_func(g_free);
  gtk_widget_add_css_class(trust.bar, "tcode-conflict");
  gtk_widget_add_css_class(trust.bar, "tcode-trust");
  gtk_label_set_xalign(trust.label, 0.0f);
  gtk_label_set_wrap(trust.label, TRUE);
  gtk_widget_set_hexpand(label, TRUE);
  gtk_box_append(GTK_BOX(trust.bar), label);
  add_button("Trust Folder", G_CALLBACK(on_trust), t);
  add_button("Not Now", G_CALLBACK(on_not_now), t);
  gtk_widget_set_visible(trust.bar, FALSE);
  return trust.bar;
}

gboolean trust_project(App *t, const char *what)
{
  if (!t->project.root)
    return FALSE;
  if (trust_has(t->project.root))
    return TRUE;
  if (!trust.bar || g_strcmp0(trust.dismissed, t->project.root) == 0)
    return FALSE;
  for (guint i = 0; i < trust.what->len; i++)
    if (strcmp(trust.what->pdata[i], what) == 0)
      return FALSE;
  g_ptr_array_add(trust.what, g_strdup(what));
  update_label();
  gtk_widget_set_visible(trust.bar, TRUE);
  return FALSE;
}

void trust_reset(App *t G_GNUC_UNUSED)
{
  if (!trust.bar)
    return;
  g_ptr_array_set_size(trust.what, 0);
  gtk_widget_set_visible(trust.bar, FALSE);
}
