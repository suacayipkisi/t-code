/*
 * An editor tab's page: [ GtkSourceView (scrolled) | GtkSourceMap minimap ], its look (the Dark
 * Classic style scheme, the editor font) and the settings that shape it (tab size, word wrap, ...).
 * Loading and saving the file are in editor.c, which keeps their state here too.
 */
#pragma once

#include "app.h"
#include "editor/indentconf.h"

typedef void (*EditorSavedFunc)(gpointer data, gboolean ok);

/* A save's caller, told when it is done. */
typedef struct {
  EditorSavedFunc func;
  gpointer data;
} SaveWaiter;

/* The state of one editor's page; tab->view. */
typedef struct FindBar FindBar;       /* editor/findbar.c */
typedef struct DiffGutter DiffGutter; /* editor/diffgutter.c */

struct EditorView {
  Tab *tab;
  GtkWidget *column;      /* [ the save conflict bar (editor.c), overlay ] */
  GtkWidget *overlay;     /* the text, with the find bar over it */
  GtkWidget *map;         /* the minimap */
  GtkWidget *overview;    /* the problems strip: the whole file's errors and warnings, and the
                           * selected name's occurrences */
  GPtrArray *diagnostics; /* lsp/diagnostics.c */
  FindBar *find;          /* made on the first Ctrl+F */
  DiffGutter *diff;
  struct SignatureHelp *signature; /* lsp/signature.c */
  struct Semantic *semantic;       /* lsp/semantic.c */
  struct Occurrences *occurrences; /* editor/occurrences.c */
  struct Folding *folding;         /* editor/folding.c */
  struct Sticky *sticky;           /* editor/sticky.c */
  struct Breakpoints *breakpoints; /* editor/breakpoints.c */
  char *closers;                   /* closing brackets and quotes typed for the user (editing.c), from the
                            * cursor to closers_end: typing one steps over it, Backspace takes the pair */
  GtkTextMark *closers_end;
  GtkSourceFile *file;
  GFileMonitor *monitor; /* the file changed on disk: reloaded when nothing is unsaved */
  GCancellable *cancel;  /* cancelled when the tab goes, for loads and saves in flight */
  gboolean readonly;
  gboolean loading;
  guint changes;               /* counts changes, to know whether a save wrote the latest text */
  char *etag;                  /* the file on disk as last loaded or saved: other changes reload it */
  gboolean saving, save_again; /* a save is in flight; another one is due after it */
  gboolean save_failed;        /* told the user once; quiet until a save works again */
  GtkWidget *conflict;         /* the bar shown when the file changed on disk under unsaved changes */
  GtkWidget *recovery_bar;     /* the bar offering to restore unsaved changes after a crash */
  gboolean journaled;          /* this session wrote (or restored) the file's recovery journal */
  gboolean overwrite;          /* the next save writes over a file changed on disk */
  GArray *next_waiters;        /* SaveWaiter: for the save after the one in flight */
  guint autosave_id, reload_id, scroll_id, recovery_id;
  gboolean scroll_pending;    /* the cursor is scrolled to once the view is shown */
  gboolean lsp_pending;       /* loaded while hidden: the language server gets it once shown */
  int goto_line, goto_column; /* where the cursor goes once loaded (0: the start) */
  gboolean own_indent;        /* the file's own indentation (detect indentation) overrides the settings */
  gboolean own_tabs;
  int own_width;
  IndentStyle config;      /* what the project's .editorconfig / .clang-format say (indentconf.c) */
  const char *config_from; /* which of the two said it, or NULL */
  gboolean tabs;           /* the indentation in use (editor_view_apply_settings) */
  int width;
  const char *indent_from; /* where it comes from, for the status bar */
};

/* An editor tab's page, empty until editor_view_init() fills it. */
GtkWidget *editor_page(void);

/* Fills tab->page with a new buffer and its view: tab->source, tab->buffer and tab->view (empty
 * until editor.c loads the file). */
void editor_view_init(Tab *tab, gboolean readonly);

/* Frees a tab's view: the file, its watch and timers. NULL is fine. */
void editor_view_free(EditorView *view);

/* Applies the settings (tab size, spaces, word wrap, minimap, code coloring) to an editor. */
void editor_view_apply_settings(Tab *tab);

/* The editor font (t->settings.font at t->font_scale) for all editors. */
void editor_view_set_font(App *t);

/* The problems strip draws the editor's diagnostics again. */
void editor_view_show_problems(Tab *tab);

/* Brings the cursor into view: when it is outside, the view scrolls to have it in the middle
 * (vertically; sideways only as far as needed). */
void editor_view_reveal_cursor(Tab *tab);

/* The language for path, from its name and text (a .h with C++ in it is C++). NULL: plain text. */
GtkSourceLanguage *editor_language(const char *path, const char *text);

/* The Dark Classic style scheme (styles/dark-classic.xml); the first call also sets up the CSS all
 * editors share. Markdown previews color their code blocks with it too. */
GtkSourceStyleScheme *editor_style_scheme(App *t);

/* ~/.config/t-code/styles/dark-classic.xml (free it): the user's copy of the editor's colors, used
 * instead of t-code's when it is there. */
char *user_colors_file(void);

/* Reads the style schemes again (the user's copy changed); editor_view_apply_settings gives each
 * editor the new one. */
void editor_view_reload_colors(void);
