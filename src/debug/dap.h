/*
 * Talking to a debugger with the Debug Adapter Protocol (DAP): gdb runs as
 * "gdb -i dap", gets requests ({"seq": 3, "type": "request", "command": "next", "arguments": {...}})
 * on its stdin, framed like the language server's messages (lsp/channel.c), answers each one and
 * sends events of its own ("stopped", "exited", "output"). GLib and json-glib only.
 */
#pragma once

#include <json-glib/json-glib.h>

typedef struct Dap Dap;

/* The answer to a request: its body (may be NULL) when it worked, else error says why. Called once,
 * and never after dap_free(). */
typedef void (*DapReply)(gpointer data, JsonNode *body, const char *error);

/* An event with its body (may be NULL); event NULL: the debugger went away (the owner then calls
 * dap_free()). */
typedef void (*DapEvent)(gpointer owner, const char *event, JsonNode *body);

/* Starts the debugger (argv) in cwd with env (NULL: t-code's) and sends it "initialize"; NULL with
 * error set when it cannot start. */
Dap *dap_start(const char *const *argv, const char *cwd, char **env, DapEvent on_event, gpointer owner, GError **error);

/* Sends command with arguments (a JSON object, or NULL for none); reply (may be NULL) gets the
 * answer. */
void dap_request(Dap *d, const char *command, const char *arguments, DapReply reply, gpointer data);

/* The same; destroy (may be NULL) frees data after the reply, or when the answer is dropped (dap_free). */
void dap_request_full(Dap *d, const char *command, const char *arguments, DapReply reply, gpointer data,
                      GDestroyNotify destroy);

/* What the debugger said it can do (the answer to initialize: "supportsStepBack", ...), or NULL
 * before it answered. */
JsonNode *dap_capabilities(Dap *d);

/* Stops the debugger (killed, if it still runs) and frees it; answers still to come are dropped.
 * NULL is fine. */
void dap_free(Dap *d);

/* The debugger's pid; 0 once it exited. */
int dap_pid(Dap *d);
