/*
 * Ending the programs t-code started, and only those (GLib-only). A program a terminal tab runs is
 * a session leader (VTE starts it in a new session); its helpers stay in that session even when
 * they make their own process group (MATLAB's figure windows do), and a helper that starts a new
 * session is still its descendant. Nothing is ever found by name: a process gets a signal only
 * while its start time is still the one seen, through a pidfd (no pid reuse race).
 */
#pragma once

#include <glib.h>

typedef struct Children Children;

/* The session leader pid that t-code started; NULL when it is not running. */
Children *children_new(int leader);

/* Sends sig (0: none) to the leader, every process in its session, their descendants and every
 * process seen in an earlier call that still runs; returns how many were found (zombies left out).
 * Processes orphaned since the last call stay counted. */
int children_signal(Children *c, int sig);

void children_free(Children *c);
