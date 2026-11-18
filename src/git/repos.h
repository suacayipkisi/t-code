/*
 * The other git repositories inside a project (GLib only): clones inside the project's own work tree,
 * or the repositories of a folder that is none itself. Submodules are not among them: they belong to
 * the repository that holds them.
 */
#pragma once

#include <glib.h>

/* The repositories nested in the work tree top, from its "git status --porcelain=v2 -z
 * --untracked-files=all --ignored=matching" output (size bytes): git lists each untracked folder
 * holding a repository of its own as "? path/", an ignored folder as "! path/" (a repository when it
 * holds a .git; what is deeper in it, like a build folder's downloads, is not looked at), and a
 * submodule not at all. Returns their work tree tops (absolute, sorted; char *, freed with the
 * array). */
GPtrArray *git_nested_repos(const char *top, const char *out, gsize size);

/* The repositories under root (not root itself) at most depth folders down, max of them at most:
 * folders holding a .git (folder or file), a .hg or a .svn (vcs.h), not looked into further (their own nested ones come from
 * their git status). Folders starting with a dot, node_modules and symbolic links are skipped. Blocks:
 * call it in a thread. Returns absolute work tree tops, sorted (char *, freed with the array). */
GPtrArray *git_find_repos(const char *root, int depth, guint max);

/* Whether a record of the git status output names an untracked folder ("? path/": a nested repository). */
gboolean git_is_nested_record(const char *record, gsize len);
