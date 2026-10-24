/*
 * The project's Python interpreter and Rust features and target: picked from the command palette,
 * kept per project in the session, used by the language servers (lsp/client.c) and by Build, Run
 * and Test (project/run.c). Picking one restarts that language's server.
 */
#pragma once

#include "app.h"

/* The project's Python: the picked interpreter while it exists, else root's own venv's python
 * (root: a Python file's project root, or NULL for the project's). NULL when neither: python3 is
 * used then. */
char *toolchain_python(App *t, const char *root);

/* The picked Cargo features (NULL: the default ones, "all", or "a,b") and target (NULL: the host). */
char *toolchain_rust_features(App *t);
char *toolchain_rust_target(App *t);

/* Keeps the project's "python", "rust-features" or "rust-target" (NULL or "": the default) and
 * restarts that language's server a moment later (features are switched one by one). */
void toolchain_set(App *t, const char *key, const char *value);

/* "Python: Select Interpreter…": the project's venvs, $VIRTUAL_ENV's and python3. */
void toolchain_pick_python(App *t);

/* "Rust: Select Features…": the default ones, all, or each of Cargo.toml's switched on and off. */
void toolchain_pick_features(App *t);

/* "Rust: Select Target…": the host or an installed target (rustup target list --installed). */
void toolchain_pick_target(App *t);
