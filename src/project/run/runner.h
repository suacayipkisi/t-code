/*
 * What the build and run files share (run.c: finding the project, C / C++ builds, the commands
 * for each goal; languages.c: scripts and other languages' projects; tasks.c: .t-code/tasks.json).
 * Only they include it.
 */
#pragma once

#include "app.h"

typedef enum {
  PROJECT_NONE,
  PROJECT_TASKS, /* .t-code/tasks.json or t-code.json */
  PROJECT_CMAKE,
  PROJECT_MESON,
  PROJECT_MAKE,
  PROJECT_FILE, /* a C / C++ file alone */
  PROJECT_SCRIPT,
  PROJECT_RUSTC, /* a Rust file alone */
  PROJECT_CARGO,
  PROJECT_GO,
  PROJECT_NODE,
  PROJECT_MAVEN,
  PROJECT_GRADLE,
} ProjectKind;

typedef enum {
  GOAL_BUILD,
  GOAL_RUN,
  GOAL_DEBUG, /* build, then hand the program to the debugger */
  GOAL_TEST,
} Goal;

typedef struct {
  const char *ext;
  const char *run;   /* the interpreter (its first word is the program), NULL: the file runs itself */
  const char *check; /* Build: checks the syntax without running it, or NULL */
} Script;

typedef struct {
  ProjectKind kind;
  char *root;           /* the folder with the project file, or the file itself (FILE, SCRIPT, RUSTC) */
  const char *file;     /* the current file */
  const Script *script; /* PROJECT_SCRIPT: how it runs */
  char *python;         /* a Python file's project root, or NULL */
  char *interpreter;    /* the Python it runs with (toolchain_python), or NULL: python3 */
  char *cargo_flags;    /* PROJECT_CARGO: the picked features and target (" --features a") */
  char *task_name;      /* PROJECT_TASKS: profile or task name */
} Project;

/* run.c: the nearest directory, from start upwards, that holds one of names (NULL: none); whether
 * file's extension is one of exts. */
char *find_up(const char *start, const char *const *names);
gboolean has_ext(const char *file, const char *const *exts);

/* run.c: the shell command that sends "<verb> <what>" (what already a shell word) to this t-code
 * through the control socket; what (taken), run only when program is installed. */
char *send_verb(const char *verb, const char *what);
char *if_installed(const char *program, char *what);

/* languages.c: how file runs when it is a script (NULL: not one) and its interpreter's program
 * ("go" for "go run"; the caller frees it); a Python file's project root; a file of another language
 * than C / C++ in its project or alone (FALSE: none). */
const Script *script_of(const char *file);
char *script_program(const Script *script);
char *python_root(const char *start);
gboolean find_language_project(Project *p, const char *start);

/* languages.c: the commands for a script, a Rust file alone and another language's project (Cargo,
 * Go, Node, Maven, Gradle); the Node package manager the project uses. */
char *script_command(const Project *p, Goal goal);
char *rustc_command(const char *file, Goal goal);
char *language_command(const Project *p, Goal goal);
const char *node_manager(const char *root);

/* tasks.c: .t-code/tasks.json's (or t-code.json's) profile in root: its name (NULL when it cannot
 * be read), the folder its commands run in (its cwd, else root), and the command for goal. */
char *tasks_name(const char *root);
char *tasks_folder(const char *root);
char *tasks_command(const char *root, Goal goal);
