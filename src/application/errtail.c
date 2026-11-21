/*
 * The end of a program's stderr (see errtail.h).
 */
#include "application/errtail.h"

#include <string.h>

struct ErrTail {
  int refs; /* the caller's (until err_tail_finish / err_tail_free), the read in flight's, the timer's */
  GInputStream *in;
  GCancellable *cancel;
  GString *text;
  gboolean ended;
  ErrTailDone done; /* set by err_tail_finish */
  gpointer data;
  guint wait_id;
};

static void tail_unref(ErrTail *t)
{
  if (--t->refs > 0)
    return;
  g_clear_handle_id(&t->wait_id, g_source_remove);
  g_object_unref(t->in);
  g_object_unref(t->cancel);
  g_string_free(t->text, TRUE);
  g_free(t);
}

/* Keeps the last ERR_TAIL_BYTES, cut at a line start when there is one. */
static void tail_keep_tail(GString *text)
{
  if (text->len <= ERR_TAIL_BYTES)
    return;
  gsize cut = text->len - ERR_TAIL_BYTES;
  const char *newline = memchr(text->str + cut, '\n', text->len - cut);
  if (newline && (gsize)(newline - text->str) + 1 < text->len)
    cut = newline - text->str + 1;
  g_string_erase(text, 0, cut);
}

static char *tail_trimmed(GString *text)
{
  char *copy = g_strdup(text->str);
  /* a cut in the middle of a UTF-8 sequence or invalid bytes: still text for the log */
  if (!g_utf8_validate(copy, -1, NULL)) {
    char *valid = g_utf8_make_valid(copy, -1);
    g_free(copy);
    copy = valid;
  }
  return g_strstrip(copy);
}

/* Reports once: the stream ended, or the wait ran out. */
static void tail_report(ErrTail *t)
{
  ErrTailDone done = t->done;

  if (!done)
    return;
  t->done = NULL;
  if (t->wait_id) { /* the stream ended first: the timer's reference goes (the caller still holds one) */
    g_source_remove(t->wait_id);
    t->wait_id = 0;
    t->refs--;
  }
  char *text = tail_trimmed(t->text);
  done(text, t->data);
  g_free(text);
  g_cancellable_cancel(t->cancel); /* nothing more is wanted */
}

static void tail_read_more(ErrTail *t);

static void tail_on_read(GObject *source, GAsyncResult *result, gpointer data)
{
  ErrTail *t = data;
  GBytes *bytes = g_input_stream_read_bytes_finish(G_INPUT_STREAM(source), result, NULL);
  gsize size = 0;
  const char *chunk = bytes ? g_bytes_get_data(bytes, &size) : NULL;

  if (chunk && size > 0 && !g_cancellable_is_cancelled(t->cancel)) {
    g_string_append_len(t->text, chunk, size);
    tail_keep_tail(t->text);
    tail_read_more(t); /* its own reference */
  } else {
    t->ended = TRUE;
    tail_report(t);
  }
  if (bytes)
    g_bytes_unref(bytes);
  tail_unref(t);
}

static void tail_read_more(ErrTail *t)
{
  t->refs++;
  g_input_stream_read_bytes_async(t->in, 4096, G_PRIORITY_DEFAULT, t->cancel, tail_on_read, t);
}

ErrTail *err_tail_new(GInputStream *in)
{
  ErrTail *t = g_new0(ErrTail, 1);

  t->refs = 1;
  t->in = g_object_ref(in);
  t->cancel = g_cancellable_new();
  t->text = g_string_new(NULL);
  tail_read_more(t);
  return t;
}

char *err_tail_text(ErrTail *t)
{
  return tail_trimmed(t->text);
}

static gboolean tail_on_wait(gpointer data)
{
  ErrTail *t = data;

  t->wait_id = 0;
  tail_report(t);
  tail_unref(t);
  return G_SOURCE_REMOVE;
}

void err_tail_finish(ErrTail *t, guint wait_ms, ErrTailDone done, gpointer data)
{
  t->done = done;
  t->data = data;
  if (t->ended) {
    tail_report(t);
  } else {
    t->refs++;
    t->wait_id = g_timeout_add(wait_ms, tail_on_wait, t);
  }
  tail_unref(t);
}

void err_tail_free(ErrTail *t)
{
  if (!t)
    return;
  t->done = NULL;
  g_cancellable_cancel(t->cancel);
  tail_unref(t);
}

char *err_tail_last_line(const char *text)
{
  const char *end = text + strlen(text), *start;

  while (end > text && g_ascii_isspace(end[-1]))
    end--;
  for (start = end; start > text && start[-1] != '\n';)
    start--;
  return g_strstrip(g_strndup(start, end - start));
}
