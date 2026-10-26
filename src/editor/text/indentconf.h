/*
 * What a project's config files say about a file's indentation (GLib only, so tests/ can run it
 * without a display): .editorconfig (indent_style, indent_size, tab_width), then .clang-format
 * (UseTab, IndentWidth, BasedOnStyle) for C and C++ files.
 */
#pragma once

#include <glib.h>

/* An indentation as far as it is known. */
typedef struct {
  int tabs;  /* 1 tabs, 0 spaces, -1 not said */
  int width; /* columns per level, 0 when not said */
} IndentStyle;

/* What .editorconfig files say about file: the sections that match it, the nearer file and the
 * later section winning, up to the one with "root = true". */
void indent_from_editorconfig(const char *file, IndentStyle *style);

/* What the nearest .clang-format (or _clang-format) says, for a C or C++ file: its Cpp section,
 * with clang-format's defaults for what it leaves out. */
void indent_from_clang_format(const char *file, IndentStyle *style);

/* Both: .editorconfig first, .clang-format for what it leaves open (style starts as not said).
 * Returns ".editorconfig" when that said anything, else ".clang-format" when that did, else NULL. */
const char *indent_from_config(const char *file, IndentStyle *style);
