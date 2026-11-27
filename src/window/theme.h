/*
 * t-code's look: the color theme's colors (window/palette.c) on rounded panels ("cards") over GTK's built-in
 * Adwaita theme, or square ones (the settings' theme), and the user's own CSS over it.
 */
#pragma once

#include "app.h"

/* t-code's own icons (assets/icons: the file icons and the interface's symbolic ones), built into the
 * binary by the Makefile; load_theme() adds them to the icon theme. Names: tcode-file-<kind>[-light]
 * (files/icons.h) and tcode-<name>-symbolic. */
#define TCODE_ICONS_RESOURCE "/io/github/suacayipkisi/tcode/icons"

/* Before any window: forces Adwaita (dark), adds t-code's icons and the global CSS, the square theme when the
 * settings ask for it, and the user's theme.css. Only the first call does anything. */
void load_theme(App *t);

/* t->settings.divider: what goes between the panels (0: the theme's). */
enum { DIVIDER_THEME, DIVIDER_DOTS, DIVIDER_GAP, DIVIDER_LINE };

/* The divider color the user picked for a color theme (its id: "dark-classic"), or NULL: the theme's border. */
const char *divider_color_for(App *t, const char *theme);

/* Keeps color (CSS; NULL or "": none) as theme's divider color in t->settings.divider_colors. */
void set_divider_color(App *t, const char *theme, const char *color);

/* Turns the square theme on or off, as t->settings.square says; the compact sizes and the dividers between
 * the panels (their style, size and color) as the settings say. */
void apply_theme(App *t);

/* ~/.config/t-code/theme.css (free it): the user's own CSS, over t-code's. */
char *user_theme_file(void);

/* Reads theme.css again (none: nothing over t-code's own look). */
void load_user_theme(void);

/* Gives widget rounded corners on the window's darker frame; its children are cut to them. */
void make_card(GtkWidget *widget);

/* A text button that gets shorter ("Initialize Re…") in a narrow panel instead of keeping it wide.
 * gtk_button_set_label undoes it: call it again after. */
static inline void label_shrinks(GtkWidget *button)
{
  GtkWidget *label = gtk_button_get_child(GTK_BUTTON(button));

  if (GTK_IS_LABEL(label))
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
}

/* Switches the color theme while t-code runs (window/palette.c): the panels, editors, terminals and
 * previews take its colors. Saves nothing. */
void color_theme_apply(App *t, const char *id);

/* Lets the user pick a color theme in the picker ("Preferences: Color Theme…"); saved in settings.ini. */
void color_theme_pick(App *t);
