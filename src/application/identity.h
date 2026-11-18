/*
 * Which t-code this is: the stable one or the dev variant (make DEV=1) that sits next to it.
 * Plain macros, no GLib or GTK, so any module (GLib-only ones too) can include it.
 *   TCODE_NAME          config / state / cache folder, socket and data folder names
 *   TCODE_DISPLAY_NAME  the About window's program name
 *   TCODE_APP_ID        the GtkApplication ID and the icon name
 *   TCODE_DEV           1 in the dev variant: no update check at start-up
 *   TAPI_LATEST         the newest plugin API version spoken ("tapi_1" .. "tapi_<TAPI_LATEST>",
 *                       each holding all the ones before it: docs/plugins.md)
 * The Makefile defines them for the dev variant; tests and the stable build get these defaults.
 */
#pragma once

#ifndef TCODE_NAME
#define TCODE_NAME "t-code"
#endif
#ifndef TCODE_DISPLAY_NAME
#define TCODE_DISPLAY_NAME "t-code"
#endif
#ifndef TCODE_APP_ID
#define TCODE_APP_ID "io.github.suacayipkisi.tcode"
#endif
#ifndef TCODE_DEV
#define TCODE_DEV 0
#endif

#define TAPI_LATEST 1
