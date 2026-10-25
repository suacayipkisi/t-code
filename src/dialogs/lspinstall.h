/*
 * The status bar's "No LSP" window: which language server the current file lacks and the commands
 * that install it (lsp/install.c), each with a Copy button; Try Again starts the servers that are
 * there now.
 */
#pragma once

#include "app.h"

/* Opens the window for server (the program, "pylsp"), replacing one already open. */
void show_lsp_install(App *t, const char *server);
