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

#include "git/vcs.h"

typedef struct App App;
typedef struct Preview Preview;       /* editor/preview.c */
typedef struct Picker Picker;         /* popups/picker.c */
typedef struct EditorView EditorView; /* editor/editorview.h */
typedef struct Lsp Lsp;               /* lsp/client/client.c */
typedef struct Debugger Debugger;     /* debug/debugger.c */

typedef enum {
  TAB_EDITOR,  /* a source file (GtkSourceView) */
  TAB_PREVIEW, /* a rendered markdown file */
  TAB_VIEW,    /* a plugin's view (plugins/views.h): path is "view:<plugin>/<id>" */
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

#define PANELS 11               /* t-code's own panels that go into the docks (window.h's panel_names) */
#define PANEL_SCM_OFF (1u << 1) /* settings.panels_off's bit of Source Control (panel_names[1]) */

struct App {
  GtkWindow *window;

  /* layout */
  struct {
    GtkPaned *hpaned;     /* the left side | the rest */
    GtkPaned *rpaned;     /* the editors and the bottom side | the right side */
    GtkPaned *vpaned;     /* the editors over the bottom side */
    GtkWidget *left;      /* the left side: the root of a tree of docks (window/splits.h, window/docks/) */
    GtkWidget *bottom;    /* the bottom side: another */
    GtkWidget *right;     /* the right side: another */
    gboolean pressed;     /* a mouse button is down in the window (a paned may be being dragged) */
    guint snapped;        /* a bit per side dragged below its smallest size: it hides on release */
    int sizes[3];         /* each side's last size that was big enough, given back when it shows again */
    guint placed;         /* a bit per side (bottom, right) given its size at its paned's first allocation */
    GtkWidget *edges[3];  /* thin strips on the left, bottom and right edges: dragging one shows that hidden side */
    GtkWidget *menu_bar;  /* the title bar's menus (window/titlebar.c) */
    GtkWidget *logo;      /* t-code's icon left of them */
    GtkWidget *logo_menu; /* the icon as a button opening the same menus, while the menu bar is off */
    GtkWidget *side_buttons[3]; /* the title bar's buttons showing or hiding the left, bottom and right sides */
    GtkWidget *search_bar;      /* the title bar's search: opens Go to File, '#' for commands */
  } layout;

  /* dock */
  struct {
    GPtrArray *list;     /* every dock (window/docks/docks.h's Dock), any number on each side */
    GtkStack *scm_stack; /* the dock stack currently holding t->scm */
    guint checks_id;     /* the idle that updates the Panels menu's checks */
  } dock;

  /* editors */
  struct {
    GtkStack *area;      /* the hint while nothing is open, else the main window's group tree */
    GtkWidget *view;     /* area under the drop markers (editor/dropzone.c): what the window holds */
    GtkWidget *roots[2]; /* the group trees (editor/groups.c): the main window's, the second window's or NULL */
    GPtrArray *groups;   /* every tab group (a GtkNotebook) in order: the main window's, then the second's */
    GtkNotebook *active; /* the group new tabs go to */
    GtkWindow *second;   /* the second window (window/secondwindow.c), or NULL */
    gboolean inlay_peek; /* Ctrl+Alt held: inlay hints the other way round from the setting (lsp/inline/inlay.c) */
    guint peek_id;       /* starts the peek once Ctrl+Alt is held a moment */
  } editors;

  /* shell */
  struct {
    GtkWidget *panel;  /* the Terminal panel: the terminals as tabs */
    GtkNotebook *tabs; /* one page per terminal, the first one always there */
    VteTerminal *term; /* the first terminal: Build and Run type into it */
  } shell;             /* the shell pane (window/terminal.c) */

  GtkWidget *scm;
  GtkWidget *files; /* the Files panel (files/files.c) */
  Picker *picker;
  GMenu *recent_menu;

  /* status */
  struct {
    GtkWidget *bar;              /* the status bar itself */
    GtkWidget *build_buttons[2]; /* its Build and Run buttons */
    GtkLabel *branch, *build, *position;
    GtkLabel *errors, *warnings; /* the open files' problems, each after its icon */
    GtkLabel *progress;          /* "rust-analyzer: Indexing 45%": what a language server is busy with */
    guint progress_id;           /* shows the progress when it runs */
    GtkWidget *flags;            /* "No compile flags": clangd cannot find a file's headers */
    GtkWidget *no_lsp;           /* "No LSP": the current file's language server is not installed */
    GtkWidget *font_scale;       /* Shows text scale if it's not initial value(1.0), click sets initial value */
    GtkWidget *indent;           /* "Spaces: 4": the current editor's indentation, a click changes it */
    guint say_id;                /* a status_say() message is shown in build's place until this runs */
    GtkBox *plugins;             /* plugins' lasting items (socket "status-set"), each a label */
  } status;

  /* project */
  struct {
    char *root;
    char *repo_top;    /* the work tree root of root's repository; NULL outside one */
    VcsKind repo_kind; /* and its kind (git, hg, svn; several may hold root: gitstatus.c chooses) */
    char *scm_top;     /* the repository Source Control shows: repo_top or one nested in the project (gitstatus.c) */
    VcsKind scm_kind;  /* and its kind */
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
  GHashTable *plugin_diagnostics; /* path → (plugin → JSON list): socket "diagnostics-set" (lsp/inline/diagnostics.c) */
  GPtrArray *socket_events;       /* the control socket's "subscribe" clients (socket.c's Client) */
  Lsp *lsp;                       /* clangd, retained for its compile-flag status hint */
  GPtrArray *lsps;                /* lazily started language servers, each idle-stopped after its last file */
  GPtrArray *plugins;             /* installed plugins (plugins/plugins.h) */
  GtkWidget *problems;            /* the Problems panel widget (lsp/problems.h) */

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
    gboolean line_blame;     /* who last changed the cursor's line, after its end (editor/decor/blame.c) */
    int ruler;               /* a vertical line after this column in editors, 0 for none */
    int autosave_ms;         /* stores the value of the autosave after edit as ms(milisecons) */
    gboolean clang_tidy;     /* clangd also runs the project's .clang-tidy checks */
    int server_idle;         /* seconds a language server runs on after its last file closes; -1: it never stops */
    guint servers_off;       /* built-in language servers turned off: bit 1 << ServerKind (lsp/servers.h) */
    gboolean format_on_save; /* Ctrl+S formats the file with clangd first */
    gboolean lldb;           /* C, C++ and Rust debug with lldb-dap instead of gdb (debug/adapters.h) */
    char *font;              /* editors and the shell, a Pango font description */
    char *files_font;        /* the Files panel */
    gboolean square;         /* the square theme (flat panels, lines between them), not rounded cards */
    int corner_radius;       /* the rounded theme's panel corners in pixels (the controls' follow) */
    int selection_opacity;   /* the editors' selection, in the accent color over the text: 0 to 100 % */
    char *color_theme;       /* window/palette.c's id: "dark-classic", ... */
    gboolean plain;          /* syntax coloring off (F7) */
    double font_scale;       /* editors, terminals and the Files panel (Ctrl+= / Ctrl+-); session.ini keeps it */
    double preview_scale;    /* Markdown previews' text size; session.ini keeps it */
    guint panels_off;        /* t-code's own panels turned off: bit 1 << their index in panel_names */
    gboolean menu_bar;       /* the menu bar in the title bar; off: its menus open from t-code's icon */
    gboolean status_bar;     /* the status bar under everything */
    gboolean build_buttons;  /* the status bar's Build and Run buttons */
    char **status_off;       /* status bar items turned off, by id ("branch", "position", "plugin:<key>", ...) */
    gboolean side_buttons;   /* the title bar's buttons showing or hiding the sides */
    gboolean search_bar;     /* the title bar's search (Ctrl+P and Ctrl+Shift+P work without it) */
  } settings;
};
