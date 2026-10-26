/*
 * The parameter hint: while the cursor is in a call's arguments, the function's
 * signature shows above it with the argument being typed in bold, and its documentation. It opens on
 * "(" and ",", follows the typing, and closes outside the call or on Escape.
 */
#pragma once

#include "app.h"

typedef struct SignatureHelp SignatureHelp;

/* Adds the hint to a new editor. */
void signature_attach(Tab *tab);

/* Asks for the hint at the cursor now (a function was completed; Ctrl+Shift+Space). */
void signature_show(Tab *tab);

/* Frees an editor's hint. NULL is fine. */
void signature_free(SignatureHelp *help);
