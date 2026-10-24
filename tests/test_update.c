/* Tests for version comparisons in src/update/update.c */
#include "update/update.h"

#include <glib.h>

static void test_newer_version(void)
{
  /* Standard semver increments */
  g_assert_true(newer_version("0.3.0", "0.4.0"));
  g_assert_false(newer_version("0.4.0", "0.3.0"));
  g_assert_true(newer_version("0.3.0", "0.3.1"));
  g_assert_false(newer_version("0.3.1", "0.3.0"));
  g_assert_true(newer_version("0.9.9", "1.0.0"));
  g_assert_false(newer_version("1.0.0", "0.9.9"));

  /* Identical versions */
  g_assert_false(newer_version("0.3.0", "0.3.0"));
  g_assert_false(newer_version("1.0", "1.0"));

  /* Leading 'v' prefixes */
  g_assert_true(newer_version("0.3.0", "v0.4.0"));
  g_assert_false(newer_version("v0.4.0", "0.3.0"));
  g_assert_true(newer_version("v0.3.0", "v0.4.0"));
  g_assert_false(newer_version("v0.4.0", "v0.3.0"));

  /* Pre-release suffixes */
  g_assert_false(newer_version("0.3.0-rc1", "0.3.0"));
  g_assert_false(newer_version("0.3.0", "0.3.0-rc1"));
  g_assert_true(newer_version("0.3.0-rc1", "0.3.1"));

  /* Varying component lengths */
  g_assert_true(newer_version("0.3", "0.4.0"));
  g_assert_true(newer_version("0.3.0", "0.3.0.1"));
  g_assert_false(newer_version("0.3.0.1", "0.3.0"));
}

/* "Check for Updates Now" says what it found when there is nothing to update to. */
static void test_check_message(void)
{
  char *message;

  g_assert_null(update_check_message("0.3.2", "v0.3.3")); /* the update window instead */
  message = update_check_message("0.3.2", "v0.3.2");
  g_assert_cmpstr(message, ==, "You have the latest release, t-code 0.3.2.");
  g_free(message);
  message = update_check_message("0.4.0", "v0.3.2"); /* a build from the source tree */
  g_assert_cmpstr(message, ==, "You have t-code 0.4.0, newer than the latest release (0.3.2).");
  g_free(message);
  message = update_check_message("0.3.2", NULL);
  g_assert_cmpstr(message, ==, "Could not check for updates: no answer from GitHub (offline?).");
  g_free(message);
}

int main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/update/newer-version", test_newer_version);
  g_test_add_func("/update/check-message", test_check_message);
  return g_test_run();
}
