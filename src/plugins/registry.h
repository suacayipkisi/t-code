/*
 * The plugin registry (docs/plugins.md, "The registry"; GLib / GIO only): its index.json, installing
 * a plugin from the commit it pins, and what plugins.json remembers about the plugins installed from
 * it (their commit, whether it was reviewed, which warnings were shown).
 *
 * A pin is checked by its tree hash: the sha256 of one line per regular file of the plugin's folder,
 * sorted by path: "<file's sha256> <x or -> <path>\n" (x: executable). Symbolic links and other
 * special files make a folder unfit. The registry's CI computes the same.
 */
#pragma once

#include <gio/gio.h>

/* Where the index is, unless $TCODE_REGISTRY names another (a URL or a file). */
#define REGISTRY_INDEX_URL "https://raw.githubusercontent.com/suacayipkisi/t-code-plugins/main/index.json"

typedef struct {
  char *id, *name, *description, *version, *api;
  char *repository; /* https://github.com/<owner>/<repo>, or file:///... (tests, mirrors) */
  char *commit;     /* 40 hex digits */
  char *path;       /* the plugin's folder inside the repository; "" for its top */
  char *tree;       /* the tree hash, 64 hex digits */
  char **permissions, **categories;
  gboolean reviewed;
} RegistryEntry;

void registry_entry_free(RegistryEntry *entry);

/* The entries of an index.json (RegistryEntry *); broken entries are left out with a warning. NULL
 * with error when the text is not an index at all. */
GPtrArray *registry_parse(const char *json, GError **error);

/* The tree hash of dir (see above), or NULL with error. */
char *registry_tree_hash(const char *dir, GError **error);

/* Where entry's commit can be downloaded: GitHub's archive of it, or <repository>/<commit>.tar.gz. */
char *registry_archive_url(const RegistryEntry *entry);

/* Fetches the index (curl, or a file) and parses it; done gets the entries (NULL and an error message
 * when it failed: offline, no curl, not an index), which it owns. */
typedef void (*RegistryFetched)(GPtrArray *entries, const char *error, gpointer data);
void registry_fetch(RegistryFetched done, gpointer data);

/* Downloads entry's commit, checks its tree hash and its plugin.json's id, and puts the plugin at
 * plugins_dir/<id> (replacing an older one). done gets NULL, or what went wrong. */
typedef void (*RegistryInstalled)(const char *error, gpointer data);
void registry_install(const RegistryEntry *entry, const char *plugins_dir, RegistryInstalled done, gpointer data);

/* The warnings an unreviewed plugin has had. */
typedef enum {
  REGISTRY_WARNED_INSTALL = 1 << 0,
  REGISTRY_WARNED_ENABLE = 1 << 1,
  REGISTRY_WARNED_RUN = 1 << 2,
} RegistryWarned;

/* What plugins.json's "registry" keeps for a plugin installed from the registry. */
typedef struct {
  char *repository, *commit;
  gboolean reviewed;
  RegistryWarned warned;
} RegistryRecord;

void registry_record_free(RegistryRecord *record);

/* The record for id (NULL: installed by hand, or not at all); registry_set_record replaces it, or
 * removes it when record is NULL, keeping plugins.json's other members. */
RegistryRecord *registry_record(const char *id);
void registry_set_record(const char *id, const RegistryRecord *record);
