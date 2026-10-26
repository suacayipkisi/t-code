/*
 * The { } blocks of a C-like text, outside strings and comments (GLib only): what code folding
 * folds and what sticky scroll pins at the editor's top.
 */
#pragma once

#include <glib.h>

typedef struct {
  int head;      /* the line that names the block: the line of its '{', or the line before when the '{'
                   * starts its own line ("void f()" over "{") */
  int open_line; /* the lines of '{' and '}' */
  int close_line;
} Block;

/* The blocks of text in the order of their '{' (an outer block before the ones inside it), closed
 * ones only. */
GArray *blocks_of(const char *text);
