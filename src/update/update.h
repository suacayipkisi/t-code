/*
 * The new-release check: the latest GitHub release's version against t-code's own.
 */
#pragma once

#include "app.h"

/* Once, a few seconds after a window shows (and only when the setting asks for it): the latest
 * GitHub release's version goes against t-code's; a newer one opens a small window whose button
 * leads to the release page. Also "Check for Updates Now" in the command palette. */
void updates_start(App *t);

/* The same check, right now ("Check for Updates Now"): it asks even when the setting is off; when
 * nothing is newer (or no answer came) a small dialog says so. */
void updates_check_now(App *t);

/* Compares two version strings: TRUE when theirs is newer than ours. */
gboolean newer_version(const char *ours, const char *theirs);

/* What "Check for Updates Now" says when the latest release (its tag, NULL: no answer) is not newer
 * than ours; NULL when it is (the update window shows instead). Free it. */
char *update_check_message(const char *ours, const char *latest);
