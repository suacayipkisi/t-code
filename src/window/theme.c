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
#include "popups/menus.h"
#include "popups/picker.h"
#include "project/session.h"
#include "project/userfiles.h"
#include "window/terminal.h"

#include <string.h>

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
    /* Settings, Keybinds, ... (their windows' "tcode-dialog"): an edge that shows on any theme, black on
     * black too, where the theme's dark shadow does not. */
    "window.csd.tcode-dialog { box-shadow: 0 0 0 1px color-mix(in srgb, var(--tc-fg) 24%, var(--tc-panel)),"
    "  0 4px 18px 4px rgba(0, 0, 0, .5); }"
    "window.solid-csd.tcode-dialog, window.tcode-dialog:not(.csd) { border: 1px solid color-mix(in srgb, var(--tc-fg) 24%, var(--tc-panel)); }"
    "windowcontrols { border-spacing: 2px; margin-left: 8px; }" /* their icons: titlebar.c */
    "windowcontrols > button { min-width: 36px; min-height: 22px; padding: 0; margin: 4px 0; border-radius: 7px; }"
    "windowcontrols > button > image, windowcontrols > button:hover > image,"
    "windowcontrols > button:active > image { background: none; box-shadow: none; padding: 0; margin: 0;"
    "  border-radius: 0; min-width: 16px; min-height: 16px; -gtk-icon-size: 14px; }"
    "windowcontrols > button.close:hover { background: #c42b1c; color: #ffffff; }"
    "windowcontrols > button.close:active { background: #a52617; color: #ffffff; }"
    /* The trees' arrows (Files, Outline, ...): t-code's own chevrons instead of the theme's. */
    "treeexpander expander { -gtk-icon-source: -gtk-icontheme('tcode-chevron-right-symbolic'); }"
    "treeexpander expander:checked { -gtk-icon-source: -gtk-icontheme('tcode-chevron-down-symbolic'); }"

    /* Editor tabs (editor/tabs.c's strip over each group): rounded on top, the current one in the
     * editor's color, flowing into it. */
    "notebook, notebook.frame { background: none; border: none; }"
    ".tcode-tabs { background: var(--tc-frame); padding: 4px 4px 0 0; }"
    ".tcode-drop-hint { color: var(--tc-accent); }" /* editor/dropzone.c's markers */
    ".tcode-tab { min-height: 30px; padding: 0 4px 0 12px; margin: 0 1px; border-radius: 9px 9px 0 0;"
    "  color: var(--tc-fg-dim); }"
    ".tcode-tab:first-child { margin-left: 0; }" /* the first tab at the group's left edge */
    ".tcode-tab:hover { background: var(--tc-raised); color: var(--tc-fg); }"
    ".tcode-tab:checked { background: var(--tc-panel); color: var(--tc-fg-strong); box-shadow: inset 0 2px var(--tc-accent); }"
    ".tcode-tab button { min-width: 20px; min-height: 20px; padding: 0; margin: 0;"
    "  border-radius: 6px; background: none; box-shadow: none; opacity: 0; }"
    ".tcode-tab:hover button, .tcode-tab:checked button { opacity: 1; }"
    ".tcode-tab button:hover { background: color-mix(in srgb, var(--tc-fg) 12%, transparent); }"
    ".tcode-tab label.temporary { font-style: italic; }"
    ".tcode-terminal-tab button { min-width: 20px; min-height: 20px; padding: 0; margin: 0;"
    "  border-radius: 6px; background: none; box-shadow: none; }"
    ".tcode-terminal-tab button:hover { background: color-mix(in srgb, var(--tc-fg) 12%, transparent); }"
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
    "popover modelbutton { border-radius: 6px; }"
    /* Menus (the menu bar's, t-code's icon's, the right-click ones): roomy rows, t-code's hover and
     * chevrons, dim shortcuts and section titles. */
    "popover.menu > contents { padding: 5px; }"
    /* The drop-downs' lists (Settings): GTK's 8 px margin around the list goes, and its own background,
     * which showed at the rounded rows' corners. */
    "dropdown > popover > contents { padding: 3px; }"
    "dropdown > popover listview { margin: 0; padding: 0; background: none; }"
    "dropdown > popover listview > row image { margin-left: 8px; }" /* the check after the current choice */
    "dropdown > popover listview > row { padding: 3px 8px; border-radius: 6px; color: var(--tc-fg); }"
    "dropdown > popover listview > row:hover, dropdown > popover listview > row:selected {"
    "  background: color-mix(in srgb, var(--tc-fg) 10%, transparent); color: var(--tc-fg-strong); }"
    "popover.menu modelbutton { min-height: 26px; padding: 0 10px; color: var(--tc-fg); }"
    "popover.menu modelbutton:hover, popover.menu modelbutton:selected {"
    "  background: color-mix(in srgb, var(--tc-fg) 10%, transparent); color: var(--tc-fg-strong); }"
    "popover.menu modelbutton:disabled { color: var(--tc-fg-dim); }"
    "popover.menu modelbutton accelerator { color: var(--tc-fg-dim); margin-left: 28px; }"
    "popover.menu modelbutton > arrow { -gtk-icon-source: -gtk-icontheme('tcode-chevron-right-symbolic');"
    "  -gtk-icon-size: 12px; min-width: 12px; min-height: 12px; margin-left: 16px; color: var(--tc-fg-dim); }"
    "popover.menu modelbutton > check { margin-right: 8px; }"
    /* popups/menus.c's icons beside the often used items; the others' labels line up with theirs */
    "popover.menu modelbutton > image { margin-right: 8px; min-width: 16px; -gtk-icon-size: 16px; color: var(--tc-fg-dim); }"
    "popover.menu modelbutton:hover > image { color: var(--tc-fg-strong); }"
    "popover.menu modelbutton.tcode-no-icon > label { margin-left: 24px; }"
    "popover.menu modelbutton > check:checked { color: var(--tc-accent); }"
    "popover.menu label.title { color: var(--tc-fg-dim); font-size: smaller; font-weight: bold; padding: 6px 10px 2px; }"
    "popover.menu separator { background: var(--tc-border); margin: 4px 6px; min-height: 1px; }";

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
    "popover modelbutton, dropdown > popover listview > row { border-radius: 2px; }"
    ".tcode-dock-header { background: var(--tc-frame); }"
    ".tcode-sidebar stackswitcher button, .tcode-dock-close { border-radius: 4px; }"
    ".tcode-scm row, .tcode-search row { margin: 0; border-radius: 0; }"
    ".tcode-search .field, .tcode-search button, .tcode-find entry, .tcode-find button,"
    ".tcode-keybinds .keys, .tcode-signature { border-radius: 2px; }"
    ".tcode-find { border-radius: 0; }"
    ".tcode-picker { border-radius: 4px; }"
    ".tcode-picker-list row { border-radius: 2px; }"
    ".tcode-search-bar { border-radius: 2px; }"
    ".tcode-search-bar.open { border-radius: 2px 2px 0 0; }"
    ".tcode-picker-pop > contents { border-radius: 0 0 2px 2px; }"
    ".tcode-editor scrollbar slider { border-radius: 0; }";

/* Compact: thinner tabs, dock headers, terminal tabs and menus, over the theme's and the panels' sizes. The
 * editor tabs keep the docks' header height beside them (26 px icons + its line, the square theme's
 * tabs draw that line themselves). */
static const char *const compact_css =
    ".tcode-tabs { padding: 3px 3px 0 0; }"
    ".tcode-tab { min-height: 24px; padding: 0 2px 0 9px; }"
    ".tcode-tab button, .tcode-terminal-tab button, .tcode-terminals-strip button {"
    "  min-width: 16px; min-height: 16px; }"
    ".tcode-sidebar stackswitcher { padding: 2px; border-spacing: 2px; }"
    ".tcode-sidebar stackswitcher button, .tcode-dock-close { min-height: 20px; min-width: 20px; padding: 1px 3px; }"
    ".tcode-terminal-tab { min-height: 20px; padding: 0 2px 0 6px; }"
    ".tcode-terminal-tab.first { padding-right: 6px; }"
    "dropdown > popover listview > row { min-height: 20px; padding: 2px 6px; }"
    "headerbar { min-height: 28px; }"
    "headerbar button, headerbar menubar > item, windowcontrols > button { min-height: 20px; }"
    /* Menus: shorter rows, a smaller font. */
    "popover.menu > contents { padding: 3px; }"
    "popover.menu modelbutton { min-height: 22px; padding: 0 8px; }"
    "popover.menu label.title { padding: 4px 8px 1px; }"
    "popover.menu separator { margin: 3px 4px; }"
    "popover.menu, popover.tcode-menu { font-size: 0.93em; }"
    "popover.tcode-menu > contents { padding: 3px; }"
    "popover.tcode-menu button { min-height: 22px; padding: 0 8px; }";

static const char *const compact_square_css = ".tcode-tab { min-height: 27px; }";

static GtkCssProvider *square;    /* square_css while the square theme is on, else corners_css() */
static GtkCssProvider *layout;    /* compact_css and the dividers between the panels (layout_css()) */
static GtkCssProvider *user_look; /* theme.css */

/* A divider color GTK does not read (settings.ini's divider-color typed by hand): said in the log. */
static void on_layout_error(GtkCssProvider *provider G_GNUC_UNUSED, GtkCssSection *section, const GError *error,
                            gpointer data G_GNUC_UNUSED)
{
  char *where = gtk_css_section_to_string(section);

  g_warning("the layout's CSS (divider-color?): %s at %s", error->message, where);
  g_free(where);
}

/* Before any window: GTK picks up the theme name when it styles the first one. Light or dark as the
 * color theme says, whatever the desktop prefers (window/palette.c): GTK's light variant under a dark
 * theme left light buttons and dark text on dark panels. */
void load_theme(App *t)
{
  GtkCssProvider *provider;

  if (square) /* loaded already: the welcome window came before the main window */
    return;
  provider = gtk_css_provider_new();
  menus_init();
  g_object_set(gtk_settings_get_default(), "gtk-theme-name", "Adwaita", NULL);
  gtk_icon_theme_add_resource_path(gtk_icon_theme_get_for_display(gdk_display_get_default()), TCODE_ICONS_RESOURCE);
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
  layout = gtk_css_provider_new();
  g_signal_connect(layout, "parsing-error", G_CALLBACK(on_layout_error), NULL);
  gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(layout),
                                             GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 2);
  user_look = gtk_css_provider_new();
  gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(user_look),
                                             GTK_STYLE_PROVIDER_PRIORITY_USER);
  apply_theme(t);
  load_user_theme();
}

/* The rounded theme's corners for the corner radius setting: the panels get radius, the rest the same
 * share of it as of the default 10 px (tabs 9, buttons 8, title bar buttons 7, ...). */
static char *corners_css(int radius)
{
  /* clang-format off */
  static const struct { const char *selector; int at_ten; } corners[] = {
    { ".tcode-card, popover > contents", 10 },
    { "button, entry, spinbutton, textview, .tcode-scm .message, tooltip, tooltip.background", 8 },
    { "headerbar button, headerbar menubar > item, windowcontrols > button,"
      " .tcode-sidebar stackswitcher button, .tcode-dock-close", 7 },
    { ".tcode-tab button, .tcode-terminal-tab button, popover modelbutton, dropdown > popover listview > row", 6 },
  };
  /* clang-format on */
  GString *out = g_string_new(NULL);
  int tab = (9 * radius + 5) / 10;

  for (size_t i = 0; i < G_N_ELEMENTS(corners); i++)
    g_string_append_printf(out, "%s { border-radius: %dpx; }", corners[i].selector,
                           (corners[i].at_ten * radius + 5) / 10);
  g_string_append_printf(out, ".tcode-tab { border-radius: %dpx %dpx 0 0; }", tab, tab);
  /* the title bar's search and its dropdown: one frame */
  int bar = (8 * radius + 5) / 10;
  g_string_append_printf(
      out,
      ".tcode-search-bar { border-radius: %dpx; } .tcode-search-bar.open { border-radius: %dpx %dpx 0 0; }"
      " .tcode-picker-pop > contents { border-radius: 0 0 %dpx %dpx; }",
      bar, bar, bar, bar, bar);
  return g_string_free(out, FALSE);
}

/* GtkPaned's separator is this much wider than its CSS size, the extra under the panel after it (where a
 * drag can start): measured with GTK 4.20, 1 px gives 9, 6 gives 14. */
#define SEPARATOR_EXTRA 8

const char *divider_color_for(App *t, const char *theme)
{
  size_t n = theme ? strlen(theme) : 0;

  for (int i = 0; n && t->settings.divider_colors && t->settings.divider_colors[i]; i++) {
    const char *entry = t->settings.divider_colors[i];
    if (strncmp(entry, theme, n) == 0 && entry[n] == '=' && entry[n + 1])
      return entry + n + 1;
  }
  return NULL;
}

void set_divider_color(App *t, const char *theme, const char *color)
{
  GStrvBuilder *kept = g_strv_builder_new();
  size_t n = strlen(theme);

  for (int i = 0; t->settings.divider_colors && t->settings.divider_colors[i]; i++) {
    const char *entry = t->settings.divider_colors[i];
    if (!(strncmp(entry, theme, n) == 0 && entry[n] == '='))
      g_strv_builder_add(kept, entry);
  }
  if (color && *color)
    g_strv_builder_take(kept, g_strdup_printf("%s=%s", theme, color));
  g_strfreev(t->settings.divider_colors);
  t->settings.divider_colors = g_strv_builder_end(kept);
  g_strv_builder_unref(kept);
}

/* The dividers between the panels (the gaps between the cards, the lines inside them) as the settings say,
 * and the compact sizes. */
static char *layout_css(App *t)
{
  GString *out = g_string_new(NULL);
  int style = t->settings.divider ? t->settings.divider : t->settings.square ? DIVIDER_LINE : DIVIDER_DOTS;
  int size = t->settings.divider_size ? t->settings.divider_size : style == DIVIDER_LINE ? 1 : 6;
  const char *own = divider_color_for(t, palette_id());
  /* each color theme's own (window/palette.c); the square theme's lines stay its plain border */
  const char *color = own                                           ? own
                      : t->settings.square && style == DIVIDER_LINE ? "var(--tc-border)"
                                                                    : "var(--tc-divider)";

  if (t->settings.compact) {
    g_string_append(out, compact_css);
    if (t->settings.square)
      g_string_append(out, compact_square_css);
  }
  g_string_append_printf(out, "paned > separator { min-width: %dpx; min-height: %dpx; color: %s;", size, size, color);
  if (style == DIVIDER_LINE)
    g_string_append_printf(out, " background: %s; }", color);
  else
    g_string_append(out, " background: none; }");
  if (style == DIVIDER_DOTS && size >= 2) {
    /* ⋮ in a gap side by side, ⋯ in one above another: three 2 px squares. GTK draws no icon theme image
     * on a separator, nor a picture narrower than it: the squares are bars across it, and a layer over
     * them in the frame's color leaves 2 px in the middle of the gap. The separator is SEPARATOR_EXTRA
     * wider than the gap, under the next panel: the middle is the gap's, not the separator's. */
    static const char *const stops = "%s 0%%, %s 14.3%%, transparent 14.3%%, transparent 42.9%%, %s 42.9%%,"
                                     " %s 57.1%%, transparent 57.1%%, transparent 85.7%%, %s 85.7%%, %s 100%%";
    char *bars = g_strdup_printf(stops, color, color, color, color, color, color);
    double box = size + SEPARATOR_EXTRA, from = 100.0 * (size / 2 - 1) / box, to = 100.0 * (size / 2 + 1) / box;
    char *mask = g_strdup_printf("var(--tc-frame) 0%%, var(--tc-frame) %.4f%%, transparent %.4f%%, transparent %.4f%%,"
                                 " var(--tc-frame) %.4f%%, var(--tc-frame) 100%%",
                                 from, from, to, to);
    g_string_append_printf(out,
                           "paned > separator { background-repeat: no-repeat; background-position: center; }"
                           "paned.horizontal > separator { background-image: linear-gradient(to right, %s),"
                           "  linear-gradient(to bottom, %s); background-size: 100%% 14px; }"
                           "paned.vertical > separator { background-image: linear-gradient(to bottom, %s),"
                           "  linear-gradient(to right, %s); background-size: 14px 100%%; }",
                           mask, bars, mask, bars);
    g_free(bars);
    g_free(mask);
  }
  if (!t->settings.square && style != DIVIDER_LINE && size != 6) /* the frame's margins as wide as its gaps */
    g_string_append_printf(out, ".tcode-workspace { margin: 0 %dpx; }", size);
  if (t->settings.vertical_bar_width > 0) {
    /* The icons fill it (docks/widgets.c lets them) and grow with it: 16 px in the ~28 px column the icons
     * make by themselves, the same share of a wider one; their buttons as tall as wide-ish. */
    int width = t->settings.vertical_bar_width, icon = CLAMP((width * 4 + 3) / 7, 16, 48);
    g_string_append_printf(out,
                           ".tcode-sidebar.vertical > .tcode-dock-header { min-width: %dpx; }"
                           ".tcode-sidebar.vertical stackswitcher button { -gtk-icon-size: %dpx;"
                           "  min-height: %dpx; }",
                           width, icon, icon + 6);
  }
  if (t->settings.panel_outline && !t->settings.square) /* the square theme has its lines */
  {
    g_string_append_printf(out, ".tcode-card { border: 1px solid %s; }", own ? own : "var(--tc-outline)");
    /* the editor tabs against that edge: the strip's top room goes into the tabs, the first one's corner
     * follows the card's */
    g_string_append_printf(out,
                           ".tcode-tabs { padding-top: 0; padding-left: 0; }"
                           ".tcode-tab { min-height: %dpx; }"
                           ".tcode-tab:first-child { border-top-left-radius: %dpx; }",
                           t->settings.compact ? 27 : 34, MAX(t->settings.corner_radius - 1, 0));
  }
  /* Inside a card (a panel's own splits): always a thin line, in the divider's color when one is set. */
  g_string_append_printf(out,
                         ".tcode-card paned > separator { min-width: 1px; min-height: 1px; background: %s;"
                         "  background-image: none; }",
                         own ? own : "var(--tc-border)");
  return g_string_free(out, FALSE);
}

void apply_theme(App *t)
{
  char *css_text;

  if (!square)
    return;
  css_text = layout_css(t);
  gtk_css_provider_load_from_string(layout, css_text);
  g_free(css_text);
  if (t->settings.square) {
    gtk_css_provider_load_from_string(square, square_css);
    return;
  }
  css_text = corners_css(t->settings.corner_radius);
  gtk_css_provider_load_from_string(square, css_text);
  g_free(css_text);
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
  apply_theme(t); /* the dividers: this theme's color */
  terminal_apply_colors(t);
  files_restyle(t);
  for (int i = 0; i < tab_count(t); i++) {
    Tab *tab = tab_nth(t, i);
    if (tab->kind == TAB_EDITOR) {
      editor_view_apply_settings(tab); /* the new scheme */
      semantic_recolor(tab);
    } else if (tab->kind == TAB_PREVIEW) {
      preview_restyle(tab);
    }
  }
  preview_restyle_texts();
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
