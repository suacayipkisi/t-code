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
 * defaults. In a folder not trusted (project/trust.h) build scripts, proc macros and the check on
 * save are switched off: each runs the project's code. */
char *langenv_rust_options(const char *features, const char *target, gboolean trusted);

/* cargo's flags for them, each with a space before it (" --features a,b --target x"), shell-quoted;
 * "" for the defaults. */
char *langenv_cargo_flags(const char *features, const char *target);

/* The nearest folder, from start up to top, that holds one of names as a file (NULL: none). top
 * comes from langenv_climb_top: root (the project folder) when start is in it, else the home folder
 * when start is in it, else start itself, so a file in /tmp/x never takes /tmp's Makefile. */
char *langenv_find_up(const char *start, const char *const *names, const char *top);
const char *langenv_climb_top(const char *start, const char *root);
