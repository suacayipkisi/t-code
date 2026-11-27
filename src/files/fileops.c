/* The file tree's file operations, GLib-only: see fileops.h. */
#define _DEFAULT_SOURCE /* lstat, readlink, symlink under -std=c23 */
#include "files/fileops.h"

#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

gboolean files_shown(const char *name, gboolean show_hidden)
{
  static const char *const never[] = { ".git", ".svn", ".hg" };

  if (name[0] != '.')
    return TRUE;
  if (!show_hidden)
    return FALSE;
  for (size_t i = 0; i < G_N_ELEMENTS(never); i++)
    if (strcmp(name, never[i]) == 0)
      return FALSE;
  return TRUE;
}

int files_compare(const char *a, gboolean a_dir, const char *b, gboolean b_dir)
{
  if (!a_dir != !b_dir)
    return a_dir ? -1 : 1;
  int c = g_ascii_strcasecmp(a, b);
  return c ? c : strcmp(a, b);
}

gboolean files_is_source(const char *name)
{
  static const char *const extensions[] = { "c",   "h",   "cc",  "cpp", "cxx", "c++", "hh",  "hpp",   "hxx", "inl",
                                            "ipp", "tpp", "m",   "mm",  "py",  "rs",  "go",  "java",  "kt",  "js",
                                            "mjs", "ts",  "tsx", "jsx", "lua", "sh",  "zig", "swift", "cs",  "rb" };
  const char *dot = strrchr(name, '.');

  for (size_t i = 0; dot && dot != name && i < G_N_ELEMENTS(extensions); i++)
    if (g_ascii_strcasecmp(dot + 1, extensions[i]) == 0)
      return TRUE;
  return FALSE;
}

gboolean files_inside(const char *path, const char *folder)
{
  size_t n = strlen(folder);

  return strncmp(path, folder, n) == 0 && (path[n] == '\0' || path[n] == '/');
}

const char *files_relative(const char *path, const char *root)
{
  size_t n = strlen(root);

  if (strncmp(path, root, n) == 0 && path[n] == '/')
    return path + n + 1;
  return strcmp(path, root) == 0 ? path + n : path;
}

static gboolean exists(const char *path)
{
  struct stat st;

  return lstat(path, &st) == 0;
}

static gboolean fail(GError **error, const char *what, const char *path)
{
  int code = errno;
  char *name = g_path_get_basename(path);

  g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(code), "cannot %s %s: %s", what, name, g_strerror(code));
  g_free(name);
  return FALSE;
}

char *files_free_name(const char *dir, const char *name, gboolean is_dir)
{
  const char *dot = is_dir ? NULL : strrchr(name, '.');
  int stem = dot && dot != name ? (int)(dot - name) : (int)strlen(name);
  const char *ext = dot && dot != name ? dot : "";
  char *path = g_build_filename(dir, name, NULL);

  for (int i = 1; exists(path); i++) {
    char *other = i == 1 ? g_strdup_printf("%.*s copy%s", stem, name, ext)
                         : g_strdup_printf("%.*s copy %d%s", stem, name, i, ext);
    g_free(path);
    path = g_build_filename(dir, other, NULL);
    g_free(other);
  }
  return path;
}

gboolean files_create(const char *dir, const char *name, gboolean folder, char **made, GError **error)
{
  gboolean dir_wanted = folder || g_str_has_suffix(name, "/");
  char *path = g_canonicalize_filename(name, dir); /* without a trailing "/" */
  char *parent = g_path_get_dirname(path);
  gboolean ok = FALSE;

  *made = NULL;
  if (g_mkdir_with_parents(dir_wanted ? path : parent, 0755) < 0) {
    fail(error, "create", path);
  } else if (dir_wanted) {
    ok = TRUE;
  } else {
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    ok = fd >= 0 ? close(fd) == 0 : fail(error, "create", path);
  }
  if (ok)
    *made = g_steal_pointer(&path);
  g_free(parent);
  g_free(path);
  return ok;
}

gboolean files_rename(const char *path, const char *name, char **dest, GError **error)
{
  char *parent = g_path_get_dirname(path);

  *dest = NULL;
  if (!*name || strchr(name, '/')) {
    g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL, "a name cannot have \"/\" in it");
  } else {
    char *to = g_build_filename(parent, name, NULL);
    if (strcmp(to, path) != 0 && exists(to)) /* the same name: nothing to do */
      g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_EXIST, "%s already exists", name);
    else if (g_rename(path, to) < 0)
      fail(error, "rename", path);
    else
      *dest = g_steal_pointer(&to);
    g_free(to);
  }
  g_free(parent);
  return *dest != NULL;
}

gboolean files_delete(const char *path, GError **error)
{
  struct stat st;

  if (lstat(path, &st) < 0)
    return fail(error, "delete", path);
  if (S_ISDIR(st.st_mode)) {
    GDir *dir = g_dir_open(path, 0, NULL);
    const char *name;
    gboolean ok = TRUE;
    while (ok && dir && (name = g_dir_read_name(dir))) {
      char *child = g_build_filename(path, name, NULL);
      ok = files_delete(child, error);
      g_free(child);
    }
    if (dir)
      g_dir_close(dir);
    if (!ok)
      return FALSE;
    return g_rmdir(path) == 0 || fail(error, "delete", path);
  }
  return g_unlink(path) == 0 || fail(error, "delete", path);
}

static gboolean copy_file(const char *src, const char *dst, mode_t mode, GError **error)
{
  char buf[65536];
  int in = open(src, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK), out; /* it may have changed since lstat */
  ssize_t n;

  if (in < 0)
    return fail(error, "copy", src);
  out = open(dst, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, mode);
  if (out < 0) {
    close(in);
    return fail(error, "copy", src);
  }
  while ((n = read(in, buf, sizeof buf)) > 0) {
    for (ssize_t done = 0; done < n;) {
      ssize_t w = write(out, buf + done, n - done);
      if (w < 0) {
        fail(error, "copy", src);
        close(in);
        close(out);
        unlink(dst);
        return FALSE;
      }
      done += w;
    }
  }
  close(in);
  if (close(out) < 0 || n < 0) {
    fail(error, "copy", src);
    unlink(dst);
    return FALSE;
  }
  return TRUE;
}

/* Copies a file, a symlink, or a folder with everything in it. */
static gboolean copy_tree(const char *src, const char *dst, GError **error)
{
  struct stat st;

  if (lstat(src, &st) < 0)
    return fail(error, "copy", src);
  if (S_ISLNK(st.st_mode)) {
    char *target = g_file_read_link(src, NULL);
    gboolean ok = target && symlink(target, dst) == 0;
    g_free(target);
    return ok || fail(error, "copy", src);
  }
  if (S_ISFIFO(st.st_mode)) /* opening it would wait for a writer */
    return mkfifo(dst, st.st_mode & 07777) == 0 || fail(error, "copy", src);
  if (S_ISREG(st.st_mode))
    return copy_file(src, dst, st.st_mode & 07777, error);
  if (!S_ISDIR(st.st_mode)) { /* a socket or a device: reading one never ends or means nothing */
    g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL, "cannot copy %s: not a file or a folder", src);
    return FALSE;
  }

  GDir *dir;
  const char *name;
  gboolean ok = TRUE;
  if (g_mkdir(dst, st.st_mode & 07777) < 0 || !(dir = g_dir_open(src, 0, NULL)))
    return fail(error, "copy", src);
  while (ok && (name = g_dir_read_name(dir))) {
    char *from = g_build_filename(src, name, NULL), *to = g_build_filename(dst, name, NULL);
    ok = copy_tree(from, to, error);
    g_free(from);
    g_free(to);
  }
  g_dir_close(dir);
  return ok;
}

/* Whether dir is folder or inside it, also through symlinks (a copy into it would never end). */
static gboolean folder_inside(const char *dir, const char *folder)
{
  char *real_dir = realpath(dir, NULL), *real_folder = realpath(folder, NULL);
  gboolean inside = files_inside(dir, folder) || (real_dir && real_folder && files_inside(real_dir, real_folder));

  free(real_dir);
  free(real_folder);
  return inside;
}

gboolean files_put(const char *src, const char *dir, gboolean cut, char **dest, GError **error)
{
  struct stat st;
  char *parent = g_path_get_dirname(src), *name = g_path_get_basename(src);
  gboolean ok = FALSE, same = cut && strcmp(parent, dir) == 0;

  *dest = NULL;
  if (lstat(src, &st) < 0) {
    g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_NOENT, "%s is gone", name);
  } else if (same) {
    /* already there */
  } else if (S_ISDIR(st.st_mode) && folder_inside(dir, src)) {
    g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL, "cannot put a folder inside itself");
  } else {
    *dest = files_free_name(dir, name, S_ISDIR(st.st_mode));
    if (cut && g_rename(src, *dest) == 0) {
      ok = TRUE;
    } else if (cut && errno != EXDEV) {
      fail(error, "move", src);
    } else if (copy_tree(src, *dest, error)) { /* a copy, or a move to another file system */
      ok = TRUE;
      if (cut && !files_delete(src, NULL))
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_FAILED, "copied, but cannot remove %s", name);
    }
    if (!ok)
      g_clear_pointer(dest, g_free);
  }
  g_free(parent);
  g_free(name);
  return ok;
}
