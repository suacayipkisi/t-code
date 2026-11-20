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

/* How F5 debugs the current file's project. */
typedef enum {
  DEBUG_CANNOT, /* another language's: Ctrl+F5 runs it */
  DEBUG_BUILT,  /* C, C++, Rust: build_for_debug() builds it, then the program goes to the debugger */
  DEBUG_PYTHON, /* debugpy runs the file (or module) itself */
  DEBUG_GO,     /* Delve builds and runs the package (or file) itself */
} DebugWay;

typedef struct {
  DebugWay way;
  char *program;    /* PYTHON: the file; GO: the package's folder, or a file alone */
  char *module;     /* PYTHON: "app.cli" when the file is in its project's packages, else NULL */
  char *cwd;        /* PYTHON, GO: where it runs (as Ctrl+F5 runs it) */
  char *python;     /* PYTHON: the project's interpreter, or NULL: python3 */
  char *pythonpath; /* PYTHON: the project's root, which goes into PYTHONPATH, or NULL */
} DebugPlan;

/* How F5 debugs the current file's project (free with debug_plan_clear). */
DebugPlan project_debug_plan(App *t);
void debug_plan_clear(DebugPlan *plan);

/* Saves every changed file, then builds the project in the shell for the debugger: once it built,
 * the program goes to debug_launch() (debug/debugger.h). Returns the folder the program is to run
 * in (free it). */
char *build_for_debug(App *t);

/* The folder build_for_debug() gives the debugger for the current project (free it). */
char *project_run_folder(App *t);

/* What Build would build, for the status bar: "CMake: name", "Make: name", "gcc: file.c", "Cargo: name", "python3: file.py". */
char *project_summary(App *t);
