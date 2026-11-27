/*
 * Mercurial and Subversion answers in git's formats (see vcs.h).
 */
#include "git/vcs.h"

#include <string.h>

/* Kinds ----------------------------------------------------------------------------------------- */

static const char *const vcs_programs[VCS_KINDS] = { NULL, "git", "hg", "svn" };
static const char *const vcs_titles[VCS_KINDS] = { NULL, "Git", "Mercurial", "Subversion" };
static const char *const vcs_marks[VCS_KINDS] = { NULL, ".git", ".hg", ".svn" };

const char *vcs_program(VcsKind kind)
{
  return kind > VCS_NONE && kind < VCS_KINDS ? vcs_programs[kind] : NULL;
}

const char *vcs_title(VcsKind kind)
{
  return kind > VCS_NONE && kind < VCS_KINDS ? vcs_titles[kind] : NULL;
}

VcsKind vcs_kind_named(const char *program)
{
  for (int k = VCS_GIT; k < VCS_KINDS; k++)
    if (g_strcmp0(program, vcs_programs[k]) == 0)
      return k;
  return VCS_NONE;
}

void vcs_place_free(VcsPlace *place)
{
  g_free(place->top);
  g_free(place);
}

/* Whether dir holds kind's mark (git: a folder or, for a linked work tree or submodule, a file). */
static gboolean vcs_has_mark(const char *dir, VcsKind kind)
{
  char *path = g_build_filename(dir, vcs_marks[kind], NULL);
  gboolean yes = g_file_test(path, kind == VCS_GIT ? G_FILE_TEST_EXISTS : G_FILE_TEST_IS_DIR);

  g_free(path);
  return yes;
}

VcsKind vcs_mark_in(const char *dir)
{
  for (int k = VCS_HG; k < VCS_KINDS; k++)
    if (vcs_has_mark(dir, k))
      return k;
  return VCS_NONE;
}

GPtrArray *vcs_find_up(const char *dir)
{
  GPtrArray *found = g_ptr_array_new_with_free_func((GDestroyNotify)vcs_place_free);
  gboolean seen[VCS_KINDS] = { FALSE };
  char *at = g_canonicalize_filename(dir, NULL);

  for (;;) {
    for (int k = VCS_GIT; k < VCS_KINDS; k++)
      if (!seen[k] && vcs_has_mark(at, k)) {
        VcsPlace *place = g_new0(VcsPlace, 1);
        place->kind = k;
        place->top = g_strdup(at);
        g_ptr_array_add(found, place);
        seen[k] = TRUE;
      }
    char *up = g_path_get_dirname(at);
    gboolean root = strcmp(up, at) == 0;
    g_free(at);
    at = up;
    if (root)
      break;
  }
  g_free(at);
  return found;
}

static GHashTable *vcs_preferred; /* top -> kind chosen there */

void vcs_prefer(const char *top, VcsKind kind)
{
  if (!top)
    return;
  if (!vcs_preferred)
    vcs_preferred = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  if (kind == VCS_NONE)
    g_hash_table_remove(vcs_preferred, top);
  else
    g_hash_table_insert(vcs_preferred, g_strdup(top), GINT_TO_POINTER(kind));
}

VcsKind vcs_kind_of(const char *dir, char **top)
{
  GPtrArray *found = vcs_find_up(dir);
  VcsPlace *chosen = found->len ? found->pdata[0] : NULL;
  VcsKind kind;

  /* A choice the user made wins over a nearer repository. */
  for (guint i = 0; vcs_preferred && i < found->len; i++) {
    VcsPlace *place = found->pdata[i];
    if (GPOINTER_TO_INT(g_hash_table_lookup(vcs_preferred, place->top)) == (int)place->kind) {
      chosen = place;
      break;
    }
  }
  kind = chosen ? chosen->kind : VCS_NONE;
  if (top)
    *top = chosen ? g_strdup(chosen->top) : NULL;
  g_ptr_array_unref(found);
  return kind;
}

/* A little XML tree (svn's --xml answers) -------------------------------------------------------- */

typedef struct VcsNode VcsNode;
struct VcsNode {
  char *name;
  char **attr_names, **attr_values;
  GString *text;
  GPtrArray *children; /* VcsNode */
};

static void vcs_node_free(VcsNode *n)
{
  g_free(n->name);
  g_strfreev(n->attr_names);
  g_strfreev(n->attr_values);
  g_string_free(n->text, TRUE);
  g_ptr_array_unref(n->children);
  g_free(n);
}

static VcsNode *vcs_node_new(const char *name, const char **names, const char **values)
{
  VcsNode *n = g_new0(VcsNode, 1);

  n->name = g_strdup(name);
  n->attr_names = g_strdupv((char **)names);
  n->attr_values = g_strdupv((char **)values);
  n->text = g_string_new(NULL);
  n->children = g_ptr_array_new_with_free_func((GDestroyNotify)vcs_node_free);
  return n;
}

static void vcs_on_start(GMarkupParseContext *context G_GNUC_UNUSED, const char *name, const char **names,
                         const char **values, gpointer data, GError **error G_GNUC_UNUSED)
{
  GPtrArray *stack = data;
  VcsNode *n = vcs_node_new(name, names, values);

  g_ptr_array_add(((VcsNode *)stack->pdata[stack->len - 1])->children, n);
  g_ptr_array_add(stack, n);
}

static void vcs_on_end(GMarkupParseContext *context G_GNUC_UNUSED, const char *name G_GNUC_UNUSED, gpointer data,
                       GError **error G_GNUC_UNUSED)
{
  GPtrArray *stack = data;

  if (stack->len > 1)
    g_ptr_array_remove_index(stack, stack->len - 1);
}

static void vcs_on_text(GMarkupParseContext *context G_GNUC_UNUSED, const char *text, gsize len, gpointer data,
                        GError **error G_GNUC_UNUSED)
{
  GPtrArray *stack = data;

  g_string_append_len(((VcsNode *)stack->pdata[stack->len - 1])->text, text, len);
}

/* The document under a nameless root (what was read of it when it is cut short), or NULL. */
static VcsNode *vcs_xml_parse(const char *xml, gsize size)
{
  static const GMarkupParser parser = { vcs_on_start, vcs_on_end, vcs_on_text, NULL, NULL };
  VcsNode *root;
  GPtrArray *stack;
  GMarkupParseContext *context;
  const char *start;

  if (!xml)
    return NULL;
  /* GMarkup takes no XML declaration; the output need not end in a NUL */
  start = size >= 5 && memcmp(xml, "<?xml", 5) == 0 ? g_strstr_len(xml, (gssize)size, "?>") : NULL;
  start = start ? start + 2 : xml;
  root = vcs_node_new("", (const char *[]){ NULL }, (const char *[]){ NULL });
  stack = g_ptr_array_new();
  g_ptr_array_add(stack, root);
  context = g_markup_parse_context_new(&parser, 0, stack, NULL);
  g_markup_parse_context_parse(context, start, size - (gsize)(start - xml), NULL);
  g_markup_parse_context_free(context);
  g_ptr_array_free(stack, TRUE);
  return root;
}

static const char *vcs_attr(const VcsNode *n, const char *name)
{
  for (int i = 0; n && n->attr_names[i]; i++)
    if (strcmp(n->attr_names[i], name) == 0)
      return n->attr_values[i];
  return NULL;
}

static VcsNode *vcs_child(const VcsNode *n, const char *name)
{
  for (guint i = 0; n && i < n->children->len; i++)
    if (strcmp(((VcsNode *)n->children->pdata[i])->name, name) == 0)
      return n->children->pdata[i];
  return NULL;
}

static const char *vcs_child_text(const VcsNode *n, const char *name)
{
  VcsNode *c = vcs_child(n, name);
  return c ? c->text->str : "";
}

/* Every node called name at any depth under n, in order. */
static void vcs_collect(const VcsNode *n, const char *name, GPtrArray *into)
{
  for (guint i = 0; i < n->children->len; i++) {
    VcsNode *c = n->children->pdata[i];
    if (strcmp(c->name, name) == 0)
      g_ptr_array_add(into, c);
    else
      vcs_collect(c, name, into);
  }
}

static GPtrArray *vcs_all(const VcsNode *n, const char *name)
{
  GPtrArray *found = g_ptr_array_new();

  if (n)
    vcs_collect(n, name, found);
  return found;
}

/* Times ----------------------------------------------------------------------------------------- */

/* "3 days ago", as git log's %ar. */
static char *vcs_ago(gint64 when)
{
  static const struct {
    gint64 seconds;
    const char *unit;
  } units[] = { { 365 * 86400, "year" }, { 30 * 86400, "month" }, { 7 * 86400, "week" },
                { 86400, "day" },        { 3600, "hour" },        { 60, "minute" } };
  gint64 since = g_get_real_time() / G_USEC_PER_SEC - when;

  for (guint i = 0; i < G_N_ELEMENTS(units); i++)
    if (since >= units[i].seconds) {
      gint64 n = since / units[i].seconds;
      return g_strdup_printf("%" G_GINT64_FORMAT " %s%s ago", n, units[i].unit, n == 1 ? "" : "s");
    }
  since = MAX(since, 0);
  return g_strdup_printf("%" G_GINT64_FORMAT " second%s ago", since, since == 1 ? "" : "s");
}

/* svn's "2026-11-18T19:23:59.439382Z" in seconds (-1 when it is no such date). */
static gint64 vcs_svn_time(const char *iso)
{
  GDateTime *time = iso && *iso ? g_date_time_new_from_iso8601(iso, NULL) : NULL;
  gint64 seconds = time ? g_date_time_to_unix(time) : -1;

  if (time)
    g_date_time_unref(time);
  return seconds;
}

/* "%Y-%m-%d %H:%M" in local time, as git log --date=format: gives it. */
static char *vcs_local_date(gint64 seconds)
{
  GDateTime *time = g_date_time_new_from_unix_local(seconds);
  char *text = time ? g_date_time_format(time, "%Y-%m-%d %H:%M") : g_strdup("");

  if (time)
    g_date_time_unref(time);
  return text;
}

static char *vcs_first_line(const char *text)
{
  const char *end = strchr(text, '\n');
  return g_strndup(text, end ? (gsize)(end - text) : strlen(text));
}

static char **vcs_args_of(const char *first, ...)
{
  GPtrArray *args = g_ptr_array_new();
  va_list ap;

  va_start(ap, first);
  for (const char *a = first; a; a = va_arg(ap, const char *))
    g_ptr_array_add(args, g_strdup(a));
  va_end(ap);
  g_ptr_array_add(args, NULL);
  return (char **)g_ptr_array_free(args, FALSE);
}

/* Status ---------------------------------------------------------------------------------------- */

#define HG_HEAD "{node}\\x1f{branch}\\x1f{activebookmark}"

const char *const *vcs_status_args(VcsKind kind)
{
  static const char *const hg[] = { "status", "--print0", "--copies", NULL };
  static const char *const svn[] = { "status", "--xml", "--no-ignore", "--non-interactive", NULL };

  return kind == VCS_HG ? hg : kind == VCS_SVN ? svn : NULL;
}

const char *const *vcs_head_args(VcsKind kind)
{
  static const char *const hg[] = { "log", "-r", ".", "-T", HG_HEAD, NULL };
  static const char *const svn[] = { "info", "--xml", "--non-interactive", NULL };

  return kind == VCS_HG ? hg : kind == VCS_SVN ? svn : NULL;
}

gboolean vcs_merging(VcsKind kind, const char *top)
{
  if (kind != VCS_HG || !top)
    return FALSE;
  char *state = g_build_filename(top, ".hg", "merge", "state2", NULL);
  gboolean merging = g_file_test(state, G_FILE_TEST_EXISTS);
  g_free(state);
  return merging;
}

const char *const *vcs_unresolved_args(void)
{
  static const char *const hg[] = { "resolve", "--list", NULL };

  return hg;
}

/* One file's change, before it is written in git's format. */
typedef struct {
  char *path, *orig; /* orig: a rename's old path */
  char code;         /* M A D R C U(ntracked) I(gnored) !(conflict) */
} VcsEntry;

static void vcs_entry_free(VcsEntry *e)
{
  g_free(e->path);
  g_free(e->orig);
  g_free(e);
}

static void vcs_add_entry(GPtrArray *entries, const char *path, const char *orig, char code)
{
  VcsEntry *e = g_new0(VcsEntry, 1);

  e->path = g_strdup(path);
  e->orig = g_strdup(orig);
  e->code = code;
  g_ptr_array_add(entries, e);
}

static VcsEntry *vcs_find_entry(GPtrArray *entries, const char *path, char code)
{
  for (guint i = 0; i < entries->len; i++) {
    VcsEntry *e = entries->pdata[i];
    if (e->code == code && strcmp(e->path, path) == 0)
      return e;
  }
  return NULL;
}

/* hg status --copies: "M path", an added copy followed by "  source"; records end with sep ('\0' for
 * --print0, else '\n'). */
static GPtrArray *vcs_hg_entries(const char *out, gsize size, char sep)
{
  GPtrArray *entries = g_ptr_array_new_with_free_func((GDestroyNotify)vcs_entry_free);
  VcsEntry *last = NULL;

  for (const char *p = out, *end = out ? out + size : NULL; out && p < end;) {
    const char *nul = memchr(p, sep, end - p);
    gsize len = (nul ? nul : end) - p;
    char *record = g_strndup(p, len);
    p += len + 1;
    if (len > 2 && record[0] == ' ' && record[1] == ' ' && last && last->code == 'A') {
      g_set_str(&last->orig, record + 2); /* where the added file was copied from */
    } else if (len > 2 && record[1] == ' ') {
      char code = 0;
      switch (record[0]) {
        case 'M': code = 'M'; break;
        case 'A': code = 'A'; break;
        case 'R':
        case '!': code = 'D'; break; /* removed, or missing from disk */
        case '?': code = 'U'; break;
        case 'I': code = 'I'; break;
      }
      if (code) {
        vcs_add_entry(entries, record + 2, NULL, code);
        last = entries->pdata[entries->len - 1];
      }
    }
    g_free(record);
  }
  /* A copy whose source is removed is a rename (hg mv); one whose source stays is a copy. */
  GPtrArray *moved = g_ptr_array_new();
  for (guint i = 0; i < entries->len; i++) {
    VcsEntry *e = entries->pdata[i];
    if (e->code == 'A' && e->orig) {
      VcsEntry *source = vcs_find_entry(entries, e->orig, 'D');
      e->code = source ? 'R' : 'C';
      if (source)
        g_ptr_array_add(moved, source);
    }
  }
  for (guint i = 0; i < moved->len; i++)
    g_ptr_array_remove(entries, moved->pdata[i]);
  g_ptr_array_free(moved, TRUE);
  return entries;
}

/* svn status --xml: an entry per path, its wc-status item. */
static GPtrArray *vcs_svn_entries(const char *out, gsize size)
{
  GPtrArray *entries = g_ptr_array_new_with_free_func((GDestroyNotify)vcs_entry_free);
  VcsNode *root = vcs_xml_parse(out, size);
  GPtrArray *list = vcs_all(root, "entry");

  for (guint i = 0; i < list->len; i++) {
    VcsNode *n = list->pdata[i], *status = vcs_child(n, "wc-status");
    const char *path = vcs_attr(n, "path"), *item = vcs_attr(status, "item"), *props = vcs_attr(status, "props");
    char code = 0;
    if (!path || !item)
      continue;
    if (g_strcmp0(vcs_attr(status, "tree-conflicted"), "true") == 0 || strcmp(item, "conflicted") == 0)
      code = '!';
    else if (strcmp(item, "added") == 0)
      code = vcs_attr(status, "moved-from") ? 'R' : vcs_attr(status, "copied") ? 'C' : 'A';
    else if (strcmp(item, "deleted") == 0)
      code = vcs_attr(status, "moved-to") ? 0 : 'D'; /* the other half of a move */
    else if (strcmp(item, "modified") == 0 || strcmp(item, "replaced") == 0 || strcmp(item, "obstructed") == 0)
      code = 'M';
    else if (strcmp(item, "missing") == 0)
      code = 'D';
    else if (strcmp(item, "unversioned") == 0)
      code = 'U';
    else if (strcmp(item, "ignored") == 0)
      code = 'I';
    else if (g_strcmp0(props, "modified") == 0 && strcmp(path, ".") != 0)
      code = 'M'; /* only its properties changed; the top's own are left out (no file to show) */
    if (code)
      vcs_add_entry(entries, path, code == 'R' ? vcs_attr(status, "moved-from") : NULL, code);
  }
  g_ptr_array_free(list, TRUE);
  if (root)
    vcs_node_free(root);
  return entries;
}

/* The branch shown for svn: the folder under branches/ or tags/, else the last part of the URL. */
static char *vcs_svn_branch(const char *url)
{
  const char *at;

  if (!url)
    return NULL;
  if ((at = strstr(url, "/branches/")) || (at = strstr(url, "/tags/"))) {
    at = strchr(at + 1, '/') + 1;
    const char *slash = strchr(at, '/');
    return g_strndup(at, slash ? (gsize)(slash - at) : strlen(at));
  }
  at = strrchr(url, '/');
  return g_strdup(at && at[1] ? at + 1 : url);
}

char *vcs_svn_url(const char *info)
{
  VcsNode *root = info ? vcs_xml_parse(info, strlen(info)) : NULL;
  GPtrArray *list = vcs_all(root, "relative-url");
  char *url = list->len ? g_strdup(((VcsNode *)list->pdata[0])->text->str) : NULL;

  g_ptr_array_free(list, TRUE);
  if (root)
    vcs_node_free(root);
  return url;
}

/* The head's revision and branch. *initial: no commit yet. */
static void vcs_read_head(VcsKind kind, const char *head, char **oid, char **branch, gboolean *initial)
{
  *oid = *branch = NULL;
  *initial = FALSE;
  if (!head)
    return;
  if (kind == VCS_HG) {
    char **f = g_strsplit(head, "\x1f", 3);
    if (g_strv_length(f) == 3) {
      *initial = strspn(f[0], "0") == strlen(f[0]);
      *oid = g_strdup(f[0]);
      *branch = g_strdup(*g_strstrip(f[2]) ? f[2] : f[1]);
    }
    g_strfreev(f);
  } else {
    VcsNode *root = vcs_xml_parse(head, strlen(head));
    GPtrArray *list = vcs_all(root, "entry");
    if (list->len) {
      const char *revision = vcs_attr(list->pdata[0], "revision");
      char *url = g_strdup(vcs_child_text(list->pdata[0], "relative-url"));
      *oid = g_strdup(revision);
      *initial = !revision || strcmp(revision, "0") == 0;
      *branch = vcs_svn_branch(url);
      g_free(url);
    }
    g_ptr_array_free(list, TRUE);
    if (root)
      vcs_node_free(root);
  }
}

/* hg resolve --list's "U path" lines: those files are conflicts, whatever hg status called them. */
static void vcs_mark_unresolved(GPtrArray *entries, const char *unresolved)
{
  char **lines = unresolved ? g_strsplit(unresolved, "\n", -1) : NULL;

  for (int i = 0; lines && lines[i]; i++) {
    if (lines[i][0] != 'U' || lines[i][1] != ' ' || !lines[i][2])
      continue;
    const char *path = lines[i] + 2;
    gboolean found = FALSE;
    for (guint k = 0; k < entries->len; k++) {
      VcsEntry *e = entries->pdata[k];
      if (strcmp(e->path, path) == 0) {
        e->code = '!';
        found = TRUE;
      }
    }
    if (!found)
      vcs_add_entry(entries, path, NULL, '!');
  }
  g_strfreev(lines);
}

char *vcs_git_status(VcsKind kind, const char *status, gsize size, const char *head, const char *unresolved,
                     GHashTable *staged, gboolean v1, gsize *length)
{
  GString *out = g_string_new(NULL);
  GPtrArray *entries = kind == VCS_HG ? vcs_hg_entries(status, size, '\0') : vcs_svn_entries(status, size);
  char *oid, *branch;
  gboolean initial;

  vcs_mark_unresolved(entries, unresolved);
  vcs_read_head(kind, head, &oid, &branch, &initial);
  /* A staged path that no longer changed (committed, reverted) leaves the list. */
  if (staged) {
    GHashTableIter iter;
    gpointer path;
    g_hash_table_iter_init(&iter, staged);
    while (g_hash_table_iter_next(&iter, &path, NULL)) {
      gboolean changed = FALSE;
      for (guint i = 0; i < entries->len && !changed; i++) {
        VcsEntry *e = entries->pdata[i];
        changed = e->code != 'U' && e->code != 'I' && strcmp(e->path, path) == 0;
      }
      if (!changed)
        g_hash_table_iter_remove(&iter);
    }
  }
  if (!v1 && head) {
    g_string_append_printf(out, "# branch.oid %s%c", initial ? "(initial)" : oid ? oid : "(initial)", 0);
    g_string_append_printf(out, "# branch.head %s%c", branch ? branch : "(detached)", 0);
    g_string_append_printf(out, "# branch.upstream %s%c", kind == VCS_HG ? "default" : "server", 0);
  }
  for (guint i = 0; i < entries->len; i++) {
    VcsEntry *e = entries->pdata[i];
    if (e->code == 'U' || e->code == 'I') {
      char mark = e->code == 'U' ? '?' : '!';
      if (v1)
        g_string_append_printf(out, "%c%c %s%c", mark, mark, e->path, 0);
      else
        g_string_append_printf(out, "%c %s%c", mark, e->path, 0);
      continue;
    }
    if (e->code == '!') {
      if (v1)
        g_string_append_printf(out, "UU %s%c", e->path, 0);
      else
        g_string_append_printf(out, "u UU N... 000000 000000 000000 000000 0 0 0 %s%c", e->path, 0);
      continue;
    }
    gboolean in_index = staged && g_hash_table_contains(staged, e->path);
    char x = in_index ? e->code : '.', y = in_index ? '.' : e->code;
    gboolean moved = (e->code == 'R' || e->code == 'C') && e->orig;
    if (v1)
      g_string_append_printf(out, "%c%c %s%c", x == '.' ? ' ' : x, y == '.' ? ' ' : y, e->path, 0);
    else if (moved)
      g_string_append_printf(out, "2 %c%c N... 000000 000000 000000 0 0 %c100 %s%c", x, y, e->code, e->path, 0);
    else
      g_string_append_printf(out, "1 %c%c N... 000000 000000 000000 0 0 %s%c", x, y, e->path, 0);
    if (moved)
      g_string_append_printf(out, "%s%c", e->orig, 0);
  }
  g_ptr_array_unref(entries);
  g_free(oid);
  g_free(branch);
  *length = out->len;
  return g_string_free(out, FALSE);
}

/* Files ---------------------------------------------------------------------------------------- */

const char *const *vcs_files_args(VcsKind kind)
{
  static const char *const hg[] = { "status",      "--modified", "--added", "--clean", "--unknown",
                                    "--no-status", "--print0",   ".",       NULL };
  static const char *const svn[] = { "status", "--xml", "--verbose", "--non-interactive", NULL };

  return kind == VCS_HG ? hg : kind == VCS_SVN ? svn : NULL;
}

void vcs_files(VcsKind kind, const char *out, gsize size, GPtrArray *tracked, GPtrArray *unknown)
{
  if (kind == VCS_HG) {
    for (const char *p = out, *end = out ? out + size : NULL; out && p < end;) {
      const char *nul = memchr(p, '\0', end - p);
      gsize len = (nul ? nul : end) - p;
      if (len)
        g_ptr_array_add(tracked, g_strndup(p, len));
      p += len + 1;
    }
    return;
  }
  VcsNode *root = out ? vcs_xml_parse(out, size) : NULL;
  GPtrArray *list = vcs_all(root, "entry");
  for (guint i = 0; i < list->len; i++) {
    VcsNode *n = list->pdata[i];
    const char *path = vcs_attr(n, "path"), *item = vcs_attr(vcs_child(n, "wc-status"), "item");
    if (!path || !item || strcmp(path, ".") == 0)
      continue;
    if (strcmp(item, "unversioned") == 0)
      g_ptr_array_add(unknown, g_strdup(path));
    else if (strcmp(item, "deleted") != 0 && strcmp(item, "missing") != 0 && strcmp(item, "ignored") != 0 &&
             strcmp(item, "external") != 0 && strcmp(item, "none") != 0)
      g_ptr_array_add(tracked, g_strdup(path));
  }
  g_ptr_array_free(list, TRUE);
  if (root)
    vcs_node_free(root);
}

/* Branches -------------------------------------------------------------------------------------- */

char **vcs_branches_args(VcsKind kind, gboolean with_folder)
{
  if (kind == VCS_HG)
    return vcs_args_of("log", "-r", "sort((head() and not closed()) or bookmark(), '-date -rev')", "-T",
                       "{branch}\\x1f{join(bookmarks, '\\x1e')}\\x1f{date|age}\\x1f{desc|firstline}\\n", NULL);
  return vcs_args_of("list", "--xml", "--non-interactive", "^/", with_folder ? "^/branches" : NULL, NULL);
}

char *vcs_svn_branch_url(const char *name)
{
  return strcmp(name, "trunk") == 0 ? g_strdup("^/trunk") : g_strconcat("^/branches/", name, NULL);
}

typedef struct {
  char *name;
  gint64 revision, when;
  char *author;
} VcsBranch;

static int vcs_newer(gconstpointer a, gconstpointer b)
{
  const VcsBranch *x = *(VcsBranch *const *)a, *y = *(VcsBranch *const *)b;
  return x->revision < y->revision ? 1 : x->revision > y->revision ? -1 : 0;
}

static void vcs_branch_free(VcsBranch *b)
{
  g_free(b->name);
  g_free(b->author);
  g_free(b);
}

static void vcs_branch_line(GString *out, const char *name, const char *current, const char *ago, const char *subject)
{
  g_string_append_printf(out, "%c\trefs/heads/%s\t%s\t%s\t%s\n", g_strcmp0(name, current) == 0 ? '*' : ' ', name, name,
                         ago, subject);
}

char *vcs_git_branches(VcsKind kind, const char *out, const char *current)
{
  GString *lines = g_string_new(NULL);

  if (kind == VCS_HG) {
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    char **records = g_strsplit(out ? out : "", "\n", -1);
    for (int i = 0; records[i]; i++) {
      char **f = g_strsplit(records[i], "\x1f", 4);
      if (g_strv_length(f) == 4) {
        char **marks = g_strsplit(f[1], "\x1e", -1);
        for (int k = 0; marks[k]; k++)
          if (*marks[k] && g_hash_table_add(seen, g_strdup(marks[k])))
            vcs_branch_line(lines, marks[k], current, f[2], f[3]);
        g_strfreev(marks);
        if (g_hash_table_add(seen, g_strdup(f[0]))) /* the newest head of each branch */
          vcs_branch_line(lines, f[0], current, f[2], f[3]);
      }
      g_strfreev(f);
    }
    g_strfreev(records);
    g_hash_table_unref(seen);
    return g_string_free(lines, FALSE);
  }
  /* svn: the first list is the top (trunk in it), the second the branches folder. */
  VcsNode *root = out ? vcs_xml_parse(out, strlen(out)) : NULL;
  GPtrArray *lists = vcs_all(root, "list"), *found = g_ptr_array_new_with_free_func((GDestroyNotify)vcs_branch_free);
  for (guint l = 0; l < lists->len && l < 2; l++) {
    GPtrArray *entries = vcs_all(lists->pdata[l], "entry");
    for (guint i = 0; i < entries->len; i++) {
      VcsNode *e = entries->pdata[i], *commit = vcs_child(e, "commit");
      const char *name = vcs_child_text(e, "name");
      if (g_strcmp0(vcs_attr(e, "kind"), "dir") != 0 || !name || !*name || (l == 0 && strcmp(name, "trunk") != 0))
        continue;
      VcsBranch *b = g_new0(VcsBranch, 1);
      b->name = g_strdup(name);
      const char *revision = commit ? vcs_attr(commit, "revision") : NULL;
      b->revision = revision ? g_ascii_strtoll(revision, NULL, 10) : 0;
      b->when = vcs_svn_time(vcs_child_text(commit, "date"));
      const char *author = commit ? vcs_child_text(commit, "author") : "";
      b->author = *author ? g_strdup(author) : NULL;
      g_ptr_array_add(found, b);
    }
    g_ptr_array_free(entries, TRUE);
  }
  g_ptr_array_sort(found, vcs_newer);
  for (guint i = 0; i < found->len; i++) {
    VcsBranch *b = found->pdata[i];
    char *url = vcs_svn_branch_url(b->name), *ago = b->when >= 0 ? vcs_ago(b->when) : g_strdup("");
    char *subject =
        g_strdup_printf("r%" G_GINT64_FORMAT "%s%s", b->revision, b->author ? " · " : "", b->author ? b->author : "");
    vcs_branch_line(lines, b->name, g_strcmp0(url, current) == 0 ? b->name : NULL, ago, subject);
    g_free(subject);
    g_free(ago);
    g_free(url);
  }
  g_ptr_array_unref(found);
  g_ptr_array_free(lists, TRUE);
  if (root)
    vcs_node_free(root);
  return g_string_free(lines, FALSE);
}

/* Changelists ----------------------------------------------------------------------------------- */

GHashTable *vcs_svn_changelists(const char *status, gsize size)
{
  GHashTable *lists = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  VcsNode *root = status && size ? vcs_xml_parse(status, size) : NULL;
  GPtrArray *named = vcs_all(root, "changelist");

  for (guint i = 0; i < named->len; i++) {
    const char *name = vcs_attr(named->pdata[i], "name");
    GPtrArray *entries = vcs_all(named->pdata[i], "entry");
    for (guint k = 0; name && k < entries->len; k++) {
      const char *path = vcs_attr(entries->pdata[k], "path");
      if (path)
        g_hash_table_replace(lists, g_strdup(path), g_strdup(name));
    }
    g_ptr_array_free(entries, TRUE);
  }
  g_ptr_array_free(named, TRUE);
  if (root)
    vcs_node_free(root);
  return lists;
}

/* Shelves --------------------------------------------------------------------------------------- */

char *vcs_git_stashes(const char *out)
{
  /* "name" padded to 16 (a longer one runs into the age), "(age)" padded to 12, the description;
   * the age is hg's abbreviated one ("3d ago", "1h from now") or a date. */
  GRegex *line = g_regex_new("^(.+?) *\\(([0-9]+[YMWdhms] (?:ago|from now)|in the distant future|"
                             "[0-9]{4}-[0-9]{2}-[0-9]{2})\\) *(.*)$",
                             0, 0, NULL);
  GString *lines = g_string_new(NULL);
  char **records = g_strsplit(out ? out : "", "\n", -1);

  for (int i = 0; records[i]; i++) {
    GMatchInfo *match = NULL;
    if (g_regex_match(line, records[i], 0, &match)) {
      char *name = g_match_info_fetch(match, 1), *age = g_match_info_fetch(match, 2),
           *message = g_strstrip(g_match_info_fetch(match, 3));
      g_string_append_printf(lines, "%s\t%s\t%s\n", name, age, *message ? message : name);
      g_free(message);
      g_free(age);
      g_free(name);
    }
    g_match_info_free(match);
  }
  g_strfreev(records);
  g_regex_unref(line);
  return g_string_free(lines, FALSE);
}

/* Versions ------------------------------------------------------------------------------------- */

char *vcs_rev(VcsKind kind, const char *rev)
{
  if (kind == VCS_HG)
    return g_strdup(g_strcmp0(rev, "HEAD") == 0 ? "." : rev);
  if (kind == VCS_SVN) {
    if (!rev || strcmp(rev, "HEAD") == 0)
      return g_strdup("BASE");
    if (g_str_has_suffix(rev, "^")) {
      gint64 n = g_ascii_strtoll(rev, NULL, 10);
      return g_strdup_printf("%" G_GINT64_FORMAT, MAX(n - 1, 0));
    }
  }
  return g_strdup(rev);
}

char **vcs_cat_args(VcsKind kind, const char *rev, const char *path, const char *url)
{
  char *r = vcs_rev(kind, rev), **args;

  if (kind == VCS_HG) {
    args = vcs_args_of("cat", "-r", r, "--", path, NULL);
  } else if (kind == VCS_SVN) {
    if (url && strcmp(r, "BASE") != 0) {
      char *at = g_strdup_printf("%s/%s@%s", url, path, r);
      args = vcs_args_of("cat", "--non-interactive", at, NULL);
      g_free(at);
    } else {
      char *at = g_strdup_printf("%s@", path); /* a path with "@" in it keeps it */
      args = vcs_args_of("cat", "--non-interactive", "-r", r, at, NULL);
      g_free(at);
    }
  } else {
    char *spec = g_strconcat(r ? r : "HEAD", ":./", path, NULL);
    args = vcs_args_of("show", spec, NULL);
    g_free(spec);
  }
  g_free(r);
  return args;
}

char **vcs_id_args(VcsKind kind, const char *rev, gboolean shortened, const char *path)
{
  char *r = vcs_rev(kind, rev), **args;

  if (kind == VCS_HG)
    args = vcs_args_of("log", "-r", r, "-T", shortened ? "{node|short}" : "{node}", NULL);
  else if (kind == VCS_SVN && strcmp(r, "BASE") == 0) {
    char *at = g_strdup_printf("%s@", path ? path : "."); /* "@" ends a path holding one */
    args = vcs_args_of("info", "--non-interactive", "--show-item", "revision", "--", at, NULL);
    g_free(at);
  } else if (kind == VCS_SVN)
    args = vcs_args_of("log", "--non-interactive", "-q", "-l", "1", "-r", r, "--xml", NULL); /* checks it exists */
  else if (shortened)
    args = vcs_args_of("rev-parse", "--short", "--verify", "--quiet", r, NULL);
  else
    args = vcs_args_of("rev-parse", "--verify", "--quiet", r, NULL);
  g_free(r);
  return args;
}

/* Logs ------------------------------------------------------------------------------------------- */

#define HG_DATE "{date(date, \"%Y-%m-%d %H:%M\")}"

char **vcs_log_args(VcsKind kind, int max)
{
  char *n = g_strdup_printf("%d", max), **args;

  if (kind == VCS_HG)
    args = vcs_args_of("log", "-l", n, "-T",
                       "\\x1e{node}\\x1f{node|short}\\x1f{desc|firstline}\\x1f{author|person}\\x1f{author|email}\\x1f"
                       "{date|age}\\x1f" HG_DATE "\\x1f{join(bookmarks, ', ')}{if(tags, if(bookmarks, ', '))}"
                       "{join(tags, ', ')}\\x1f{desc}\\x1d{diffstat}\\n",
                       NULL);
  else
    args = vcs_args_of("log", "--non-interactive", "--xml", "-l", n, NULL);
  g_free(n);
  return args;
}

/* svn log --xml's entries: revision, author, seconds, message. */
typedef struct {
  const char *revision, *author, *message;
  gint64 when;
} VcsSvnCommit;

static GArray *vcs_svn_commits(VcsNode *root)
{
  GArray *commits = g_array_new(FALSE, TRUE, sizeof(VcsSvnCommit));
  GPtrArray *list = vcs_all(root, "logentry");

  for (guint i = 0; i < list->len; i++) {
    VcsNode *n = list->pdata[i];
    VcsSvnCommit c = { vcs_attr(n, "revision"), vcs_child_text(n, "author"), vcs_child_text(n, "msg"),
                       vcs_svn_time(vcs_child_text(n, "date")) };
    if (c.revision)
      g_array_append_val(commits, c);
  }
  g_ptr_array_free(list, TRUE);
  return commits;
}

char *vcs_git_log(VcsKind kind, const char *out)
{
  if (kind != VCS_SVN)
    return g_strdup(out);
  GString *log = g_string_new(NULL);
  VcsNode *root = vcs_xml_parse(out, strlen(out));
  GArray *commits = vcs_svn_commits(root);

  for (guint i = 0; i < commits->len; i++) {
    VcsSvnCommit *c = &g_array_index(commits, VcsSvnCommit, i);
    char *subject = vcs_first_line(c->message), *when = vcs_ago(c->when), *date = vcs_local_date(c->when);
    g_string_append_printf(log, "\x1e%s\x1fr%s\x1f%s\x1f%s\x1f\x1f%s\x1f%s\x1f\x1f%s\x1d\n", c->revision, c->revision,
                           subject, c->author, when, date, c->message);
    g_free(subject);
    g_free(when);
    g_free(date);
  }
  g_array_unref(commits);
  if (root)
    vcs_node_free(root);
  return g_string_free(log, FALSE);
}

char **vcs_file_log_args(VcsKind kind, int max, const char *name)
{
  char *n = g_strdup_printf("%d", max), **args;

  if (kind == VCS_HG)
    args = vcs_args_of("log", "-f", "-l", n, "-T",
                       "{node}\\x1f{node|short}\\x1f" HG_DATE "\\x1f{author|person}\\x1f{desc|firstline}\\n", "--",
                       name, NULL);
  else {
    char *at = g_strdup_printf("%s@", name); /* "@" ends a path holding one; "--": a name starting with "-" */
    args = vcs_args_of("log", "--non-interactive", "--xml", "-l", n, "--", at, NULL);
    g_free(at);
  }
  g_free(n);
  return args;
}

char *vcs_git_file_log(VcsKind kind, const char *out)
{
  if (kind != VCS_SVN)
    return g_strdup(out);
  GString *log = g_string_new(NULL);
  VcsNode *root = vcs_xml_parse(out, strlen(out));
  GArray *commits = vcs_svn_commits(root);

  for (guint i = 0; i < commits->len; i++) {
    VcsSvnCommit *c = &g_array_index(commits, VcsSvnCommit, i);
    char *subject = vcs_first_line(c->message), *date = vcs_local_date(c->when);
    g_string_append_printf(log, "%s\x1fr%s\x1f%s\x1f%s\x1f%s\n", c->revision, c->revision, date, c->author, subject);
    g_free(subject);
    g_free(date);
  }
  g_array_unref(commits);
  if (root)
    vcs_node_free(root);
  return g_string_free(log, FALSE);
}

char **vcs_messages_args(VcsKind kind, int max)
{
  char *n = g_strdup_printf("%d", max), **args;

  if (kind == VCS_HG)
    args = vcs_args_of("log", "-l", n, "-T", "{desc}\\x1e", NULL);
  else
    args = vcs_args_of("log", "--non-interactive", "--xml", "-l", n, NULL);
  g_free(n);
  return args;
}

char *vcs_git_messages(VcsKind kind, const char *out)
{
  if (kind != VCS_SVN)
    return g_strdup(out);
  GString *messages = g_string_new(NULL);
  VcsNode *root = vcs_xml_parse(out, strlen(out));
  GArray *commits = vcs_svn_commits(root);

  for (guint i = 0; i < commits->len; i++)
    g_string_append_printf(messages, "%s\x1e", g_array_index(commits, VcsSvnCommit, i).message);
  g_array_unref(commits);
  if (root)
    vcs_node_free(root);
  return g_string_free(messages, FALSE);
}

char **vcs_commit_files_args(VcsKind kind, const char *rev)
{
  if (kind == VCS_HG)
    return vcs_args_of("status", "--change", rev, "--copies", NULL); /* lines: the panel reads text */
  return vcs_args_of("log", "--non-interactive", "--xml", "-v", "-r", rev, NULL);
}

char *vcs_git_commit_files(VcsKind kind, const char *out, const char *url)
{
  GString *files = g_string_new(NULL);

  if (kind == VCS_HG) {
    GPtrArray *entries = vcs_hg_entries(out, strlen(out), '\n');
    for (guint i = 0; i < entries->len; i++) {
      VcsEntry *e = entries->pdata[i];
      if (e->code == 'R' || e->code == 'C')
        g_string_append_printf(files, "%c100\t%s\t%s\n", e->code, e->orig, e->path);
      else
        g_string_append_printf(files, "%c\t%s\n", e->code, e->path);
    }
    g_ptr_array_unref(entries);
    return g_string_free(files, FALSE);
  }
  /* svn: paths from the repository's root ("/trunk/d/f.c"); the working copy is url ("^/trunk"). */
  VcsNode *root = vcs_xml_parse(out, strlen(out));
  GPtrArray *list = vcs_all(root, "path");
  const char *prefix = url && g_str_has_prefix(url, "^") ? url + 1 : "";
  gsize prefix_len = strlen(prefix);

  for (guint i = 0; i < list->len; i++) {
    VcsNode *n = list->pdata[i];
    const char *path = n->text->str, *action = vcs_attr(n, "action"), *from = vcs_attr(n, "copyfrom-path");
    if (g_strcmp0(vcs_attr(n, "kind"), "dir") == 0 || !action || strncmp(path, prefix, prefix_len) != 0 ||
        path[prefix_len] != '/')
      continue;
    const char *rel = path + prefix_len + 1;
    if (*action == 'A' && from && strncmp(from, prefix, prefix_len) == 0 && from[prefix_len] == '/')
      g_string_append_printf(files, "C100\t%s\t%s\n", from + prefix_len + 1, rel);
    else
      g_string_append_printf(files, "%c\t%s\n", *action == 'R' ? 'M' : *action, rel);
  }
  g_ptr_array_free(list, TRUE);
  if (root)
    vcs_node_free(root);
  return g_string_free(files, FALSE);
}

char **vcs_show_args(VcsKind kind, const char *rev)
{
  if (kind == VCS_HG)
    return vcs_args_of("diff", "--git", "-c", rev, NULL);
  return vcs_args_of("diff", "--non-interactive", "--git", "-c", rev, NULL);
}

char *vcs_git_show(const char *patch)
{
  GString *show = g_string_new(NULL);
  char **lines = g_strsplit(patch ? patch : "", "\n", -1);
  char *path = NULL;
  int added = 0, removed = 0;
  gboolean in_hunk = FALSE;

  for (int i = 0;; i++) {
    gboolean next = !lines[i] || g_str_has_prefix(lines[i], "diff --git ");
    if (next && path) {
      g_string_append_printf(show, "%d\t%d\t%s\n", added, removed, path);
      g_clear_pointer(&path, g_free);
    }
    if (!lines[i])
      break;
    if (next) {
      const char *b = strstr(lines[i], " b/");
      path = g_strdup(b ? b + 3 : lines[i] + strlen("diff --git "));
      added = removed = 0;
      in_hunk = FALSE;
    } else if (g_str_has_prefix(lines[i], "@@")) {
      in_hunk = TRUE;
    } else if (in_hunk && lines[i][0] == '+') {
      added++;
    } else if (in_hunk && lines[i][0] == '-') {
      removed++;
    }
  }
  g_strfreev(lines);
  g_string_append(show, patch ? patch : "");
  return g_string_free(show, FALSE);
}

/* Blame ------------------------------------------------------------------------------------------ */

char **vcs_blame_args(VcsKind kind, const char *name)
{
  if (kind == VCS_HG)
    return vcs_args_of("annotate", "-r", "wdir()", "-T",
                       "{lines % \"{node}\\x1f{user}\\x1f{date|hgdate}\\x1f{desc|firstline}\\n\"}", "--", name, NULL);
  char *at = g_strdup_printf("%s@", name); /* "@" ends a path holding one; "--": a name starting with "-" */
  char **args = vcs_args_of("blame", "--non-interactive", "--xml", "--", at, NULL);

  g_free(at);
  return args;
}

static char *vcs_porcelain(const char *commit, const char *author, gint64 when, const char *summary)
{
  return g_strdup_printf("%s 1 1 1\nauthor %s\nauthor-time %" G_GINT64_FORMAT "\nsummary %s\n\t\n", commit, author,
                         when, summary);
}

#define NOT_COMMITTED "0000000000000000000000000000000000000000"

char *vcs_git_blame(VcsKind kind, const char *out, int line)
{
  char *text = NULL;

  if (!out || line < 0)
    return NULL;
  if (kind == VCS_HG) {
    char **lines = g_strsplit(out, "\n", -1);
    if ((guint)line < g_strv_length(lines)) {
      char **f = g_strsplit(lines[line], "\x1f", 4);
      if (g_strv_length(f) == 4) {
        gboolean working = strspn(f[0], "f") == strlen(f[0]);
        char *person = g_strdup(f[1]), *lt = strchr(person, '<');
        if (lt && lt > person) /* "Ada <ada@x>": the name */
          *lt = '\0';
        text = vcs_porcelain(working ? NOT_COMMITTED : f[0], g_strstrip(person), g_ascii_strtoll(f[2], NULL, 10), f[3]);
        g_free(person);
      }
      g_strfreev(f);
    }
    g_strfreev(lines);
    return text;
  }
  VcsNode *root = vcs_xml_parse(out, strlen(out));
  GPtrArray *list = vcs_all(root, "entry");
  char *number = g_strdup_printf("%d", line + 1);

  for (guint i = 0; i < list->len && !text; i++) {
    VcsNode *n = list->pdata[i], *commit = vcs_child(n, "commit");
    if (g_strcmp0(vcs_attr(n, "line-number"), number) != 0)
      continue;
    if (!commit) {
      text = vcs_porcelain(NOT_COMMITTED, "", 0, "");
    } else {
      char *summary = g_strdup_printf("r%s", vcs_attr(commit, "revision"));
      text = vcs_porcelain(vcs_attr(commit, "revision"), vcs_child_text(commit, "author"),
                           vcs_svn_time(vcs_child_text(commit, "date")), summary);
      g_free(summary);
    }
  }
  g_free(number);
  g_ptr_array_free(list, TRUE);
  if (root)
    vcs_node_free(root);
  return text;
}
