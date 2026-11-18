/* File icons, GLib-only: see icons.h. */
#include "files/icons.h"

#include <string.h>

/* An icon called tcode-file-<file> (dark) and tcode-file-<file>-light: assets/icons/{dark,light}/<file>.svg. */
#define ICON(var, file) static const FileIcon icon_##var = { { "tcode-file-" file, "tcode-file-" file "-light" } }

ICON(folder, "folder");
ICON(folder_open, "folder-open");
ICON(file, "file");
ICON(c, "c");
ICON(cpp, "cpp");
ICON(h, "h");
ICON(make, "make");
ICON(config, "config");
ICON(markdown, "markdown");
ICON(json, "json");
ICON(shell, "shell");
ICON(python, "python");
ICON(lua, "lua");
ICON(js, "js");
ICON(ts, "ts");
ICON(html, "html");
ICON(css, "css");
ICON(rust, "rust");
ICON(go, "go");
ICON(java, "java");
ICON(image, "image");
ICON(archive, "archive");
ICON(pdf, "pdf");
ICON(text, "text");
ICON(binary, "binary");
ICON(git, "git");
ICON(license, "license");
ICON(lock, "lock");

/* clang-format off */
static const struct { const char *name; const FileIcon *icon; } icon_names[] = {
  { "Makefile", &icon_make },        { "makefile", &icon_make },        { "GNUmakefile", &icon_make },
  { "CMakeLists.txt", &icon_make },  { ".gitignore", &icon_git },       { ".gitattributes", &icon_git },
  { ".gitmodules", &icon_git },      { ".clang-format", &icon_config }, { ".clang-tidy", &icon_config },
  { ".clangd", &icon_config },       { ".editorconfig", &icon_config }, { "LICENSE", &icon_license },
  { "LICENSE.md", &icon_license },   { "LICENSE.txt", &icon_license },  { "COPYING", &icon_license },
};

static const struct { const char *ext; const FileIcon *icon; } icon_exts[] = {
  { "c", &icon_c },         { "cpp", &icon_cpp },       { "cc", &icon_cpp },       { "cxx", &icon_cpp },
  { "c++", &icon_cpp },     { "h", &icon_h },           { "hpp", &icon_h },        { "hh", &icon_h },
  { "hxx", &icon_h },       { "inl", &icon_h },         { "mk", &icon_make },      { "cmake", &icon_make },
  { "md", &icon_markdown }, { "markdown", &icon_markdown },                       { "json", &icon_json },
  { "yaml", &icon_config }, { "yml", &icon_config },    { "toml", &icon_config },  { "ini", &icon_config },
  { "conf", &icon_config }, { "cfg", &icon_config },    { "xml", &icon_config },   { "desktop", &icon_config },
  { "sh", &icon_shell },    { "bash", &icon_shell },    { "zsh", &icon_shell },    { "py", &icon_python },
  { "lua", &icon_lua },     { "js", &icon_js },         { "mjs", &icon_js },       { "ts", &icon_ts },
  { "html", &icon_html },   { "htm", &icon_html },      { "css", &icon_css },      { "rs", &icon_rust },
  { "go", &icon_go },       { "java", &icon_java },     { "png", &icon_image },    { "jpg", &icon_image },
  { "jpeg", &icon_image },  { "gif", &icon_image },     { "svg", &icon_image },    { "ico", &icon_image },
  { "webp", &icon_image },  { "bmp", &icon_image },     { "zip", &icon_archive },  { "tar", &icon_archive },
  { "gz", &icon_archive },  { "xz", &icon_archive },    { "bz2", &icon_archive },  { "zst", &icon_archive },
  { "7z", &icon_archive },  { "pdf", &icon_pdf },       { "txt", &icon_text },     { "log", &icon_text },
  { "o", &icon_binary },    { "a", &icon_binary },      { "so", &icon_binary },    { "bin", &icon_binary },
  { "lock", &icon_lock },
};
/* clang-format on */

const FileIcon *file_icon(const char *name, gboolean dir, gboolean open)
{
  const char *ext = strrchr(name, '.');

  if (dir)
    return open ? &icon_folder_open : &icon_folder;
  for (size_t i = 0; i < G_N_ELEMENTS(icon_names); i++)
    if (strcmp(name, icon_names[i].name) == 0)
      return icon_names[i].icon;
  for (size_t i = 0; ext && ext != name && i < G_N_ELEMENTS(icon_exts); i++)
    if (g_ascii_strcasecmp(ext + 1, icon_exts[i].ext) == 0)
      return icon_exts[i].icon;
  return &icon_file;
}
