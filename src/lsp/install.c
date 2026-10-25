 /*
 * How to install a language server (see install.h): one row per server, with its package on each
 * distribution (NULL where it has none) and the language's own installer.
 */
#include "lsp/install.h"

#include <string.h>

typedef struct {
  const char *server, *title;
  const char *fedora, *debian, *arch, *void_linux;
  const char *other; /* the language's own installer, for every distribution */
  const char *note;
} ServerInstall;

#define NPM_NOTE "npm comes with Node.js: install that first if npm is missing."

static const ServerInstall servers[] = {
  { "clangd", "the C/C++ language server (clangd)", "sudo dnf install clang-tools-extra", "sudo apt install clangd",
    "sudo pacman -S clang", "sudo xbps-install clang-tools-extra", NULL, NULL },
  { "pylsp", "the Python language server (pylsp)", "sudo dnf install python3-lsp-server+all",
    "sudo apt install python3-pylsp python3-pyflakes python3-pycodestyle",
    "sudo pacman -S python-lsp-server python-pyflakes python-pycodestyle", NULL,
    "pipx install 'python-lsp-server[all]'", "pyflakes and pycodestyle give its errors and warnings." },
  { "gopls", "the Go language server (gopls)", "sudo dnf install gopls", "sudo apt install gopls",
    "sudo pacman -S gopls", NULL, "go install golang.org/x/tools/gopls@latest",
    "go install puts gopls in ~/go/bin, which must be on PATH." },
  { "rust-analyzer", "the Rust language server (rust-analyzer)", "sudo dnf install rust-analyzer", NULL,
    "sudo pacman -S rust-analyzer", NULL, "rustup component add rust-analyzer", NULL },
  { "typescript-language-server", "the JavaScript and TypeScript language server (typescript-language-server)", NULL,
    NULL, "sudo pacman -S typescript-language-server", NULL,
    "sudo npm install -g typescript-language-server typescript", NPM_NOTE },
  { "vscode-json-language-server", "the JSON language server", NULL, NULL, NULL, NULL,
    "sudo npm install -g vscode-langservers-extracted", NPM_NOTE },
  { "yaml-language-server", "the YAML language server (yaml-language-server)", NULL, NULL,
    "sudo pacman -S yaml-language-server", NULL, "sudo npm install -g yaml-language-server", NPM_NOTE },
  { "marksman", "the Markdown language server (marksman)", NULL, NULL, "sudo pacman -S marksman", NULL,
    "mkdir -p ~/.local/bin && curl -L -o ~/.local/bin/marksman "
    "https://github.com/artempyanykh/marksman/releases/latest/download/marksman-linux-x64 && "
    "chmod +x ~/.local/bin/marksman",
    "The download is for x86_64 (marksman-linux-arm64 on ARM); ~/.local/bin must be on PATH." },
};

static const ServerInstall *find(const char *server)
{
  for (size_t i = 0; server && i < G_N_ELEMENTS(servers); i++)
    if (strcmp(servers[i].server, server) == 0)
      return &servers[i];
  return NULL;
}

char *install_os_release(void)
{
  static const char *const paths[] = { "/run/host/os-release", "/etc/os-release", "/usr/lib/os-release" };
  char *text = NULL;

  for (size_t i = 0; i < G_N_ELEMENTS(paths); i++)
    if (g_file_get_contents(paths[i], &text, NULL, NULL))
      return text;
  return NULL;
}

/* The value of KEY= in an os-release text, unquoted, or NULL. */
static char *os_release_value(const char *os_release, const char *key)
{
  char **lines = g_strsplit(os_release, "\n", -1), *value = NULL;
  size_t length = strlen(key);

  for (int i = 0; lines[i] && !value; i++)
    if (strncmp(lines[i], key, length) == 0 && lines[i][length] == '=') {
      value = g_strstrip(g_strdup(lines[i] + length + 1));
      size_t n = strlen(value);
      if (n >= 2 && (value[0] == '"' || value[0] == '\'') && value[n - 1] == value[0]) {
        memmove(value, value + 1, n - 2);
        value[n - 2] = '\0';
      }
    }
  g_strfreev(lines);
  return value;
}

const char *install_distro(const char *os_release)
{
  static const char *const families[] = { "fedora", "debian", "ubuntu", "arch", "void" };
  const char *found = NULL;

  if (!os_release)
    return NULL;
  char *id = os_release_value(os_release, "ID"), *like = os_release_value(os_release, "ID_LIKE");
  char *ids = g_strjoin(" ", id ? id : "", like ? like : "", NULL);
  char **words = g_strsplit(ids, " ", -1);

  /* ID first: Ubuntu's ID_LIKE is "debian", a Fedora remix's is "fedora". */
  for (int i = 0; words[i] && !found; i++)
    for (size_t j = 0; j < G_N_ELEMENTS(families) && !found; j++)
      if (strcmp(words[i], families[j]) == 0)
        found = strcmp(families[j], "ubuntu") == 0 ? "debian" : families[j];
  g_strfreev(words);
  g_free(ids);
  g_free(like);
  g_free(id);
  return found;
}

const char *install_title(const char *server)
{
  const ServerInstall *s = find(server);

  return s ? s->title : NULL;
}

GStrv install_commands(const char *server, const char *distro)
{
  const ServerInstall *s = find(server);
  GStrvBuilder *builder;

  if (!s)
    return NULL;
  builder = g_strv_builder_new();
  const char *packages[] = { s->fedora, s->debian, s->arch, s->void_linux };
  static const char *const names[] = { "fedora", "debian", "arch", "void" };
  for (size_t i = 0; i < G_N_ELEMENTS(packages); i++)
    if (packages[i] && (!distro || strcmp(distro, names[i]) == 0))
      g_strv_builder_add(builder, packages[i]);
  if (s->other)
    g_strv_builder_add(builder, s->other);
  GStrv commands = g_strv_builder_end(builder);
  g_strv_builder_unref(builder);
  return commands;
}

const char *install_note(const char *server)
{
  const ServerInstall *s = find(server);

  return s ? s->note : NULL;
}
