/*
 * Editing commands on the lines around the cursor: comment, duplicate, move and
 * delete lines, redo; and typing help: closing brackets and quotes, Enter after an opening bracket
 * (one level deeper); a double click selects a name without the punctuation around it.
 */
#pragma once

#include "app.h"

/* Adds the typing help to a new editor (its keys come here before the view's). */
void editing_attach(Tab *tab);

/* The name (letters, digits, "_") at iter, or the one just before it; FALSE when there is none. */
gboolean editor_word_at(const GtkTextIter *iter, GtkTextIter *start, GtkTextIter *end);

/* Comments the selected lines out with the language's line comment ("// "), or back in when they
 * all are. Without a line comment (CSS, HTML), the language's block comment goes around them. */
void editor_toggle_comment(Tab *tab);

/* Copies the selected lines (or the cursor's line) below themselves. */
void editor_duplicate_lines(Tab *tab);

/* Moves the selected lines (or the cursor's line) one line down or up. */
void editor_move_lines(Tab *tab, gboolean down);

/* Deletes the selected lines (or the cursor's line). */
void editor_delete_lines(Tab *tab);

/* Redoes what Ctrl+Z undid (Ctrl+Y; Ctrl+Shift+Z is GtkSourceView's own). */
void editor_redo(Tab *tab);
