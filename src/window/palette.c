/*
 * Color themes (see palette.h). A theme is its base colors; pal_compute() works out the rest by
 * mixing them (a border is the background with a little of the text in it, ...), unless the theme
 * gives a color itself (Dark Classic gives all of its own).
 */
#include "window/palette.h"

#include "application/identity.h"

#include <glib/gstdio.h>
#include <string.h>

typedef struct {
  const char *id, *name;
  gboolean light;
  const char *bg, *frame, *fg, *accent;
  /* comment keyword control type function variable constant string number escape operator */
  const char *code[11];
  const char *ansi[16];
  const char *const *given; /* "name", "#color" pairs, NULL-ended: colors not worked out */
} Theme;

/* Dark Classic's own colors, every one (t-code's colors before themes). */
static const char *const dark_classic_given[] = { "frame",
                                                  "#181818",
                                                  "raised",
                                                  "#252526",
                                                  "input",
                                                  "#313131",
                                                  "border",
                                                  "#2b2b2b",
                                                  "border-strong",
                                                  "#454545",
                                                  "fg-bright",
                                                  "#e7e7e7",
                                                  "fg-strong",
                                                  "#ffffff",
                                                  "fg-dim",
                                                  "#9d9d9d",
                                                  "fg-faint",
                                                  "#8b8b8b",
                                                  "accent-hover",
                                                  "#026ec1",
                                                  "accent-fg",
                                                  "#ffffff",
                                                  "selection",
                                                  "#04395e",
                                                  "inactive-selection",
                                                  "#37373d",
                                                  "hover",
                                                  "#2a2d2e",
                                                  "error",
                                                  "#f14c4c",
                                                  "warning",
                                                  "#cca700",
                                                  "info",
                                                  "#3794ff",
                                                  "warning-bg",
                                                  "#3a2f12",
                                                  "warning-border",
                                                  "#5c4a1a",
                                                  "match",
                                                  "#ea5c00",
                                                  "link",
                                                  "#4daafc",
                                                  "git-modified",
                                                  "#e2c08d",
                                                  "git-added",
                                                  "#81b88b",
                                                  "git-untracked",
                                                  "#73c991",
                                                  "git-deleted",
                                                  "#c74e39",
                                                  "git-conflict",
                                                  "#e4676b",
                                                  "git-ignored",
                                                  "#8c8c8c",
                                                  "editor-selection",
                                                  "#264f78",
                                                  "selection-unfocused",
                                                  "#3a3d41",
                                                  "current-line",
                                                  "#282828",
                                                  "line-number",
                                                  "#6e7681",
                                                  "cursor",
                                                  "#aeafad",
                                                  "bracket",
                                                  "#3a3d41",
                                                  "search",
                                                  "#623315",
                                                  "guide",
                                                  "#404040",
                                                  "guide-active",
                                                  "#707070",
                                                  "pair1",
                                                  "#ffd700",
                                                  "pair2",
                                                  "#da70d6",
                                                  "pair3",
                                                  "#179fff",
                                                  "occurrence",
                                                  "#add6ff",
                                                  "diff-added",
                                                  "#2ea043",
                                                  "diff-modified",
                                                  "#0078d4",
                                                  "diff-deleted",
                                                  "#f85149",
                                                  NULL };

static const char *const handheld_green_given[] = { "accent-fg",        "#9bbc0f",   "selection",
                                                    "#306230",          "fg-strong", "#0f380f",
                                                    "editor-selection", "#8bac0f",   NULL };
static const char *const retro_gray_given[] = { "raised",           "#c0c0c0",   "selection",
                                                "#000080",          "accent-fg", "#ffffff",
                                                "editor-selection", "#a6caf0",   "inactive-selection",
                                                "#c0c0c0",          NULL };

static const char *const hot_red_given[] = { "accent-fg", "#000000",          "selection", "#000000",      "hover",
                                             "#cc0000",   "editor-selection", "#000000",   "current-line", "#e00000",
                                             NULL };

static const char *const cooltrix_given[] = {
  "raised",     "#121b15",          "input",          "#142018",      "border",
  "#19291f",    "border-strong",    "#264030",        "fg-dim",       "#7d9e87",
  "fg-faint",   "#4e6b56",          "cursor",         "#00ff66",      "selection",
  "#18432c",    "editor-selection", "#163b27",        "current-line", "#111b15",
  "bracket",    "#1e4a30",          "search",         "#384c20",      "guide",
  "#16281e",    "guide-active",     "#264b33",        "pair1",        "#ffd166",
  "pair2",      "#b87cf8",          "pair3",          "#38bdf8",      "diff-modified",
  "#38bdf8",    "git-modified",     "#ffd166",        "link",         "#38bdf8",
  "warning-bg", "#2e2509",          "warning-border", "#5c4a12",      NULL
};

/* clang-format off */
static const Theme theme_table[] = {
  { "dark-classic", "Dark Classic", FALSE, "#1f1f1f", "#181818", "#cccccc", "#0078d4",
    { "#6a9955", "#569cd6", "#c586c0", "#4ec9b0", "#dcdcaa", "#9cdcfe", "#4fc1ff", "#ce9178", "#b5cea8", "#d7ba7d", "#d4d4d4" },
    { "#000000", "#cd3131", "#0dbc79", "#e5e510", "#2472c8", "#bc3fbc", "#11a8cd", "#e5e5e5",
      "#666666", "#f14c4c", "#23d18b", "#f5f543", "#3b8eea", "#d670d6", "#29b8db", "#e5e5e5" }, dark_classic_given },
  { "light-classic", "Light Classic", TRUE, "#ffffff", "#f8f8f8", "#3b3b3b", "#005fb8",
    { "#008000", "#0000ff", "#af00db", "#267f99", "#795e26", "#001080", "#0070c1", "#a31515", "#098658", "#ee0000", "#000000" },
    { "#000000", "#cd3131", "#107c10", "#949800", "#0451a5", "#bc05bc", "#0598bc", "#555555",
      "#666666", "#cd3131", "#14ce14", "#b5ba00", "#0451a5", "#bc05bc", "#0598bc", "#a5a5a5" }, NULL },
  { "monokai", "Monokai", FALSE, "#272822", "#1e1f1c", "#f8f8f2", "#66d9ef",
    { "#88846f", "#66d9ef", "#f92672", "#66d9ef", "#a6e22e", "#f8f8f2", "#ae81ff", "#e6db74", "#ae81ff", "#ae81ff", "#f92672" },
    { "#333333", "#c4265e", "#86b42b", "#b3b42b", "#6a7ec8", "#8c6bc8", "#56adbc", "#e3e3dd",
      "#666666", "#f92672", "#a6e22e", "#e2e22e", "#819aff", "#ae81ff", "#66d9ef", "#f8f8f2" }, NULL },
  { "dracula", "Dracula", FALSE, "#282a36", "#21222c", "#f8f8f2", "#bd93f9",
    { "#6272a4", "#ff79c6", "#ff79c6", "#8be9fd", "#50fa7b", "#f8f8f2", "#bd93f9", "#f1fa8c", "#bd93f9", "#ff79c6", "#ff79c6" },
    { "#21222c", "#ff5555", "#50fa7b", "#f1fa8c", "#bd93f9", "#ff79c6", "#8be9fd", "#f8f8f2",
      "#6272a4", "#ff6e6e", "#69ff94", "#ffffa5", "#d6acff", "#ff92df", "#a4ffff", "#ffffff" }, NULL },
  { "one-dark", "One Dark", FALSE, "#282c34", "#21252b", "#abb2bf", "#4d78cc",
    { "#7f848e", "#c678dd", "#c678dd", "#e5c07b", "#61afef", "#e06c75", "#d19a66", "#98c379", "#d19a66", "#56b6c2", "#56b6c2" },
    { "#3f4451", "#e05561", "#8cc265", "#d18f52", "#4aa5f0", "#c162de", "#42b3c2", "#d7dae0",
      "#4f5666", "#ff616e", "#a5e075", "#f0a45d", "#4dc4ff", "#de73ff", "#4cd1e0", "#e6e6e6" }, NULL },
  { "github-dark", "GitHub Dark", FALSE, "#0d1117", "#010409", "#e6edf3", "#2f81f7",
    { "#8b949e", "#ff7b72", "#ff7b72", "#ffa657", "#d2a8ff", "#e6edf3", "#79c0ff", "#a5d6ff", "#79c0ff", "#79c0ff", "#ff7b72" },
    { "#484f58", "#ff7b72", "#3fb950", "#d29922", "#58a6ff", "#bc8cff", "#39c5cf", "#b1bac4",
      "#6e7681", "#ffa198", "#56d364", "#e3b341", "#79c0ff", "#d2a8ff", "#56d4dd", "#ffffff" }, NULL },
  { "tokyo-night", "Tokyo Night", FALSE, "#1a1b26", "#16161e", "#a9b1d6", "#7aa2f7",
    { "#565f89", "#bb9af7", "#bb9af7", "#2ac3de", "#7aa2f7", "#c0caf5", "#ff9e64", "#9ece6a", "#ff9e64", "#89ddff", "#89ddff" },
    { "#15161e", "#f7768e", "#9ece6a", "#e0af68", "#7aa2f7", "#bb9af7", "#7dcfff", "#a9b1d6",
      "#414868", "#f7768e", "#9ece6a", "#e0af68", "#7aa2f7", "#bb9af7", "#7dcfff", "#c0caf5" }, NULL },
  { "catppuccin-mocha", "Catppuccin Mocha", FALSE, "#1e1e2e", "#181825", "#cdd6f4", "#cba6f7",
    { "#6c7086", "#cba6f7", "#cba6f7", "#f9e2af", "#89b4fa", "#cdd6f4", "#fab387", "#a6e3a1", "#fab387", "#f5c2e7", "#89dceb" },
    { "#45475a", "#f38ba8", "#a6e3a1", "#f9e2af", "#89b4fa", "#f5c2e7", "#94e2d5", "#bac2de",
      "#585b70", "#f38ba8", "#a6e3a1", "#f9e2af", "#89b4fa", "#f5c2e7", "#94e2d5", "#a6adc8" }, NULL },
  { "nord", "Nord", FALSE, "#2e3440", "#272c36", "#d8dee9", "#88c0d0",
    { "#616e88", "#81a1c1", "#81a1c1", "#8fbcbb", "#88c0d0", "#d8dee9", "#b48ead", "#a3be8c", "#b48ead", "#ebcb8b", "#81a1c1" },
    { "#3b4252", "#bf616a", "#a3be8c", "#ebcb8b", "#81a1c1", "#b48ead", "#88c0d0", "#e5e9f0",
      "#4c566a", "#bf616a", "#a3be8c", "#ebcb8b", "#81a1c1", "#b48ead", "#8fbcbb", "#eceff4" }, NULL },
  { "gruvbox-dark", "Gruvbox Dark", FALSE, "#282828", "#1d2021", "#ebdbb2", "#458588",
    { "#928374", "#fb4934", "#fb4934", "#fabd2f", "#8ec07c", "#83a598", "#d3869b", "#b8bb26", "#d3869b", "#fe8019", "#fe8019" },
    { "#282828", "#cc241d", "#98971a", "#d79921", "#458588", "#b16286", "#689d6a", "#a89984",
      "#928374", "#fb4934", "#b8bb26", "#fabd2f", "#83a598", "#d3869b", "#8ec07c", "#ebdbb2" }, NULL },
  { "solarized-dark", "Solarized Dark", FALSE, "#002b36", "#00212b", "#839496", "#268bd2",
    { "#586e75", "#859900", "#859900", "#b58900", "#268bd2", "#839496", "#cb4b16", "#2aa198", "#d33682", "#dc322f", "#859900" },
    { "#073642", "#dc322f", "#859900", "#b58900", "#268bd2", "#d33682", "#2aa198", "#eee8d5",
      "#002b36", "#cb4b16", "#586e75", "#657b83", "#839496", "#6c71c4", "#93a1a1", "#fdf6e3" }, NULL },
  { "solarized-light", "Solarized Light", TRUE, "#fdf6e3", "#eee8d5", "#657b83", "#268bd2",
    { "#93a1a1", "#859900", "#859900", "#b58900", "#268bd2", "#657b83", "#cb4b16", "#2aa198", "#d33682", "#dc322f", "#859900" },
    { "#073642", "#dc322f", "#859900", "#b58900", "#268bd2", "#d33682", "#2aa198", "#eee8d5",
      "#002b36", "#cb4b16", "#586e75", "#657b83", "#839496", "#6c71c4", "#93a1a1", "#fdf6e3" }, NULL },
  { "ayu-dark", "Ayu Dark", FALSE, "#0b0e14", "#090c10", "#bfbdb6", "#e6b450",
    { "#636a72", "#ff8f40", "#ff8f40", "#59c2ff", "#ffb454", "#bfbdb6", "#d2a6ff", "#aad94c", "#d2a6ff", "#95e6cb", "#f29668" },
    { "#01060e", "#ea6c73", "#7fd962", "#f9af4f", "#53bdfa", "#cda1fa", "#90e1c6", "#c7c7c7",
      "#686868", "#f07178", "#aad94c", "#ffb454", "#59c2ff", "#d2a6ff", "#95e6cb", "#ffffff" }, NULL },
  { "night-owl", "Night Owl", FALSE, "#011627", "#01111d", "#d6deeb", "#7e57c2",
    { "#637777", "#c792ea", "#c792ea", "#ffcb8b", "#82aaff", "#d6deeb", "#f78c6c", "#ecc48d", "#f78c6c", "#7fdbca", "#7fdbca" },
    { "#011627", "#ef5350", "#22da6e", "#c5e478", "#82aaff", "#c792ea", "#21c7a8", "#ffffff",
      "#575656", "#ef5350", "#22da6e", "#ffeb95", "#82aaff", "#c792ea", "#7fdbca", "#ffffff" }, NULL },
  { "rose-pine", "Rosé Pine", FALSE, "#191724", "#12101a", "#e0def4", "#c4a7e7",
    { "#6e6a86", "#31748f", "#31748f", "#9ccfd8", "#ebbcba", "#e0def4", "#f6c177", "#f6c177", "#ebbcba", "#eb6f92", "#908caa" },
    { "#26233a", "#eb6f92", "#31748f", "#f6c177", "#9ccfd8", "#c4a7e7", "#ebbcba", "#e0def4",
      "#6e6a86", "#eb6f92", "#31748f", "#f6c177", "#9ccfd8", "#c4a7e7", "#ebbcba", "#e0def4" }, NULL },
  { "everforest-dark", "Everforest Dark", FALSE, "#2d353b", "#232a2e", "#d3c6aa", "#a7c080",
    { "#859289", "#e67e80", "#e67e80", "#dbbc7f", "#a7c080", "#d3c6aa", "#d699b6", "#83c092", "#d699b6", "#e69875", "#e69875" },
    { "#343f44", "#e67e80", "#a7c080", "#dbbc7f", "#7fbbb3", "#d699b6", "#83c092", "#d3c6aa",
      "#868d80", "#e67e80", "#a7c080", "#dbbc7f", "#7fbbb3", "#d699b6", "#83c092", "#d3c6aa" }, NULL },
  { "kanagawa", "Kanagawa", FALSE, "#1f1f28", "#16161d", "#dcd7ba", "#7e9cd8",
    { "#727169", "#957fb8", "#957fb8", "#7aa89f", "#7e9cd8", "#dcd7ba", "#ffa066", "#98bb6c", "#d27e99", "#e46876", "#c0a36e" },
    { "#16161d", "#c34043", "#76946a", "#c0a36e", "#7e9cd8", "#957fb8", "#6a9589", "#c8c093",
      "#727169", "#e82424", "#98bb6c", "#e6c384", "#7fb4ca", "#938aa9", "#7aa89f", "#dcd7ba" }, NULL },
  { "github-light", "GitHub Light", TRUE, "#ffffff", "#f6f8fa", "#1f2328", "#0969da",
    { "#6e7781", "#cf222e", "#cf222e", "#953800", "#8250df", "#1f2328", "#0550ae", "#0a3069", "#0550ae", "#116329", "#cf222e" },
    { "#24292f", "#cf222e", "#116329", "#4d2d00", "#0969da", "#8250df", "#1b7c83", "#6e7781",
      "#57606a", "#a40e26", "#1a7f37", "#633c01", "#218bff", "#a475f9", "#3192aa", "#8c959f" }, NULL },
  { "catppuccin-latte", "Catppuccin Latte", TRUE, "#eff1f5", "#e6e9ef", "#4c4f69", "#8839ef",
    { "#9ca0b0", "#8839ef", "#8839ef", "#df8e1d", "#1e66f5", "#4c4f69", "#fe640b", "#40a02b", "#fe640b", "#ea76cb", "#04a5e5" },
    { "#5c5f77", "#d20f39", "#40a02b", "#df8e1d", "#1e66f5", "#ea76cb", "#179299", "#acb0be",
      "#6c6f85", "#d20f39", "#40a02b", "#df8e1d", "#1e66f5", "#ea76cb", "#179299", "#bcc0cc" }, NULL },
  /* Not for every day. */
  { "vaporwave", "Vaporwave (ａｅｓｔｈｅｔｉｃ)", FALSE, "#2b1b4a", "#1e1036", "#f8e9ff", "#ff71ce",
    { "#8b6bb8", "#ff71ce", "#01cdfe", "#05ffa1", "#b967ff", "#fffb96", "#01cdfe", "#05ffa1", "#fffb96", "#ff71ce", "#01cdfe" },
    { "#2b1b4a", "#ff71ce", "#05ffa1", "#fffb96", "#01cdfe", "#b967ff", "#01cdfe", "#f8e9ff",
      "#6c4f9c", "#ff71ce", "#05ffa1", "#fffb96", "#01cdfe", "#b967ff", "#01cdfe", "#ffffff" }, NULL },
  { "amber-crt", "Amber CRT (1983)", FALSE, "#1a0f00", "#120a00", "#ffb000", "#ffb000",
    { "#8a5f00", "#ffcc00", "#ffd966", "#ffc24d", "#ffe08a", "#ffb000", "#ffd27f", "#ffc966", "#ffcc00", "#fff0c0", "#ffb000" },
    { "#1a0f00", "#cc7a00", "#ffb000", "#ffcc33", "#996600", "#e69500", "#ffc24d", "#ffd27f",
      "#4d3300", "#ff9900", "#ffb000", "#ffe066", "#b37700", "#ffaa33", "#ffd27f", "#fff0c0" }, NULL },
  { "8-bit-blue", "8-bit Blue", FALSE, "#352879", "#6c5eb5", "#a8a0f0", "#7869c4",
    { "#6c5eb5", "#ffffff", "#bfce72", "#94e089", "#67b6bd", "#a8a0f0", "#b86962", "#94e089", "#ffff9d", "#ffffff", "#ffffff" },
    { "#000000", "#883932", "#55a049", "#bfce72", "#40318d", "#8b3f96", "#67b6bd", "#ffffff",
      "#505050", "#b86962", "#94e089", "#ffff9d", "#7869c4", "#9f4e44", "#adadad", "#ffffff" }, NULL },
  { "handheld-green", "Handheld Green", TRUE, "#9bbc0f", "#8bac0f", "#0f380f", "#306230",
    { "#306230", "#0f380f", "#0f380f", "#306230", "#0f380f", "#0f380f", "#306230", "#306230", "#0f380f", "#0f380f", "#306230" },
    { "#0f380f", "#306230", "#306230", "#306230", "#0f380f", "#306230", "#306230", "#8bac0f",
      "#0f380f", "#306230", "#306230", "#306230", "#0f380f", "#306230", "#306230", "#9bbc0f" }, handheld_green_given },
  { "retro-gray", "Retro Gray", TRUE, "#ffffff", "#c0c0c0", "#000000", "#000080",
    { "#008000", "#000080", "#800080", "#008080", "#800000", "#000000", "#808000", "#800000", "#0000ff", "#ff00ff", "#000000" },
    { "#000000", "#800000", "#008000", "#808000", "#000080", "#800080", "#008080", "#c0c0c0",
      "#808080", "#ff0000", "#00ff00", "#ffff00", "#0000ff", "#ff00ff", "#00ffff", "#ffffff" }, retro_gray_given },
  { "green-rain", "Green Rain", FALSE, "#000000", "#000000", "#00ff41", "#00ff41",
    { "#008f11", "#00ff41", "#39ff14", "#00d832", "#7cff7c", "#00c82c", "#adffad", "#9aff9a", "#00ff41", "#ccffcc", "#00ff41" },
    { "#000000", "#00aa22", "#00ff41", "#66ff66", "#008f11", "#22cc44", "#33ff77", "#00dd33",
      "#003b00", "#00cc33", "#00ff41", "#99ff99", "#00aa22", "#44ff66", "#66ff99", "#ccffcc" }, NULL },
  { "cooltrix", "Cooltrix", FALSE, "#0d1310", "#080c0a", "#e2ebf0", "#00ff66",
    { "#5c8269", "#b87cf8", "#cf8bfb", "#4deeea", "#38bdf8", "#e2ebf0", "#fbc531", "#2ee66b", "#ffd166", "#7bed9f", "#a7c4b0" },
    { "#0d1410", "#f85149", "#2ee66b", "#ffd166", "#38bdf8", "#b87cf8", "#4deeea", "#e2ebf0",
      "#3a5944", "#ff7b72", "#39ff84", "#ffe082", "#79c0ff", "#d2a8ff", "#76ffff", "#ffffff" }, cooltrix_given },
  { "pink", "Pink", TRUE, "#ffe4f1", "#ffc9e3", "#8b0a50", "#e0218a",
    { "#d87093", "#e0218a", "#ff1493", "#c71585", "#da1884", "#8b0a50", "#ff69b4", "#b03060", "#ff6ec7", "#ff00ff", "#e0218a" },
    { "#8b0a50", "#e0218a", "#da1884", "#ff69b4", "#c71585", "#ff1493", "#ff6ec7", "#ffe4f1",
      "#b03060", "#ff1493", "#ff69b4", "#ffb6c1", "#db7093", "#ff00ff", "#ff77ff", "#ffffff" }, NULL },
  { "hot-red", "Hot Red", FALSE, "#ff0000", "#cc0000", "#ffffff", "#ffff00",
    { "#ffff99", "#ffff00", "#ffff00", "#ffffff", "#ffff00", "#ffffff", "#ffff00", "#ffff66", "#ffff00", "#000000", "#ffff00" },
    { "#000000", "#ffff00", "#ffffff", "#ffff00", "#ffffff", "#ffff00", "#ffffff", "#ffffff",
      "#000000", "#ffff00", "#ffffff", "#ffff00", "#ffffff", "#ffff00", "#ffffff", "#ffffff" }, hot_red_given },
};
/* clang-format on */

/* The CSS variable names (--tc-<name>), in PalColor's order; also the names "given" pairs use. */
static const char *const color_names[PAL_COUNT] = {
  "frame",
  "panel",
  "raised",
  "input",
  "border",
  "border-strong",
  "fg",
  "fg-bright",
  "fg-strong",
  "fg-dim",
  "fg-faint",
  "accent",
  "accent-hover",
  "accent-fg",
  "selection",
  "inactive-selection",
  "hover",
  "error",
  "warning",
  "info",
  "warning-bg",
  "warning-border",
  "match",
  "link",
  "git-modified",
  "git-added",
  "git-untracked",
  "git-deleted",
  "git-conflict",
  "git-ignored",
  "editor-selection",
  "selection-unfocused",
  "current-line",
  "line-number",
  "cursor",
  "bracket",
  "search",
  "guide",
  "guide-active",
  "pair1",
  "pair2",
  "pair3",
  "occurrence",
  "diff-added",
  "diff-modified",
  "diff-deleted",
  "comment",
  "keyword",
  "control",
  "type",
  "function",
  "variable",
  "constant",
  "string",
  "number",
  "escape",
  "operator",
};

static const Theme *current_theme = &theme_table[0];
static GdkRGBA pal_colors[PAL_COUNT], pal_terminal[16];
static char pal_hex[PAL_COUNT][8];
static GtkCssProvider *pal_variables;

/* Working the colors out ---------------------------------------------------------------------- */

static GdkRGBA pal_parse(const char *text)
{
  GdkRGBA c = { 0, 0, 0, 1 };

  gdk_rgba_parse(&c, text);
  return c;
}

/* a with amount (0..1) of b in it. */
static GdkRGBA pal_mix(GdkRGBA a, GdkRGBA b, double amount)
{
  return (GdkRGBA){ a.red + (b.red - a.red) * amount, a.green + (b.green - a.green) * amount,
                    a.blue + (b.blue - a.blue) * amount, 1 };
}

static double pal_luminance(GdkRGBA c)
{
  return 0.2126 * c.red + 0.7152 * c.green + 0.0722 * c.blue;
}

static void pal_compute(const Theme *theme)
{
  GdkRGBA bg = pal_parse(theme->bg), fg = pal_parse(theme->fg), accent = pal_parse(theme->accent), term[16];
  GdkRGBA strong = theme->light ? pal_parse("#000000") : pal_parse("#ffffff");
  GdkRGBA *c = pal_colors;

  for (int i = 0; i < 16; i++)
    term[i] = pal_terminal[i] = pal_parse(theme->ansi[i]);
  c[PAL_FRAME] = pal_parse(theme->frame);
  c[PAL_PANEL] = bg;
  c[PAL_RAISED] = pal_mix(bg, fg, 0.06);
  c[PAL_INPUT] = pal_mix(bg, fg, 0.10);
  c[PAL_BORDER] = pal_mix(bg, fg, 0.10);
  c[PAL_BORDER_STRONG] = pal_mix(bg, fg, 0.22);
  c[PAL_FG] = fg;
  c[PAL_FG_BRIGHT] = pal_mix(fg, strong, 0.4);
  c[PAL_FG_STRONG] = strong;
  c[PAL_FG_DIM] = pal_mix(fg, bg, 0.28);
  c[PAL_FG_FAINT] = pal_mix(fg, bg, 0.42);
  c[PAL_ACCENT] = accent;
  c[PAL_ACCENT_HOVER] = pal_mix(accent, bg, 0.15);
  c[PAL_ACCENT_FG] = pal_luminance(accent) > 0.55 ? pal_parse("#000000") : pal_parse("#ffffff");
  c[PAL_SELECTION] = pal_mix(bg, accent, 0.35);
  c[PAL_INACTIVE_SELECTION] = pal_mix(bg, fg, 0.14);
  c[PAL_HOVER] = pal_mix(bg, fg, 0.07);
  c[PAL_ERROR] = term[theme->light ? 1 : 9];
  c[PAL_WARNING] = term[3];
  c[PAL_INFO] = term[theme->light ? 4 : 12];
  c[PAL_WARNING_BG] = pal_mix(bg, term[3], 0.18);
  c[PAL_WARNING_BORDER] = pal_mix(bg, term[3], 0.35);
  c[PAL_MATCH] = term[3];
  c[PAL_LINK] = accent;
  c[PAL_GIT_MODIFIED] = term[3];
  c[PAL_GIT_ADDED] = term[2];
  c[PAL_GIT_UNTRACKED] = term[theme->light ? 2 : 10];
  c[PAL_GIT_DELETED] = term[1];
  c[PAL_GIT_CONFLICT] = term[9];
  c[PAL_GIT_IGNORED] = pal_mix(fg, bg, 0.45);
  c[PAL_EDITOR_SELECTION] = pal_mix(bg, accent, 0.40);
  c[PAL_SELECTION_UNFOCUSED] = pal_mix(bg, fg, 0.15);
  c[PAL_CURRENT_LINE] = pal_mix(bg, fg, 0.04);
  c[PAL_LINE_NUMBER] = pal_mix(fg, bg, 0.45);
  c[PAL_CURSOR] = fg;
  c[PAL_BRACKET] = pal_mix(bg, fg, 0.15);
  c[PAL_SEARCH] = pal_mix(bg, term[3], 0.35);
  c[PAL_GUIDE] = pal_mix(bg, fg, 0.15);
  c[PAL_GUIDE_ACTIVE] = pal_mix(bg, fg, 0.38);
  c[PAL_PAIR1] = term[theme->light ? 3 : 11];
  c[PAL_PAIR2] = term[theme->light ? 5 : 13];
  c[PAL_PAIR3] = term[theme->light ? 4 : 12];
  c[PAL_OCCURRENCE] = accent;
  c[PAL_DIFF_ADDED] = term[2];
  c[PAL_DIFF_MODIFIED] = accent;
  c[PAL_DIFF_DELETED] = term[1];
  for (int i = 0; i < 11; i++)
    c[PAL_COMMENT + i] = pal_parse(theme->code[i]);
  for (int i = 0; theme->given && theme->given[i]; i += 2)
    for (int k = 0; k < PAL_COUNT; k++)
      if (strcmp(color_names[k], theme->given[i]) == 0)
        c[k] = pal_parse(theme->given[i + 1]);
  for (int k = 0; k < PAL_COUNT; k++)
    g_snprintf(pal_hex[k], sizeof pal_hex[k], "#%02x%02x%02x", (int)(c[k].red * 255 + 0.5),
               (int)(c[k].green * 255 + 0.5), (int)(c[k].blue * 255 + 0.5));
}

static const Theme *pal_find(const char *id)
{
  static const char *const renamed[][2] = {
    /* themes' ids before 0.3 */
    { "dark-modern", "dark-classic" },
    { "light-modern", "light-classic" },
    { "commodore-64", "8-bit-blue" },
    { "game-boy", "handheld-green" },
    { "windows-95", "retro-gray" },
    { "matrix", "green-rain" },
    { "barbie", "pink" },
    { "hotdog-stand", "hot-red" },
  };

  for (size_t i = 0; id && i < G_N_ELEMENTS(renamed); i++)
    if (strcmp(id, renamed[i][0]) == 0)
      id = renamed[i][1];
  for (size_t i = 0; id && i < G_N_ELEMENTS(theme_table); i++)
    if (strcmp(theme_table[i].id, id) == 0)
      return &theme_table[i];
  return &theme_table[0];
}

/* The panels' pal_variables and GTK's light or dark ---------------------------------------------- */

static void pal_load_variables(void)
{
  GString *css = g_string_new(":root {");

  for (int k = 0; k < PAL_COUNT; k++)
    g_string_append_printf(css, " --tc-%s: %s;", color_names[k], pal_hex[k]);
  g_string_append(css, " }");
  if (!pal_variables) {
    pal_variables = gtk_css_provider_new();
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(pal_variables),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION - 2);
  }
  gtk_css_provider_load_from_string(pal_variables, css->str);
  g_string_free(css, TRUE);
}

#if GTK_CHECK_VERSION(4, 20, 0)
/* The desktop switched to light (or dark): t-code stays as its theme is. GTK takes the desktop's
 * choice through the settings, so it is put back whenever it changes. */
static void keep_color_scheme(GtkSettings *settings, GParamSpec *spec G_GNUC_UNUSED, gpointer data G_GNUC_UNUSED)
{
  GtkInterfaceColorScheme scheme,
      want = current_theme->light ? GTK_INTERFACE_COLOR_SCHEME_LIGHT : GTK_INTERFACE_COLOR_SCHEME_DARK;

  g_object_get(settings, "gtk-interface-color-scheme", &scheme, NULL);
  if (scheme != want)
    g_object_set(settings, "gtk-interface-color-scheme", want, NULL);
}
#endif

/* GTK's own widgets (buttons, fields, menus) in the theme's light or dark, whatever the desktop says. */
static void set_color_scheme(void)
{
  GtkSettings *settings = gtk_settings_get_default();
  static gboolean watching;

  g_object_set(settings, "gtk-application-prefer-dark-theme", !current_theme->light, NULL);
#if GTK_CHECK_VERSION(4, 20, 0) /* the desktop's light or dark comes through this one since 4.20 */
  keep_color_scheme(settings, NULL, NULL);
  if (!watching)
    g_signal_connect(settings, "notify::gtk-interface-color-scheme", G_CALLBACK(keep_color_scheme), NULL);
#endif
  watching = TRUE;
}

void palette_add_css(const char *css)
{
  static GHashTable *added; /* the strings given so far */
  GtkCssProvider *provider;

  if (!added)
    added = g_hash_table_new(NULL, NULL);
  if (!g_hash_table_add(added, (gpointer)css))
    return;
  provider = gtk_css_provider_new();
  gtk_css_provider_load_from_string(provider, css);
  gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(provider),
                                             GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref(provider);
}

void palette_init(App *t G_GNUC_UNUSED, const char *id)
{
  current_theme = pal_find(id);
  pal_compute(current_theme);
  pal_load_variables();
  set_color_scheme();
}

/* The editor's style scheme ------------------------------------------------------------------ */

#define DARK_CLASSIC_SCHEME "t-code-dark-classic" /* styles/dark-classic.xml, or the user's copy of it */

/* Where the schemes made for the other themes go. */
static char *pal_schemes_dir(void)
{
  return g_build_filename(g_get_user_cache_dir(), TCODE_NAME, "styles", NULL);
}

/* styles/dark-classic.xml with the theme's colors in its <color> lines: the scheme for the theme. */
static void pal_write_scheme(App *t, const char *path)
{
  static const struct {
    const char *name;
    PalColor color;
  } map[] = {
    { "fg", PAL_FG },
    { "bg", PAL_PANEL },
    { "comment", PAL_COMMENT },
    { "keyword", PAL_KEYWORD },
    { "control", PAL_CONTROL },
    { "type", PAL_TYPE },
    { "function", PAL_FUNCTION },
    { "variable", PAL_VARIABLE },
    { "constant", PAL_CONSTANT },
    { "string", PAL_STRING },
    { "number", PAL_NUMBER },
    { "escape", PAL_ESCAPE },
    { "operator", PAL_OPERATOR },
    { "error", PAL_ERROR },
    { "warning", PAL_WARNING },
    { "info", PAL_INFO },
    { "selection", PAL_EDITOR_SELECTION },
    { "selection-unfocused", PAL_SELECTION_UNFOCUSED },
    { "cursor", PAL_CURSOR },
    { "current-line", PAL_CURRENT_LINE },
    { "line-number", PAL_LINE_NUMBER },
    { "bracket", PAL_BRACKET },
    { "search", PAL_SEARCH },
    { "frame", PAL_FRAME },
    { "border", PAL_BORDER },
    { "added", PAL_DIFF_ADDED },
    { "deleted", PAL_DIFF_DELETED },
    { "accent", PAL_ACCENT },
  };
  char *template = g_build_filename(t->paths.styles ? t->paths.styles : ".", "dark-classic.xml", NULL), *text = NULL;
  GString *out;
  char *dir = g_path_get_dirname(path);

  if (!g_file_get_contents(template, &text, NULL, NULL)) {
    g_free(template);
    g_free(dir);
    return;
  }
  out = g_string_new(text);
  char *new_id = g_strdup_printf("id=\"t-code-%s\"", current_theme->id);
  char *new_name = g_strdup_printf("name=\"%s (t-code)\"", current_theme->name);
  g_string_replace(out, "id=\"" DARK_CLASSIC_SCHEME "\"", new_id, 1);
  g_string_replace(out, "name=\"Dark Classic (t-code)\"", new_name, 1);
  g_free(new_name);
  g_free(new_id);
  for (size_t i = 0; i < G_N_ELEMENTS(map); i++) {
    char *key = g_strdup_printf("<color name=\"%s\" value=\"", map[i].name);
    char *at = strstr(out->str, key);
    if (at) {
      gssize from = at - out->str + strlen(key);
      char *end = strchr(out->str + from, '"');
      g_string_erase(out, from, end - (out->str + from));
      g_string_insert(out, from, palette_hex(map[i].color));
    }
    g_free(key);
  }
  g_mkdir_with_parents(dir, 0700);
  g_file_set_contents(path, out->str, -1, NULL);
  g_string_free(out, TRUE);
  g_free(text);
  g_free(template);
  g_free(dir);
}

GtkSourceStyleScheme *palette_scheme(App *t)
{
  GtkSourceStyleSchemeManager *schemes = gtk_source_style_scheme_manager_get_default();
  static char *made; /* the theme whose scheme is written and found */
  char *id;
  GtkSourceStyleScheme *scheme;

  if (current_theme == &theme_table[0])
    return gtk_source_style_scheme_manager_get_scheme(schemes, DARK_CLASSIC_SCHEME);
  id = g_strconcat("t-code-", current_theme->id, NULL);
  if (g_strcmp0(made, current_theme->id) != 0) {
    char *dir = pal_schemes_dir(), *file = g_strconcat(id, ".xml", NULL), *path = g_build_filename(dir, file, NULL);
    static gboolean searched;
    pal_write_scheme(t, path);
    if (!searched)
      gtk_source_style_scheme_manager_append_search_path(schemes, dir);
    searched = TRUE;
    gtk_source_style_scheme_manager_force_rescan(schemes);
    g_set_str(&made, current_theme->id);
    g_free(path);
    g_free(file);
    g_free(dir);
  }
  scheme = gtk_source_style_scheme_manager_get_scheme(schemes, id);
  g_free(id);
  return scheme ? scheme : gtk_source_style_scheme_manager_get_scheme(schemes, DARK_CLASSIC_SCHEME);
}

const GdkRGBA *palette_rgba(PalColor color)
{
  if (pal_colors[PAL_FG].alpha == 0) /* before palette_init (tests): Dark Classic */
    pal_compute(current_theme);
  return &pal_colors[color];
}

const char *palette_hex(PalColor color)
{
  palette_rgba(color);
  return pal_hex[color];
}

const GdkRGBA *palette_terminal(void)
{
  palette_rgba(PAL_FG);
  return pal_terminal;
}

const char *palette_known_id(const char *id)
{
  return pal_find(id)->id;
}

const char *palette_id(void)
{
  return current_theme->id;
}

gboolean palette_light(void)
{
  return current_theme->light;
}

int palette_count(void)
{
  return G_N_ELEMENTS(theme_table);
}

const char *palette_nth_id(int n)
{
  return theme_table[n].id;
}

const char *palette_nth_name(int n)
{
  return theme_table[n].name;
}
