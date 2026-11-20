/*
 * File templates in the window (see templates.h).
 */
#include "dialogs/templates.h"

#include "editor/tabs.h"
#include "popups/picker.h"
#include "window/statusbar.h"

#include <string.h>

#define SAVE_WIDTH 440
#define EMPTY_PAYLOAD "empty"

/* The "Save as Template" window ------------------------------------------------------------------ */

typedef struct {
  App *app;
  GtkWidget *window, *name, *description, *error;
  char *file; /* the tab's file name */
  char *text; /* its text when the window opened */
} SaveWindow;

static GtkWidget *save_window; /* the one open, or NULL */

static void save_window_free(gpointer data)
{
  SaveWindow *w = data;

  g_free(w->file);
  g_free(w->text);
  g_free(w);
}

static void on_save(SaveWindow *w)
{
  char *root = templates_dir();
  const char *name = gtk_editable_get_text(GTK_EDITABLE(w->name));
  GError *error = NULL;

  if (templates_save(root, name, gtk_editable_get_text(GTK_EDITABLE(w->description)), w->file, w->text, -1, &error)) {
    char *title = g_strstrip(g_strdup(name)), *said = g_strdup_printf("Template \"%s\" saved", title);
    status_say(w->app, said);
    g_free(said);
    g_free(title);
    gtk_window_close(GTK_WINDOW(w->window));
  } else {
    gtk_label_set_text(GTK_LABEL(w->error), error->message);
    gtk_widget_set_visible(w->error, TRUE);
    gtk_widget_grab_focus(w->name);
    g_error_free(error);
  }
  g_free(root);
}

static GtkWidget *field(GtkWidget *box, const char *label, GtkWidget *entry)
{
  GtkWidget *title = gtk_label_new_with_mnemonic(label);

  gtk_label_set_xalign(GTK_LABEL(title), 0);
  gtk_label_set_mnemonic_widget(GTK_LABEL(title), entry);
  gtk_box_append(GTK_BOX(box), title);
  gtk_box_append(GTK_BOX(box), entry);
  return entry;
}

void show_save_template(App *t, Tab *tab)
{
  GtkTextIter start, end;

  if (!tab)
    tab = current_tab(t);
  if (!tab || tab->kind != TAB_EDITOR || !tab->buffer || !tab->path) {
    status_say(t, "Open a file to save it as a template");
    return;
  }
  if (save_window)
    gtk_window_destroy(GTK_WINDOW(save_window));

  SaveWindow *w = g_new0(SaveWindow, 1);
  w->app = t;
  w->file = g_path_get_basename(tab->path);
  gtk_text_buffer_get_bounds(GTK_TEXT_BUFFER(tab->buffer), &start, &end);
  w->text = gtk_text_buffer_get_text(GTK_TEXT_BUFFER(tab->buffer), &start, &end, TRUE);

  w->window = save_window = gtk_window_new();
  g_object_add_weak_pointer(G_OBJECT(save_window), (gpointer *)&save_window);
  g_object_set_data_full(G_OBJECT(w->window), "tcode-save-template", w, save_window_free);
  gtk_window_set_title(GTK_WINDOW(w->window), "Save as Template");
  gtk_window_set_transient_for(GTK_WINDOW(w->window), t->window);
  gtk_window_set_modal(GTK_WINDOW(w->window), TRUE);
  gtk_window_set_default_size(GTK_WINDOW(w->window), SAVE_WIDTH, -1);
  GtkEventController *keys = GTK_EVENT_CONTROLLER(gtk_shortcut_controller_new());
  gtk_shortcut_controller_add_shortcut(
      GTK_SHORTCUT_CONTROLLER(keys),
      gtk_shortcut_new(gtk_keyval_trigger_new(GDK_KEY_Escape, 0), gtk_named_action_new("window.close")));
  gtk_widget_add_controller(w->window, keys);

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_margin_top(box, 18);
  gtk_widget_set_margin_bottom(box, 18);
  gtk_widget_set_margin_start(box, 18);
  gtk_widget_set_margin_end(box, 18);

  char *text = g_strdup_printf("Save %s as a template", w->file);
  GtkWidget *heading = gtk_label_new(text);
  g_free(text);
  gtk_label_set_xalign(GTK_LABEL(heading), 0);
  gtk_label_set_wrap(GTK_LABEL(heading), TRUE);
  gtk_widget_add_css_class(heading, "heading");
  gtk_box_append(GTK_BOX(box), heading);

  const char *dot = strrchr(w->file, '.');
  char *stem = dot && dot != w->file ? g_strndup(w->file, dot - w->file) : g_strdup(w->file);
  w->name = field(box, "_Name", gtk_entry_new());
  gtk_editable_set_text(GTK_EDITABLE(w->name), stem);
  g_free(stem);
  w->description = field(box, "_Description", gtk_entry_new());
  gtk_entry_set_placeholder_text(GTK_ENTRY(w->description), "What it is for (optional)");
  g_signal_connect_swapped(w->name, "activate", G_CALLBACK(on_save), w);
  g_signal_connect_swapped(w->description, "activate", G_CALLBACK(on_save), w);

  dot = strrchr(w->file, '.');
  text = dot && dot != w->file
             ? g_strdup_printf("Offered when you make a new %s file. A template with the same name is replaced.", dot)
             : g_strdup_printf("Offered when you make a new file named %s. A template with the same name is replaced.",
                               w->file);
  GtkWidget *note = gtk_label_new(text);
  g_free(text);
  gtk_label_set_xalign(GTK_LABEL(note), 0);
  gtk_label_set_wrap(GTK_LABEL(note), TRUE);
  gtk_widget_add_css_class(note, "dim-label");
  gtk_widget_set_margin_top(note, 4);
  gtk_box_append(GTK_BOX(box), note);

  w->error = gtk_label_new(NULL);
  gtk_label_set_xalign(GTK_LABEL(w->error), 0);
  gtk_label_set_wrap(GTK_LABEL(w->error), TRUE);
  gtk_widget_add_css_class(w->error, "error");
  gtk_widget_set_visible(w->error, FALSE);
  gtk_box_append(GTK_BOX(box), w->error);

  GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *cancel = gtk_button_new_with_label("Cancel"), *save = gtk_button_new_with_label("Save");
  gtk_widget_set_halign(buttons, GTK_ALIGN_END);
  gtk_widget_set_margin_top(buttons, 10);
  gtk_widget_add_css_class(save, "suggested-action");
  g_signal_connect_swapped(cancel, "clicked", G_CALLBACK(gtk_window_close), w->window);
  g_signal_connect_swapped(save, "clicked", G_CALLBACK(on_save), w);
  gtk_box_append(GTK_BOX(buttons), cancel);
  gtk_box_append(GTK_BOX(buttons), save);
  gtk_box_append(GTK_BOX(box), buttons);

  gtk_window_set_child(GTK_WINDOW(w->window), box);
  gtk_window_present(GTK_WINDOW(w->window));
  gtk_widget_grab_focus(w->name);
}

/* The picker -------------------------------------------------------------------------------------- */

static struct {
  GPtrArray *offered; /* the templates shown (Template *) */
  gboolean empty_row; /* "Empty File" first */
  TemplatePicked then;
} offer;

static void query_templates(App *t, const char *query, guint generation)
{
  int shown = 0;

  if (offer.empty_row && (!*query || fuzzy_score(query, "Empty File") >= 0)) {
    picker_add(t, generation, "Empty File", NULL, EMPTY_PAYLOAD);
    shown++;
  }
  for (guint i = 0; i < offer.offered->len; i++) {
    const Template *tpl = offer.offered->pdata[i];
    if (*query && fuzzy_score(query, tpl->name) < 0 && fuzzy_score(query, tpl->file) < 0)
      continue;
    char *detail = *tpl->description ? g_strdup_printf("%s · %s", tpl->description, tpl->file) : g_strdup(tpl->file);
    char *payload = g_strdup_printf("%u", i);
    picker_add(t, generation, tpl->name, detail, payload);
    g_free(payload);
    g_free(detail);
    shown++;
  }
  if (!shown)
    picker_message(t, generation, "No matching template");
}

static void on_template_chosen(App *t, const char *payload)
{
  TemplatePicked then = offer.then;
  const Template *tpl = NULL;

  if (strcmp(payload, EMPTY_PAYLOAD) != 0) {
    guint64 i = g_ascii_strtoull(payload, NULL, 10);
    if (i < offer.offered->len)
      tpl = offer.offered->pdata[i];
  }
  offer.then = NULL;
  if (then)
    then(t, tpl);
}

/* Fills offer with the templates suiting for_name (NULL: all); FALSE when there are none. */
static gboolean gather(const char *for_name)
{
  char *root = templates_dir();

  g_clear_pointer(&offer.offered, g_ptr_array_unref);
  offer.offered = templates_list(root);
  g_free(root);
  for (guint i = offer.offered->len; for_name && i-- > 0;)
    if (!template_matches(offer.offered->pdata[i], for_name))
      g_ptr_array_remove_index(offer.offered, i);
  return offer.offered->len > 0;
}

gboolean pick_template(App *t, const char *for_name, TemplatePicked then)
{
  if (!gather(for_name))
    return FALSE;
  offer.empty_row = for_name != NULL;
  offer.then = then;
  char *placeholder =
      for_name ? g_strdup_printf("Start %s from a template", for_name) : g_strdup("New file from a template");
  picker_open(t, placeholder, query_templates, on_template_chosen);
  g_free(placeholder);
  return TRUE;
}

static void delete_picked(App *t, const Template *tpl)
{
  GError *error = NULL;
  char *said;

  if (!tpl)
    return;
  if (template_delete(tpl, &error)) {
    said = g_strdup_printf("Template \"%s\" deleted", tpl->name);
  } else {
    said = g_strdup_printf("Could not delete the template: %s", error->message);
    g_error_free(error);
  }
  status_say(t, said);
  g_free(said);
}

void pick_template_to_delete(App *t)
{
  if (!gather(NULL)) {
    status_say(t, "No templates yet: \"Template: Save File as Template…\" makes one");
    return;
  }
  offer.empty_row = FALSE;
  offer.then = delete_picked;
  picker_open(t, "Delete which template?", query_templates, on_template_chosen);
}
