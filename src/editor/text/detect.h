/*
 * What a file's text says about it, without a display (GLib only, so tests/ can run it): its own
 * indentation, and whether a header is C++.
 */
#pragma once

#include <glib.h>

/* The indentation text uses: tabs when more lines start with
 * a tab than with spaces; else spaces, in the most common step between neighboring lines' indents
 * (2 to 8; *width stays when there is none). Single spaces (" * " in block comments) and blank lines
 * do not count. FALSE without any indented line: the settings stay. */
gboolean detect_indent(const char *text, gssize len, gboolean *tabs, int *width);

/* Whether a header's text is C++ (class, namespace, template, std::, ...) rather than C. */
gboolean is_cxx_text(const char *text, gssize len);
