/*
 * Completion while typing: GtkSourceView shows the list when a word
 * is typed (or after ".", "->", "::"), and on Ctrl+Space; the language server fills it. A picked
 * function comes in as a snippet: Tab goes from one argument to the next.
 */
#pragma once

#include "app.h"

/* Adds completion to a new editor. */
void completion_attach(Tab *tab);

/* Files the language server does not take (Markdown, Makefiles, scripts): the words in the file
 * are offered instead. Once the file is loaded. */
void completion_use_words(Tab *tab);

/* Opens the list at the cursor (Ctrl+Space). */
void completion_show(Tab *tab);
