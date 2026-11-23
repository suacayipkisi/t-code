/*
 * What a file's text says about it, without a display (GLib only, so tests/ can run it): its own
 * indentation, whether a header is C++, whether a .m file is Objective-C or MATLAB, and the web
 * links in a line.
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

/* Whether the .m file at path (text: its text, NULL when not read) is Objective-C rather than MATLAB:
 * #import, @interface or @implementation at a line's start say so; a line starting with % or
 * "function" says MATLAB; otherwise its folder decides (a .mm file, or a header of the same name). */
gboolean is_objc_m(const char *path, const char *text);

/* The web link (http://, https://, ftp://, file://, mailto: or www.) in line (len bytes, -1: up to
 * its NUL) that covers the byte at index: its bytes [*start, *end), without the punctuation that
 * ends a sentence or closes brackets around it. FALSE: none there. */
gboolean url_at(const char *line, gssize len, int index, int *start, int *end);
