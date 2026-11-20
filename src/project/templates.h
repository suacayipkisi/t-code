/*
 * File templates (GLib only): a file saved with a name and a short description, offered again when a
 * new file of its kind is made. Each one is a folder in ~/.config/t-code/templates holding the file
 * and template.ini ([template] name, description, file).
 */
#pragma once

#include <glib.h>

typedef struct {
  char *dir;         /* its folder */
  char *name;        /* what the user called it */
  char *description; /* "" when none was given */
  char *file;        /* the saved file's name (main.c): the new file's name at first */
} Template;

void template_free(Template *tpl);

/* ~/.config/t-code/templates (free it). */
char *templates_dir(void);

/* The templates in root, sorted by name (free the array: it frees them). Folders without a readable
 * template.ini are left out. */
GPtrArray *templates_list(const char *root);

/* Whether tpl suits a new file named file_name (a path or a name): the same extension, any case
 * (main.c for x.c), or the same name when neither has one (Makefile, .gitignore). */
gboolean template_matches(const Template *tpl, const char *file_name);

/* Saves text (length bytes; -1: up to its end) as file_name's template called name, replacing one
 * already called that (any case). FALSE with an error when name is empty or writing fails. */
gboolean templates_save(const char *root, const char *name, const char *description, const char *file_name,
                        const char *text, gssize length, GError **error);

/* Removes tpl's folder (its file and template.ini). */
gboolean template_delete(const Template *tpl, GError **error);

/* The saved file's text (free it); NULL with an error when it cannot be read. */
char *template_contents(const Template *tpl, gsize *length, GError **error);
