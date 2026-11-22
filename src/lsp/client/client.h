/*
 * Language servers are created lazily per language and project: clangd for C/C++, plus pylsp,
 * gopls, rust-analyzer, typescript-language-server (JavaScript too), the JSON language server,
 * yaml-language-server, marksman and neocmakelsp when installed. Open editors are kept in sync with their server
 * change by change; what it says comes back as diagnostics (lsp/inline/diagnostics.c). Other modules ask
 * it things (hover, completion, definition) with lsp_request().
 */
#pragma once

#include "app.h"

#include "lsp/json.h"

/* Documents ---------------------------------------------------------------------------------- */

/* An editor's file was loaded (or loaded again): its language server gets it, when supported;
 * optional servers must be installed, and read-only old versions are not sent. */
void lsp_open(Tab *tab);

/* The editor was shown: marks it most recently used, and re-opens it on the server if it was
 * evicted from the open-document LRU pool. */
void lsp_touch(Tab *tab);

/* The editor was saved. */
void lsp_saved(Tab *tab);

/* The editor goes: the server forgets the file; requests still waiting for it get NULL. */
void lsp_close(Tab *tab);

/* Whether the server has the editor's file open (it can answer about it). */
gboolean lsp_has(Tab *tab);

/* Stops all language servers when the project closes or t-code quits. */
void lsp_stop(App *t);

/* Starts the server again with the open files (its settings changed, or the user asked: it reads
 * the compile flags again); one that gave up after crashing or was not found is tried again. */
void lsp_restart(App *t);
/* Restarts the server of language ("python", "go", "rust"): its project settings changed. */
void lsp_restart_language(App *t, const char *language);
/* Restarts only clangd, for clang-specific settings and compile-flag hints. */
void lsp_restart_clangd(App *t);

/* Why clangd cannot find an open file's #include, for the status bar: the C library's headers are
 * not installed (problem "No C headers"), or no compilation database applies to the file ("No
 * compile flags", naming a misspelled one like "compile-flags.txt"); what to do about it, else NULL.
 * problem is set only with a hint. */
char *lsp_flags_hint(App *t, const char **problem);

/* Whether an editor's file is routed to an LSP (optional servers must be installed). */
gboolean lsp_wants(Tab *tab);

/* The program of the language server an editor's file would use, when it is not installed
 * ("pylsp"), else NULL (also for tab NULL, a tab that is no editor or a file no server reads). */
const char *lsp_missing_server(Tab *tab);

/* What a language server is busy with, for the status bar: "rust-analyzer: Indexing 45% (12/40)",
 * "pylsp: starting", "typescript-language-server: did not start"; the current editor's server
 * first. NULL when none is busy. */
char *lsp_progress(App *t);

/* Why a language server did not start (it answered initialize with an error), with what may help;
 * NULL when none failed. The current file's server first. */
char *lsp_failure(App *t);

/* After the user installed a server: tries the ones that were not found again and opens the
 * editors that now have one. */
void lsp_open_installed(App *t);

/* Which server serves which file changed (a built-in one turned on or off in Settings, plugins
 * reloaded): every editor whose server is no longer its own leaves it (its diagnostics go) and opens
 * with the right one, or with none. */
void lsp_reattach(App *t);

/* Requests ----------------------------------------------------------------------------------- */

/* The answer to a request: its "result", or NULL (an error, no server, the editor went, or it was
 * cancelled). Called exactly once. */
typedef void (*LspReply)(JsonNode *result, gpointer data);

/* Sends a request about an editor's file; params is the JSON object of its parameters (see
 * lsp_position_params). Changes not sent yet go first. cancel (may be NULL) drops it: reply gets
 * NULL (right after the cancel signal, or with the answer if that comes first). */
void lsp_request(Tab *tab, const char *method, const char *params, GCancellable *cancel, LspReply reply, gpointer data);

/* The server's capability at path ("completionProvider.triggerCharacters"), or NULL before it has
 * answered initialize. */
JsonNode *lsp_capability(Tab *tab, const char *path);

/* Positions ---------------------------------------------------------------------------------- */

/* '"textDocument": {"uri": ...}, "position": {...}' for iter, to go inside a request's params. */
char *lsp_position_params(Tab *tab, const GtkTextIter *iter);

/* '"textDocument": {"uri": ...}' alone. */
char *lsp_document_params(Tab *tab);

/* '{"start": ..., "end": ...}' of the text from start to end, in the server's positions. */
char *lsp_range(Tab *tab, const GtkTextIter *start, const GtkTextIter *end);

/* Whether the tab's server counts positions in UTF-16 units (else in UTF-8 bytes). */
gboolean lsp_utf16(Tab *tab);

/* The place in the buffer of an LSP position ({"line": ., "character": .}), kept inside the text. */
void lsp_iter_at(Tab *tab, JsonNode *position, GtkTextIter *iter);
/* Like lsp_iter_at(), with the originating server's position encoding. */
void lsp_iter_at_encoding(Tab *tab, JsonNode *position, GtkTextIter *iter, gboolean utf16);

/* "file:///a%20b.c" -> "/a b.c" and back; a URI given an alias goes to its file and back. */
char *lsp_uri_to_path(const char *uri);
char *lsp_path_to_uri(const char *path);

/* The file path holds the text of uri, a document a server keeps (jdt://...): the server hears of the
 * file as uri, and uri means the file, until t-code quits. Its read-only editor gets the server too. */
void lsp_uri_alias(const char *path, const char *uri);

/* Whether path holds the text of such a URI. */
gboolean lsp_is_alias(const char *path);

/* Asks tab's server for the text of uri, a document in a scheme other than file, with the request
 * its plugin names for that scheme ("uri-contents"); reply gets a string (or NULL). FALSE (reply is
 * not called) when the server names none. */
gboolean lsp_request_contents(Tab *tab, const char *uri, LspReply reply, gpointer data);

/* Every running language server's pid goes into owners (application/processes.h) as
 * "Language server: clangd". */
void lsp_own_processes(App *t, GHashTable *owners);
