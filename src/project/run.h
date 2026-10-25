/*
 * Build (Ctrl+Shift+B), Run (Ctrl+F5), Test (command palette) and build before debugging (F5) in
 * the shell pane. Projects are detected from the current file upwards: CMake, Meson, Make, Cargo,
 * Go, Node, Maven, Gradle, Python, or a task profile.
 */
#pragma once

#include "app.h"

/* Saves changed files, then builds (and with run, runs) the project in the shell. */
void build_project(App *t, gboolean run);

/* The shell command build_project() types (free it). */
char *project_command(App *t, gboolean run);

/* The detected or configured project test command, before sending it to the shell. */
char *project_test_command(App *t);

/* Saves changed files, then runs the project test command in the shell. */
void run_project_tests(App *t);

/* Whether F5 can debug what Build builds: C / C++ (CMake, Make, a file alone; with nothing to build
 * it says so), not another language's file or project, which Ctrl+F5 runs. */
gboolean project_can_debug(App *t);

/* Saves every changed file, then builds the project in the shell for the debugger: once it built,
 * the program goes to debug_launch() (debug/debugger.h). Returns the folder the program is to run
 * in (free it). */
char *build_for_debug(App *t);

/* The folder build_for_debug() gives the debugger for the current project (free it). */
char *project_run_folder(App *t);

/* What Build would build, for the status bar: "CMake: name", "Make: name", "gcc: file.c", "Cargo: name", "python3: file.py". */
char *project_summary(App *t);
