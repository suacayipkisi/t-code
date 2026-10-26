/*
 * An editor's symbols (textDocument/documentSymbol), kept for the Outline panel and the
 * breadcrumbs: asked again after the text changes, as a tree whether the server sends one
 * (DocumentSymbol) or a flat list (SymbolInformation, nested here by their ranges). Positions are
 * the editor's own (lines and character offsets), worked out when the answer came.
 */
#pragma once

#include "app.h"

typedef struct Symbols Symbols;

typedef struct Sym Sym;
struct Sym {
  char *name, *detail;
  int kind;                               /* LSP's SymbolKind: 5 class, 6 method, 12 function, 23 struct, ... */
  int line, column, end_line, end_column; /* all of it */
  int name_line, name_column;             /* its name, where Go to goes */
  GPtrArray *children;                    /* Sym */
};

/* Starts keeping a new editor's symbols; the state goes to tab->view->symbols. */
void symbols_attach(Tab *tab);

/* Frees the state (NULL is fine). */
void symbols_free(Symbols *symbols);

/* The text changed or the server has the file again: asked again soon. */
void symbols_update(Tab *tab);

/* The symbols at the top (Sym, in the file's order); NULL before the first answer. */
GPtrArray *symbols_roots(Tab *tab);

/* The symbols around iter, outermost first (the array is the caller's, the Syms are not). */
GPtrArray *symbols_at(Tab *tab, const GtkTextIter *iter);

/* A short mark for a kind: "ƒ" for functions and methods, "C" for classes, ... */
const char *symbols_kind_mark(int kind);
