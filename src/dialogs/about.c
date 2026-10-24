/*
 * The About window (the header bar's menu, or "About t-code" in the command palette): GTK's own
 * GtkAboutDialog. Its System tab lists the libraries and the programs t-code runs, with their
 * versions, so a bug report can be pasted from it. The programs' versions come from running them
 * with --version asynchronously, filled in as they answer.
 */
#include "dialogs/about.h"

#include <string.h>
#ifdef __GLIBC__
#include <gnu/libc-version.h>
#endif

#ifndef TCODE_VERSION /* set by the Makefile */
#define TCODE_VERSION "unknown"
#endif

static const char license[] =
#include "license.h"
    ;

static const char *const tools[] = { "clangd", "gdb", "git", "rg" };

typedef struct {
  GtkAboutDialog *dialog; /* weak: NULL once the window is closed */
  char *libraries;        /* the part known without running anything */
  char *answers[G_N_ELEMENTS(tools)];
} SystemInfo;

/* Fills the System tab: the libraries, then each program's version, or "…" while it has not
 * answered. */
static void show_system_info(SystemInfo *info)
{
  GString *text = g_string_new(info->libraries);

  g_string_append(text, "\nPrograms\n");
  for (size_t i = 0; i < G_N_ELEMENTS(tools); i++)
    g_string_append_printf(text, "  %-8s %s\n", tools[i], info->answers[i] ? info->answers[i] : "…");
  gtk_about_dialog_set_system_information(info->dialog, text->str);
  g_string_free(text, TRUE);
}

/* Frees what the last reference to a SystemInfo held (g_atomic_rc_box_release_full). */
static void system_info_clear(gpointer data)
{
  SystemInfo *info = data;

  if (info->dialog)
    g_object_remove_weak_pointer(G_OBJECT(info->dialog), (gpointer *)&info->dialog);
  g_free(info->libraries);
  for (size_t i = 0; i < G_N_ELEMENTS(tools); i++)
    g_free(info->answers[i]);
}

/* Drops a reference; the window and every running --version hold one. */
static void system_info_unref(SystemInfo *info)
{
  g_atomic_rc_box_release_full(info, system_info_clear);
}

typedef struct {
  SystemInfo *info;
  size_t tool;
} Query;

/* A program answered --version (or failed): its version goes into the System tab, if the window is
 * still open. */
static void on_version(GObject *source, GAsyncResult *result, gpointer data)
{
  Query *query = data;
  SystemInfo *info = query->info;
  char *out = NULL;

  if (g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result, &out, NULL, NULL) && out) {
    /* The first line from its first word that starts with a digit: "clangd version 21.1.8 (…)",
         * "Version: 2.0.15" and "ripgrep 15.2.0 (rev …)" all start at the version. */
    char *line = g_strstrip(g_strndup(out, strcspn(out, "\n")));
    const char *version = line;

    while (*version && !(g_ascii_isdigit(*version) && (version == line || version[-1] == ' ')))
      version++;
    info->answers[query->tool] = g_strdup(*version ? version : line);
    g_free(line);
  } else {
    info->answers[query->tool] = g_strdup("not found");
  }
  g_free(out);
  if (info->dialog)
    show_system_info(info);
  system_info_unref(info);
  g_free(query);
}

/* Runs "<tool> --version" in the background; on_version takes the answer. */
static void ask_version(SystemInfo *info, size_t tool)
{
  GSubprocess *process = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, NULL,
                                          tools[tool], "--version", NULL);
  Query *query;

  if (!process) {
    info->answers[tool] = g_strdup("not found");
    return;
  }
  query = g_new(Query, 1);
  *query = (Query){ g_atomic_rc_box_acquire(info), tool };
  g_subprocess_communicate_utf8_async(process, NULL, NULL, on_version, query);
  g_object_unref(process);
}

/* The part of the System tab known without running anything: t-code's own paths and the library
 * versions. */
static char *libraries(App *t)
{
  /* clang-format off */
  return g_strdup_printf("t-code %s\n"
                         "  styles        %s\n"
                         "\n"
                         "Libraries\n"
                         "  GTK            %u.%u.%u\n"
                         "  GtkSourceView  %u.%u.%u\n"
                         "  VTE            %u.%u.%u\n"
                         "  GLib           %u.%u.%u\n"
#ifdef __GLIBC__
                         "  glibc          %s\n",
#else
                         "  C library      %s\n",
#endif
                         TCODE_VERSION, t->paths.styles,
                         gtk_get_major_version(), gtk_get_minor_version(), gtk_get_micro_version(),
                         gtk_source_get_major_version(), gtk_source_get_minor_version(), gtk_source_get_micro_version(),
                         vte_get_major_version(), vte_get_minor_version(), vte_get_micro_version(),
                         glib_major_version, glib_minor_version, glib_micro_version,
#ifdef __GLIBC__
                         gnu_get_libc_version()
#else
                         "not glibc (musl)"
#endif
  );
  /* clang-format on */
}

void show_about(App *t)
{
  static GtkWidget *window;
  static const char *const authors[] = {
    "suacayipkisi https://github.com/suacayipkisi",
    "ibrahmsql https://github.com/ibrahmsql",
    NULL
  };
  static const char *const built_on[] = {
    "clangd https://clangd.llvm.org",
    "GDB https://www.sourceware.org/gdb",
    "GTK https://www.gtk.org",
    "GtkSourceView https://gitlab.gnome.org/GNOME/gtksourceview",
    "VTE https://gitlab.gnome.org/GNOME/vte",
    "md4c https://github.com/mity/md4c",
    "Cascadia Code https://github.com/microsoft/cascadia-code",
    NULL,
  };
  SystemInfo *info;

  if (window) {
    gtk_window_present(GTK_WINDOW(window));
    return;
  }
  /* clang-format off */
  window = g_object_new(GTK_TYPE_ABOUT_DIALOG,
                        "transient-for", t->window,
                        "modal", TRUE,
                        "program-name", "t-code",
                        "version", TCODE_VERSION,
                        "logo-icon-name", "io.github.tcode",
                        "comments", "A lightweight C/C++ IDE:\n"
                                    "a file manager, editor tabs and a shell, sharing language servers",
                        "website", "https://github.com/suacayipkisi/t-code",
                        "website-label", "github.com/suacayipkisi/t-code",
                        "copyright", "© 2026 suacayipkisi",
                        "license", license,
                        "wrap-license", TRUE,
                        "authors", authors,
                        NULL);
  /* clang-format on */
  gtk_about_dialog_add_credit_section(GTK_ABOUT_DIALOG(window), "Built on", (const char **)built_on);
  g_object_add_weak_pointer(G_OBJECT(window), (gpointer *)&window);

  info = g_atomic_rc_box_new0(SystemInfo);
  info->dialog = GTK_ABOUT_DIALOG(window);
  g_object_add_weak_pointer(G_OBJECT(window), (gpointer *)&info->dialog);
  info->libraries = libraries(t);
  for (size_t i = 0; i < G_N_ELEMENTS(tools); i++)
    ask_version(info, i);
  show_system_info(info);
  system_info_unref(info);
  gtk_window_present(GTK_WINDOW(window));
}
