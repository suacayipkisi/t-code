/*
 * The Memory View: the bytes at an address the user names while paused ("p", "arr", "&n",
 * "0x7ffd1230"), as a hex dump in a read-only editor tab beside the source ("Debug: Open Memory View…",
 * the Debug panel's MEM button): eight bytes a line, their address first, their characters last.
 * It is read again at each pause; the bytes that changed since the last one are listed under it.
 */
#pragma once

#include "app.h"

/* Asks for an expression (the program must be paused), then shows the memory it points to. */
void memory_open(App *t);

/* Shows the memory at expression, evaluated in the paused frame. */
void memory_show(App *t, const char *expression);

/* The program paused: the view, if open, reads its bytes again. */
void memory_update(App *t);

/* Debugging ended: the address is forgotten (another run has other addresses). */
void memory_ended(App *t);

/* The view's text for count bytes read at address, with changed (NULL: none) the bytes the last read
 * had there, the same length, for the lines under it (free it). */
char *memory_dump_text(const char *expression, guint64 address, const guchar *bytes, gsize count,
                       const guchar *changed);
