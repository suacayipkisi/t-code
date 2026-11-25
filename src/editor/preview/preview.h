/*
 * Markdown preview tabs (Ctrl+Shift+V, or "Open Preview" in the file manager): md4c parses the
 * file and it is shown as styled text in a read-only GtkTextView (no browser engine), TeX math
 * included. It follows
 * the file as it is saved.
 */
#pragma once

#include "app.h"

/* Whether path's extension is a Markdown one (.md, .markdown, ...). */
gboolean is_markdown(const char *path);

/* Whether path is an image t-code shows instead of editing (.png, .jpg, .gif, .webp, ...; not .svg, which
 * is text: it opens in an editor and has a preview). */
gboolean is_image(const char *path);

/* Whether path has a preview: Markdown, and SVG drawn. */
gboolean has_preview(const char *path);

/* Opens a preview of path in a new tab, or switches to the one that shows it already. */
void open_preview(App *t, const char *path);

/* The preview's zoom for the window's text size. */
void preview_set_zoom(Tab *tab, double scale);

/* The color theme changed: the preview is shown again in its colors. */
void preview_restyle(Tab *tab);

/* A Markdown text shown like a preview (a plugin's view): its widget (a scrolled window) in *widget,
 * the Preview freed with it. dir: where its relative links and images are. Empty at first ... */
Preview *preview_text_new(App *t, const char *dir, GtkWidget **widget);
/* ... its text replaced (append: added to the end, as streamed text is), shown again shortly, its
 * end kept in view when it was; the text it shows. */
void preview_text_set(Preview *p, const char *markdown, gboolean append);
const char *preview_text(Preview *p);
/* The color theme changed: the texts' previews take its colors. */
void preview_restyle_texts(void);

/* Frees a preview; its tab is going. */
void preview_free(Preview *preview);
