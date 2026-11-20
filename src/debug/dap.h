/*
 * Talking to a debugger with the Debug Adapter Protocol (DAP): gdb runs as
 * "gdb -i dap", gets requests ({"seq": 3, "type": "request", "command": "next", "arguments": {...}})
 * on its stdin, framed like the language server's messages (lsp/channel.c), answers each one and
 * sends events of its own ("stopped", "exited", "output"). The same goes for lldb-dap, debugpy and
 * Delve. GLib and json-glib only.
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

/* A request of the debugger's own (runInTerminal) with its arguments: the answer's body (a JSON object,
 * freed by the caller), or NULL with *error set (freed too) when it is refused. */
typedef char *(*DapHandle)(gpointer owner, const char *command, JsonNode *arguments, char **error);

/* Starts the debugger (argv) in cwd with env (NULL: t-code's) and sends it "initialize" naming it
 * adapter_id ("gdb", "lldb-dap", "debugpy", "go"); on_request (NULL: every request of its own is refused)
 * gets its requests, and then "runInTerminal" is said to work. An argument holding "{socket}" makes it
 * a debugger that dials in (dlv dap --client-addr {socket}): that becomes "unix:<path>" of a socket
 * t-code listens on, and the messages go over it instead of stdin / stdout. tty (may be NULL) becomes the
 * debugger's controlling terminal, in a session of its own (Delve puts its programs in the foreground
 * there). NULL with error set when it cannot start. */
Dap *dap_start(const char *const *argv, const char *cwd, char **env, const char *tty, const char *adapter_id,
               DapEvent on_event, DapHandle on_request, gpointer owner, GError **error);

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
