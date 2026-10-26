/*
 * Snippets (see snippets.h), through GtkSourceView's snippet manager and its completion provider.
 */
#include "editor/editing/snippets.h"

const char *const snippets_template =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<!--\n"
    "  Your snippets, beside t-code's own, read when t-code starts (GtkSourceView reads snippet files\n"
    "  once): restart it after a change. Each snippet has a\n"
    "  trigger (typed, then Tab) and a text for some languages (GtkSourceView's ids: c, cpp, python3,\n"
    "  go, rust, js, typescript, sh, ...; none for all). In the text, $1, $2 ... are the fields Tab goes\n"
    "  through, ${1:default} gives one a default, a field repeated mirrors the first, and $0 is where\n"
    "  the cursor ends. More files (*.snippets) in this folder are read too.\n"
    "-->\n"
    "<snippets _group=\"Mine\">\n"
    "  <snippet _name=\"todo\" trigger=\"todo\" _description=\"a TODO comment\">\n"
    "    <text languages=\"c;chdr;cpp;cpphdr;go;rust;js;typescript\"><![CDATA[// TODO(${1:me}): $0]]></text>\n"
    "  </snippet>\n"
    "</snippets>\n";

char *snippets_user_file(void)
{
  return g_build_filename(g_get_user_config_dir(), "t-code", "snippets", "user.snippets", NULL);
}

void snippets_init(App *t)
{
  char *user = g_build_filename(g_get_user_config_dir(), "t-code", "snippets", NULL);
  const char *const path[] = { user, t->paths.snippets, NULL };

  /* Before any editor: the manager reads its files once, the first time a snippet is looked for. */
  gtk_source_snippet_manager_set_search_path(gtk_source_snippet_manager_get_default(), path);
  g_free(user);
}

void snippets_attach(Tab *tab)
{
  GtkSourceCompletion *completion = gtk_source_view_get_completion(tab->source);
  GtkSourceCompletionSnippets *provider = gtk_source_completion_snippets_new();

  gtk_source_view_set_enable_snippets(tab->source, TRUE);
  gtk_source_completion_add_provider(completion, GTK_SOURCE_COMPLETION_PROVIDER(provider));
  g_object_unref(provider); /* the completion holds it */
}
