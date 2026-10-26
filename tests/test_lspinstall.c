/*
 * Tests for lsp/install.c (GLib only): the distribution read from os-release, and the commands that
 * install each language server t-code runs.
 */
#include "lsp/install.h"

#include <string.h>

static void test_distro(void)
{
  g_assert_cmpstr(install_distro("NAME=\"Fedora Linux\"\nID=fedora\nVERSION_ID=44\n"), ==, "fedora");
  g_assert_cmpstr(install_distro("ID=ubuntu\nID_LIKE=debian\n"), ==, "debian");
  g_assert_cmpstr(install_distro("ID=linuxmint\nID_LIKE=\"ubuntu debian\"\n"), ==, "debian");
  g_assert_cmpstr(install_distro("ID=debian\n"), ==, "debian");
  g_assert_cmpstr(install_distro("ID=arch\n"), ==, "arch");
  g_assert_cmpstr(install_distro("ID=endeavouros\nID_LIKE='arch'\n"), ==, "arch");
  g_assert_cmpstr(install_distro("ID=\"void\"\n"), ==, "void");
  g_assert_null(install_distro("ID=nixos\n"));
  g_assert_null(install_distro("VERSION_ID=1\n"));
  g_assert_null(install_distro(NULL));
}

/* Each server t-code runs (lsp/client/client.c) has a title and at least one command for every family. */
static void test_every_server(void)
{
  static const char *const servers[] = { "clangd",
                                         "pylsp",
                                         "gopls",
                                         "rust-analyzer",
                                         "typescript-language-server",
                                         "vscode-json-language-server",
                                         "yaml-language-server",
                                         "marksman",
                                         "neocmakelsp" };
  static const char *const distros[] = { "fedora", "debian", "arch", "void", NULL };

  for (size_t i = 0; i < G_N_ELEMENTS(servers); i++) {
    g_assert_nonnull(install_title(servers[i]));
    for (size_t j = 0; j < G_N_ELEMENTS(distros); j++) {
      GStrv commands = install_commands(servers[i], distros[j]);
      g_assert_nonnull(commands);
      g_assert_cmpuint(g_strv_length(commands), >=, 1);
      g_strfreev(commands);
    }
  }
  g_assert_null(install_title("nonesuch"));
  g_assert_null(install_commands("nonesuch", "fedora"));
  g_assert_null(install_note("nonesuch"));
}

/* The distribution's package comes first, the language's own installer after it; an unknown
 * distribution gets every package. */
static void test_commands(void)
{
  GStrv fedora = install_commands("pylsp", "fedora"), arch = install_commands("marksman", "arch");
  GStrv other = install_commands("marksman", "fedora"), unknown = install_commands("clangd", NULL);

  g_assert_cmpuint(g_strv_length(fedora), ==, 2);
  g_assert_cmpstr(fedora[0], ==, "sudo dnf install python3-lsp-server+all python3-lsp-ruff");
  g_assert_true(g_str_has_prefix(fedora[1], "pipx install"));
  g_assert_cmpstr(arch[0], ==, "sudo pacman -S marksman");
  g_assert_cmpuint(g_strv_length(other), ==, 1); /* no Fedora package: the download only */
  g_assert_nonnull(strstr(other[0], "marksman-linux-x64"));
  g_assert_cmpuint(g_strv_length(unknown), ==, 4);
  g_assert_cmpstr(unknown[0], ==, "sudo dnf install clang-tools-extra");
  g_assert_nonnull(install_note("gopls"));
  g_strfreev(unknown);
  g_strfreev(other);
  g_strfreev(arch);
  g_strfreev(fedora);
}

int main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/lspinstall/distro", test_distro);
  g_test_add_func("/lspinstall/every-server", test_every_server);
  g_test_add_func("/lspinstall/commands", test_commands);
  return g_test_run();
}
