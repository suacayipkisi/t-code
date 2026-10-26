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

/* Walks the code of a C-like text: comments, strings and character literals are skipped. Start with
 * { text, text, 0, FALSE }. */
typedef struct {
  const char *p;          /* the next character to look at */
  const char *line_start; /* the start of the line of the character code_next() returned */
  int line;               /* that line, from 0 */
  gboolean comment;       /* inside a block comment */
} CodeScan;

/* The next character of code, or NULL at the end. */
const char *code_next(CodeScan *scan);

/* The blocks of text in the order of their '{' (an outer block before the ones inside it), closed
 * ones only. */
GArray *blocks_of(const char *text);
