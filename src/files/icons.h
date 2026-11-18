/*
 * The Files panel's file icons, GLib-only: which of t-code's own SVG icons (assets/icons/dark and
 * light, built into the binary as resources) a file or folder gets, by file name or extension.
 */
#pragma once

#include <glib.h>

/* Where the icons are in the resources: an icon theme resource path (files under scalable/apps). */
#define FILE_ICONS_RESOURCE "/io/github/suacayipkisi/tcode/icons"

/* An icon's names in the icon theme: name[FALSE] for dark color themes, name[TRUE] for light ones. */
typedef struct {
  const char *name[2];
} FileIcon;

/* The icon for a file called name, or for a folder (open or closed). */
const FileIcon *file_icon(const char *name, gboolean dir, gboolean open);
