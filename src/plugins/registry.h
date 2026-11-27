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
#include <json-glib/json-glib.h>

/* Where the index is, unless $TCODE_REGISTRY names another (a URL or a file). Its signature is beside it,
 * at the same place plus ".sig". */
#define REGISTRY_INDEX_URL "https://raw.githubusercontent.com/suacayipkisi/t-code-plugins/main/index.json"

/* The public key (Ed25519, base64) the index is signed with; $TCODE_REGISTRY_KEY names another (a mirror
 * with its own key, the tests). The registry's PUBLIC_KEY file holds the same. */
#define REGISTRY_PUBLIC_KEY "lfYa9p1HxNpytjfrep5NNb2a0Rsu6AWYWPQ+iLL5qHI="

/* A program a plugin needs (its "requires"): to be built, or to run. The plugin, not t-code, says how
 * to install it: t-code shows the command for the system it runs on, to be copied. */
typedef struct {
  char **programs;   /* "program": a name, or several of which one is enough ("latexmk", "tectonic") */
  char *why;         /* "for": what it is needed for; "" */
  gboolean optional; /* "optional": only some of its commands need it */
  char **install;    /* "install": system, command, system, command, ... in the manifest's order
                        ("fedora", "sudo dnf install cargo", "any", "curl … | sh"); may be empty */
} PluginNeed;

void plugin_need_free(PluginNeed *need);

/* obj's "requires" (PluginNeed *; an empty array without one), the broken entries left out with a
 * warning naming who (the plugin). */
GPtrArray *plugin_needs_of(JsonObject *obj, const char *who);

/* The system names an os-release text gives, the most fitting first: its ID, then the words of its
 * ID_LIKE, then "any" ("ubuntu", "debian", "any"); and those of the system t-code runs on, read once
 * (the host's /run/host/os-release in a Flatpak, else /etc/os-release). g_strfreev / not to be freed. */
char **plugin_systems_from(const char *os_release);
char *const *plugin_systems(void);

/* The install command of need that fits systems best, or NULL when it gives none for them. */
const char *plugin_need_command(const PluginNeed *need, char *const *systems);

typedef struct {
  char *id, *name, *description, *version, *api;
  char *provider;   /* who publishes it; the repository's owner when the index does not say */
  char *repository; /* https://github.com/<owner>/<repo>, or file:///... (tests, mirrors) */
  char *commit;     /* 40 hex digits */
  char *path;       /* the plugin's folder inside the repository; "" for its top */
  char *tree;       /* the tree hash, 64 hex digits */
  char **permissions, **categories;
  GPtrArray *needs; /* PluginNeed *: "requires", as in its plugin.json */
  gboolean reviewed;
  /* Its newest commit when this t-code cannot load it (its "versions" row asks for a newer plugin API:
   * the fields above are then the newest one it can), else NULL. */
  char *newer_version, *newer_api;
} RegistryEntry;

void registry_entry_free(RegistryEntry *entry);

/* "someone" for https://github.com/someone/repo; "" for anything else. Newly allocated. */
char *registry_owner(const char *repository);

/* The entries of an index.json (RegistryEntry *); broken entries are left out with a warning. NULL
 * with error when the text is not an index at all. An entry with "versions" (its commits, oldest first)
 * gets the newest commit whose plugin API this t-code speaks (plugins/tapi.h). */
GPtrArray *registry_parse(const char *json, GError **error);

/* The tree hash of dir (see above), or NULL with error. */
char *registry_tree_hash(const char *dir, GError **error);

/* Where entry's commit can be downloaded: GitHub's archive of it, or <repository>/<commit>.tar.gz. */
char *registry_archive_url(const RegistryEntry *entry);

/* Whether signature (index.json.sig's text: base64) is key's (base64) over the index's bytes. */
gboolean registry_signed(const char *index, gsize length, const char *signature, const char *key);

/* Fetches the index and its signature (curl, or files), checks the signature and parses it; done gets the
 * entries, which it owns, and NULL. A checked index is kept in the cache: when the fresh one cannot be had
 * or checked, the kept one comes back with a note saying why (error). Nothing kept: NULL entries and what
 * went wrong (offline, no curl, not signed, not an index). */
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
