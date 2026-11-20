/*
 * What the Testing panel (testing.c) and the editors' ▶ (marks.c) share; included by these two only.
 */
#pragma once

#include "testing/frameworks.h"
#include "testing/testing.h"

/* The tests listed for the project (TestCase, the panel's), or NULL while there is no panel. */
GPtrArray *testing_list(void);

/* Whether a test can be debugged: a CTest test's command under gdb, a JUnit test through the plugin whose
 * "debug" takes its file (the Java plugin's java-debug). */
gboolean testing_can_debug(const TestCase *test);

/* The list or a test's result changed: every editor finds its tests' lines again. */
void testing_marks_changed(void);
