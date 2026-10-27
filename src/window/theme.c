/*
 * t-code's look: the color theme's colors (window/palette.c) on rounded panels ("cards") that sit on the darker
 * frame of the title bar and the status bar. GTK's built-in theme is the base, not the desktop's
 * (Yaru, Breeze, ...), so t-code looks the same everywhere. This CSS goes over it, and the panels'
 * own CSS (window/statusbar.c, git/scm.c, popups/picker.c, ...) over this. The square theme goes
 * over the panels' CSS, and the user's theme.css over everything.
 */
#include "window/theme.h"
#include "application/identity.h"
#include "window/palette.h"
#include "editor/editorview.h"
#include "editor/preview/preview.h"
#include "editor/tabs.h"
#include "files/files.h"
#include "lsp/inline/semantic.h"
#include "popups/picker.h"
#include "project/session.h"
#include "project/userfiles.h"
#include "window/terminal.h"

#define CARD_RADIUS "10px"

static const char *const css =
    /* The frame: title bar, status bar and the gaps between the cards. */
    "window.background { background: var(--tc-frame); color: var(--tc-fg); }"
    ".tcode-workspace { margin: 0 6px; }"
    "paned > separator { min-width: 6px; min-height: 6px; background: none; box-shadow: none; border: none; }"
    ".tcode-card { background: var(--tc-panel); border-radius: " CARD_RADIUS "; }"
    ".tcode-card paned > separator { min-width: 1px; min-height: 1px; background: var(--tc-border); }"
    ".tcode-shell { padding: 6px 8px; }"

    /* Title bar: flat rounded buttons, and window controls of our own instead of the theme's
     * circles (the close button turns red under the pointer). */
    "headerbar { min-height: 30px; padding: 0 4px 0 6px; background: var(--tc-frame); box-shadow: none; border: none; }"
    /* Thin: the theme's own heights and paddings (46px in all) go. */
    "headerbar * { min-height: 0; margin-top: 0; margin-bottom: 0; padding-top: 0; padding-bottom: 0; }"
    "headerbar .title { font-size: 0.9em; font-weight: normal; color: var(--tc-fg); }"
    "headerbar button { min-height: 22px; min-width: 24px; padding: 0 5px; margin: 4px 1px;"
    "  border-radius: 7px; background: none; box-shadow: none; border: none; color: var(--tc-fg); }"
    "headerbar button:hover { background: color-mix(in srgb, var(--tc-fg) 8%, transparent); }"
    "headerbar button:active, headerbar button:checked { background: color-mix(in srgb, var(--tc-fg) 14%, transparent); color: var(--tc-fg-strong); }"
    /* The menu bar (Files, T-Code, ...): its items look like the buttons, the open one pressed. */
    "headerbar menubar { padding: 0; }"
    "headerbar menubar > item { min-height: 22px; padding: 0 8px; margin: 4px 1px; border-radius: 7px; color: var(--tc-fg); }"
    "headerbar menubar > item:hover { background: color-mix(in srgb, var(--tc-fg) 8%, transparent); }"
    "headerbar menubar > item:selected { background: color-mix(in srgb, var(--tc-fg) 14%, transparent); color: var(--tc-fg-strong); }"
    "windowcontrols { border-spacing: 2px; margin-left: 8px; }"
    "windowcontrols > button { min-width: 36px; min-height: 22px; padding: 0; margin: 4px 0; border-radius: 7px; }"
    "windowcontrols > button > image, windowcontrols > button:hover > image,"
    "windowcontrols > button:active > image { background: none; box-shadow: none; padding: 0; margin: 0;"
    "  border-radius: 0; min-width: 16px; min-height: 16px; -gtk-icon-size: 14px; }"
    "windowcontrols > button.close:hover { background: #c42b1c; color: #ffffff; }"
    "windowcontrols > button.close:active { background: #a52617; color: #ffffff; }"

    /* Editor tabs (editor/tabs.c's strip over each group): rounded on top, the current one in the
     * editor's color, flowing into it. */
    "notebook, notebook.frame { background: none; border: none; }"
    ".tcode-tabs { background: var(--tc-frame); padding: 4px 4px 0; }"
    ".tcode-tab { min-height: 30px; padding: 0 4px 0 12px; margin: 0 1px; border-radius: 9px 9px 0 0;"
    "  color: var(--tc-fg-dim); }"
    ".tcode-tab:hover { background: var(--tc-raised); color: var(--tc-fg); }"
    ".tcode-tab:checked { background: var(--tc-panel); color: var(--tc-fg-strong); box-shadow: inset 0 2px var(--tc-accent); }"
    ".tcode-tab button { min-width: 20px; min-height: 20px; padding: 0; margin: 0;"
    "  border-radius: 6px; background: none; box-shadow: none; opacity: 0; }"
    ".tcode-tab:hover button, .tcode-tab:checked button { opacity: 1; }"
    ".tcode-tab button:hover { background: color-mix(in srgb, var(--tc-fg) 12%, transparent); }"
    ".tcode-tab label.temporary { font-style: italic; }"
    /* The strip's scrollbar: thin, over the tabs' bottom edge. */
    ".tcode-tabs scrollbar.horizontal slider { min-height: 3px; border-radius: 0; background: rgba(121, 121, 121, .4); }"
    ".tcode-tabs scrollbar.horizontal:hover slider { min-height: 5px; background: rgba(100, 100, 100, .7); }"
    ".tcode-tabs scrollbar { background: none; border: none; }"
    "notebook > stack { background: var(--tc-panel); }"

    /* Everything else rounded too. */
    "button, entry, spinbutton, textview, .tcode-scm .message { border-radius: 8px; }"
    "tooltip, tooltip.background { border-radius: 8px; background: var(--tc-raised); border: 1px solid var(--tc-border-strong); color: var(--tc-fg); }"
    "popover > contents, popover > arrow { background: var(--tc-raised); border-color: var(--tc-border-strong); }"
    "popover > contents { border-radius: 10px; }"
    "popover modelbutton { border-radius: 6px; }";

/* The square theme: flat panels, side by side with a line between them, square tabs with
 * the accent line on top of the current one, and small corners on the controls. Over the panels'
 * own CSS, so their corners go too. */
static const char *const square_css =
    ".tcode-workspace { margin: 0; }"
    "paned > separator { min-width: 1px; min-height: 1px; background: var(--tc-border); }"
    ".tcode-card { border-radius: 0; }"
    "headerbar { border-bottom: 1px solid var(--tc-border); }"
    "headerbar button, windowcontrols > button { border-radius: 4px; }"
    /* The menu bar and its menus without corners: with the menus' shadow, even small ones looked round. */
    "headerbar menubar > item, menubar popover > contents, menubar popover modelbutton { border-radius: 0; }"
    ".tcode-status { border-top: 1px solid var(--tc-border); padding: 0 6px; }"
    ".tcode-status button { border-radius: 0; padding: 3px 8px; }"

    ".tcode-tabs { padding: 0; box-shadow: inset 0 -1px var(--tc-border); }"
    /* 36px and the line under them, level with the docks' headers beside them. */
    ".tcode-tab { min-height: 37px; margin: 0; border-radius: 0;"
    "  box-shadow: inset -1px 0 var(--tc-border), inset 0 -1px var(--tc-border); }"
    ".tcode-tab:hover { background: var(--tc-panel); }"
    /* The current tab keeps the line under the tabs too: flowing into the editor, it
     * looked a pixel or two taller than the others. */
    ".tcode-tab:checked { box-shadow: inset 0 1px var(--tc-accent), inset -1px 0 var(--tc-border), inset 0 -1px var(--tc-border); }"
    ".tcode-tab button { border-radius: 4px; }"

    "button, entry, spinbutton, textview, .tcode-scm .message { border-radius: 2px; }"
    "tooltip, tooltip.background { border-radius: 2px; }"
    "popover > contents { border-radius: 4px; }"
    "popover modelbutton { border-radius: 2px; }"
    ".tcode-dock-header { background: var(--tc-frame); }"
    ".tcode-sidebar stackswitcher button, .tcode-dock-close { border-radius: 4px; }"
    ".tcode-scm row, .tcode-search row { margin: 0; border-radius: 0; }"
    ".tcode-search .field, .tcode-search button, .tcode-find entry, .tcode-find button,"
    ".tcode-keybinds .keys, .tcode-signature { border-radius: 2px; }"
    ".tcode-find { border-radius: 0; }"
    ".tcode-picker { border-radius: 4px; }"
    ".tcode-picker row { border-radius: 2px; }"
    ".tcode-editor scrollbar slider { border-radius: 0; }";

static GtkCssProvider *square;    /* square_css while the square theme is on */
static GtkCssProvider *user_look; /* theme.css */

/* Before any window: GTK picks up the theme name when it styles the first one. Light or dark as the
 * color theme says, whatever the desktop prefers (window/palette.c): GTK's light variant under a dark
 * theme left light buttons and dark text on dark panels. */
void load_theme(App *t)
{
  GtkCssProvider *provider = gtk_css_provider_new();

  g_object_set(gtk_settings_get_default(), "gtk-theme-name", "Adwaita", NULL);
  move_old_colors();                        /* before the editors look for it */
  palette_init(t, t->settings.color_theme); /* the colors' CSS variables, under everything here */
  gtk_css_provider_load_from_string(provider, css);
  /* Under the panels' own CSS, which uses GTK_STYLE_PROVIDER_PRIORITY_APPLICATION. */
  gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(provider),
                                             GTK_STYLE_PROVIDER_PRIORITY_APPLICATION - 1);
  g_object_unref(provider);
  square = gtk_css_provider_new();
  gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(square),
                                             GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
  user_look = gtk_css_provider_new();
  gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(user_look),
                                             GTK_STYLE_PROVIDER_PRIORITY_USER);
  apply_theme(t);
  load_user_theme();
}

void apply_theme(App *t)
{
  if (square)
    gtk_css_provider_load_from_string(square, t->settings.square ? square_css : "");
}

char *user_theme_file(void)
{
  return g_build_filename(g_get_user_config_dir(), TCODE_NAME, "theme.css", NULL);
}

void load_user_theme(void)
{
  char *path = user_theme_file(), *text = NULL;

  if (!user_look)
    return;
  g_file_get_contents(path, &text, NULL, NULL); /* none: nothing over t-code's own */
  gtk_css_provider_load_from_string(user_look, text ? text : "");
  g_free(text);
  g_free(path);
}

/* A rounded panel: its children (terminals, the minimap, the scrollbar) are cut to its corners. */
void make_card(GtkWidget *widget)
{
  gtk_widget_add_css_class(widget, "tcode-card");
  gtk_widget_set_overflow(widget, GTK_OVERFLOW_HIDDEN);
}

/* Color themes ---------------------------------------------------------------------------------- */

void color_theme_apply(App *t, const char *id)
{
  palette_init(t, id);
  terminal_apply_colors(t);
  files_restyle(t);
  for (int i = 0; i < tab_count(t); i++) {
    Tab *tab = tab_nth(t, i);
    if (tab->kind == TAB_EDITOR) {
      editor_view_apply_settings(tab); /* the new scheme */
      semantic_recolor(tab);
    } else {
      preview_restyle(tab);
    }
  }
}

/* The picker: "Preferences: Color Theme…" -------------------------------------------------- */

static void theme_query(App *t, const char *text, guint generation)
{
  for (int i = 0; i < palette_count(); i++)
    if (!*text || fuzzy_score(text, palette_nth_name(i)) >= 0)
      picker_add(t, generation, palette_nth_name(i), g_strcmp0(palette_nth_id(i), palette_id()) == 0 ? "current" : "",
                 palette_nth_id(i));
}

static void theme_choose(App *t, const char *id)
{
  g_set_str(&t->settings.color_theme, id);
  color_theme_apply(t, id);
  session_save_settings(t);
}

/* The arrows went to a theme: shown at once, not saved. */
static void theme_preview(App *t, const char *id)
{
  color_theme_apply(t, id);
}

/* Closed without a pick: the saved theme again. */
static void theme_cancel(App *t)
{
  if (g_strcmp0(palette_id(), t->settings.color_theme) != 0)
    color_theme_apply(t, t->settings.color_theme);
}

void color_theme_pick(App *t)
{
  picker_open(t, "Pick a color theme (the arrows show each one)", theme_query, theme_choose);
  picker_set_preview(t, theme_preview, theme_cancel);
}
