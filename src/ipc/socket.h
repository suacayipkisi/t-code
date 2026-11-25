/*
 * The control socket plugins, scripts and tools send requests to (the protocol, tapi_1, is
 * docs/plugins.md's): one per line, or each ended by a NUL byte. A connection that says
 * "hello tapi_1" gets an "ok" / "error <code> <message>" answer to every request; one that does not
 * is answered as before the API had a version. "subscribe" keeps the connection open for JSON-lines
 * events. Its path is in $TCODE_SOCKET for every program t-code starts.
 */
#pragma once

#include "app.h"

/* Listens on $XDG_RUNTIME_DIR/t-code-<pid>.sock (t->paths.socket). */
void start_socket(App *t);
/* Events for "subscribe" clients (one JSON line each; nothing is done when nobody listens): a file
 * loaded, saved, its tab closed ... */
void socket_did_open(App *t, const char *path);
void socket_did_save(App *t, const char *path);
void socket_did_close(App *t, const char *path);
/* ... its text changed (sent once typing pauses) ... */
void socket_did_change(App *t, const char *path);
/* ... another file became the active tab ("" when the active tab is not an editor) ... */
void socket_active_changed(App *t, const char *path);
/* ... another project opened ... */
void socket_project_changed(App *t);
/* ... a line of a plugin's view was activated (double click, Enter; line from 1), the view's tab was
 * closed by the user (plugins/views.h). */
void socket_view_activated(App *t, const char *owner, const char *id, int line, const char *text);
void socket_view_closed(App *t, const char *owner, const char *id);
/* ... the text typed into a view's input (Enter), with the mode picked beside it (NULL: no modes); one
 * of a view prompt's choices was picked; the view's Stop was pressed. */
void socket_view_input(App *t, const char *owner, const char *id, const char *text, const char *mode);
void socket_view_choice(App *t, const char *owner, const char *id, const char *prompt, const char *choice);
void socket_view_stop(App *t, const char *owner, const char *id);

/* Stops listening and removes the socket file. */
void stop_socket(App *t);
