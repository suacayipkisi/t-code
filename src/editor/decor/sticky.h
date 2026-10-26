/*
 * Sticky scroll: the first lines of the blocks the editor's top is inside
 * (namespace, function, loop, ...) stay at the top of the editor while their insides scroll by, in
 * their colors, with their line numbers. A click on one goes to it.
 */
#pragma once

#include "app.h"

typedef struct Sticky Sticky;

/* Puts the sticky lines over tab's text (editorview.c). */
void sticky_attach(Tab *tab);

/* Frees them; NULL is fine. */
void sticky_free(Sticky *s);

/* Shows or hides them as the setting says (editor_view_apply_settings). */
void sticky_apply_settings(Tab *tab);
