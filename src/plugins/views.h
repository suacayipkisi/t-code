/*
 * Plugins' views (tapi_1's view-open and the rest): a read-only text a plugin fills, as a panel in a
 * dock on the left, bottom or right (dragged anywhere like t-code's own) or as a tab in an editor
 * group. Each is the plugin's own, by an id it chooses; a double click or Enter on a line sends the
 * view_activated event, and a view tab the user closes sends view_closed.
 *
 * A view may show its text as Markdown, and may have an input box under it (Enter sends view_input,
 * with the mode picked beside it), prompts with choices (view_choice) and a busy line with Stop
 * (view_stop): what a chat with a program needs.
 */
#pragma once

#include "app.h"

/* Where a view goes ("left", "bottom", "right", "editor": the active group, "side": the group beside
 * it). */
typedef enum { VIEW_LEFT, VIEW_BOTTOM, VIEW_RIGHT, VIEW_EDITOR, VIEW_SIDE } ViewPlace;

/* The place a word names; FALSE when it names none. */
gboolean view_place(const char *word, ViewPlace *place);

/* What a new view has besides its text; an open view keeps what it was made with. */
typedef struct {
  gboolean markdown;  /* its text is Markdown, shown like a preview */
  const char *input;  /* an input box under it, this its placeholder; NULL: none */
  char *const *modes; /* the modes picked beside the input (NULL or empty: none) ... */
  const char *mode;   /* ... and the one picked first (NULL: the first) */
  gboolean hidden;    /* a dock's view: only its tab added, nothing switched to (opened with a project) */
} ViewOptions;

/* Opens owner's view id with title and text (NULL: none), or, when it is open already, gives it the
 * title and text and shows it (options->hidden: neither shows it). icon: an icon name for a dock's tab (NULL: a default one). options:
 * NULL for a plain view. */
void view_open(App *t, const char *owner, const char *id, const char *title, ViewPlace place, const char *icon,
               const char *text, const ViewOptions *options);

/* How view_set_text changes the text. */
typedef enum {
  VIEW_TEXT_SET,    /* replaces it */
  VIEW_TEXT_APPEND, /* adds a line (scrolled to) */
  VIEW_TEXT_EXTEND, /* adds to its end, on the same line (streamed text) */
} ViewTextChange;

/* Changes the text of owner's view; FALSE when it has no such view. */
gboolean view_set_text(App *t, const char *owner, const char *id, const char *text, ViewTextChange change);

/* Shows owner's view (its dock or tab, switched to); closes it. FALSE when it has no such view. */
gboolean view_show(App *t, const char *owner, const char *id);
gboolean view_close(App *t, const char *owner, const char *id);

/* A prompt above the view's input: text with a button per choice (picking one sends view_choice and
 * takes the prompt away); no choices: the prompt named prompt goes. FALSE when owner has no such view,
 * or it has no input. */
gboolean view_prompt(App *t, const char *owner, const char *id, const char *prompt, const char *text,
                     char *const *choices);

/* The busy line above the view's input: text with Stop beside it (view_stop), or none for NULL. FALSE
 * when owner has no such view, or it has no input. */
gboolean view_busy(App *t, const char *owner, const char *id, const char *text);

/* Puts text into the view's input box (a space after it, for what is typed next), shows the view and
 * gives the box the keyboard. FALSE when owner has no such view, or it has no input. */
gboolean view_input(App *t, const char *owner, const char *id, const char *text);

/* A view tab's title (the label of its tab), or NULL for another tab. */
const char *view_title(Tab *tab);
