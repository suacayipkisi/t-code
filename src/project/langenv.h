/*
 * A project's language environment, without GTK: its Python virtual environments, its Cargo
 * features, and what the chosen ones (project/toolchain.c) become for rust-analyzer and cargo.
 */
#pragma once

#include <glib.h>

/* The Python interpreters to pick from for a project: the venvs right in root (a folder with
 * pyvenv.cfg and bin/python; .venv first, then venv, then the others by name), $VIRTUAL_ENV's and
 * python3 on PATH, each once. */
GPtrArray *langenv_interpreters(const char *root);

/* How to name an interpreter: "python (.venv)" for /p/.venv/bin/python, else its file's name. */
char *langenv_interpreter_name(const char *python);

/* The python of root's own venv (the first of langenv_interpreters' venvs), or NULL. Reads only
 * root's folder. */
char *langenv_venv_python(const char *root);

/* The features a Cargo.toml declares in its [features] table, in its order ("default" left out);
 * NULL when it cannot be read. */
GStrv langenv_cargo_features(const char *cargo_toml);

/* features is NULL or "" (the default ones), "all", or a comma-separated list; target is NULL or ""
 * (the host's) or a target triple. */

/* rust-analyzer's initializationOptions for them ({"cargo": {...}}), or NULL when both are the
 * defaults. */
char *langenv_rust_options(const char *features, const char *target);

/* cargo's flags for them, each with a space before it (" --features a,b --target x"), shell-quoted;
 * "" for the defaults. */
char *langenv_cargo_flags(const char *features, const char *target);
