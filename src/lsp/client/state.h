/*
 * What the language server client's files share (client.c: requests and what the editors call;
 * positions.c: LSP positions and ranges; documents.c: keeping the server's copy of each file in step;
 * server.c: starting, answering and stopping a server). Only they include it (and the tests).
 */
#pragma once

#include "lsp/client/client.h"
#include "lsp/channel.h"
#include "lsp/servers.h"

#ifndef IDLE_SECONDS /* tests set a shorter one */
#define IDLE_SECONDS 30
#endif
#ifndef FLUSH_MS
#define FLUSH_MS 50 /* changes typed within this go in one didChange */
#endif
#ifndef PULL_MS
#define PULL_MS 300 /* a server that is asked for its diagnostics: this long after the last change */
#endif
#ifndef MAX_OPEN_DOCS
#define MAX_OPEN_DOCS 5 /* per-server LRU of open documents */
#endif
#define MAX_CRASHES 3

/* A compile database written (make, cmake) is told to the server this long after its last change. */
#define DATABASE_MS 1500

/* An open editor, as the server knows it. */
typedef struct {
  Tab *tab;
  Lsp *lsp;
  int version;
  guint64 use;      /* LRU order: bumped when opened, shown, edited or queried */
  gboolean opened;  /* didOpen went to the running server */
  GString *changes; /* contentChanges not sent yet, comma separated */
  guint flush_id;
  guint pull_id;            /* asks a server with pull diagnostics (textDocument/diagnostic) for them */
  gboolean include_missing; /* clangd's last diagnostics had an #include it cannot find */
} Doc;

/* A request waiting for its answer. */
typedef struct {
  Lsp *lsp;
  gint64 id;
  Tab *tab; /* NULL for initialize */
  LspReply reply;
  gpointer data;
  GCancellable *cancel;
  gulong cancelled_id;
  gboolean cancelled; /* its answer, if it comes first, is not given */
} Pending;

struct Lsp {
  gatomicrefcount refs;
  ServerKind kind;
  char *workspace; /* the project folder it runs in; fixed: a project switch stops every server (lsp_stop) */
  App *app;
  GSubprocess *process;
  Channel *ch;
  gboolean ready; /* initialize answered */
  gboolean utf16; /* positions count UTF-16 units, not UTF-8 bytes */
  gboolean stopping;
  JsonNode *capabilities;
  GHashTable *pending; /* id -> Pending */
  gint64 next_id;
  GHashTable *docs; /* Tab -> Doc */
  guint64 use;      /* next LRU stamp for docs */
  guint idle_id, restart_id;
  int crashes;
  gboolean missing;       /* the server is not installed: not tried again */
  GFileMonitor *watch[2]; /* the root and its build/ folder, for a compile database written */
  GString *written;       /* didChangeWatchedFiles changes not sent yet, comma separated */
  guint written_id;
  GHashTable *progress; /* $/progress token -> Progress, while that work runs */
  char *progress_last;  /* the token that spoke last */
};

/* Work a server reports with $/progress. */
typedef struct {
  char *title; /* the begin's: "Indexing" */
  char *text;  /* for the status bar: "Indexing 45% (12/40)" */
} Progress;

/* client.c */
void lsp_fail_pending(Lsp *l, Tab *tab);
void lsp_finish(Pending *p, JsonNode *result);
void lsp_notify(Lsp *l, const char *method, const char *params);
void lsp_send(Lsp *l, Tab *tab, const char *method, const char *params, GCancellable *cancel, LspReply reply,
              gpointer data);

/* documents.c */
void doc_close_on_server(Doc *doc);
void doc_free(Doc *doc);
Doc *doc_of(Tab *tab);
void doc_flush(Doc *doc);
void doc_on_delete(GtkTextBuffer *buffer G_GNUC_UNUSED, const GtkTextIter *start, const GtkTextIter *end, Doc *doc);
void doc_on_insert(GtkTextBuffer *buffer G_GNUC_UNUSED, const GtkTextIter *where, const char *text, int len, Doc *doc);
void doc_pull_later(Doc *doc);
void doc_send(Doc *doc);

/* positions.c */
char *lsp_range_json(Lsp *l, const GtkTextIter *start, const GtkTextIter *end);

/* server.c */
gboolean lsp_idle_stop(gpointer data);
Lsp *lsp_of(Tab *tab);
void lsp_server_start(Lsp *l);
void lsp_server_stop(Lsp *l);
