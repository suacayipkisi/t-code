/*
 * t-code: the two types every part of the window shares.
 *   App  the whole window and its state (one per t-code process)
 *   Tab  one editor tab (GtkSourceView) or Markdown preview tab
 * Each module declares its functions in its own header next to its .c file; this one holds only
 * what they all need. The folders:
 *   main.c     start-up and shutdown
 *   window/    the window itself: layout, theme, terminals, status bar, shortcuts
 *   files/     the Files panel: the project's folder tree
 *   editor/    editor tabs: GtkSourceView, the page around it (minimap, guides), Markdown previews
 *   search/    the Search panel
 *   git/       Source Control panel, git status, comparing with old versions
 *   popups/    the popup list (Ctrl+P, command palette)
 *   dialogs/   Settings, Keyboard Shortcuts, About
 *   project/   the project folder, the saved session, Build and Run
 *   ipc/       the control socket ($TCODE_SOCKET): open files from scripts and tools
 *   lsp/       the language server: clangd's diagnostics, hover, completion, ...
 *   debug/     the debugger: gdb, the Debug panel
 */
#pragma once

#include <gtk/gtk.h>
#include <gtksourceview/gtksource.h>
#include <vte/vte.h>

typedef struct App App;
typedef struct Preview Preview;       /* editor/preview.c */
typedef struct Picker Picker;         /* popups/picker.c */
typedef struct EditorView EditorView; /* editor/editorview.h */
typedef struct Lsp Lsp;               /* lsp/client.c */
typedef struct Debugger Debugger;     /* debug/debugger.c */

typedef enum {
  TAB_EDITOR,  /* a source file (GtkSourceView) */
  TAB_PREVIEW, /* a rendered markdown file */
} TabKind;

typedef struct Tab {
  App *app;
  TabKind kind;
  GtkWidget *page;       /* the editor's or the preview */
  GtkWidget *frame;      /* the notebook page when it is not page: a comparison's two sides */
  GtkSourceView *source; /* editors only */
  GtkSourceBuffer *buffer;
  Preview *preview; /* previews only */
  GtkLabel *label;
  GtkWidget *item; /* its tab in the group's tab strip (editor/tabs.c): the label and a close button */
  char *path;
  gboolean temporary; /* a click in Files opens it, the next one replaces it */
  gboolean modified;
  EditorView *view; /* editors only: loading, saving, the minimap (editor/editorview.h) */
  struct {
    int line, column; /* 1-based */
  } cursor;
  struct {
    int errors, warnings;
  } problems; /* clangd's (lsp/inline/diagnostics.c) */
  struct {
    char *title;          /* shown instead of the file name (an old version) */
    char *with;           /* old versions only: the working file they are compared with */
    struct Tab *old_side; /* the left side's: the right one, in the same notebook page */
  } compare;              /* a comparison's two sides (git/compare.c) */
} Tab;

/* The editor area holds two tab groups side by side (the second one only while split); a third
 * one is the second window's ("Open in New Window"), while it is open. */
#define GROUPS 3
#define WINDOW_GROUP 2
#define DOCKS 12 /* 0..3: left stacked, 4..7: bottom side-by-side, 8..11: right stacked */
#define PANELS 9 /* the panels that go into the docks (window.h's panel_names) */

struct App {
  GtkWindow *window;

  /* layout */
  struct {
    GtkPaned *hpaned;  /* the left side | the rest */
    GtkPaned *rpaned;  /* the editors and the bottom side | the right side */
    GtkPaned *vpaned;  /* the editors over the bottom side */
    GtkWidget *left;   /* the left side: a vertical paned chain holding dock.cards[0..3] */
    GtkWidget *bottom; /* the bottom side: a horizontal chain holding dock.cards[4..7] */
    GtkWidget *right;  /* the right side: a vertical chain holding dock.cards[8..11] */
  } layout;

  /* dock */
  struct {
    GtkPaned *paneds[3][3];  /* the 3 paneds per side (left, bottom, right) */
    GtkWidget *cards[DOCKS]; /* the 12 dock cards around the editor */
    GtkStack *stacks[DOCKS]; /* the stack of panels in each dock */
    gboolean collapsed[DOCKS];
    int saved_pos[DOCKS];
    GtkStack *scm_stack; /* the dock stack currently holding t->scm */
    guint checks_id;     /* the idle that updates the Panels menu's checks */
  } dock;

  /* editors */
  struct {
    GtkStack *area;
    GtkNotebook *groups[GROUPS]; /* groups[WINDOW_GROUP]: NULL while there is no second window */
    GtkWindow *second;           /* the second window (window/secondwindow.c), or NULL */
    int active;                  /* the group new tabs go to */
    GtkPaned *split;             /* groups[0] | groups[1] */
    GtkToggleButton *split_button;
    gboolean inlay_peek; /* Ctrl+Alt held: inlay hints the other way round from the setting (lsp/inline/inlay.c) */
    guint peek_id;       /* starts the peek once Ctrl+Alt is held a moment */
  } editors;

  /* shell */
  struct {
    VteTerminal *term;
    GPid pid;
  } shell; /* the shell pane (window/terminal.c) */

  GtkWidget *scm;
  GtkWidget *files; /* the Files panel (files/files.c) */
  Picker *picker;
  GMenu *recent_menu;

  /* status */
  struct {
    GtkLabel *branch, *problems, *build, *position;
    GtkLabel *progress; /* "rust-analyzer: Indexing 45%": what a language server is busy with */
    guint progress_id;  /* shows the progress when it runs */
    GtkWidget *flags;   /* "No compile flags": clangd cannot find a file's headers */
    GtkWidget *no_lsp;  /* "No LSP": the current file's language server is not installed */
    GtkWidget *indent;  /* "Spaces: 4": the current editor's indentation, a click changes it */
    guint say_id;       /* a status_say() message is shown in build's place until this runs */
  } status;

  /* project */
  struct {
    char *root;
    char *git_top; /* the work tree root of root's repository; NULL outside one */
    struct {
      gboolean tabs;
      int width; /* 0: none chosen */
    } indent;    /* chosen for this project in the status bar (session.c); beats everything else */
  } project;

  /* paths */
  struct {
    char *styles;   /* the editor's style scheme (Dark Classic) */
    char *snippets; /* t-code's own snippets (editor/editing/snippets.c) */
    char *icons;    /* desktop/ in the build tree, holding the app icon */
    char *temp;     /* old file versions for comparisons */
    char *socket;   /* $TCODE_SOCKET (ipc/socket.c) */
  } paths;

  GSocketService *service;
  GPtrArray *socket_events; /* the control socket's "subscribe" clients (socket.c's Client) */
  Lsp *lsp;                 /* clangd, retained for its compile-flag status hint */
  GPtrArray *lsps;          /* lazily started language servers, each idle-stopped after its last file */
  GPtrArray *plugins;       /* installed plugins (plugins/plugins.h) */
  GtkWidget *problems;      /* the Problems panel widget (lsp/problems.h) */

  /* debugging */
  struct {
    GHashTable *breakpoints; /* path -> GArray of Breakpoint, by line (editor/gutter/breakpoints.h) */
    char *stop_path;         /* where the program is stopped (its innermost frame with a file), or NULL */
    int stop_line;
    char *disassembly;    /* the Disassembly View's file (debug/disassembly.c), or NULL */
    int disassembly_line; /* its line of the instruction that runs next (1-based), or 0 */
    Debugger *session;    /* gdb, while debugging (debug/debugger.c) */
    GtkWidget *panel;     /* the Debug panel: its buttons over the program's terminal */
    VteTerminal *term;    /* the program's terminal */
  } debug;

  gboolean closing;

  /* settings */
  struct {
    gboolean minimap;
    gboolean sticky_scroll; /* the heads of the blocks around the editor's top stay at its top */
    gboolean reopen_last;   /* started from the app menu: open the last project */
    gboolean check_updates; /* at start-up, the latest release on GitHub goes against t-code's */
    gboolean autosave;      /* a changed editor saves a second after its last change */
    gboolean tabs_to_spaces;
    int tab_size;
    gboolean detect_indent;  /* a file's own indentation overrides the two above */
    gboolean guides;         /* indent and bracket pair guides in editors */
    gboolean word_wrap;      /* long lines continue on the next row instead of running off */
    gboolean inlay_hints;    /* parameter names and deduced types inside the lines (lsp/inline/inlay.c) */
    gboolean breadcrumbs;    /* the file's path and the symbols around the cursor over editors */
    int ruler;               /* a vertical line after this column in editors, 0 for none */
    gboolean clang_tidy;     /* clangd also runs the project's .clang-tidy checks */
    gboolean format_on_save; /* Ctrl+S formats the file with clangd first */
    char *font;              /* editors and the shell, a Pango font description */
    char *files_font;        /* the Files panel */
    gboolean square;         /* the square theme (flat panels, lines between them), not rounded cards */
    char *color_theme;       /* window/palette.c's id: "dark-classic", ... */
    gboolean plain;          /* syntax coloring off (F7) */
    double font_scale;       /* editors, terminals and the Files panel (Ctrl+= / Ctrl+-); session.ini keeps it */
    double preview_scale;    /* Markdown previews' text size; session.ini keeps it */
  } settings;
};
