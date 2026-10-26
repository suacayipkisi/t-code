/*
 * Tests for project/langenv.c (GLib only): the Python interpreters of a project, Cargo.toml's
 * features, and rust-analyzer's options and cargo's flags for the picked ones.
 */
#include "project/langenv.h"
#include "util.h"

#include <glib/gstdio.h>

static char *root;

/* Writes text to root/name, making its folders; executable when exec. */
static void put(const char *name, const char *text, gboolean exec)
{
  g_free(write_at(root, name, text, -1, exec ? 0755 : 0));
}

static void make_venv(const char *dir)
{
  char *config = g_build_filename(dir, "pyvenv.cfg", NULL), *python = g_build_filename(dir, "bin/python", NULL);

  put(config, "home = /usr/bin\n", FALSE);
  put(python, "#!/bin/sh\n", TRUE);
  g_free(python);
  g_free(config);
}

static char *in_root(const char *name)
{
  return g_build_filename(root, name, NULL);
}

static void setup(void)
{
  root = g_dir_make_tmp("t-code-langenv-XXXXXX", NULL);
  put("bin/python3", "#!/bin/sh\n", TRUE); /* the only python3 on PATH */
  char *bin = in_root("bin");
  g_setenv("PATH", bin, TRUE);
  g_unsetenv("VIRTUAL_ENV");
  g_free(bin);
}

static void remove_all(const char *path)
{
  GDir *dir = g_dir_open(path, 0, NULL);
  const char *name;

  while (dir && (name = g_dir_read_name(dir))) {
    char *child = g_build_filename(path, name, NULL);
    if (g_file_test(child, G_FILE_TEST_IS_DIR) && !g_file_test(child, G_FILE_TEST_IS_SYMLINK))
      remove_all(child);
    else
      g_unlink(child);
    g_free(child);
  }
  if (dir)
    g_dir_close(dir);
  g_rmdir(path);
}

static void teardown(void)
{
  remove_all(root);
  g_clear_pointer(&root, g_free);
}

/* The venvs in the root (.venv, venv, then by name; a folder without pyvenv.cfg is none), the
 * active one, then python3, each once. */
static void test_interpreters(void)
{
  setup();
  make_venv("zeta");
  make_venv("venv");
  make_venv(".venv");
  put("notenv/bin/python", "#!/bin/sh\n", TRUE);
  make_venv("outside/env");
  char *active = in_root("outside/env");
  g_setenv("VIRTUAL_ENV", active, TRUE);

  GPtrArray *list = langenv_interpreters(root);
  const char *want[] = { ".venv/bin/python", "venv/bin/python", "zeta/bin/python", "outside/env/bin/python",
                         "bin/python3" };
  g_assert_cmpuint(list->len, ==, G_N_ELEMENTS(want));
  for (guint i = 0; i < list->len; i++) {
    char *path = in_root(want[i]);
    g_assert_cmpstr(g_ptr_array_index(list, i), ==, path);
    g_free(path);
  }
  g_ptr_array_unref(list);

  /* The active venv is the project's own: listed once. */
  char *own = in_root(".venv");
  g_setenv("VIRTUAL_ENV", own, TRUE);
  list = langenv_interpreters(root);
  g_assert_cmpuint(list->len, ==, 4);
  g_ptr_array_unref(list);
  g_free(own);
  g_free(active);
  teardown();
}

/* The root's own venv, not the active one elsewhere nor python3. */
static void test_venv_python(void)
{
  setup();
  make_venv("outside/env");
  char *active = in_root("outside/env"), *project = in_root("project"), *python;
  g_setenv("VIRTUAL_ENV", active, TRUE);
  g_mkdir(project, 0755);
  g_assert_null(langenv_venv_python(project));

  make_venv("project/venv");
  python = langenv_venv_python(project);
  g_assert_true(g_str_has_suffix(python, "/project/venv/bin/python"));
  g_free(python);
  g_free(project);
  g_free(active);
  teardown();
}

static void test_interpreter_name(void)
{
  setup();
  make_venv(".venv");
  char *venv = in_root(".venv/bin/python"), *system = in_root("bin/python3");
  char *name = langenv_interpreter_name(venv);
  g_assert_cmpstr(name, ==, "python (.venv)");
  g_free(name);
  name = langenv_interpreter_name(system);
  g_assert_cmpstr(name, ==, "python3");
  g_free(name);
  g_free(system);
  g_free(venv);
  teardown();
}

/* [features] keys in order: not "default", not other tables' keys, comments and quotes handled. */
static void test_cargo_features(void)
{
  setup();
  put("Cargo.toml",
      "[package]\nname = \"demo\"\n\n"
      "[features]\n# what to build\ndefault = [\"json\"]\njson = []\n\"tls-rustls\" = [\"dep:rustls\"]\n"
      "  serde = [\"dep:serde\"]  \n\n"
      "[dependencies]\nrustls = { version = \"0.23\", optional = true }\n",
      FALSE);
  char *path = in_root("Cargo.toml");
  GStrv features = langenv_cargo_features(path);
  const char *const want[] = { "json", "tls-rustls", "serde", NULL };
  g_assert_cmpstrv(features, want);
  g_strfreev(features);
  g_free(path);

  path = in_root("missing/Cargo.toml");
  g_assert_null(langenv_cargo_features(path));
  g_free(path);
  teardown();
}

static void test_rust_options(void)
{
  char *options;

  g_assert_null(langenv_rust_options(NULL, NULL));
  g_assert_null(langenv_rust_options("", ""));
  options = langenv_rust_options("all", NULL);
  g_assert_cmpstr(options, ==, "{\"cargo\":{\"features\":\"all\"}}");
  g_free(options);
  options = langenv_rust_options("json, serde", "wasm32-unknown-unknown");
  g_assert_cmpstr(options, ==, "{\"cargo\":{\"features\":[\"json\",\"serde\"],\"target\":\"wasm32-unknown-unknown\"}}");
  g_free(options);
}

static void test_cargo_flags(void)
{
  char *flags;

  flags = langenv_cargo_flags(NULL, NULL);
  g_assert_cmpstr(flags, ==, "");
  g_free(flags);
  flags = langenv_cargo_flags("all", "x86_64-unknown-linux-musl");
  g_assert_cmpstr(flags, ==, " --all-features --target 'x86_64-unknown-linux-musl'");
  g_free(flags);
  flags = langenv_cargo_flags("json,serde", NULL);
  g_assert_cmpstr(flags, ==, " --features 'json,serde'");
  g_free(flags);
}

int main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/langenv/interpreters", test_interpreters);
  g_test_add_func("/langenv/venv-python", test_venv_python);
  g_test_add_func("/langenv/interpreter-name", test_interpreter_name);
  g_test_add_func("/langenv/cargo-features", test_cargo_features);
  g_test_add_func("/langenv/rust-options", test_rust_options);
  g_test_add_func("/langenv/cargo-flags", test_cargo_flags);
  return g_test_run();
}
