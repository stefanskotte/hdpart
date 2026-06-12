# HDPart GUI Overhaul Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Declutter the HDPart main window into two grouped BevelBox panels with a single partition-action toolbar and a footer, add a screen-top Amiga menu bar mirroring all actions, add keyboard shortcuts, and refactor the shared modal-dialog scaffolding — without changing any engine behavior.

**Architecture:** All UI work is in `src/gui.c` (GadTools, absolute positioning, the existing `g_gad[]` table and `gui_*` handlers). Menus and buttons are two front-ends to the *same* handlers. A new `gui_draw_chrome()` draws the BevelBox group frames (redrawn on refresh, since they are not gadgets). Four `dlg_*` helpers absorb the duplicated open/center/refresh/close scaffolding from the modal dialogs. Layout stays on hardcoded topaz-8 metrics (font-awareness is an explicit non-goal).

**Tech Stack:** AmigaOS GadTools / Intuition / Graphics (V37+), Bartman `amiga-debug` GCC cross-toolchain, host C test harness for engine logic only.

**Design spec:** `docs/superpowers/specs/2026-06-13-hdpart-gui-overhaul-design.md`

---

## Conventions for every task

**Build/stage commands (run from repo root):**
```bash
export AMIGA_BIN="/Users/sfs/.vscode/extensions/bartmanabyss.amiga-debug-1.8.2/bin/darwin"
export PATH="$AMIGA_BIN/opt/bin:$AMIGA_BIN:$PATH"
make                       # -> out/HDPart.exe ; must end "entry-order OK"
./tests/run-host-tests.sh  # engine tests; must print ALL/DISCOVER/DRIVER TESTS PASSED
make hd                    # stage into amiga_hd/ + amiga_boot/ for FS-UAE
cmp -s out/HDPart.exe amiga_hd/HDPart && echo OK   # confirm staged
```

**Why no unit tests for these tasks:** the GUI is not unit-testable in this project; engine/discover/driver logic (covered by host tests) is **not** touched here. The per-task gate is: compiles clean, host tests still pass, and the on-target checklist passes in FS-UAE (`HDPart-204-devtest`, run `HDPart:HDPart` from a Shell). On-target steps are marked **[ON-TARGET]** and require the user at the emulator.

---

## File Structure

- **Modify only `src/gui.c`** for all UI tasks (Tasks 1–9).
- **Modify `Makefile`** for the `ADFVER` bump (Task 8, one line).
- No new files. `src/gui.c` stays a single file (existing project convention).

---

## Phase 1 — Window reflow + BevelBox chrome (no menus/shortcuts yet)

### Task 1: Move the disk-map bar + column header into the new Partitions-panel position

**Files:**
- Modify: `src/gui.c` — `gui_bar_rect()` (~1524), `gui_draw_partheader()` (find it), `gui_draw_easter()` (find it)

- [ ] **Step 1: Find the header/easter coordinate functions**

Run: `grep -n "gui_draw_partheader\|gui_draw_easter\|s_pad(hdr" src/gui.c`
Read both functions and note the x/y constants they use (they currently align to the old `y=48` bar at `y=58` header etc.).

- [ ] **Step 2: Reposition the bar inside the (future) Partitions panel**

In `gui_bar_rect()` replace the body with the new layout coordinates (bar sits inside the Partitions BevelBox, which spans x6..574):

```c
static void gui_bar_rect(int *bx, int *by, int *bw, int *bh)
{
    *bx = 12 + g_leftb; *by = 64 + g_topb; *bw = 556; *bh = 16;
}
```

- [ ] **Step 3: Reposition the column header to sit just under the bar**

In `gui_draw_partheader()` change its baseline Y so the headings print at `y = 84 + g_topb` (just below the bar at 64+16) and its left x to `12 + g_leftb`. (Match the exact draw call style already in that function — only the x/y constants change.)

- [ ] **Step 4: Reposition the easter-egg pi to the new window's bottom-right**

In `gui_draw_easter()` change its x/y so it lands in the bottom-right corner of the widened window (x near `560 + g_leftb`, y near `198 + g_topb`). Exact pixels tuned in Task 3's on-target step.

- [ ] **Step 5: Build (host tests + compile)**

Run the build/stage commands. Expected: compiles clean, `entry-order OK`, host tests pass. (Visual correctness is verified at the end of Phase 1, Task 3.)

- [ ] **Step 6: Commit**

```bash
git add src/gui.c
git commit -m "refactor(gui): move disk-map bar + header to new panel coordinates"
```

### Task 2: Reposition gadgets into the two panels + footer; drop bold Scan; rename Load…; add button underscores

**Files:**
- Modify: `src/gui.c` — `build_gadgets()` (218–316)

- [ ] **Step 1: Rewrite the gadget positions in `build_gadgets()`**

Replace the gadget-positioning block (from the Driver cycle through the bottom button array) so positions match the new layout. Keep every `CreateGadget` kind/tag the same; only `ng_LeftEdge/TopEdge/Width/Height`, the Scan font (no bold), the `...`→`_Load…` text, and the button labels' underscores change. Concrete values:

```c
    /* Row 1 — Disk panel: Driver cycle + Load button */
    ng.ng_TextAttr   = &g_font;
    ng.ng_VisualInfo = g_vi;
    ng.ng_LeftEdge = 64 + g_leftb; ng.ng_TopEdge = 8 + g_topb; ng.ng_Width = 420; ng.ng_Height = 14;
    ng.ng_GadgetText = (UBYTE *)"Driver:"; ng.ng_GadgetID = GID_DEVICE; ng.ng_Flags = 0;
    g_drvlabels[0] = "Select or load a driver"; g_drvlabels[1] = 0;
    g = CreateGadget(CYCLE_KIND, g, &ng, GTCY_Labels, (ULONG)g_drvlabels, TAG_END);
    g_gad[GID_DEVICE] = g;

    ng.ng_LeftEdge = 490 + g_leftb; ng.ng_TopEdge = 8 + g_topb; ng.ng_Width = 84; ng.ng_Height = 14;
    ng.ng_GadgetText = (UBYTE *)"_Load..."; ng.ng_GadgetID = GID_DRIVER;
    g = CreateGadget(BUTTON_KIND, g, &ng, GA_Disabled, (ULONG)(AslBase == 0), GT_Underscore, (ULONG)'_', TAG_END);
    g_gad[GID_DRIVER] = g;

    /* Row 2 — Disk panel: Unit cycle + Scan button (normal weight, single height) */
    ng.ng_LeftEdge = 64 + g_leftb; ng.ng_TopEdge = 28 + g_topb; ng.ng_Width = 420; ng.ng_Height = 14;
    ng.ng_GadgetText = (UBYTE *)"Unit:"; ng.ng_GadgetID = GID_UNIT;
    g_unitlabels[0] = "(press Scan)"; g_unitlabels[1] = 0;
    g = CreateGadget(CYCLE_KIND, g, &ng, GTCY_Labels, (ULONG)g_unitlabels, GA_Disabled, TRUE, TAG_END);
    g_gad[GID_UNIT] = g;

    ng.ng_LeftEdge = 490 + g_leftb; ng.ng_TopEdge = 28 + g_topb; ng.ng_Width = 84; ng.ng_Height = 14;
    ng.ng_GadgetText = (UBYTE *)"Scan"; ng.ng_GadgetID = GID_SCAN;   /* no underscore: Scan has no shortcut */
    g = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);
    g_gad[GID_SCAN] = g;

    /* Partition listview (inside Partitions panel, under bar+header) */
    ng.ng_LeftEdge = 12 + g_leftb; ng.ng_TopEdge = 94 + g_topb; ng.ng_Width = 556; ng.ng_Height = 66;
    ng.ng_GadgetText = 0; ng.ng_GadgetID = GID_PARTS;
    g = CreateGadget(LISTVIEW_KIND, g, &ng, GTLV_Labels, 0, GTLV_ShowSelected, 0, TAG_END);
    g_gad[GID_PARTS] = g;

    /* Partition-action toolbar (one row under the list) */
    {
        static const struct { int id; const char *txt; int x; int w; } pbtn[] = {
            { GID_NEW,    "_New",     12,  56 }, { GID_EDIT,   "_Edit...", 72,  72 },
            { GID_DELETE, "_Delete", 148,  72 }, { GID_SPLIT,  "Spli_t...",224, 74 },
            { GID_RESIZE, "_Resize...",302, 88 }
        };
        int k;
        for (k = 0; k < 5; k++) {
            ng.ng_LeftEdge = pbtn[k].x + g_leftb; ng.ng_TopEdge = 162 + g_topb;
            ng.ng_Width = pbtn[k].w; ng.ng_Height = 14;
            ng.ng_GadgetText = (UBYTE *)pbtn[k].txt; ng.ng_GadgetID = pbtn[k].id;
            g = CreateGadget(BUTTON_KIND, g, &ng, GA_Disabled, TRUE, GT_Underscore, (ULONG)'_', TAG_END);
            g_gad[pbtn[k].id] = g;
        }
    }

    /* Footer: status text (left) + Refresh + Save (right) */
    ng.ng_LeftEdge = 12 + g_leftb; ng.ng_TopEdge = 186 + g_topb; ng.ng_Width = 360; ng.ng_Height = 12;
    ng.ng_GadgetText = (UBYTE *)"Status:"; ng.ng_GadgetID = GID_STATUS;
    g = CreateGadget(TEXT_KIND, g, &ng, GTTX_Text, (ULONG)"no disk selected", TAG_END);
    g_gad[GID_STATUS] = g;

    ng.ng_LeftEdge = 408 + g_leftb; ng.ng_TopEdge = 184 + g_topb; ng.ng_Width = 76; ng.ng_Height = 14;
    ng.ng_GadgetText = (UBYTE *)"Re_fresh"; ng.ng_GadgetID = GID_REFRESH;
    g = CreateGadget(BUTTON_KIND, g, &ng, GA_Disabled, TRUE, GT_Underscore, (ULONG)'_', TAG_END);
    g_gad[GID_REFRESH] = g;

    ng.ng_LeftEdge = 490 + g_leftb; ng.ng_TopEdge = 184 + g_topb; ng.ng_Width = 84; ng.ng_Height = 14;
    ng.ng_GadgetText = (UBYTE *)"_Save"; ng.ng_GadgetID = GID_SAVE;
    g = CreateGadget(BUTTON_KIND, g, &ng, GA_Disabled, TRUE, GT_Underscore, (ULONG)'_', TAG_END);
    g_gad[GID_SAVE] = g;
```

Delete the now-removed pieces: the old double-height bold Scan block, the old `g_font_bold` use here, the old Refresh/Resize mid-window buttons, and the old 6-entry bottom button array (New/Delete/Edit/Split/Init/Save). **Note: there is no INIT button in the window anymore** — Init Disk becomes menu-only (Task 4). Remove `g_gad[GID_INIT]` creation here.

- [ ] **Step 2: Stop referencing the removed INIT button in `gui_update_buttons()`**

In `gui_update_buttons()` (151) delete the line:
```c
    GT_SetGadgetAttrs(g_gad[GID_INIT],   g_win, 0, GA_Disabled, (ULONG)!hasGeo,   TAG_END);
```
(Init enable-state moves to the menu in Task 4. `g_gad[GID_INIT]` stays NULL, which `GT_SetGadgetAttrs` must not be called on.)

- [ ] **Step 3: Build**

Run build/stage. Expected: clean compile, `entry-order OK`, host tests pass.

- [ ] **Step 4: Commit**

```bash
git add src/gui.c
git commit -m "refactor(gui): reflow gadgets into Disk/Partitions panels + footer"
```

### Task 3: Add BevelBox chrome + widen the window; verify Phase 1 on-target

**Files:**
- Modify: `src/gui.c` — add `gui_draw_chrome()`, call it; window-open width/height (1213–1223); refresh handler (1246–1250)

- [ ] **Step 1: Add `gui_draw_chrome()` above `gui_draw_bar()`**

```c
/* Draw the two BevelBox group frames + their captions. Not gadgets, so must be
   redrawn on every window refresh (like gui_draw_bar). Coordinates are window-
   relative and match the gadget layout in build_gadgets(). */
static void gui_draw_chrome(void)
{
    struct RastPort *rp;
    if (!g_win) return;
    rp = g_win->RPort;
    /* Disk panel */
    DrawBevelBox(rp, 6 + g_leftb, 2 + g_topb, 568, 44, GT_VisualInfo, (ULONG)g_vi, TAG_END);
    SetAPen(rp, 1); SetBPen(rp, 0);
    Move(rp, 14 + g_leftb, 2 + g_topb + 3); Text(rp, (CONST_STRPTR)" Disk ", 6);
    /* Partitions panel */
    DrawBevelBox(rp, 6 + g_leftb, 52 + g_topb, 568, 128, GT_VisualInfo, (ULONG)g_vi, TAG_END);
    Move(rp, 14 + g_leftb, 52 + g_topb + 3); Text(rp, (CONST_STRPTR)" Partitions ", 12);
}
```

- [ ] **Step 2: Call `gui_draw_chrome()` on window open and on refresh**

After `GT_RefreshWindow(g_win, 0);` (1226) add `gui_draw_chrome();` BEFORE `gui_init_picker();`.
In the `IDCMP_REFRESHWINDOW` case (1246–1250), add `gui_draw_chrome();` between `GT_BeginRefresh(g_win);` and `gui_draw_bar();`:
```c
                case IDCMP_REFRESHWINDOW:
                    GT_BeginRefresh(g_win);
                    gui_draw_chrome();
                    gui_draw_bar();
                    GT_EndRefresh(g_win, TRUE);
                    break;
```

- [ ] **Step 3: Widen the window**

In the main `OpenWindowTags` (1213) change the size lines:
```c
        WA_Width,  g_leftb + 580 + g_scr->WBorRight,
        WA_Height, g_topb + 204 + g_scr->WBorBottom,
```

- [ ] **Step 4: Build + stage**

Run build/stage commands; confirm `cmp` prints OK.

- [ ] **Step 5: [ON-TARGET] Verify Phase 1 layout**

In FS-UAE `HDPart-204-devtest`, run `HDPart:HDPart`. Confirm:
- Two captioned BevelBox panels ("Disk", "Partitions"); disk-map bar + header sit inside Partitions.
- Driver+Load… on row 1, Unit+Scan on row 2 (Scan normal weight, not bold/tall).
- Partition toolbar (New/Edit…/Delete/Split…/Resize…) one row under the list; Refresh+Save in the footer next to Status.
- Scan a real scratch unit: list + map populate; selecting a row enables Edit/Delete/Resize; Save enables when dirty.
- Drag another window across HDPart and back: the BevelBoxes + captions + bar redraw cleanly (no gaps).
- Tune any cramped pixel values (button widths/x, easter-egg corner) and rebuild until it matches the mockup.

- [ ] **Step 6: Commit**

```bash
git add src/gui.c
git commit -m "feat(gui): BevelBox group panels + widen window to 580"
```

---

## Phase 2 — Screen-top menu bar

### Task 4: Add the menu strip, lifecycle, MENUPICK dispatch, and About

**Files:**
- Modify: `src/gui.c` — module state (38–45), add `build_menus()`/`gui_about()`, window-open IDCMP + SetMenuStrip, main loop, cleanup

- [ ] **Step 1: Add menu module state + a NewMenu array**

Near the other module state (after line 43) add:
```c
static struct Menu *g_menu = 0;     /* menu strip attached to g_win */
```
Add `#include <libraries/gadtools.h>` is already present. Add the menu template above `build_gadgets()`:
```c
/* Screen-top menu strip. nm_CommKey letters are the right-Amiga shortcuts.
   Scan has none (S belongs to Save). Items map 1:1 to the existing handlers. */
static struct NewMenu g_newmenu[] = {
    { NM_TITLE, "Project",       0,  0, 0, 0 },
    {  NM_ITEM, "About HDPart...",0, 0, 0, (APTR)0 },
    {  NM_ITEM, NM_BARLABEL,     0,  0, 0, 0 },
    {  NM_ITEM, "Save",         "S", 0, 0, 0 },
    {  NM_ITEM, NM_BARLABEL,     0,  0, 0, 0 },
    {  NM_ITEM, "Quit",         "Q", 0, 0, 0 },
    { NM_TITLE, "Disk",          0,  0, 0, 0 },
    {  NM_ITEM, "Scan",          0,  0, 0, 0 },
    {  NM_ITEM, "Load Driver...","L",0, 0, 0 },
    {  NM_ITEM, NM_BARLABEL,     0,  0, 0, 0 },
    {  NM_ITEM, "Refresh",      "F", 0, 0, 0 },
    {  NM_ITEM, "Init Disk...", "I", 0, 0, 0 },
    { NM_TITLE, "Partition",     0,  0, 0, 0 },
    {  NM_ITEM, "New",          "N", 0, 0, 0 },
    {  NM_ITEM, "Edit...",      "E", 0, 0, 0 },
    {  NM_ITEM, "Delete",       "D", 0, 0, 0 },
    {  NM_ITEM, NM_BARLABEL,     0,  0, 0, 0 },
    {  NM_ITEM, "Split...",     "T", 0, 0, 0 },
    {  NM_ITEM, "Resize...",    "R", 0, 0, 0 },
    { NM_END, 0, 0, 0, 0, 0 }
};
/* FULLMENUNUM indices (menu,item) for enable/disable + dispatch readability. */
enum { MN_PROJECT=0, MN_DISK=1, MN_PART=2 };
enum { IT_ABOUT=0, IT_SAVE=2, IT_QUIT=4 };               /* Project items */
enum { ID_SCAN=0, ID_LOAD=1, ID_REFRESH=3, ID_INIT=4 };  /* Disk items   */
enum { IP_NEW=0, IP_EDIT=1, IP_DELETE=2, IP_SPLIT=4, IP_RESIZE=5 }; /* Partition items */
```

- [ ] **Step 2: Add `gui_about()` near the other handlers (e.g. just before `gui_load_driver`)**

```c
static void gui_about(void)
{
    gui_msg("About HDPart",
            "HDPart 0.2\n"
            "RDB hard-disk partition tool\n"
            "for AmigaOS 2.04+ (KS V37+).");
}
```

- [ ] **Step 3: Add `build_menus()` above the main entry function**

```c
/* Create + layout the menu strip and attach it to g_win. Returns 1 on success. */
static int build_menus(void)
{
    g_menu = CreateMenus(g_newmenu, TAG_END);
    if (!g_menu) return 0;
    if (!LayoutMenus(g_menu, g_vi, GTMN_NewLookMenus, TRUE, TAG_END)) {
        FreeMenus(g_menu); g_menu = 0; return 0;
    }
    SetMenuStrip(g_win, g_menu);
    return 1;
}
```

- [ ] **Step 4: Attach the strip after the window opens; add MENUPICK to IDCMP**

In the main `OpenWindowTags` (1219) add `IDCMP_MENUPICK |` to `WA_IDCMP`. After `if (!g_win) goto cleanup_gad;` and before `GT_RefreshWindow`, add:
```c
    if (!build_menus()) goto cleanup_win;   /* new cleanup label, Step 7 */
```

- [ ] **Step 5: Handle `IDCMP_MENUPICK` in the main loop**

Add a new case in the `switch (cls)` (after the `IDCMP_GADGETUP` case), walking the pick chain:
```c
                case IDCMP_MENUPICK: {
                    UWORD mnum = code;
                    while (mnum != MENUNULL) {
                        struct MenuItem *mi = ItemAddress(g_menu, mnum);
                        UWORD mn = MENUNUM(mnum), it = ITEMNUM(mnum);
                        if (mn == MN_PROJECT) {
                            if (it == IT_ABOUT) gui_about();
                            else if (it == IT_SAVE) gui_save();
                            else if (it == IT_QUIT) {
                                if (!g_dirty || gui_confirm("Quit HDPart",
                                        "Discard unsaved changes and quit?")) done = TRUE;
                            }
                        } else if (mn == MN_DISK) {
                            if (it == ID_SCAN) gui_scan_selected();
                            else if (it == ID_LOAD) gui_load_driver();
                            else if (it == ID_REFRESH) gui_refresh_current();
                            else if (it == ID_INIT) gui_init_disk();
                        } else if (mn == MN_PART) {
                            if (it == IP_NEW) gui_new();
                            else if (it == IP_EDIT) {
                                if (g_sel_part >= 0 && g_sel_part < g_model.num_parts) {
                                    gui_edit_dialog(g_sel_part); gui_refresh_parts(); gui_update_buttons();
                                }
                            }
                            else if (it == IP_DELETE) gui_delete();
                            else if (it == IP_SPLIT) gui_split();
                            else if (it == IP_RESIZE) {
                                if (g_sel_part >= 0 && g_sel_part < g_model.num_parts)
                                    gui_resize_dialog(g_sel_part);
                            }
                        }
                        mnum = mi ? mi->NextSelect : MENUNULL;
                    }
                    break;
                }
```

- [ ] **Step 6: Detach + free the strip on cleanup**

Change the close sequence (1287) and add a `cleanup_win` label:
```c
    ClearMenuStrip(g_win);
    FreeMenus(g_menu); g_menu = 0;
    CloseWindow(g_win); g_win = 0;
    goto cleanup_gad;
cleanup_win:
    FreeMenus(g_menu); g_menu = 0;
    CloseWindow(g_win); g_win = 0;
cleanup_gad:
```
(Ensure the normal path runs `ClearMenuStrip`+`FreeMenus`+`CloseWindow` then falls into `cleanup_gad`; the `cleanup_win` label handles the build_menus-failed path where the strip was never set.)

- [ ] **Step 7: Build + stage**

Run build/stage. Expected: clean compile, host tests pass, `cmp` OK.

- [ ] **Step 8: [ON-TARGET] Verify menus fire**

In FS-UAE: hold the right mouse button → menu bar appears at the **screen top** with Project/Disk/Partition. Each item triggers the correct action (About shows the requester; Save/Scan/Load/Refresh/Init/New/Edit/Delete/Split/Resize behave as the buttons do; Quit asks to discard if dirty). Command keys (right-Amiga + S/Q/L/F/I/N/E/D/T/R) work. Also verify on `HDPart-204-ownscreen` the strip attaches to the custom screen.

- [ ] **Step 9: Commit**

```bash
git add src/gui.c
git commit -m "feat(gui): screen-top menu bar mirroring all actions"
```

### Task 5: Ghost menu items in lockstep with buttons

**Files:**
- Modify: `src/gui.c` — `gui_update_buttons()` (151)

- [ ] **Step 1: Add a menu enable/disable helper above `gui_update_buttons()`**

```c
/* Enable/disable one menu item (menu,item) on the attached strip. */
static void gui_menu_enable(UWORD menu, UWORD item, int on)
{
    UWORD num = FULLMENUNUM(menu, item, NOSUB);
    if (!g_win || !g_menu) return;
    if (on) OnMenu(g_win, num); else OffMenu(g_win, num);
}
```

- [ ] **Step 2: Mirror each button's state onto its menu item**

At the end of `gui_update_buttons()` (before the closing brace) add:
```c
    gui_menu_enable(MN_PROJECT, IT_SAVE,  hasModel && g_dirty);
    gui_menu_enable(MN_DISK,    ID_SCAN,  g_target_driver[0] != 0);
    gui_menu_enable(MN_DISK,    ID_REFRESH, g_cur_unitidx >= 0);
    gui_menu_enable(MN_DISK,    ID_INIT,  hasGeo);
    gui_menu_enable(MN_PART,    IP_NEW,   hasModel);
    gui_menu_enable(MN_PART,    IP_EDIT,  hasSel);
    gui_menu_enable(MN_PART,    IP_DELETE,hasSel);
    gui_menu_enable(MN_PART,    IP_SPLIT, hasGeo);
    gui_menu_enable(MN_PART,    IP_RESIZE,hasSel);
```

- [ ] **Step 3: Build + stage**

Run build/stage. Expected: clean compile, host tests pass.

- [ ] **Step 4: [ON-TARGET] Verify ghosting**

With no disk: Save/Edit/Delete/Resize/Split/New/Init greyed in their menus. Scan a disk, select a row: Edit/Delete/Resize enable; deselect: they grey again; make an edit: Save enables in both the footer button and the Project menu.

- [ ] **Step 5: Commit**

```bash
git add src/gui.c
git commit -m "feat(gui): keep menu-item enable-state in sync with buttons"
```

---

## Phase 3 — Button keyboard shortcuts (plain keys)

### Task 6: Add IDCMP_VANILLAKEY dispatch for the underlined button letters

**Files:**
- Modify: `src/gui.c` — window-open IDCMP (1219), main loop

The button labels already carry `GT_Underscore` underlines from Task 2 (l, e/E, d, t, r, f, v/s for Save, n). GadTools does not auto-activate them; map the plain keys here.

- [ ] **Step 1: Add `IDCMP_VANILLAKEY |` to the main window `WA_IDCMP`**

- [ ] **Step 2: Add a VANILLAKEY case to the main loop switch**

```c
                case IDCMP_VANILLAKEY: {
                    UBYTE ch = (UBYTE)code; if (ch >= 'A' && ch <= 'Z') ch += 32;
                    switch (ch) {
                        case 'l': gui_load_driver(); break;
                        case 'n': gui_new(); break;
                        case 'd': gui_delete(); break;
                        case 't': gui_split(); break;
                        case 'f': gui_refresh_current(); break;
                        case 's': gui_save(); break;
                        case 'e':
                            if (g_sel_part >= 0 && g_sel_part < g_model.num_parts) {
                                gui_edit_dialog(g_sel_part); gui_refresh_parts(); gui_update_buttons();
                            }
                            break;
                        case 'r':
                            if (g_sel_part >= 0 && g_sel_part < g_model.num_parts)
                                gui_resize_dialog(g_sel_part);
                            break;
                    }
                    break;
                }
```
(Scan is deliberately absent — it has no shortcut.)

- [ ] **Step 3: Build + stage**

Run build/stage. Expected: clean compile, host tests pass.

- [ ] **Step 4: [ON-TARGET] Verify button shortcuts**

With the main window active, pressing l/n/d/t/f/s and (with a selection) e/r each fires the matching action; the disabled actions safely no-op when their precondition is unmet (handlers already guard). Confirm the underline shows on each button.

- [ ] **Step 5: Commit**

```bash
git add src/gui.c
git commit -m "feat(gui): plain-key shortcuts for the underlined button letters"
```

---

## Phase 4 — Version bump

### Task 7: Bump the window title and `make adf` version

**Files:**
- Modify: `src/gui.c` — `WA_Title` (1217); `Makefile` — `ADFVER`

- [ ] **Step 1: Set the main window title to `HDPart 0.2`**

In the main `OpenWindowTags`, change `WA_Title, (ULONG)"HDPart 0.1",` to `WA_Title, (ULONG)"HDPart 0.2",`.

- [ ] **Step 2: Bump `ADFVER` in the Makefile**

Run: `grep -n "ADFVER" Makefile` and set it to match `0.2` (the value used in the `make adf` floppy name), per the existing "bump ADFVER to match the gui.c title" convention.

- [ ] **Step 3: Build + stage**

Run build/stage. Expected: clean compile, host tests pass.

- [ ] **Step 4: Commit**

```bash
git add src/gui.c Makefile
git commit -m "chore(gui): bump version to 0.2 (window title + ADFVER)"
```

---

## Phase 5 — Dialog scaffolding refactor (behavior-preserving)

### Task 8: Add the `dlg_*` helpers and convert `gui_request`

**Files:**
- Modify: `src/gui.c` — add `dlg_center`/`dlg_open`/`dlg_refresh`/`dlg_close` above `gui_request`; rewrite `gui_request`'s open/close/refresh to use them

- [ ] **Step 1: Add the four helpers above `gui_request` (337)**

```c
/* Shared modal-dialog scaffolding. Each dialog supplies its own gadgets + body
   logic; these own the centering, the OpenWindowTags flags, the standard refresh
   response, and cleanup. */
static void dlg_center(int w, int h, int *left, int *top)
{
    int L = g_win->LeftEdge + (g_win->Width  - w) / 2;
    int T = g_win->TopEdge  + (g_win->Height - h) / 2;
    *left = L < 0 ? 0 : L;
    *top  = T < 0 ? 0 : T;
}
static struct Window *dlg_open(const char *title, int L, int T, int w, int h,
                               ULONG idcmp, struct Gadget *glist)
{
    return OpenWindowTags(0,
        WA_Left, L, WA_Top, T, WA_Width, w, WA_Height, h,
        WA_Title, (ULONG)title, WA_Gadgets, (ULONG)glist,
        WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW | idcmp,
        WA_Flags, WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_ACTIVATE | WFLG_SMART_REFRESH,
        g_pub ? WA_PubScreen : WA_CustomScreen, (ULONG)g_scr,
        TAG_END);
}
static void dlg_refresh(struct Window *w) { GT_BeginRefresh(w); GT_EndRefresh(w, TRUE); }
static void dlg_close(struct Window *w, struct Gadget *glist)
{
    if (w) CloseWindow(w);
    if (glist) FreeGadgets(glist);
}
```

- [ ] **Step 2: Convert `gui_request` to use them**

Replace its `OpenWindowTags(...)` block (384–390) with:
```c
    dwL = 0; dwT = 0; dlg_center(dwW, dwH, &dwL, &dwT);
    dw = dlg_open(title, dwL, dwT, dwW, dwH, BUTTONIDCMP, glist);
    if (!dw) { FreeGadgets(glist); return 0; }
```
(`gui_request` draws its own body text on refresh, so keep its `IDCMP_REFRESHWINDOW` branch as-is — it already calls `gui_draw_text`; do NOT swap that one for `dlg_refresh`.) Replace the cleanup (408–409):
```c
    dlg_close(dw, glist);
    return result;
```
Remove the now-dead manual `dwL/dwT` assignment lines (362–363) since `dlg_center` computes them.

- [ ] **Step 3: Build + stage**

Run build/stage. Expected: clean compile, host tests pass.

- [ ] **Step 4: [ON-TARGET] Verify requesters**

Trigger a confirm (e.g. Quit while dirty) and a message (About): both open centered over the main window, redraw their text on refresh, and return the right result.

- [ ] **Step 5: Commit**

```bash
git add src/gui.c
git commit -m "refactor(gui): extract dlg_center/open/refresh/close; convert gui_request"
```

### Task 9: Convert the Edit, Resize, and Split dialogs to the helpers

**Files:**
- Modify: `src/gui.c` — `gui_edit_dialog` (~700), `gui_resize_dialog` (~960), `gui_split` (~1130)

For EACH of the three dialogs, apply the same three edits (the dialogs differ only in their gadgets/body, which stay unchanged):

- [ ] **Step 1: Replace each dialog's `OpenWindowTags(...)` with `dlg_open`**

In each dialog, replace its `dw = OpenWindowTags(0, WA_Left, dwL, ... TAG_END);` with the centering + open pair (use that dialog's existing IDCMP qualifiers as the `idcmp` arg). Example for `gui_resize_dialog` (its IDCMP was `BUTTONIDCMP | INTEGERIDCMP | CYCLEIDCMP`):
```c
        dlg_center(dwW, dwH, &dwL, &dwT);
        dw = dlg_open("Resize partition", dwL, dwT, dwW, dwH,
                      BUTTONIDCMP | INTEGERIDCMP | CYCLEIDCMP, dglist);
        if (!dw) { FreeGadgets(dglist); return; }
```
For `gui_edit_dialog` use title `"Edit Partition"` and idcmp `BUTTONIDCMP | STRINGIDCMP | INTEGERIDCMP | CHECKBOXIDCMP | CYCLEIDCMP`. For `gui_split` use title `"Split remaining free space"` and idcmp `BUTTONIDCMP | INTEGERIDCMP`. Delete each dialog's now-redundant manual `dwL/dwT` centering lines.

- [ ] **Step 2: Replace each dialog's `IDCMP_REFRESHWINDOW` branch with `dlg_refresh(dw)`**

Each currently reads `else if (cl == IDCMP_REFRESHWINDOW) { GT_BeginRefresh(dw); GT_EndRefresh(dw, TRUE); }`. Replace the body with `dlg_refresh(dw);` (these dialogs have no custom-drawn body text, unlike `gui_request`).

- [ ] **Step 3: Replace each dialog's `CloseWindow(dw); FreeGadgets(dglist);` cleanup with `dlg_close(dw, dglist);`**

- [ ] **Step 4: Build + stage**

Run build/stage. Expected: clean compile, host tests pass.

- [ ] **Step 5: [ON-TARGET] Verify all three dialogs**

Edit a partition (toggle bootable, change FS/MaxTransfer/Mask, OK + Cancel), Resize a partition (grow/shrink, both anchors), and Split free space. Each opens centered, refreshes on drag-over, and applies/cancels exactly as before. Confirm the partition list + map update correctly.

- [ ] **Step 6: Commit**

```bash
git add src/gui.c
git commit -m "refactor(gui): convert Edit/Resize/Split dialogs to dlg_ helpers"
```

---

## Final verification

- [ ] **Step 1: Full host-test pass** — `./tests/run-host-tests.sh` → ALL/DISCOVER/DRIVER TESTS PASSED.
- [ ] **Step 2: [ON-TARGET] Full regression** — walk the spec's Testing checklist end to end on `HDPart-204-devtest` and once on `HDPart-204-ownscreen` (menu attaches to the custom screen). Confirm the non-disk-device fix still holds (scan serial.device, then Load… — no crash).
- [ ] **Step 3: `make adf`** builds `out/HDPart-0.2.adf` without error.
- [ ] **Step 4:** Update the `hdpart-gui-overhaul-todo` and `hdpart-project` memories: mark the declutter/menu/shortcut/dialog-refactor work DONE; note font-awareness (#7) remains the open item.
```
