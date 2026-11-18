/*
 * A control socket connection (internal to src/ipc/): socket.c reads its requests, queues its output
 * and sends it events; requests.c answers its requests.
 */
#pragma once

#include "app.h"

#include <gio/gio.h>

typedef struct Client {
  App *app;
  GSocketConnection *conn;
  GDataInputStream *in; /* its requests */
  GQueue pending;       /* GBytes not written yet */
  gsize backlog;        /* their bytes */
  gboolean writing, failed, subscribed;
  gboolean held; /* a request is answered later (pick, ask): the next ones wait for it */
  int api;       /* the tapi version it said hello with; 0: a legacy connection */
  char *owner;   /* the plugin it said hello for (its id), or NULL: a script, the user */
  int refs;
} Client;

void client_ref(Client *c);
void client_unref(Client *c);

/* Queues output for the client (dropped once it went away). */
void client_send(Client *c, const char *data, gsize size);
/* Text and a line break. */
void send_line(Client *c, const char *text);

/* tapi answers; a legacy connection gets none: "ok" or "ok <value>" ... */
void answer_ok(Client *c, const char *value);
/* ... and "error <code> <message>": code is one word a plugin can test, message is for people. */
void answer_error(Client *c, const char *code, const char *message);

/* Its requests are read again after one that was answered later (handle() returned FALSE). */
void client_resume(Client *c);

/* Turns the connection into an event stream ("subscribe"). */
void client_subscribe(Client *c);

/* Whether the request now being handled came over the socket (its edits and saves are "by":"plugin"). */
extern gboolean socket_handling;

/* One request (requests.c). FALSE when its answer comes later: the connection's next requests wait
 * until client_resume(). */
gboolean handle(Client *c, const char *command, const char *arg);

/* A save asked for over the socket: its did_save says "by":"plugin". */
void socket_saving(App *t, const char *path);
