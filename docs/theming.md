# Theming t-code

#### First, if it is too long to read, just copy this whole file and give it to your AI and tell what you want. End of the day AI exists for something like this, right.

t-code's look comes in three layers. Each one goes over the one before it, so a small change only needs the
layer it belongs to:

1. **Settings** (Ctrl+, → Appearance, or `~/.config/t-code/settings.ini`): the color theme, rounded or square
   panels, corner radius, density, the dividers between the panels, the panel outline and the fonts. Start here.
2. **`theme.css`** (`~/.config/t-code/theme.css`, command palette: *Preferences: Open Theme CSS*): your own GTK CSS
   over everything t-code draws. Saving the file applies it at once. Deleting it takes you back to t-code's own
   look.
3. **The editor's text colors**: a GtkSourceView style scheme (*Preferences: Open Editor Colors*). Syntax colors,
   the selection and the current line live there, not in CSS.

The rest of this page is about `theme.css`: every color and every part you can change, with examples to copy.

## Contents

- [Settings first](#settings-first)
- [Colors: the `--tc-*` variables](#colors-the---tc--variables)
- [The parts of the window](#the-parts-of-the-window)
- [Recipes](#recipes)
- [How it works, and when something does not apply](#how-it-works-and-when-something-does-not-apply)

## Settings first

These already have a setting, so you do not need CSS for them:

| Setting (Appearance) | `settings.ini` key | What it does |
|---|---|---|
| Color theme | `color-theme` | Ayu Dark (a new install's), Dark Classic, Light Classic, Dracula, Nord, ... (also *Preferences: Color Theme*) |
| Theme | `theme` | `rounded`: panels as rounded cards on a darker frame; `square`: flat, lines between them |
| Corner radius | `corner-radius` | The rounded theme's corners in px. Buttons, tabs and menus follow |
| Density | `compact` | `true`: thinner tabs, panel headers, menus and line number column; `false`: roomier |
| Panel dividers | `divider` | `0` the theme's, `1` a gap with ⋮ dots, `2` a gap, `3` a line |
| Divider size (px) | `divider-size` | The gap's or the line's width; `0`: the theme's (a 6 px gap, a 1 px line) |
| Divider color | `divider-colors` | One color per color theme: `dark-classic=#4a4a4a;nord=#5e6a80;`. A theme without one uses its own |
| Panel outline | `panel-outline` | A thin edge around each panel (rounded theme), in the divider color |
| Editor font / Files panel font | `font`, `files-font` | Pango font descriptions: `JetBrains Mono 11` |
| Menu titles (Interface) | `menu-titles` | The menu bar as `0` names, `1` icons, `2` icons and names |
| Vertical left tab bar (Interface) | `left-vertical-tabs` | The left panels' icons in a column along the window's edge |
| Vertical tab bar width (px) | `vertical-tab-bar-width` | That column's width; `0`: as wide as its icons |

## Colors: the `--tc-*` variables

Every color t-code uses comes from the color theme as a CSS variable. Changing a variable changes every place that
uses it, for the panels and t-code's own drawing alike. Put the ones you change in a `:root` block:

```css
:root {
  --tc-accent: #f38ba8;      /* buttons, the current tab's line, focused fields */
  --tc-frame: #11111b;       /* title bar, status bar, the gaps between the panels */
}
```

A `:root` block applies to every color theme. To change only one theme, switch to that theme and use its Settings
where one exists (the divider color is kept per theme). Otherwise change the theme back by hand when you switch.

### The panels

| Variable | Used for |
|---|---|
| `--tc-frame` | The title bar, the status bar, the editor tab strip, the gaps between the panels |
| `--tc-panel` | Panels and editors |
| `--tc-raised` | Popovers, menus, tooltips, the find bar |
| `--tc-input` | Text fields |
| `--tc-border` | Lines between things (dock headers, splits inside a panel) |
| `--tc-border-strong` | Around popups and fields |
| `--tc-divider` | The ⋮ dots or the line between the panels |
| `--tc-outline` | The thin edge around each panel |
| `--tc-fg` | Text |
| `--tc-fg-bright` | Headings |
| `--tc-fg-strong` | The current tab, bold text |
| `--tc-fg-dim` | Notes, inactive tabs, panel icons |
| `--tc-fg-faint` | Folders beside names, placeholders |
| `--tc-accent` | Buttons, the current tab's line, focused fields, checks |
| `--tc-accent-hover` | A button under the pointer |
| `--tc-accent-fg` | Text on the accent |
| `--tc-selection` | The selected row of a focused list |
| `--tc-inactive-selection` | The selected row of a list without focus |
| `--tc-hover` | A row under the pointer |
| `--tc-error`, `--tc-warning`, `--tc-info` | Problems, the Source Control icon's changes color (vertical tab bar) |
| `--tc-warning-bg`, `--tc-warning-border` | The save conflict bar |
| `--tc-match` | Search matches in the Search panel |
| `--tc-link` | Links |

### Git

| Variable | Used for |
|---|---|
| `--tc-git-modified`, `--tc-git-added`, `--tc-git-untracked` | File names and letters in Files and Source Control |
| `--tc-git-deleted`, `--tc-git-conflict`, `--tc-git-ignored` | |

### The editor (what t-code draws itself)

| Variable | Used for |
|---|---|
| `--tc-editor-selection`, `--tc-selection-unfocused` | The selection (also in the style scheme) |
| `--tc-current-line`, `--tc-line-number`, `--tc-cursor` | |
| `--tc-bracket` | The matching bracket |
| `--tc-search` | Find bar matches |
| `--tc-guide`, `--tc-guide-active` | Indent guides; the block around the cursor |
| `--tc-pair1`, `--tc-pair2`, `--tc-pair3` | Bracket pair colors by depth |
| `--tc-occurrence` | Other places of the word under the cursor |
| `--tc-diff-added`, `--tc-diff-modified`, `--tc-diff-deleted` | The diff marks in the gutter |
| `--tc-comment`, `--tc-keyword`, `--tc-control`, `--tc-type`, `--tc-function`, `--tc-variable`, `--tc-constant`, `--tc-string`, `--tc-number`, `--tc-escape`, `--tc-operator` | The code's colors (the editor reads them through its style scheme) |

## The parts of the window

The selectors below are stable: t-code's own CSS uses them too. GTK's own node names (`headerbar`, `popover`,
`paned`, `button`, ...) work as well.

| Part | Selector | Notes |
|---|---|---|
| The whole window | `window.background` | The frame's color |
| The area with all the panels | `.tcode-workspace` | Its margin is the frame on the left and right |
| A panel (a card) | `.tcode-card` | Files, editors, docks: background, `border`, `border-radius` |
| The dividers between panels | `paned > separator` | `paned.horizontal > separator` (side by side), `paned.vertical > separator` (above one another). Prefer the Settings |
| A dock (left, bottom, right) | `.tcode-sidebar` | `.tcode-sidebar.vertical` when its tab bar is a column |
| A dock's tab bar | `.tcode-dock-header` | The row (or column) of panel icons |
| A panel icon | `.tcode-sidebar stackswitcher button` | `:checked` for the shown panel |
| Dock buttons (collapse, close, more) | `.tcode-dock-close` | |
| Source Control's count | `.tcode-scm-badge` | In a column: `button.tcode-scm-changed` (the icon's color instead) |
| Editor tab strip | `.tcode-tabs` | |
| An editor tab | `.tcode-tab` | `:checked` current, `:hover`; `.tcode-tab button` its close button; `label.temporary` a preview tab |
| Terminal tabs (in the dock header) | `.tcode-terminals-strip`, `.tcode-terminal-tab` | `.tcode-terminal-tab:checked`, `.first` the first terminal |
| A terminal | `.tcode-shell` | Padding around the text; the colors come from the color theme |
| The editor | `.tcode-editor` | The minimap: `.tcode-minimap`; the scroll overview: `.tcode-overview` |
| Breadcrumbs | `.tcode-breadcrumbs` | `label.separator` for the `›` |
| Find bar | `.tcode-find` | |
| Hover popup, signature help | `.tcode-hover`, `.tcode-signature` | |
| Title bar | `headerbar` | `headerbar button`, `headerbar menubar > item`, `windowcontrols > button` |
| Title bar search | `.tcode-search-bar` | `.open` while its list shows (`.tcode-picker-pop`) |
| Quick open / command palette | `.tcode-picker`, `.tcode-picker-list row` | |
| Status bar | `.tcode-status` | `.tcode-status button` its items |
| Menus (menu bar, right-click) | `popover.menu` | `popover.menu modelbutton` an item, `> image` its icon, `accelerator` its shortcut, `separator` |
| t-code's own small menus | `popover.tcode-menu` | The panel icon menu, "More Panels" |
| Drop-down lists (Settings) | `dropdown > popover listview > row` | |
| Tooltips | `tooltip` | |
| Files panel | `.tcode-files` | `.tcode-files listview row` a file |
| Open Tabs | `.tcode-opentabs` | `list row`, `.close` |
| Source Control | `.tcode-scm` | `.tcode-scm row`, `.tcode-scm .message` the commit message |
| Search panel | `.tcode-search` | `row`, `row.heading` a file, `.dim` |
| Problems | `.tcode-problems` | `.tcode-problem-row`, `.tcode-problem-error`, `-warning`, `-info` |
| Outline, Hierarchy | `.tcode-outline`, `.tcode-hierarchy` | `row` |
| Testing, Plugins | `.tcode-testing`, `.tcode-plugins-panel` | |
| Run and Debug | `.tcode-debug-view` | `.tcode-debug-bar` the debug buttons |
| Markdown preview | `.tcode-preview` | Its text size is a setting (Ctrl+= in the preview) |
| Settings, Keybinds, Process Monitor | `.tcode-settings`, `.tcode-keybinds`, `.tcode-processes` | Dialogs: `window.tcode-dialog` |

## Recipes

Copy what you need into `theme.css`.

### Colors

```css
/* One accent everywhere */
:root { --tc-accent: #c678dd; --tc-accent-hover: #b55fcf; }

/* A darker frame, panels as they are */
:root { --tc-frame: #0b0b0b; }

/* Clearer panel edges on a black theme */
:root { --tc-outline: #3a3a3a; --tc-divider: #6a6a6a; }

/* Git colors you can tell apart */
:root { --tc-git-modified: #e5c07b; --tc-git-added: #98c379; --tc-git-deleted: #e06c75; }
```

### Panels and dividers

```css
/* No edge around the panels (or: Settings → Panel outline) */
.tcode-card { border: none; }

/* Rounder panels, only the panels */
.tcode-card { border-radius: 16px; }

/* A soft shadow under each panel instead of an edge */
.tcode-card { border: none; box-shadow: 0 1px 4px rgba(0, 0, 0, 0.4); }
```

The dividers' style, size and color are Settings. To color them by hand, change `--tc-divider` rather than
`paned > separator`: the separator is wider than the gap it shows, so a background put on it does not sit where
you expect it to.

### Tabs

```css
/* The current tab: a bottom line instead of the top one */
.tcode-tab:checked { box-shadow: inset 0 -2px var(--tc-accent); }

/* Italic inactive tabs */
.tcode-tab:not(:checked) label { font-style: italic; }

/* Close buttons always visible, not only on hover */
.tcode-tab button { opacity: 1; }

/* Taller tabs whatever the density */
.tcode-tab { min-height: 32px; }
```

### Dock headers and icons

```css
/* Bigger panel icons */
.tcode-sidebar stackswitcher button { -gtk-icon-size: 18px; min-width: 28px; min-height: 28px; }

/* The shown panel's icon in the accent color */
.tcode-sidebar stackswitcher button:checked { color: var(--tc-accent); }

/* No line under the dock headers */
.tcode-dock-header { border-bottom: none; }
```

### Menus

```css
/* Roomier menus even in compact */
popover.menu modelbutton { min-height: 28px; padding: 0 12px; }

/* Menus without icons */
popover.menu modelbutton > image { opacity: 0; min-width: 0; margin-right: 0; }
popover.menu modelbutton.tcode-no-icon > label { margin-left: 0; }

/* Square menus in the rounded theme */
popover > contents, popover modelbutton { border-radius: 0; }
```

### Title bar and status bar

```css
/* A taller title bar */
headerbar { min-height: 36px; }

/* The status bar in the accent color */
.tcode-status { background: var(--tc-accent); color: var(--tc-accent-fg); }
.tcode-status button { color: var(--tc-accent-fg); }
```

### Lists (Files, Source Control, Search)

```css
/* Denser file rows */
.tcode-files listview row { min-height: 20px; }

/* A clearer selected row */
.tcode-files listview row:selected { background: var(--tc-accent); color: var(--tc-accent-fg); }

/* Search results without the indent */
.tcode-search row { padding-left: 12px; }
```

### Fonts outside the editor

```css
/* The whole interface's font (the editor and Files have Settings of their own) */
window { font-family: "Inter"; font-size: 10pt; }

/* Smaller menus */
popover.menu { font-size: 0.9em; }
```

## How it works, and when something does not apply

- **Order.** GTK's Adwaita theme is the base, t-code's CSS goes over it, then the square theme, the compact sizes and
  the dividers, and `theme.css` goes over everything. A rule in `theme.css` wins over t-code's rule for the same
  selector. If it does not, t-code's selector is more specific: copy the selector from the table above, or add the
  widget's node name (`.tcode-tab` → `notebook .tcode-tab`).
- **It is GTK CSS**, not web CSS: no `calc()` in most places, no `display`, `position` or `width` (use `min-width`,
  `min-height`, `padding`, `margin`). Colors, `color-mix(in srgb, …)`, gradients, `border`, `border-radius`,
  `box-shadow`, `opacity`, `font-*` and `-gtk-icon-size` work. The [GTK CSS reference](https://docs.gtk.org/gtk4/css-properties.html)
  lists every property.
- **Finding a widget's selector.** Start t-code with `GTK_DEBUG=interactive t-code`. The GTK Inspector opens beside
  it: pick a widget with the crosshair and its CSS nodes and classes show under *CSS Nodes*. You can also try rules
  live in its *CSS* tab before you put them in `theme.css`.
- **Not CSS:** the editor's syntax colors (the style scheme, *Preferences: Open Editor Colors*) and the terminal's 16
  colors (they come from the color theme).
- **A rule broke the look?** Delete `theme.css`, or comment the rule out (`/* … */`) and save. t-code applies the file
  again as soon as it is saved.
