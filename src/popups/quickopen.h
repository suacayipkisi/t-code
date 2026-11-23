/*
 * Go to File (Ctrl+P, the title bar's search): open a file by typing part of its name. Text starting
 * with '#' finds commands instead (the command palette, Ctrl+Shift+P, opens it with "# " typed).
 */
#pragma once

#include "app.h"

/* Opens the picker on the project's files. */
void quick_open_show(App *t);

/* The same with text typed: COMMANDS_PREFIX (window/commands.h) opens it on the commands. */
void quick_open_show_text(App *t, const char *text);
