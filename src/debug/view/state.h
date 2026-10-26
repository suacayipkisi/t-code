/*
 * What the Run and Debug panel's files share (debugview.c: the panel and what the debugger calls;
 * variables.c: the variables and their rows; watch.c; stack.c: Call Stack; breakpoints.c). Only they
 * include it.
 */
#pragma once

#include "app.h"
#include "lsp/json.h"

#define PREVIEW_ITEMS 8 /* children shown in a value made from them: "{1, 2, 3, …}" */
#define NOT_PAUSED "not available: the program is not paused"

struct _TcodeVar {
  GObject parent;
  char *name;
  char *value;
  char *key;        /* where it is, to compare with the last pause: "main/p/x" */
  gint64 reference; /* gdb's variablesReference: > 0 when it has children */
  gboolean changed; /* its value is not the last pause's */
  gboolean asked;   /* its children were asked for */
  gboolean preview; /* its value is made from its children's */
  gboolean missing; /* no value: a watch expression that cannot be evaluated now */
  gboolean waiting; /* Registers: its row is added once its children came (after the variables) */
  GListStore *children;
};

#define TCODE_TYPE_VAR (tcode_var_get_type())
G_DECLARE_FINAL_TYPE(TcodeVar, tcode_var, TCODE, VAR, GObject)

/* The "value-changed" signal: a value made from a variable's children came. */
extern guint value_changed;

typedef struct {
  App *app;
  GtkWidget *note;      /* instead of the variables: not debugging, running */
  GtkWidget *variables; /* the variables' list (scrolled) */
  GListStore *root;     /* the shown frame's variables */
  GListStore *watch;    /* the watch expressions' values */
  GPtrArray *watches;   /* the watch expressions (char *) */
  GtkWidget *watch_entry;
  GtkListBox *stack;
  GtkListBox *breakpoints;
  JsonNode *frames;    /* the last pause's (a copy) */
  int shown;           /* the frame whose variables are listed */
  gboolean all_frames; /* the frames without source are listed one by one, not as one row */
  guint generation;    /* counts pauses and frame changes: older answers are dropped */
  guint watch_round;   /* counts the watch list's changes, the same way for its answers */
  GHashTable *last;    /* key -> value at the last pause */
  GHashTable *now;     /* the same for this one */
} DebugView;

extern DebugView view;

/* What an answer was asked for. */
typedef struct {
  guint generation;
  guint watch_round; /* the watch rows are made again after an expression is added or removed */
  TcodeVar *parent;  /* NULL: the frame's top level */
  char *prefix;      /* the keys' start */
} Asked;

/* breakpoints.c */
void bplist_on_activated(GtkListBox *box G_GNUC_UNUSED, GtkListBoxRow *row, gpointer data G_GNUC_UNUSED);
void bplist_on_menu(GtkWidget *list, double x, double y, gpointer data G_GNUC_UNUSED);
void bplist_on_remove_all(GtkButton *button G_GNUC_UNUSED, gpointer data G_GNUC_UNUSED);

/* stack.c */
void stack_fill(void);
void stack_on_frame_activated(GtkListBox *box G_GNUC_UNUSED, GtkListBoxRow *row, gpointer data G_GNUC_UNUSED);
void stack_on_menu(GtkWidget *list, double x, double y, gpointer data G_GNUC_UNUSED);
GtkListBoxRow *stack_row_of_frame(int index);

/* variables.c */
void var_ask_children(TcodeVar *var);
void var_asked_free(gpointer data);
Asked *var_asked_new(TcodeVar *parent, const char *prefix);
void var_bind_row(GtkSignalListItemFactory *factory G_GNUC_UNUSED, GtkListItem *item, gpointer data G_GNUC_UNUSED);
gboolean var_changed(const char *key, const char *value);
GListModel *var_children_of(gpointer item, gpointer data G_GNUC_UNUSED);
void var_list_frame(int index);
void var_setup_row(GtkSignalListItemFactory *factory G_GNUC_UNUSED, GtkListItem *item, gpointer data);
void var_show_note(const char *note);
void var_unbind_row(GtkSignalListItemFactory *factory G_GNUC_UNUSED, GtkListItem *item, gpointer data G_GNUC_UNUSED);

/* watch.c */
void watch_evaluate(void);
void watch_on_add(GtkButton *button G_GNUC_UNUSED, gpointer data G_GNUC_UNUSED);
void watch_on_entry(GtkEntry *entry, gpointer data G_GNUC_UNUSED);
gboolean watch_on_entry_key(GtkEventControllerKey *keys G_GNUC_UNUSED, guint key, guint code G_GNUC_UNUSED,
                            GdkModifierType state G_GNUC_UNUSED, GtkWidget *entry);
void watch_remove(const char *expression);
