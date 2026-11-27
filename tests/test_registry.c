/*
 * Tests for the plugin registry (src/plugins/registry.c): the index, the tree hash (the same value
 * the registry's CI computes: tools/tree_hash.py in t-code-plugins), plugins.json's records, and
 * installing from an archive (curl reads file:// URLs, so no network is needed).
 */
#define _DEFAULT_SOURCE /* symlink() */
#include "plugins/registry.h"
#include "plugins/ed25519.h"
#include "ed25519_sign.h"
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

/* "requires": a program or several, what for, optional, install commands by system; broken entries
 * left out with a warning. The command that fits a system: its ID, then its ID_LIKE, then "any". */
static void test_needs(void)
{
  JsonNode *root = json_from_string(
      "{\"requires\": ["
      " {\"program\": \"cargo\", \"for\": \"building it\", \"install\": {\"fedora\": \"sudo dnf install cargo\","
      "  \"debian\": \"sudo apt install cargo\", \"any\": \"curl https://sh.rustup.rs | sh\", \"arch\": 3}},"
      " {\"program\": [\"latexmk\", \"tectonic\"], \"optional\": true},"
      " {\"program\": \"bin/x\"},"
      " {\"program\": [\"ok\", 2]},"
      " {\"program\": \"git\", \"install\": \"dnf install git\"},"
      " \"hunspell\""
      "]}",
      NULL);
  GPtrArray *needs;
  const PluginNeed *need;

  g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING, "*test: \"requires\" entry 3 left out: its \"program\"*");
  g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING, "*test: \"requires\" entry 4 left out: its \"program\"*");
  g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING, "*test: \"requires\" entry 5 left out: its \"install\"*");
  g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING, "*test: \"requires\" entry 6 left out: its \"program\"*");
  needs = plugin_needs_of(json_node_get_object(root), "test");
  g_test_assert_expected_messages();
  g_assert_cmpuint(needs->len, ==, 2);
  need = g_ptr_array_index(needs, 0);
  g_assert_cmpstr(need->programs[0], ==, "cargo");
  g_assert_null(need->programs[1]);
  g_assert_cmpstr(need->why, ==, "building it");
  g_assert_false(need->optional);
  g_assert_cmpuint(g_strv_length(need->install), ==, 6); /* arch's number left out */

  char **ubuntu = plugin_systems_from("NAME=\"Ubuntu\"\nID=ubuntu\nID_LIKE=debian\n");
  char **mint = plugin_systems_from("ID=linuxmint\nID_LIKE=\"ubuntu debian\"\n");
  char **fedora = plugin_systems_from("ID='fedora'\n");
  char **alpine = plugin_systems_from("ID=alpine\n");
  char **none = plugin_systems_from(NULL);
  g_assert_cmpstr(ubuntu[0], ==, "ubuntu");
  g_assert_cmpstr(ubuntu[1], ==, "debian");
  g_assert_cmpstr(ubuntu[2], ==, "any");
  g_assert_cmpstr(plugin_need_command(need, ubuntu), ==, "sudo apt install cargo");
  g_assert_cmpstr(plugin_need_command(need, mint), ==, "sudo apt install cargo");
  g_assert_cmpstr(plugin_need_command(need, fedora), ==, "sudo dnf install cargo");
  g_assert_cmpstr(plugin_need_command(need, alpine), ==, "curl https://sh.rustup.rs | sh");
  g_assert_cmpstr(none[0], ==, "any");

  need = g_ptr_array_index(needs, 1);
  g_assert_cmpstr(need->programs[1], ==, "tectonic");
  g_assert_true(need->optional);
  g_assert_cmpstr(need->why, ==, "");
  g_assert_null(plugin_need_command(need, fedora));
  g_assert_nonnull(plugin_systems()[0]);

  g_strfreev(ubuntu);
  g_strfreev(mint);
  g_strfreev(fedora);
  g_strfreev(alpine);
  g_strfreev(none);
  g_ptr_array_unref(needs);
  json_node_unref(root);
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
  int entries; /* fetched: how many, -1 for none */
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
  char *script = g_build_filename(from, "b-0123456789abcdef0123456789abcdef01234567", "plugins", "fix", "run.sh", NULL);
  run((const char *const[]){ "touch", "-d", "@1000000000", script, NULL }); /* its commit's time: 2001 */
  g_free(script);
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
  GStatBuf st;
  g_assert_cmpint(g_stat(installed, &st), ==, 0);
  g_assert_cmpint(st.st_mtime, >, (gint64)time(NULL) - 600); /* dated now: a build going by times sees it new */
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

  /* the plugin's folder a link out of the archive (to a copy that hashes the same): refused */
  const char *linked = "2222222222222222222222222222222222222222";
  char *link_root = g_build_filename(tmp_dir, "linked", "b-2222222222222222222222222222222222222222", "plugins", NULL);
  char *target =
      g_build_filename(tmp_dir, "archive", "b-0123456789abcdef0123456789abcdef01234567", "plugins", "fix", NULL);
  char *link = g_build_filename(link_root, "fix", NULL), *linked_from = g_build_filename(tmp_dir, "linked", NULL);
  g_mkdir_with_parents(link_root, 0755);
  GFile *link_file = g_file_new_for_path(link);
  g_assert_true(g_file_make_symbolic_link(link_file, target, NULL, NULL));
  g_object_unref(link_file);
  top = g_strdup_printf("%s/%s.tar.gz", repo, linked);
  run((const char *const[]){ "tar", "-czf", top, "-C", linked_from, "b-2222222222222222222222222222222222222222",
                             NULL });
  e.commit = (char *)linked;
  e.path = "plugins/fix";
  error = install(&e, plugins);
  g_assert_nonnull(strstr(error, "is a link"));
  g_free(error);

  /* outside the tests only https: a file:// repository is not downloaded */
  g_unsetenv("TCODE_TESTING");
  e.commit = (char *)commit;
  error = install(&e, plugins);
  g_assert_nonnull(strstr(error, "Could not download"));
  g_free(error);
  g_setenv("TCODE_TESTING", "1", TRUE);
  g_free(top);
  g_free(linked_from);
  g_free(link);
  g_free(target);
  g_free(link_root);

  left = g_dir_open(cache, 0, NULL);
  g_assert_null(left ? g_dir_read_name(left) : NULL);
  if (left)
    g_dir_close(left);
  g_free(cache);
  g_free(plugins);
  g_free(repository);
  g_free(repo);
}

/* Ed25519 (RFC 8032): signatures made by the registry's tools/sign_index.py (itself checked against
 * OpenSSL) with the seed 00 01 .. 1f; anything changed fails. */
static void test_ed25519(void)
{
  static const char *const key = "A6EHv/POEL4dcN0Y50vAmWfk1jCbpQ1fHdyGZBJVMbg=";
  static const char *const signatures[] = {
    "9ca53579530654d5c3df77089ef45eda613e2fedf670e96bedac4639504e5845ef4b95d5793077233dd16817b2532e9c5525872a73a4ad74b759369a9e05c102",
    "cc46d62d3754f41754b27b6ea2cb2c272bafa7a5a1f6062bd060f414e50caaeac2da66ad39cef4424a90236ea907b7d8057e3443dc5abfc9986967ee7213a407",
    "e6a71601e3a52680061f33fb9042fc5ce45933ea1f707f4740cd4e818e2efb28d9b81348df9cd3e8d050805360c3dff0228ed889e160f0763f31f1646e8e8e08",
  };
  /* L, the group's order, little-endian: S + L is the same signature, written differently */
  static const guint8 order[32] = { 0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58,       0xd6,
                                    0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14, [31] = 0x10 };
  GString *long_message = g_string_new(NULL);
  gsize key_length;
  guint8 *public_key = g_base64_decode(key, &key_length), bad_key[32];

  for (int i = 0; i < 150; i++)
    g_string_append(long_message, "t-code ");
  const char *messages[] = { "", "abc", long_message->str };
  for (int m = 0; m < 3; m++) {
    guint8 signature[64];
    for (int i = 0; i < 64; i++)
      sscanf(signatures[m] + 2 * i, "%2hhx", &signature[i]);
    const guint8 *text = (const guint8 *)messages[m];
    gsize length = strlen(messages[m]);
    g_assert_true(ed25519_verify(signature, text, length, public_key));
    if (length) {
      guint8 *changed = g_memdup2(text, length);
      changed[length - 1] ^= 1;
      g_assert_false(ed25519_verify(signature, changed, length, public_key));
      g_free(changed);
    }
    signature[3] ^= 0x10; /* R */
    g_assert_false(ed25519_verify(signature, text, length, public_key));
    signature[3] ^= 0x10;
    signature[40] ^= 0x01; /* S */
    g_assert_false(ed25519_verify(signature, text, length, public_key));
    signature[40] ^= 0x01;
    int carry = 0; /* S + L */
    for (int i = 0; i < 32; i++) {
      int sum = signature[32 + i] + order[i] + carry;
      signature[32 + i] = sum & 0xff;
      carry = sum >> 8;
    }
    g_assert_false(ed25519_verify(signature, text, length, public_key));
  }
  memcpy(bad_key, public_key, 32);
  bad_key[0] ^= 0x01; /* another key, or no point at all */
  guint8 signature[64];
  for (int i = 0; i < 64; i++)
    sscanf(signatures[0] + 2 * i, "%2hhx", &signature[i]);
  g_assert_false(ed25519_verify(signature, NULL, 0, bad_key));

  /* RFC 8032 7.1: TEST 1, 2, 3 and SHA(abc) (the message is abc's SHA-512) */
  /* clang-format off */
  static const struct { const char *key, *message, *signature; } rfc[] = {
    { "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", "",
      "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b" },
    { "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c", "72",
      "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00" },
    { "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025", "af82",
      "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a" },
    { "ec172b93ad5e563bf4932c70e1245034c35467ef2efd4d64ebf819683467e2bf",
      "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f",
      "dc2a4459e7369633a52b1bf277839a00201009a3efbf3ecb69bea2186c26b58909351fc9ac90b3ecfdfbc7c66431e0303dca179c138ac17ad9bef1177331a704" },
  };
  /* clang-format on */
  for (gsize v = 0; v < G_N_ELEMENTS(rfc); v++) {
    guint8 vector_key[32], vector_signature[64], message[64];
    gsize length = strlen(rfc[v].message) / 2;
    for (int i = 0; i < 32; i++)
      sscanf(rfc[v].key + 2 * i, "%2hhx", &vector_key[i]);
    for (int i = 0; i < 64; i++)
      sscanf(rfc[v].signature + 2 * i, "%2hhx", &vector_signature[i]);
    for (gsize i = 0; i < length; i++)
      sscanf(rfc[v].message + 2 * i, "%2hhx", &message[i]);
    g_assert_true(ed25519_verify(vector_signature, message, length, vector_key));
    vector_signature[0] ^= 0x01;
    g_assert_false(ed25519_verify(vector_signature, message, length, vector_key));
  }

  /* a key is spelled one way only. The neutral point (y = 1) as the key makes R = S B: S = 0 and R = the
   * neutral point check for any message. Spelled as y + p, or with a sign bit on x = 0, it is refused. */
  guint8 neutral_signature[64] = { 0x01 };
  memset(bad_key, 0, 32);
  bad_key[0] = 0x01;
  g_assert_true(ed25519_verify(neutral_signature, (const guint8 *)"abc", 3, bad_key));
  bad_key[31] = 0x80;
  g_assert_false(ed25519_verify(neutral_signature, (const guint8 *)"abc", 3, bad_key));
  memset(bad_key, 0xff, 32);
  bad_key[0] = 0xee;
  bad_key[31] = 0x7f;
  g_assert_false(ed25519_verify(neutral_signature, (const guint8 *)"abc", 3, bad_key));

  /* registry_signed: base64 texts, spaces and a newline around them allowed */
  const char *index = "{\"format\": 1, \"plugins\": []}\n";
  const char *index_signature =
      "H/jHxgz4lwJM7AJwy5P7K5qTcxw6Z8eHtSm7tD+ELR1hQOX6iy58dsM7t93TWhkBGXGa3yt4sD9bcyNaUmwkCQ==\n";
  g_assert_true(registry_signed(index, strlen(index), index_signature, key));
  g_assert_false(registry_signed(index, strlen(index) - 1, index_signature, key));
  g_assert_false(registry_signed(index, strlen(index), index_signature, REGISTRY_PUBLIC_KEY));
  g_assert_false(registry_signed(index, strlen(index), "", key));
  g_assert_false(registry_signed(index, strlen(index), "bm90IGEgc2lnbmF0dXJl", key)); /* 15 bytes */
  g_assert_false(registry_signed(index, strlen(index), NULL, key));
  /* the tests' own signing (ed25519_sign.h) makes the same signatures, and good ones */
  char *made = test_sign(index, strlen(index));
  g_assert_cmpstr(made, ==, "H/jHxgz4lwJM7AJwy5P7K5qTcxw6Z8eHtSm7tD+ELR1hQOX6iy58dsM7t93TWhkBGXGa3yt4sD9bcyNaUmwkCQ==");
  g_free(made);
  for (int n = 0; n < 20; n++) {
    char *text = g_strdup_printf("%d %s", n, n % 2 ? long_message->str : "");
    made = test_sign(text, strlen(text));
    g_assert_true(registry_signed(text, strlen(text), made, TEST_REGISTRY_KEY));
    g_free(made);
    g_free(text);
  }
  g_free(public_key);
  g_string_free(long_message, TRUE);
}

/* $TCODE_REGISTRY names a file: its entries come back when its signature (the file + ".sig") is good. A
 * checked index is kept: when the next one is not signed right, the kept one comes back with a note. */
static void fetched(GPtrArray *entries, const char *error, gpointer data)
{
  Result *r = data;

  r->done = TRUE;
  g_set_str(&r->error, error);
  r->entries = entries ? (int)entries->len : -1;
  if (entries)
    g_ptr_array_unref(entries);
}

static void fetch_from(Result *r, const char *where)
{
  g_setenv("TCODE_REGISTRY", where, TRUE);
  r->done = FALSE;
  registry_fetch(fetched, r);
  run_until(finished, r);
}

static void test_fetch(void)
{
  const char *signature = "H/jHxgz4lwJM7AJwy5P7K5qTcxw6Z8eHtSm7tD+ELR1hQOX6iy58dsM7t93TWhkBGXGa3yt4sD9bcyNaUmwkCQ==\n";
  char *index = write_at(tmp_dir, "index.json", "{\"format\": 1, \"plugins\": []}\n", -1, 0644);
  char *signature_file = write_at(tmp_dir, "index.json.sig", signature, -1, 0644);
  char *other = write_at(tmp_dir, "other.json", "{\"format\": 1, \"plugins\": []}\n", -1, 0644);
  Result r = { 0 };

  g_setenv("TCODE_REGISTRY_KEY", "A6EHv/POEL4dcN0Y50vAmWfk1jCbpQ1fHdyGZBJVMbg=", TRUE);
  fetch_from(&r, index);
  g_assert_null(r.error);
  g_assert_cmpint(r.entries, ==, 0); /* an empty list, checked */

  /* changed on the way: the kept copy, saying why */
  g_assert_true(g_file_set_contents(index, "{\"format\": 1, \"plugins\": [], \"x\": 2}\n", -1, NULL));
  fetch_from(&r, index);
  g_assert_cmpint(r.entries, ==, 0);
  g_assert_nonnull(strstr(r.error, "Showing the registry as of"));
  g_assert_nonnull(strstr(r.error, "signature does not match"));
  /* signed again: no note */
  g_assert_true(g_file_set_contents(
      signature_file, "buI+d2fT3hbX3hwy2f5AyCmi4H/KM6ecoYKTIxL9lCxBY0RpQwSiNvBi6om6UQlAlwwVoD01uBDeoO851iTPCw==", -1,
      NULL));
  fetch_from(&r, index);
  g_assert_null(r.error);
  /* t-code's own key does not sign the test index */
  g_unsetenv("TCODE_REGISTRY_KEY");
  fetch_from(&r, other);
  g_assert_cmpint(r.entries, ==, -1);
  g_assert_nonnull(strstr(r.error, "without its signature"));
  fetch_from(&r, "/nowhere/index.json");
  g_assert_cmpint(r.entries, ==, -1);
  g_assert_nonnull(r.error);

  g_free(r.error);
  g_unsetenv("TCODE_REGISTRY");
  g_free(other);
  g_free(signature_file);
  g_free(index);
}

int main(int argc, char **argv)
{
  tmp_dir = g_dir_make_tmp("t-code-test-registry-XXXXXX", NULL);
  char *config = g_build_filename(tmp_dir, "config", NULL), *cache = g_build_filename(tmp_dir, "cache", NULL);
  g_setenv("XDG_CONFIG_HOME", config, TRUE);
  g_setenv("XDG_CACHE_HOME", cache, TRUE);
  g_setenv("TCODE_TESTING", "1", TRUE); /* file:// repositories */
  g_free(config);
  g_free(cache);
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/registry/parse", test_parse);
  g_test_add_func("/registry/needs", test_needs);
  g_test_add_func("/registry/tree-hash", test_tree_hash);
  g_test_add_func("/registry/records", test_records);
  g_test_add_func("/registry/install", test_install);
  g_test_add_func("/registry/ed25519", test_ed25519);
  g_test_add_func("/registry/fetch", test_fetch);

  int status = g_test_run();

  const char *rm[] = { "rm", "-rf", tmp_dir, NULL };
  run(rm);
  g_free(tmp_dir);
  return status;
}
