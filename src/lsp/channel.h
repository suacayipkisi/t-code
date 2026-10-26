/*
 * Language server messages over a pair of streams, the way LSP frames them:
 * "Content-Length: <n>\r\n\r\n" then n bytes of JSON. Reads and writes are asynchronous; writes
 * wait their turn in a queue. lsp/client/client.c talks to the server's stdin / stdout through one.
 */
#pragma once

#include <gio/gio.h>

typedef struct Channel Channel;

struct Channel {
  int refs; /* the owner's, and one per read or write in flight */
  GInputStream *in;
  GOutputStream *out;
  GObject *keep; /* the connection or process the streams belong to */
  GCancellable *cancel;
  GByteArray *buf; /* read, not yet a whole message */
  char chunk[65536];
  GQueue writes; /* GBytes waiting their turn */
  GBytes *writing;
  gboolean closed;
  void (*message)(gpointer owner, const char *body);
  void (*on_closed)(gpointer owner); /* the other side went: the owner calls channel_close() */
  gpointer owner;
};

/* A channel over in and out, which belong to keep (the process, held until the channel goes). The
 * owner sets message and on_closed, then calls channel_read(). */
Channel *channel_new(GInputStream *in, GOutputStream *out, GObject *keep, gpointer owner);

/* Starts reading; the owner's callbacks get what comes. */
void channel_read(Channel *ch);

/* Sends one message (body is its JSON). */
void channel_send(Channel *ch, const char *body);

/* By the owner: drops its reference; no on_closed. */
void channel_close(Channel *ch);
