# HDPart GUI overhaul — declutter, group, and add a menu bar

**Date:** 2026-06-13
**Status:** Design approved (visual brainstorm, Direction C)
**Scope:** `src/gui.c` main window only (dialogs unchanged); `Makefile` version bump.

## Problem

The single-window GadTools UI has grown organically and reads as cluttered. The
two most pressing problems (user-confirmed):

1. **Actions are scattered in two places** — Refresh/Resize sit mid-window under
   the list; New/Delete/Edit/Split/Init/Save sit in a separate bottom row. There
   is no single "what can I do here" zone.
2. **No visual grouping** — the disk picker, the partition view, and the actions
   all run together with no separators or section labels.

Secondary issues from the audit also addressed here: ad-hoc button widths/spacing
(#3), no keyboard shortcuts (#4), the cryptic `...` load button (#5), the
unlabeled disk-map bar (#6), and the buried status line (#8). The bold +
double-height **Scan** hack (used to make the primary action obvious) is dropped —
a clean layout makes it obvious without it.

## Chosen direction (C — hybrid)

A screen-top **Amiga menu bar** (revealed by holding the right mouse button,
macOS-style — attached via `SetMenuStrip`, NOT a strip inside the window) carries
housekeeping and a mirror of every action. The **window** keeps the frequent
actions as visible buttons, organized into two BevelBox panels plus a footer.

### Window layout (widened to ~580px content)

```
 ┌ HDPart 0.2 ───────────────────────────────────────── □ ▢ ┐
 │ ┌─ Disk ────────────────────────────────────────────────┐ │
 │ │ Driver: [↕ parallel.device              ]  [ _Load… ]  │ │
 │ │ Unit:   [↕ (press Scan)                 ]  [ _Scan  ]  │ │
 │ └───────────────────────────────────────────────────────┘ │
 │ ┌─ Partitions ──────────────────────────────────────────┐ │
 │ │ [██████████████ disk-map bar █████████████████████████]│ │
 │ │  #  Name          Type       Start    End     Size    │ │
 │ │ ┌───────────────────────────────────────────────────┐ │ │
 │ │ │ (listview)                                        │ │ │
 │ │ └───────────────────────────────────────────────────┘ │ │
 │ │ [ _New ] [ _Edit… ] [ _Delete ] [ Spli_t… ] [ _Resize… ]│ │
 │ └───────────────────────────────────────────────────────┘ │
 │ Status: <message>                       [ Re_fresh ] [ Sa_ve ]│
 └───────────────────────────────────────────────────────────┘
```

- **Disk panel** (BevelBox, captioned "Disk"): Driver cycle + `Load…` button on
  row 1; Unit cycle + `Scan` button on row 2. `Scan` is a normal-weight button.
- **Partitions panel** (BevelBox, captioned "Partitions"): the disk-map bar
  (now clearly *inside* the partition group, so it reads as the layout map),
  the column header, the listview, and a single **partition-action toolbar**
  (New / Edit… / Delete / Split… / Resize…).
- **Footer**: the status line (left) and the two frequent disk-level buttons
  Refresh + Save (right).

The `...` button is renamed **`Load…`**. Button widths within the toolbar are
made consistent (uniform padding; equal where labels allow).

### Menu bar (screen-top, `SetMenuStrip`)

`rA-` = right-Amiga command key. Keys are unique across the strip.

| Project | Disk | Partition |
|---|---|---|
| About HDPart… | Scan `rA-S` | New `rA-N` |
| —— | Load Driver… `rA-L` | Edit… `rA-E` |
| Save `rA-V` | —— | Delete `rA-D` |
| —— | Refresh `rA-F` | —— |
| Quit `rA-Q` | Init Disk… `rA-I` | Split… `rA-T` |
|  |  | Resize… `rA-R` |

Notes:
- `Save = rA-V` because `rA-S` is taken by the app's signature action, Scan. The
  on-screen Save button underlines the **V** (`Sa_ve`) to match. (Easy to swap if
  preferred at review.)
- Menu items are enabled/disabled in lockstep with their button counterparts via
  the existing `gui_update_buttons()` state (extended to also `GTMENUITEM`-ghost
  the menu items).
- Init / Quit / About are **menu-only** (no window button). Save, Refresh, Scan,
  Load, and all partition ops appear in **both** menu and window (hybrid).

### Keyboard shortcuts

- **Menus**: command-key equivalents above (`nm_CommKey`), handled via
  `IDCMP_MENUPICK` → `ItemAddress` → dispatch to the existing handler.
- **Buttons**: the underlined letter (GadTools underscore in the gadget label,
  NewLook) acts as a plain-key shortcut, handled by adding `IDCMP_VANILLAKEY` to
  the main window and mapping the letter to the same handler. The main window has
  no string/integer gadgets, so plain letters are unambiguous there.

## Architecture / components

All changes are local to `src/gui.c`, following the existing patterns (absolute
GadTools positioning, `g_gad[]` table, the `gui_*` handlers).

1. **`build_gadgets()` reflow** — reposition every gadget into the new two-panel
   layout and footer; drop the Scan bold/double-height; rename `...`→`Load…`; add
   underscores to button labels for shortcuts; widen the window.
2. **`gui_draw_chrome()` (new)** — draw the two `DrawBevelBox` group frames and
   their captions, and the disk-map bar, into the window RastPort. Called on open
   and on `IDCMP_REFRESHWINDOW` (the bevel boxes are not gadgets, so they must be
   redrawn on refresh, like the existing bar/easter-egg drawing).
3. **`build_menus()` / menu lifecycle (new)** — a static `struct NewMenu[]`;
   `CreateMenus` + `LayoutMenus(g_vi, GTMN_NewLookMenus, TRUE)` + `SetMenuStrip`
   at window open; `ClearMenuStrip` + `FreeMenus` at close. Store the menu pointer
   in module state.
4. **Event loop** — add `IDCMP_MENUPICK` (walk the `MENUNUM/ITEMNUM` chain via
   `ItemAddress`, since one keystroke can queue multiple picks) and
   `IDCMP_VANILLAKEY`; both dispatch to the **same** `gui_*` handlers the buttons
   already call. No handler logic changes.
5. **`gui_update_buttons()`** — extend to also ghost/enable the corresponding
   menu items so menu and button enable-state never diverge.
6. **Window open** — widen `WA_Width`; add `IDCMP_MENUPICK | IDCMP_VANILLAKEY`;
   bump `WA_Title` to `HDPart 0.2`; recompute the few hardcoded y-offsets for the
   new panel layout and taller window.
7. **`Makefile`** — bump `ADFVER` to match the new `0.2` title (per the existing
   "bump ADFVER to match the gui.c title" convention for `make adf`).

## Data flow / error handling

No data-flow changes. Menus and buttons are two front-ends to the **same**
handlers (`gui_scan_selected`, `gui_load_driver`, `gui_new`, `gui_edit_dialog`,
`gui_delete`, `gui_split`, `gui_resize_dialog`, `gui_init_disk`, `gui_save`,
`gui_refresh_current`, plus a new tiny `gui_about` and the existing quit-confirm
path). Existing dirty-confirm / disabled-state guards are unchanged and now also
gate the menu items.

## Testing

- **Host tests** unchanged and must still pass (`./tests/run-host-tests.sh`) — no
  engine/discover/driver logic is touched.
- **On-target (FS-UAE KS2.04/WB2.04, `HDPart-204-devtest`)**, after `make hd`:
  1. Menu bar appears at the **screen top** on right-mouse-hold; each item fires
     the right action; command keys (`rA-…`) work.
  2. Buttons' underlined plain-key shortcuts work.
  3. The two BevelBox panels and captions draw correctly and **survive a window
     refresh** (drag another window over and back).
  4. Menu items ghost/enable in lockstep with buttons (e.g. Edit/Delete/Resize
     disabled with no selection; Save disabled when not dirty).
  5. Regression: Scan / Load / New / Edit / Delete / Split / Resize / Init /
     Save / Refresh / Quit-confirm all behave exactly as before.
  6. Also verify on the **own-screen** config (`HDPart-204-ownscreen`) that the
     menu attaches to the custom screen's bar.

## Non-goals (explicit — deferred to a later pass)

- **Font/screen-font awareness (audit #7).** The layout stays on hardcoded
  topaz-8 metrics; this redesign reflows *within* that assumption (and adds new
  hardcoded coordinates). Making metrics font-derived is a separate, larger
  effort. *Trade-off accepted:* if font-awareness is done later, these coordinates
  get reworked.
- **Dialog-scaffolding refactor.** Factoring the repeated
  CreateContext/center/open/event-loop/cleanup out of the Edit/Split/Resize/
  request dialogs is out of scope here; those dialogs are untouched.

Both remain captured in the memory `hdpart-gui-overhaul-todo` for a future cycle.
