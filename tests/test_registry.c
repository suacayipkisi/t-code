/*
 * Tests for the plugin registry (src/plugins/registry.c): the index, the tree hash (the same value
 * the registry's CI computes: tools/tree_hash.py in t-code-plugins), plugins.json's records, and
 * installing from an archive (curl reads file:// URLs, so no network is needed).
 */
#define _DEFAULT_SOURCE /* symlink() */
#include "plugins/registry.h"
#include "util.h"

#include <glib/gstdio.h>
#include <string.h>
#include <unistd.h>

static char *tmp_dir;

/* The tree both sides hash to the value below. */
#define FIXTURE_TREE "00fcf7657b9e4b8639a3e83513e16d5ffc14a89fb93c05c8835f7fc708a25d79"

static char *fixture(const char *top)
{
  char *dir = g_build_filename(tmp_dir, top, NULL);

  g_free(write_at(dir, "lib/a.txt", "a\n", -1, 0644));
  g_free(write_at(dir, "plugin.json", "{\"name\": \"Fix\", \"id\": \"fix\"}\n", -1, 0644));
  g_free(write_at(dir, "run.sh", "#!/bin/sh\necho hi\n", -1, 0755));
  return dir;
}

static void run(const char *const *argv)
{
  g_assert_true(g_spawn_sync(NULL, (char **)argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, NULL, NULL));
}

/* An index with good and broken entries: the broken ones are left out with a warning. */
static void test_parse(void)
{
  const char *json =
      "{\"format\": 1, \"plugins\": [\n"
      " {\"id\": \"fix\", \"name\": \"Fix\", \"provider\": \"Fix Folks\", \"version\": \"1.0\", \"api\": \"tapi_1\", "
      "\"repository\": "
      "\"https://github.com/a/b\", \"commit\": \"fedcba9876543210fedcba9876543210fedcba98\", \"tree-sha256\": "
      "\"" FIXTURE_TREE "\"},\n"
      " {\"id\": \"ok\", \"repository\": \"https://github.com/a/b\", \"commit\": "
      "\"0123456789abcdef0123456789abcdef01234567\", \"tree-sha256\": \"" FIXTURE_TREE "\", \"path\": \"sub/ok\", "
      "\"reviewed\": true, \"permissions\": [\"edit\"], \"categories\": [\"linters\", 3]},\n"
      " {\"id\": \"Bad\", \"repository\": \"https://github.com/a/b\"},\n"
      " {\"id\": \"evil\", \"repository\": \"https://example.com/a/b\"},\n"
      " {\"id\": \"short\", \"repository\": \"https://github.com/a/b\", \"commit\": \"abc\"},\n"
      " {\"id\": \"up\", \"repository\": \"https://github.com/a/b\", \"commit\": "
      "\"0123456789abcdef0123456789abcdef01234567\", \"tree-sha256\": \"" FIXTURE_TREE "\", \"path\": \"../x\"},\n"
      " {\"id\": \"ok\", \"repository\": \"https://github.com/a/b\", \"commit\": "
      "\"0123456789abcdef0123456789abcdef01234567\", \"tree-sha256\": \"" FIXTURE_TREE "\"}\n"
      "]}";
  GError *error = NULL;
  GPtrArray *entries;
  RegistryEntry *e;

  g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING, "*Bad left out: its id*");
  g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING, "*evil left out: its repository*");
  g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING, "*short left out: its commit*");
  g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING, "*up left out: its path leaves*");
  g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING, "*ok is listed twice*");
  entries = registry_parse(json, &error);
  g_test_assert_expected_messages();
  g_assert_no_error(error);
  g_assert_cmpuint(entries->len, ==, 2);
  e = g_ptr_array_index(entries, 0);
  g_assert_cmpstr(e->id, ==, "fix");
  g_assert_false(e->reviewed);
  g_assert_cmpstr(e->path, ==, "");
  g_assert_cmpstr(e->provider, ==, "Fix Folks");
  char *url = registry_archive_url(e);
  g_assert_cmpstr(url, ==, "https://github.com/a/b/archive/fedcba9876543210fedcba9876543210fedcba98.tar.gz");
  g_free(url);
  e = g_ptr_array_index(entries, 1);
  g_assert_cmpstr(e->name, ==, "ok");    /* the id when there is no name */
  g_assert_cmpstr(e->provider, ==, "a"); /* the repository's owner when there is no provider */
  g_assert_true(e->reviewed);
  g_assert_cmpstr(e->path, ==, "sub/ok");
  g_assert_cmpstr(e->permissions[0], ==, "edit");
  g_assert_cmpstr(e->categories[0], ==, "linters");
  g_assert_null(e->categories[1]);
  g_ptr_array_unref(entries);

  char *owner = registry_owner("file:///srv/mirror");
  g_assert_cmpstr(owner, ==, "");
  g_free(owner);

  g_assert_null(registry_parse("{\"plugin\": []}", &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_clear_error(&error);
  g_assert_null(registry_parse("not json", &error));
  g_assert_nonnull(error);
  g_clear_error(&error);
}

/* The tree hash: the fixed value, another one when the executable bit or a byte changes, and no hash
 * for a folder with a symbolic link. */
static void test_tree_hash(void)
{
  char *dir = fixture("tree"), *hash, *path;
  GError *error = NULL;

  hash = registry_tree_hash(dir, &error);
  g_assert_no_error(error);
  g_assert_cmpstr(hash, ==, FIXTURE_TREE);
  g_free(hash);

  path = g_build_filename(dir, "run.sh", NULL);
  g_chmod(path, 0644);
  hash = registry_tree_hash(dir, NULL);
  g_assert_cmpstr(hash, !=, FIXTURE_TREE);
  g_free(hash);
  g_chmod(path, 0755);
  g_free(path);

  path = g_build_filename(dir, "lib", "link", NULL);
  g_assert_cmpint(symlink("/etc/passwd", path), ==, 0);
  g_assert_null(registry_tree_hash(dir, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_REGULAR_FILE);
  g_clear_error(&error);
  g_unlink(path);
  g_free(path);
  g_free(dir);
}

/* plugins.json's "registry": a record comes back as it was kept, the file's other members stay, and
 * NULL removes it. */
static void test_records(void)
{
  char *path = g_build_filename(tmp_dir, "config", "t-code", "plugins.json", NULL), *text = NULL;
  RegistryRecord kept = { .repository = "https://github.com/a/b", .commit = "abc", .warned = REGISTRY_WARNED_RUN };
  RegistryRecord *r;

  g_free(write_at(tmp_dir, "config/t-code/plugins.json", "{\"disabled\": [\"x\"]}", -1, 0644));
  g_assert_null(registry_record("fix"));
  registry_set_record("fix", &kept);
  r = registry_record("fix");
  g_assert_cmpstr(r->repository, ==, "https://github.com/a/b");
  g_assert_cmpstr(r->commit, ==, "abc");
  g_assert_false(r->reviewed);
  g_assert_cmpint(r->warned, ==, REGISTRY_WARNED_RUN);
  r->reviewed = TRUE;
  r->warned |= REGISTRY_WARNED_INSTALL;
  registry_set_record("fix", r);
  registry_record_free(r);
  r = registry_record("fix");
  g_assert_true(r->reviewed);
  g_assert_cmpint(r->warned, ==, REGISTRY_WARNED_RUN | REGISTRY_WARNED_INSTALL);
  registry_record_free(r);
  g_assert_true(g_file_get_contents(path, &text, NULL, NULL));
  g_assert_nonnull(strstr(text, "\"disabled\""));
  g_free(text);
  registry_set_record("fix", NULL);
  g_assert_null(registry_record("fix"));
  g_free(path);
}

typedef struct {
  gboolean done;
  char *error;
} Result;

static void on_installed(const char *error, gpointer data)
{
  Result *r = data;

  r->done = TRUE;
  r->error = g_strdup(error);
}

static gboolean finished(gconstpointer data)
{
  return ((const Result *)data)->done;
}

/* Installs entry into plugins and waits; the error, or NULL. */
static char *install(RegistryEntry *entry, const char *plugins)
{
  Result r = { 0 };

  registry_install(entry, plugins, on_installed, &r);
  run_until(finished, &r);
  return r.error;
}

/* An archive like GitHub's (one "<repo>-<commit>" folder) at repository/<commit>.tar.gz: installed
 * when its tree hash matches (from a subfolder too, replacing an older one), refused when it does
 * not, when its plugin.json names another id, or when the download is not there. Nothing is left in
 * the cache. */
static void test_install(void)
{
  char *repo = g_build_filename(tmp_dir, "repo", NULL), *repository = g_strconcat("file://", repo, NULL);
  char *plugins = g_build_filename(tmp_dir, "plugins", NULL), *installed, *error, *top;
  char *cache = g_build_filename(tmp_dir, "cache", "t-code", NULL);
  const char *commit = "0123456789abcdef0123456789abcdef01234567";
  RegistryEntry e = { .id = "fix", .repository = repository, .path = "plugins/fix" };
  GDir *left;

  g_free(fixture("archive/b-0123456789abcdef0123456789abcdef01234567/plugins/fix"));
  g_mkdir_with_parents(repo, 0755);
  top = g_strdup_printf("%s/%s.tar.gz", repo, commit);
  char *from = g_build_filename(tmp_dir, "archive", NULL);
  run((const char *const[]){ "tar", "-czf", top, "-C", from, "b-0123456789abcdef0123456789abcdef01234567", NULL });
  g_free(from);
  g_free(top);

  e.commit = (char *)commit;
  e.tree = FIXTURE_TREE;
  g_free(write_at(plugins, "fix/old-file", "from before\n", -1, 0644));
  error = install(&e, plugins);
  g_assert_null(error);
  installed = g_build_filename(plugins, "fix", "run.sh", NULL);
  g_assert_true(g_file_test(installed, G_FILE_TEST_IS_EXECUTABLE));
  g_free(installed);
  installed = g_build_filename(plugins, "fix", "old-file", NULL);
  g_assert_false(g_file_test(installed, G_FILE_TEST_EXISTS)); /* the older one is gone */
  g_free(installed);

  e.tree = "1111111111111111111111111111111111111111111111111111111111111111";
  error = install(&e, plugins);
  g_assert_nonnull(strstr(error, "does not match"));
  g_free(error);

  e.tree = FIXTURE_TREE;
  e.id = "other";
  error = install(&e, plugins);
  g_assert_nonnull(strstr(error, "another id"));
  g_free(error);
  installed = g_build_filename(plugins, "other", NULL);
  g_assert_false(g_file_test(installed, G_FILE_TEST_EXISTS));
  g_free(installed);

  e.id = "fix";
  e.path = "nowhere";
  error = install(&e, plugins);
  g_assert_nonnull(strstr(error, "No plugin.json"));
  g_free(error);

  e.commit = "ffffffffffffffffffffffffffffffffffffffff";
  error = install(&e, plugins);
  g_assert_nonnull(strstr(error, "Could not download"));
  g_free(error);

  left = g_dir_open(cache, 0, NULL);
  g_assert_null(left ? g_dir_read_name(left) : NULL);
  if (left)
    g_dir_close(left);
  g_free(cache);
  g_free(plugins);
  g_free(repository);
  g_free(repo);
}

/* $TCODE_REGISTRY names a file: its entries come back; a missing one says so. */
static void fetched(GPtrArray *entries, const char *error, gpointer data)
{
  Result *r = data;

  r->done = TRUE;
  r->error = g_strdup(error);
  g_assert_true((entries != NULL) == (error == NULL));
  if (entries) {
    g_assert_cmpuint(entries->len, ==, 0);
    g_ptr_array_unref(entries);
  }
}

static void test_fetch(void)
{
  char *index = write_at(tmp_dir, "index.json", "{\"plugins\": []}", -1, 0644);
  Result r = { 0 };

  g_setenv("TCODE_REGISTRY", index, TRUE);
  registry_fetch(fetched, &r);
  run_until(finished, &r);
  g_assert_null(r.error);
  r.done = FALSE;
  g_setenv("TCODE_REGISTRY", "/nowhere/index.json", TRUE);
  registry_fetch(fetched, &r);
  run_until(finished, &r);
  g_assert_nonnull(r.error);
  g_free(r.error);
  g_unsetenv("TCODE_REGISTRY");
  g_free(index);
}

int main(int argc, char **argv)
{
  tmp_dir = g_dir_make_tmp("t-code-test-registry-XXXXXX", NULL);
  char *config = g_build_filename(tmp_dir, "config", NULL), *cache = g_build_filename(tmp_dir, "cache", NULL);
  g_setenv("XDG_CONFIG_HOME", config, TRUE);
  g_setenv("XDG_CACHE_HOME", cache, TRUE);
  g_free(config);
  g_free(cache);
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/registry/parse", test_parse);
  g_test_add_func("/registry/tree-hash", test_tree_hash);
  g_test_add_func("/registry/records", test_records);
  g_test_add_func("/registry/install", test_install);
  g_test_add_func("/registry/fetch", test_fetch);

  int status = g_test_run();

  const char *rm[] = { "rm", "-rf", tmp_dir, NULL };
  run(rm);
  g_free(tmp_dir);
  return status;
}
