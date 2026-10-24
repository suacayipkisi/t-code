/*
 * Crash Recovery: journals unsaved editor buffers and recovers them after a crash.
 */
#pragma once

#include "app.h"

/* Schedules a debounced snapshot write for a modified editor tab. */
void recovery_schedule_snapshot(Tab *tab);

/* Immediately writes a recovery journal snapshot for tab. */
void recovery_save_snapshot(Tab *tab);

/* Deletes the recovery journal for path. */
void recovery_discard_snapshot(const char *path);

/* The tab's changes are saved or thrown away (the buffer went back to unmodified, "Don't Save"):
 * a pending snapshot is cancelled and the journal this session wrote is deleted. A journal left
 * by an earlier session stays until its bar's Restore or Discard. */
void recovery_forget(Tab *tab);

/* recovery_forget() for every modified editor: the user chose to lose all unsaved changes. */
void recovery_forget_unsaved(App *t);

/* Checks if an unsaved snapshot exists from a previous session and prompts the user. */
void recovery_check_and_prompt(Tab *tab);
