/*
 * The control socket plugins, scripts and tools send requests to (the protocol, tapi_1, is
 * docs/plugins.md's): one per line, or each ended by a NUL byte. A connection that says
 * "hello tapi_1" gets an "ok" / "error <code> <message>" answer to every request; one that does not
 * is answered as before the API had a version. "subscribe" keeps the connection open for
 * did_open/did_save JSON-lines events. Its path is in $TCODE_SOCKET for every program t-code starts.
 */
#pragma once

#include "app.h"

/* Listens on $XDG_RUNTIME_DIR/t-code-<pid>.sock (t->paths.socket). */
void start_socket(App *t);
/* Sends JSON-lines did_open and did_save events to connected "subscribe" clients. */
void socket_did_open(App *t, const char *path);
void socket_did_save(App *t, const char *path);

/* Stops listening and removes the socket file. */
void stop_socket(App *t);
