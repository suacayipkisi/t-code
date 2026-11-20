/*
 * File templates in the window (over project/templates.c): the "Save as Template" window, which asks
 * for the template's name and a short description, and the picker that offers templates for a new
 * file or one to delete.
 */
#pragma once

#include "app.h"
#include "project/templates.h"

/* Opens the "Save as Template" window for an editor tab (NULL: the current tab): its text as it is
 * now (unsaved changes too), under the tab's file name. Says so in the status bar when it is not an
 * editor. */
void show_save_template(App *t, Tab *tab);

/* Called with the picked template (NULL: "Empty File"); it is valid during the call only. */
typedef void (*TemplatePicked)(App *t, const Template *tpl);

/* Offers the templates in the picker: for_name set, the ones that suit it (template_matches) after
 * "Empty File"; NULL, every one. then(t, tpl) gets the pick; nothing happens when the picker is closed.
 * FALSE (and no picker) when no template suits. */
gboolean pick_template(App *t, const char *for_name, TemplatePicked then);

/* Asks which template to delete, then deletes it. */
void pick_template_to_delete(App *t);
