/*
 * The Process Monitor window ("Help: Process Monitor"): t-code and every program it runs (plugins,
 * language servers, gdb, terminal tabs and what runs in them) with CPU and memory, totalled by owner
 * on top, so a slow machine can be told apart: t-code itself, or one of its programs. Each process
 * but t-code can be ended (SIGTERM). It looks again every 2 s while it is open.
 */
#pragma once

#include "app.h"

/* Opens the window, or brings it to the front. */
void show_processes(App *t);
