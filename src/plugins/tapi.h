/*
 * Plugin API versions (docs/plugins.md, "Versions and compatibility"; GLib only): "tapi_<major>" or
 * "tapi_<major>_<minor>", "tapi_1" being "tapi_1_0". Each version holds all the ones before it, so a
 * t-code speaks every version up to its own (TAPI_MAJOR, TAPI_MINOR in application/identity.h).
 *
 * A version is held as one number, TAPI_VERSION(major, minor), so versions compare with < and >.
 */
#pragma once

#include "application/identity.h"

#include <glib.h>

#define TAPI_VERSION(major, minor) ((major) * 1000 + (minor))
#define TAPI_LATEST TAPI_VERSION(TAPI_MAJOR, TAPI_MINOR)

/* The version text names ("tapi_1", "tapi_1_0", "tapi_1_1"); 0 when it is not a version at all. */
int tapi_parse(const char *text);

/* Whether this t-code speaks version (one tapi_parse gave): any from tapi_1 up to its own. */
gboolean tapi_spoken(int version);

/* Whether text names a version newer than this t-code's (FALSE for text that is not a version). */
gboolean tapi_too_new(const char *text);

/* The name of version: "tapi_1" for 1.0, "tapi_1_1" for 1.1. Newly allocated. */
char *tapi_name(int version);
