# Visual design language

The owner judges the IDE visually and iterates quickly. Keep the look **restrained, flat, consistent**: dark "islands" (rounded
panels) floating over a translucent, compositor-blurred frame. The owner dislikes anything that looks generic or "AI-made".

## Structure
Title bar (client-side decorations; the CMake toolbar lives in it) · editor island (tabs, breadcrumbs, text, minimap) · bottom panel
island (Build, Run, Problems, Terminal, Debug) · sidebar island (Explorer / Search / Git / Run and Debug) · right activity bar with three
outlined pills (Explorer-Search-Git, Output-Terminal-Problems, Debug in a circle) · status bar. Gaps between islands are small.

## Tokens (`app/theme.cppm`, `ide::theme`)
frame `#111218` alpha 214 (blurred by the compositor) · editorBg `#0a0b0d` (near-black) · sideBg `#0d0e11` · panelBg `#0b0c0f` ·
panelHead `#07080a` · sideSel `#1a1c21` · sideHover `#121317` · text `#d1d4da` · dim `#8a8e98` · lineNum `#565962` ·
accent `#b01e23` (House Targaryen / Caraxes crimson) · danger `#e5484d`. Syntax colours: warm keywords, teal types, soft greens for
strings/numbers, blue functions.

## Components
- **Pills**: fully rounded, 1 px white ring (alpha ~34) around a near-black fill (`#0a0b0d`/`#0d0e11`); used by the toolbar action
  cluster, the activity bar, dialog actions, buttons. Active icon = accent colour (no tinted circles). Icons are **Material Design Icons**
  (white, outline style), never emoji.
- **Rows** (explorer, search, debugger lists): rounded 6 px hover (`sideHover`), selection `sideSel` or accent alpha 40 with a thin accent bar,
  dim secondary text, count badges as small rounded chips.
- **Inputs**: rounded field, near-black fill, 1 px ring; focused ring = accent.
- **Dialogs**: dim scrim + two islands (narrow left, wide right) built from the same tokens.
- **Menus / tooltips**: glass panels (darker, blurred).

## Rejected by the owner (do not bring back)
Frosted-glass frame effect · translucent explorer/tab outlines · two-tone or red outlines around islands (invisible on white wallpapers)
· dark-blue frame tint · red used where it reads as "warning" (prefer white text) · tinted circle behind the active activity icon ·
filled/solid-glyph icon sets mixed with outline icons · custom folder browser for "Open folder" (use the system chooser).
The frame tint was tuned several times: keep `#111218` @ 214 unless asked.

## Accepted and liked
Outlined white pills with white Material icons; accent-coloured active icons; extra vertical padding inside pills; smaller activity
buttons; consistent island styling everywhere (dialogs included); Debug UI in two places (sidebar + bottom console).
