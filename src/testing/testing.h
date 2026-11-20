/*
 * The Testing panel: the project's tests (CTest, pytest, go test, cargo test; see frameworks.h) as a
 * tree of groups (a suite, file, package or module) and tests, each with its last result; ▶ on a row
 * runs it (a CTest test also has a debug button: built, then run under gdb), a double click goes to
 * its source, Run All, Run Failed, Stop and Refresh in the header; a selected test's output shows under
 * the tree. Listing happens only when asked for: the panel shown first, Refresh, after a run (go and
 * cargo compile to list). One command runs at a time, in the background; the list lives in memory and
 * goes with the project.
 */
#pragma once

#include "app.h"

/* Creates the panel (once per process). */
GtkWidget *testing_panel(App *t);

/* The project changed: what runs stops and the list empties (listed again when the panel shows). */
void testing_reset(App *t);

/* Finds the frameworks again and lists their tests. */
void testing_refresh(App *t);

/* Saves the changed editors, then runs every test / those that failed last time. */
void testing_run_all(App *t);
void testing_run_failed(App *t);

/* Stops what runs and drops what waits. */
void testing_stop(void);

/* Whether a command lists or runs tests now. */
gboolean testing_busy(void);

/* The tree as text, for the tests: a group per line, its tests under it indented two spaces with
 * their mark (✓ passed, ✗ failed, … running, - skipped, ○ not run), then "! <framework>: <why>" for
 * those that listed nothing. */
char *testing_text(void);

/* The text the output view shows now. */
char *testing_output_text(void);

/* Builds the CTest project, then debugs the test with id (its command under gdb); a JUnit test goes to
 * the plugin whose "debug" takes its file, with the test's id. FALSE when it cannot be debugged, a
 * command runs or a debugging session goes on. */
gboolean testing_debug_one(const char *id);

/* Debugs the test last debugged again (the debugger's Restart). */
void testing_debug_again(App *t);

/* Opens the file defining the test with id at its line (found in a thread); FALSE when there is none. */
gboolean testing_open_source(const char *id);

/* Whether a test's source is being looked for. */
gboolean testing_finding(void);

/* Selects the row of the test with id (shows its output); FALSE when there is none. */
gboolean testing_select(const char *id);

/* Runs the test with id (its row's ▶); FALSE when there is none. */
gboolean testing_run_one(const char *id);

/* Runs the tests with these ids (NULL-terminated; those listed), once the changed editors are saved;
 * FALSE when none is listed or a command runs. */
gboolean testing_run_ids(const char *const *ids);

/* ▶ in an editor's gutter (marks.c): on each line defining a listed test (see test_lines), in the color
 * of its last result; a click runs the line's tests, a right click offers Run, Debug (a CTest test) and
 * Show in Testing. The column shows only in a file defining listed tests; the lines are found again
 * after an edit and when the list or a result changes. */
typedef struct TestMarks TestMarks;
void testing_marks_attach(Tab *tab);
void testing_marks_free(TestMarks *marks);

/* tab's ▶, for the tests: a line per ▶, "<line> <state> <id>..." (state: passed, failed, running,
 * skipped, "not run"). */
char *testing_marks_text(Tab *tab);
