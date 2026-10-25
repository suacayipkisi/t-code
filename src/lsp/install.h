/*
 * How to install a language server that is not on PATH (GLib only, so tests/ can run it without a
 * display): the commands for the user's distribution, to copy into a terminal. t-code itself
 * installs nothing.
 */
#pragma once

#include <glib.h>

/* The text of the system's os-release (/run/host/os-release inside Flatpak, else /etc/os-release),
 * or NULL. */
char *install_os_release(void);

/* The distribution family of an os-release text, from its ID and ID_LIKE: "fedora", "debian"
 * (Ubuntu and Mint too), "arch", "void", or NULL for any other and for NULL. */
const char *install_distro(const char *os_release);

/* What server (the program t-code runs: "pylsp", "gopls", ...) is, for people: "the Python language
 * server (pylsp)". NULL for a program t-code does not run. */
const char *install_title(const char *server);

/* The commands that install server on distro (install_distro's, NULL for unknown): the
 * distribution's package first, then the language's own installer (pipx, go, rustup, npm, a
 * download). Unknown distributions get every distribution's package. NULL for an unknown server. */
GStrv install_commands(const char *server, const char *distro);

/* What to know before running the commands ("npm comes with Node.js."), or NULL. */
const char *install_note(const char *server);
