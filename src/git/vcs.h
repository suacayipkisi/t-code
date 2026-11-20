/*
 * Mercurial and Subversion next to git (GLib only). The rest of t-code speaks git: its status,
 * log, blame and show formats. For a Mercurial or Subversion repository this module finds it, gives
 * the arguments of the hg / svn command that answers the same question, and translates the answer
 * into git's format, so the Source Control panel, the Files panel's colors, the diff gutters, line
 * blame and the comparisons read one format. The arguments never hold the program: vcs_program()
 * is put first, and they run in the repository's top unless said otherwise.
 */
#pragma once

#include <glib.h>

typedef enum { VCS_NONE, VCS_GIT, VCS_HG, VCS_SVN } VcsKind;

#define VCS_KINDS 4

/* "git", "hg", "svn" (NULL for VCS_NONE), and the kind back from such a name (VCS_NONE for others). */
const char *vcs_program(VcsKind kind);
VcsKind vcs_kind_named(const char *program);

/* "Git", "Mercurial", "Subversion" (NULL for VCS_NONE). */
const char *vcs_title(VcsKind kind);

/* A repository: its kind and its work tree top. */
typedef struct {
  VcsKind kind;
  char *top;
} VcsPlace;

void vcs_place_free(VcsPlace *place);

/* The repositories holding dir (absolute), nearest first: for each kind the nearest folder at or
 * above dir holding its mark (a .git folder or file, a .hg folder, a .svn folder; one .svn at the
 * top of a Subversion working copy, 1.7 and newer). A folder with two marks (hg-git keeps .git and
 * .hg) gives both, git first. VcsPlace, freed with the array. Blocks only on stat calls. */
GPtrArray *vcs_find_up(const char *dir);

/* The kind of the repository holding dir and its top (*top, freed by the caller; may be NULL): the
 * nearest one, unless vcs_prefer() chose one of the others. VCS_NONE outside any. */
VcsKind vcs_kind_of(const char *dir, char **top);

/* The user chose the repository of kind at top among several holding the project: vcs_kind_of()
 * answers it for the files in it from now on. VCS_NONE forgets the choice for top. */
void vcs_prefer(const char *top, VcsKind kind);

/* Whether the folder holds a repository mark of its own, of any kind but git (repos.c looks for
 * .git): a Mercurial or Subversion repository nested in a project. */
VcsKind vcs_mark_in(const char *dir);

/* Status ------------------------------------------------------------------------------------- */

/* The status command: its output goes to vcs_git_status() with the head command's. */
const char *const *vcs_status_args(VcsKind kind);

/* The command telling the branch and the revision checked out. */
const char *const *vcs_head_args(VcsKind kind);

/* Whether an hg merge is going on in the repository at top (its merge state is there): only then
 * vcs_unresolved_args() has anything to tell. Always FALSE for svn, whose status shows conflicts. */
gboolean vcs_merging(VcsKind kind, const char *top);

/* hg resolve --list: "U path" for a file still in conflict, "R path" once marked resolved. */
const char *const *vcs_unresolved_args(void);

/* git status --porcelain=v2 --branch -z for an hg or svn repository, from its status and head
 * outputs (either NULL: none): "# branch.oid", "# branch.head" (the active bookmark or the branch;
 * svn: the folder under branches/ or tags/, or "trunk"), "# branch.upstream" (hg and svn always
 * have somewhere to pull from), then a record per file. Neither has git's index: a file is
 * "staged" when its path (relative to the top) is in staged (t-code keeps that list; NULL: none),
 * otherwise its change is unstaged (paths in it that no longer changed are taken out). Renames (hg's copy of a removed file, svn's moves) become git's
 * rename records, unknown files "?", svn's ignored ones "!", conflicts "u". v1: the short format of
 * git status --porcelain=v1 -z instead (the Files panel's colors), without the header. Returns the
 * output, its length in *length. unresolved: hg resolve --list's output during a merge (NULL: none),
 * whose "U" files are conflicts (hg status shows them modified). */
char *vcs_git_status(VcsKind kind, const char *status, gsize size, const char *head, const char *unresolved,
                     GHashTable *staged, gboolean v1, gsize *length);

/* svn info --xml's repository-relative URL of the working copy ("^/trunk"), NULL when missing. */
char *vcs_svn_url(const char *info);

/* Versions ------------------------------------------------------------------------------------ */

/* rev as the kind names it: git's HEAD is hg's "." and svn's BASE; svn's "N^" (the parent) is N - 1. */
char *vcs_rev(VcsKind kind, const char *rev);

/* The command printing path (relative to where it runs) in rev (git's names, vcs_rev() translates):
 * git show rev:./path, hg cat -r, svn cat (url: the working copy's from vcs_svn_url(), to reach a
 * path that is no longer there; NULL: through the working copy). */
char **vcs_cat_args(VcsKind kind, const char *rev, const char *path, const char *url);

/* The command printing rev's id (short: as shown), one line; for svn the revision number. */
char **vcs_id_args(VcsKind kind, const char *rev, gboolean shortened, const char *path);

/* Logs ---------------------------------------------------------------------------------------- */

/* The last max commits, as the Commits list reads git log (commits.c): each commit "\x1e", then
 * hash, short hash, subject, author, email, "3 days ago", date ("%Y-%m-%d %H:%M"), refs and the
 * message between "\x1f", "\x1d", then a line of numbers. vcs_git_log() translates the output. */
char **vcs_log_args(VcsKind kind, int max);
char *vcs_git_log(VcsKind kind, const char *out);

/* A file's last max commits (File History): one line each, hash, short hash, date, author and
 * subject between "\x1f". name: the file's, run in its folder. */
char **vcs_file_log_args(VcsKind kind, int max, const char *name);
char *vcs_git_file_log(VcsKind kind, const char *out);

/* The messages of the last max commits, each followed by "\x1e" (Up in the commit message). */
char **vcs_messages_args(VcsKind kind, int max);
char *vcs_git_messages(VcsKind kind, const char *out);

/* The files a commit changed, as git show --name-status: "M\tpath", "R100\told\tnew". url: svn's
 * working copy (vcs_svn_url()); its paths outside it are left out. */
char **vcs_commit_files_args(VcsKind kind, const char *rev);
char *vcs_git_commit_files(VcsKind kind, const char *out, const char *url);

/* A commit's patch in git's format; vcs_git_show() puts git show --numstat's lines before it. */
char **vcs_show_args(VcsKind kind, const char *rev);
char *vcs_git_show(const char *patch);

/* Blame --------------------------------------------------------------------------------------- */

/* Blame of the file name (run in its folder), as saved on disk. */
char **vcs_blame_args(VcsKind kind, const char *name);

/* git blame --porcelain's answer for line (from 0) of that output: the commit (forty zeros for a
 * line not committed yet), "author", "author-time" and "summary" (svn: "r5", it has no subjects
 * there). NULL when the line is not in it. */
char *vcs_git_blame(VcsKind kind, const char *out, int line);
