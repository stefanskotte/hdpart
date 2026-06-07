# HDPart — Plan 3a: GadTools GUI (read & display) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace HDPart's placeholder window with the real Concept-A GadTools GUI in **read-only** form: a filtered device picker (cycle gadget + Rescan), and when a disk is selected, read its RDB and **display** the geometry, a proportional disk-map bar, and the partition table. No writing, editing, Init, or Save yet (those are Plan 3b; their buttons appear but are disabled/ghosted).

**Architecture:** A new `src/gui.c` owns the GadTools window (VisualInfo, gadget context, the cycle/listview/button gadgets, the event loop, and the custom-drawn disk-map bar), talking only to the existing `discover.c` (device list) and `device.c` + `rdb.c` (open a disk, `rdb_parse` its RDB into an `RdbModel`). It opens on the Workbench public screen, falling back to its own screen (the Plan-1 fallback, generalised). Discovery gains a host-tested "partitionable" filter so floppies and directory drives never appear in the picker. The custom startup gains a StackSwap onto a generous stack so the GUI (GadTools + library calls + the ~1.8 KB `RdbModel`) never overflows the 4 KB Shell stack.

**Tech Stack:** C (freestanding m68k + hosted tests for pure helpers), intuition + gadtools + graphics libraries, the existing engine/device/discovery modules, FS-UAE on Kickstart 2.04.

---

## Prerequisites & conventions

Plans 1 and 2 are complete and verified on Kickstart 2.04: `rdb.c` (engine), `device.c` (block I/O + `dev_open`/`dev_geometry`/`dev_block_io`/`dev_inquiry_model`), `discover.c` (`discover_disks` + pure helpers), `support.c` (memcpy/memset/__mulsi3/__udivsi3/__umodsi3), `startup.c` (`_start` in `.text.startup`), `main.c` (own-screen fallback window + `scan` CLI). The `HDPart scan` CLI mode stays.

Amiga build prelude (toolchain not on default PATH):
```bash
export AMIGA_BIN="/Users/sfs/.vscode/extensions/bartmanabyss.amiga-debug-1.8.2/bin/darwin"
export PATH="$AMIGA_BIN/opt/bin:$AMIGA_BIN:$PATH"
```
Host tests: system `cc` (`./tests/run-host-tests.sh`). Build: `make` → `out/HDPart.exe`. Stage for the emulator: `make hd`. Test config: **HDPart-204-devtest** (KS2.04 + WB2.04 + scratch hardfiles). After each build, `make hd` then re-run in FS-UAE.

**Critical runtime facts learned in Plan 2 (do not relearn the hard way):**
- Default Shell stack is ~4 KB. Keep large structs (`RdbModel` ~1.8 KB) off the stack — Plan 3a installs a StackSwap so this is no longer a worry, but still avoid gratuitous big stack locals.
- GadTools needs the screen's VisualInfo; gadgets are built into a context list and passed to `OpenWindowTags(... WA_Gadgets ...)`.
- Always copy `IntuiMessage` fields you need into locals BEFORE `GT_ReplyIMsg`.
- Avoid 64-bit integer math (no `__udivdi3`); 32-bit `/` and `%` are fine (provided by `support.c`).

## File structure (this plan)

```
src/
  gui.h        NEW  gui_run() entry
  gui.c        NEW  GadTools window: device picker, partition listview, disk-map bar (read-only)
  startup.c    MOD  StackSwap onto a 64KB stack before calling hdpart_main
  discover.h   MOD  add `partitionable` field + disc_is_partitionable() pure helper
  discover.c   MOD  set partitionable in probe_one; implement disc_is_partitionable
  main.c       MOD  non-scan launch calls gui_run() instead of the placeholder window
tests/
  test_discover.c  MOD  tests for disc_is_partitionable
```

---

## Milestone 3a.0 — StackSwap hardening + partitionable filter

### Task 3a.0.1: StackSwap onto a generous stack

**Files:** Modify `src/startup.c`

- [ ] **Step 1: Add the StackSwap around the hdpart_main call**

In `src/startup.c`, add includes near the top (after the existing includes):
```c
#include <exec/memory.h>
#include <exec/tasks.h>
```

Replace the single call `rc = hdpart_main(wbmsg);` with a stack-swapped call:
```c
    /* Run on a generous private stack: the GadTools GUI + library calls + the
       ~1.8KB RdbModel would overflow the ~4KB default Shell stack. */
    {
        #define HDPART_STACK_SIZE (64UL * 1024UL)
        APTR newstack = AllocMem(HDPART_STACK_SIZE, MEMF_CLEAR);
        if (newstack) {
            struct StackSwapStruct sss;
            sss.stk_Lower   = newstack;
            sss.stk_Upper   = (ULONG)newstack + HDPART_STACK_SIZE;
            sss.stk_Pointer = (APTR)((ULONG)newstack + HDPART_STACK_SIZE);
            StackSwap(&sss);
            rc = hdpart_main(wbmsg);   /* runs on the new stack */
            StackSwap(&sss);           /* restore the original stack */
            FreeMem(newstack, HDPART_STACK_SIZE);
        } else {
            rc = hdpart_main(wbmsg);   /* fallback: original stack */
        }
    }
```
(The `rc` variable already exists; `wbmsg` is in scope. `struct StackSwapStruct` comes from `<exec/tasks.h>`; `StackSwap`/`AllocMem`/`FreeMem` from `<proto/exec.h>`, already included.)

- [ ] **Step 2: Build for m68k, confirm clean link + entry guard**

Run:
```bash
export AMIGA_BIN="/Users/sfs/.vscode/extensions/bartmanabyss.amiga-debug-1.8.2/bin/darwin"
export PATH="$AMIGA_BIN/opt/bin:$AMIGA_BIN:$PATH"
cd /Users/sfs/Devel/party && make clean && make 2>&1 | grep -iE "entry-order|undefined|error"; xxd out/HDPart.exe | head -1
```
Expected: `entry-order OK`, no undefined symbols (`StackSwap` resolves), hunk `000003f3`.

- [ ] **Step 3: Stage + smoke-test in FS-UAE (human, quick)**

`make hd`, then in the **HDPart-204-devtest** config run `HDPart:HDPart scan` and confirm it still lists the disks and returns cleanly (proves the StackSwap path works end-to-end; the scan exercises the same call chain on the new stack).

- [ ] **Step 4: Commit**

```bash
cd /Users/sfs/Devel/party
git add src/startup.c
git commit -m "feat(startup): StackSwap onto a 64KB stack (GUI/engine need >4KB)"
```

### Task 3a.0.2: Partitionable-device filter (host-tested)

**Files:** Modify `src/discover.h`, `src/discover.c`, `tests/test_discover.c`

- [ ] **Step 1: Write the failing test**

In `tests/test_discover.c`, add this test and call it from `main()` (alongside the others):
```c
static void test_is_partitionable(void)
{
    /* Real disks with media: partitionable. */
    CHECK(disc_is_partitionable("scsi.device",  100) == 1);
    CHECK(disc_is_partitionable("uaehf.device", 8192) == 1);
    CHECK(disc_is_partitionable("lide.device",  1) == 1);
    /* Floppies are never RDB-partition targets. */
    CHECK(disc_is_partitionable("trackdisk.device", 1760) == 0);
    /* Directory drives / no-media report 0 blocks. */
    CHECK(disc_is_partitionable("uae.device", 0) == 0);
    CHECK(disc_is_partitionable("scsi.device", 0) == 0);
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `cd /Users/sfs/Devel/party && ./tests/run-host-tests.sh`
Expected: FAIL — `disc_is_partitionable` undefined.

- [ ] **Step 3: Declare + implement**

In `src/discover.h`, add to the `DiscDisk` struct (after `status`):
```c
    int        partitionable;             /* 1 if this can hold an RDB (for the GUI picker) */
```
and add the pure helper declaration near the other helpers:
```c
/* True if a (driver,total_blocks) pair represents an RDB-partitionable disk:
   has media (total_blocks>0) and is not a floppy (trackdisk.device). */
int disc_is_partitionable(const char *driver, uint32_t total_blocks);
```

In `src/discover.c`, add the pure helper (next to the other pure helpers, OUTSIDE the `HDPART_AMIGA` guard):
```c
int disc_is_partitionable(const char *driver, uint32_t total_blocks)
{
    const char *fd = "trackdisk.device";
    int i;
    if (total_blocks == 0) return 0;        /* no media / directory drive */
    for (i = 0; driver[i] && fd[i] && driver[i] == fd[i]; i++) ;
    if (driver[i] == 0 && fd[i] == 0) return 0;  /* exact "trackdisk.device" */
    return 1;
}
```

In `src/discover.c` `probe_one` (inside the `HDPART_AMIGA` block), set the flag wherever geometry is known. Change the geometry-success branch so that right after `d->size_mb = disc_blocks_to_mb(...);` you add:
```c
        d->partitionable = disc_is_partitionable(d->driver, info.total_blocks);
```
and in the `else` (no media) branch, and at the top of `probe_one` for the `!h` early return, ensure `d->partitionable` is 0 (it is already zero-initialised by `add_unique`, which memsets via the explicit field writes — add `out[idx].partitionable = 0;` to `add_unique` to be explicit).

- [ ] **Step 4: Run host tests to verify pass**

Run: `cd /Users/sfs/Devel/party && ./tests/run-host-tests.sh`
Expected: `ALL TESTS PASSED` and `DISCOVER TESTS PASSED`.

- [ ] **Step 5: Build for m68k (clean link), commit**

```bash
export AMIGA_BIN="/Users/sfs/.vscode/extensions/bartmanabyss.amiga-debug-1.8.2/bin/darwin"
export PATH="$AMIGA_BIN/opt/bin:$AMIGA_BIN:$PATH"
cd /Users/sfs/Devel/party && make 2>&1 | grep -iE "entry-order|undefined|error"
git add src/discover.h src/discover.c tests/test_discover.c
git commit -m "feat(discover): partitionable flag + disc_is_partitionable (filters floppies/dir drives)"
```

---

## Milestone 3a.1 — GadTools window scaffold

Outcome: a GadTools window opens (on Workbench pubscreen, own-screen fallback) titled "HDPart 0.1" containing a device cycle gadget, a Rescan button, a partition listview, and read-only text gadgets for geometry/status, plus ghosted New/Delete/Edit/Init/Save buttons. The event loop handles close and gadget messages and exits cleanly. Gadgets are empty/placeholder at this milestone.

### Task 3a.1.1: GUI module with window + gadgets + event loop

**Files:** Create `src/gui.h`, `src/gui.c`; Modify `src/main.c`

- [ ] **Step 1: Write `src/gui.h`**

```c
#ifndef HDPART_GUI_H
#define HDPART_GUI_H
/* Open the HDPart GUI. Returns a DOS return code. Opens on the Workbench
   public screen, or its own screen if none exists. */
int gui_run(void);
#endif
```

- [ ] **Step 2: Write `src/gui.c` (window + gadgets + loop; data wiring comes next)**

```c
/* HDPart GadTools GUI (read-only display in Plan 3a). */
#include <exec/types.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <libraries/gadtools.h>
#include <graphics/gfxbase.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/gadtools.h>
#include <proto/graphics.h>
#include "gui.h"
#include "discover.h"
#include "device.h"
#include "rdb.h"

extern struct IntuitionBase *IntuitionBase;   /* opened in main.c */
struct Library *GadToolsBase = 0;
struct GfxBase *GfxBase = 0;

/* Gadget IDs */
enum { GID_DEVICE = 1, GID_RESCAN, GID_PARTS, GID_NEW, GID_DELETE, GID_EDIT,
       GID_INIT, GID_SAVE };

/* Module state for one GUI session. */
static struct Screen  *g_scr;        /* screen we render on (pub or own) */
static struct Screen  *g_pub;        /* locked pubscreen, or NULL */
static struct Window  *g_win;
static APTR            g_vi;          /* VisualInfo */
static struct Gadget  *g_glist;      /* gadtools context list */
static struct Gadget  *g_gad[16];    /* gadget pointers by a small index */
static struct TextAttr g_font = { (STRPTR)"topaz.font", 8, 0, 0 };

/* Discovery + current selection (static: keep off the stack). */
static DiscDisk g_disks[DISC_MAX];
static int      g_ndisks;
static const char *g_devlabels[DISC_MAX + 1];  /* for GTCY_Labels */
static char     g_devtext[DISC_MAX][48];
static RdbModel g_model;
static int      g_have_model;

/* Forward decls (implemented in later tasks). */
void gui_rescan(void);
void gui_select_device(int idx);
void gui_draw_bar(void);

static struct Gadget *build_gadgets(void)
{
    struct NewGadget ng;
    struct Gadget *g;
    int i;
    for (i = 0; i < 16; i++) g_gad[i] = 0;

    g = CreateContext(&g_glist);
    if (!g) return 0;

    /* Device cycle gadget */
    ng.ng_TextAttr   = &g_font;
    ng.ng_VisualInfo = g_vi;
    ng.ng_LeftEdge = 70;  ng.ng_TopEdge = 4;  ng.ng_Width = 300; ng.ng_Height = 14;
    ng.ng_GadgetText = (UBYTE *)"Disk:"; ng.ng_GadgetID = GID_DEVICE;
    ng.ng_Flags = 0;
    g_devlabels[0] = "(no disks)"; g_devlabels[1] = 0;
    g = CreateGadget(CYCLE_KIND, g, &ng, GTCY_Labels, (ULONG)g_devlabels, TAG_END);
    g_gad[GID_DEVICE] = g;

    /* Rescan button */
    ng.ng_LeftEdge = 380; ng.ng_TopEdge = 4; ng.ng_Width = 70; ng.ng_Height = 14;
    ng.ng_GadgetText = (UBYTE *)"Rescan"; ng.ng_GadgetID = GID_RESCAN;
    g = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);
    g_gad[GID_RESCAN] = g;

    /* Partition listview */
    ng.ng_LeftEdge = 10; ng.ng_TopEdge = 70; ng.ng_Width = 440; ng.ng_Height = 90;
    ng.ng_GadgetText = 0; ng.ng_GadgetID = GID_PARTS;
    g = CreateGadget(LISTVIEW_KIND, g, &ng, GTLV_Labels, 0, GTLV_ReadOnly, TRUE, TAG_END);
    g_gad[GID_PARTS] = g;

    /* Read-only status text */
    ng.ng_LeftEdge = 70; ng.ng_TopEdge = 166; ng.ng_Width = 380; ng.ng_Height = 12;
    ng.ng_GadgetText = (UBYTE *)"Status:"; ng.ng_GadgetID = 0;
    g = CreateGadget(TEXT_KIND, g, &ng, GTTX_Text, (ULONG)"no disk selected", TAG_END);
    /* not tracked; informational */

    /* Ghosted action buttons (enabled in Plan 3b) */
    {
        static const struct { int id; const char *txt; int x; } btn[] = {
            { GID_NEW, "New", 10 }, { GID_DELETE, "Delete", 70 }, { GID_EDIT, "Edit", 150 },
            { GID_INIT, "Init Disk", 280 }, { GID_SAVE, "Save", 390 }
        };
        int k;
        for (k = 0; k < 5; k++) {
            ng.ng_LeftEdge = btn[k].x; ng.ng_TopEdge = 184;
            ng.ng_Width = (btn[k].id == GID_INIT) ? 90 : 60; ng.ng_Height = 14;
            ng.ng_GadgetText = (UBYTE *)btn[k].txt; ng.ng_GadgetID = btn[k].id;
            g = CreateGadget(BUTTON_KIND, g, &ng, GA_Disabled, TRUE, TAG_END);
            g_gad[btn[k].id] = g;
        }
    }
    return g;
}

int gui_run(void)
{
    BOOL done = FALSE;

    GadToolsBase = OpenLibrary("gadtools.library", 37);
    GfxBase = (struct GfxBase *)OpenLibrary("graphics.library", 37);
    if (!GadToolsBase || !GfxBase) { if (GadToolsBase) CloseLibrary(GadToolsBase);
        if (GfxBase) CloseLibrary((struct Library *)GfxBase); return 20; }

    g_pub = LockPubScreen(0);
    if (g_pub) g_scr = g_pub;
    else {
        g_scr = OpenScreenTags(0, SA_Depth, 2, SA_Title, (ULONG)"HDPart",
                               SA_Type, CUSTOMSCREEN, TAG_END);
    }
    if (!g_scr) goto cleanup_libs;

    g_vi = GetVisualInfo(g_scr, TAG_END);
    if (!g_vi) goto cleanup_scr;

    if (!build_gadgets()) goto cleanup_vi;

    g_win = OpenWindowTags(0,
        WA_Left, 40, WA_Top, 24, WA_Width, 460, WA_Height, 210,
        WA_Title, (ULONG)"HDPart 0.1",
        WA_Gadgets, (ULONG)g_glist,
        WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW | CYCLEIDCMP | BUTTONIDCMP | LISTVIEWIDCMP,
        WA_Flags, WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_CLOSEGADGET |
                  WFLG_ACTIVATE | WFLG_SMART_REFRESH,
        g_pub ? WA_PubScreen : WA_CustomScreen, (ULONG)g_scr,
        TAG_END);
    if (!g_win) goto cleanup_gad;

    GT_RefreshWindow(g_win, 0);
    gui_rescan();          /* populate device list (Task 3a.2.1) */
    gui_draw_bar();        /* initial bar (Task 3a.4.1) */

    while (!done) {
        struct IntuiMessage *imsg;
        WaitPort(g_win->UserPort);
        while ((imsg = GT_GetIMsg(g_win->UserPort)) != 0) {
            ULONG cls = imsg->Class;
            struct Gadget *gad = (struct Gadget *)imsg->IAddress;
            UWORD code = imsg->Code;
            GT_ReplyIMsg(imsg);
            switch (cls) {
                case IDCMP_CLOSEWINDOW:
                    done = TRUE;
                    break;
                case IDCMP_REFRESHWINDOW:
                    GT_BeginRefresh(g_win);
                    gui_draw_bar();
                    GT_EndRefresh(g_win, TRUE);
                    break;
                case IDCMP_GADGETUP:
                    if (gad->GadgetID == GID_DEVICE) gui_select_device((int)code);
                    else if (gad->GadgetID == GID_RESCAN) gui_rescan();
                    break;
            }
        }
    }

    CloseWindow(g_win); g_win = 0;
cleanup_gad:
    FreeGadgets(g_glist); g_glist = 0;
cleanup_vi:
    FreeVisualInfo(g_vi); g_vi = 0;
cleanup_scr:
    if (!g_pub && g_scr) CloseScreen(g_scr);
    if (g_pub) UnlockPubScreen(0, g_pub);
    g_scr = 0; g_pub = 0;
cleanup_libs:
    CloseLibrary((struct Library *)GfxBase);
    CloseLibrary(GadToolsBase);
    return 0;
}

/* Stubs replaced in later tasks (kept so 3a.1 builds/links and runs). */
void gui_rescan(void) { }
void gui_select_device(int idx) { (void)idx; }
void gui_draw_bar(void) { }
```

- [ ] **Step 3: Route non-scan launch to `gui_run` in `src/main.c`**

In `src/main.c`, add `#include "gui.h"` near the other includes. In `hdpart_main`, AFTER the `scan` dispatch block and the `IntuitionBase` open (intuition is opened before the GUI; keep that), REPLACE the placeholder window body (the `LockPubScreen`/`OpenScreenTags`/`OpenWindowTags`/event-loop/cleanup block) with:
```c
    {
        int rc = gui_run();
        CloseLibrary((struct Library *)IntuitionBase);
        return rc;
    }
```
Keep the `IntuitionBase = OpenLibrary("intuition.library", 37);` + null check above it. (gui.c declares `IntuitionBase` as extern and uses it.)

- [ ] **Step 4: Build for m68k**

Run:
```bash
export AMIGA_BIN="/Users/sfs/.vscode/extensions/bartmanabyss.amiga-debug-1.8.2/bin/darwin"
export PATH="$AMIGA_BIN/opt/bin:$AMIGA_BIN:$PATH"
cd /Users/sfs/Devel/party && make clean && make 2>&1 | grep -iE "compil|link|elf2hunk|entry-order|undefined|error"; xxd out/HDPart.exe | head -1
```
Expected: compiles `src/gui.c`, links cleanly (gadtools/graphics inline calls resolve), `entry-order OK`, hunk `000003f3`.

- [ ] **Step 5: Host tests still pass; stage**

Run: `cd /Users/sfs/Devel/party && ./tests/run-host-tests.sh 2>&1 | tail -2 && make hd 2>&1 | tail -1`
Expected: both suites pass; staged.

- [ ] **Step 6: FS-UAE smoke (human)** — run `HDPart:HDPart` (no args) in HDPart-204-devtest. Expect the GadTools window with a "Disk:" cycle gadget, Rescan button, an empty list area, and ghosted New/Delete/Edit/Init/Save. Close gadget exits cleanly. (Gadgets are empty until 3a.2.)

- [ ] **Step 7: Commit**

```bash
cd /Users/sfs/Devel/party
git add src/gui.h src/gui.c src/main.c
git commit -m "feat(gui): GadTools window scaffold (device cycle, listview, ghosted actions)"
```

---

## Milestone 3a.2 — Populate device picker from discovery

### Task 3a.2.1: gui_rescan + device labels

**Files:** Modify `src/gui.c`

- [ ] **Step 1: Implement `gui_rescan` (replace the stub)**

```c
void gui_rescan(void)
{
    int i, n = 0;
    g_ndisks = discover_disks(g_disks, DISC_MAX);

    /* Build cycle labels from partitionable disks only. */
    for (i = 0; i < g_ndisks && n < DISC_MAX; i++) {
        DiscDisk *d = &g_disks[i];
        char *t = g_devtext[n];
        int p = 0, k;
        if (!d->partitionable) continue;
        for (k = 0; d->driver[k] && p < 30; k++) t[p++] = d->driver[k];
        t[p++] = ' '; t[p++] = 'u';
        /* unit as decimal */
        { ULONG u = d->unit; char tmp[8]; int ti = 0;
          if (u == 0) tmp[ti++] = '0';
          while (u) { tmp[ti++] = (char)('0' + (u % 10)); u /= 10; }
          while (ti > 0 && p < 44) t[p++] = tmp[--ti]; }
        t[p++] = ' ';
        /* size MB */
        { ULONG s = d->size_mb; char tmp[8]; int ti = 0;
          if (s == 0) tmp[ti++] = '0';
          while (s) { tmp[ti++] = (char)('0' + (s % 10)); s /= 10; }
          while (ti > 0 && p < 46) t[p++] = tmp[--ti];
          t[p++] = 'M'; }
        t[p] = 0;
        g_devlabels[n] = t;
        /* remember which g_disks index this label maps to */
        g_disks[i].status = g_disks[i].status; /* (index kept via parallel array below) */
        n++;
    }
    if (n == 0) { g_devlabels[0] = "(no disks found)"; g_devlabels[1] = 0; }
    else g_devlabels[n] = 0;

    if (g_win && g_gad[GID_DEVICE])
        GT_SetGadgetAttrs(g_gad[GID_DEVICE], g_win, 0,
                          GTCY_Labels, (ULONG)g_devlabels,
                          GTCY_Active, 0, TAG_END);

    if (n > 0) gui_select_device(0);
}
```

Note: the filtered labels and the underlying `g_disks` indices can diverge (because non-partitionable disks are skipped). To map a cycle position back to a `g_disks` entry, maintain a parallel index array. Add a module-static array near the other state:
```c
static int g_devmap[DISC_MAX];   /* cycle position -> g_disks index */
```
and inside the loop, right after `if (!d->partitionable) continue;`, add `g_devmap[n] = i;` (place it before `n++`). Remove the no-op `g_disks[i].status = g_disks[i].status;` line.

- [ ] **Step 2: Build + host tests + stage**

Run:
```bash
export AMIGA_BIN="/Users/sfs/.vscode/extensions/bartmanabyss.amiga-debug-1.8.2/bin/darwin"
export PATH="$AMIGA_BIN/opt/bin:$AMIGA_BIN:$PATH"
cd /Users/sfs/Devel/party && make 2>&1 | grep -iE "link|entry-order|undefined|error" && ./tests/run-host-tests.sh 2>&1 | tail -1 && make hd 2>&1 | tail -1
```
Expected: clean build; host tests pass; staged.

- [ ] **Step 3: Commit**

```bash
cd /Users/sfs/Devel/party
git add src/gui.c
git commit -m "feat(gui): populate device cycle from partitionable disks; Rescan re-discovers"
```

---

## Milestone 3a.3 — Read & display the selected disk's RDB

### Task 3a.3.1: gui_select_device → open device, rdb_parse, fill listview + status

**Files:** Modify `src/gui.c`

- [ ] **Step 1: Add a small unsigned-to-text helper and the partition label list**

Add near the top of `src/gui.c` (module statics + helper):
```c
/* Exec list of partition rows for the listview. */
#include <exec/lists.h>
#include <exec/nodes.h>
static struct List g_partlist;
static struct Node g_partnodes[RDB_MAX_PARTS];
static char        g_partrows[RDB_MAX_PARTS][64];
static char        g_statusbuf[80];

static int u2s(char *o, ULONG v)   /* write decimal, return length */
{
    char tmp[12]; int ti = 0, n = 0;
    if (v == 0) tmp[ti++] = '0';
    while (v) { tmp[ti++] = (char)('0' + (v % 10)); v /= 10; }
    while (ti > 0) o[n++] = tmp[--ti];
    return n;
}
static void s_cat(char *o, int *p, const char *s) { while (*s) o[(*p)++] = *s++; }
static void s_pad(char *o, int *p, int col) { while (*p < col) o[(*p)++] = ' '; }
```

- [ ] **Step 2: Implement `gui_select_device` (replace the stub)**

```c
void gui_select_device(int idx)
{
    DeviceHandle *h;
    DiscDisk *d;
    int i, di;

    g_have_model = 0;
    NewList(&g_partlist);

    if (g_ndisks == 0 || idx < 0) {
        if (g_win && g_gad[GID_PARTS])
            GT_SetGadgetAttrs(g_gad[GID_PARTS], g_win, 0, GTLV_Labels, (ULONG)&g_partlist, TAG_END);
        gui_draw_bar();
        return;
    }
    di = g_devmap[idx];
    d  = &g_disks[di];

    h = dev_open(d->driver, d->unit);
    if (h) {
        if (rdb_parse(&g_model, dev_block_io, h) == RDB_OK) g_have_model = 1;
        dev_close(h);
    }

    if (g_have_model) {
        for (i = 0; i < g_model.num_parts && i < RDB_MAX_PARTS; i++) {
            RdbPartition *pt = &g_model.parts[i];
            char *row = g_partrows[i];
            int p = 0;
            s_cat(row, &p, pt->name);                 s_pad(row, &p, 8);
            s_cat(row, &p, "FFS");                     s_pad(row, &p, 14);
            p += u2s(row + p, pt->low_cyl);            s_pad(row, &p, 22);
            p += u2s(row + p, pt->high_cyl);           s_pad(row, &p, 30);
            { ULONG cyls = pt->high_cyl - pt->low_cyl + 1;
              ULONG mb = disc_blocks_to_mb(cyls * g_model.cyl_blocks, g_model.block_bytes);
              p += u2s(row + p, mb); s_cat(row, &p, "MB"); }
            row[p] = 0;
            g_partnodes[i].ln_Name = row;
            AddTail(&g_partlist, &g_partnodes[i]);
        }
        { int p = 0; s_cat(g_statusbuf, &p, "RDB OK  ");
          p += u2s(g_statusbuf + p, (ULONG)g_model.num_parts);
          s_cat(g_statusbuf, &p, " partitions  "); 
          p += u2s(g_statusbuf + p, g_model.cylinders); s_cat(g_statusbuf, &p, " cyl");
          g_statusbuf[p] = 0; }
    } else {
        const char *m = "no valid RDB on this disk";
        int p = 0; s_cat(g_statusbuf, &p, m); g_statusbuf[p] = 0;
    }

    if (g_win && g_gad[GID_PARTS])
        GT_SetGadgetAttrs(g_gad[GID_PARTS], g_win, 0, GTLV_Labels, (ULONG)&g_partlist, TAG_END);
    gui_draw_bar();
}
```
(`NewList`/`AddTail` come from `<proto/exec.h>` / exec; `g_partlist`/`g_partnodes` are static so they persist while the listview references them.)

- [ ] **Step 3: Build + host tests + stage**

Run:
```bash
export AMIGA_BIN="/Users/sfs/.vscode/extensions/bartmanabyss.amiga-debug-1.8.2/bin/darwin"
export PATH="$AMIGA_BIN/opt/bin:$AMIGA_BIN:$PATH"
cd /Users/sfs/Devel/party && make 2>&1 | grep -iE "link|entry-order|undefined|error" && ./tests/run-host-tests.sh 2>&1 | tail -1 && make hd 2>&1 | tail -1
```
Expected: clean build; host tests pass; staged.

- [ ] **Step 4: Commit**

```bash
cd /Users/sfs/Devel/party
git add src/gui.c
git commit -m "feat(gui): read selected disk RDB and display partition table + status"
```

---

## Milestone 3a.4 — Disk-map bar (custom render)

### Task 3a.4.1: gui_draw_bar — proportional partition bar

**Files:** Modify `src/gui.c`

- [ ] **Step 1: Implement `gui_draw_bar` (replace the stub)**

```c
void gui_draw_bar(void)
{
    struct RastPort *rp;
    int bx = 10, by = 26, bw = 440, bh = 16;   /* bar rectangle in the window */
    int i;
    if (!g_win) return;
    rp = g_win->RPort;

    /* frame + unused background (pen 2 = black border, pen 0 = bg) */
    SetAPen(rp, 1); RectFill(rp, bx, by, bx + bw - 1, by + bh - 1);    /* fill light */
    SetAPen(rp, 2); Move(rp, bx, by); Draw(rp, bx + bw - 1, by);
    Draw(rp, bx + bw - 1, by + bh - 1); Draw(rp, bx, by + bh - 1); Draw(rp, bx, by);

    if (!g_have_model || g_model.cylinders == 0) return;

    /* each partition drawn proportional to its cylinder span over hi_cyl */
    for (i = 0; i < g_model.num_parts && i < RDB_MAX_PARTS; i++) {
        RdbPartition *pt = &g_model.parts[i];
        int x0 = bx + (int)((pt->low_cyl  * (ULONG)bw) / g_model.cylinders);
        int x1 = bx + (int)(((pt->high_cyl + 1) * (ULONG)bw) / g_model.cylinders);
        if (x1 <= x0) x1 = x0 + 1;
        if (x1 > bx + bw - 1) x1 = bx + bw - 1;
        SetAPen(rp, (UBYTE)(3 - (i & 1)));   /* alternate pens 3/2 */
        RectFill(rp, x0, by + 1, x1 - 1, by + bh - 2);
    }
}
```

- [ ] **Step 2: Build + stage**

Run:
```bash
export AMIGA_BIN="/Users/sfs/.vscode/extensions/bartmanabyss.amiga-debug-1.8.2/bin/darwin"
export PATH="$AMIGA_BIN/opt/bin:$AMIGA_BIN:$PATH"
cd /Users/sfs/Devel/party && make 2>&1 | grep -iE "link|entry-order|undefined|error" && make hd 2>&1 | tail -1
```
Expected: clean build; staged.

- [ ] **Step 3: Commit**

```bash
cd /Users/sfs/Devel/party
git add src/gui.c
git commit -m "feat(gui): proportional disk-map bar (custom RastPort render)"
```

---

## Milestone 3a.5 — FS-UAE verification (human gate)

### Task 3a.5.1: End-to-end read/display test

- [ ] **Step 1: Build, stage, fixtures**

```bash
export AMIGA_BIN="/Users/sfs/.vscode/extensions/bartmanabyss.amiga-debug-1.8.2/bin/darwin"
export PATH="$AMIGA_BIN/opt/bin:$AMIGA_BIN:$PATH"
cd /Users/sfs/Devel/party && make hd && ./tools/make-scratch-hdfs.sh
```

- [ ] **Step 2: Run the GUI (human)**

In FS-UAE **HDPart-204-devtest**, run `HDPart:HDPart` (no args). Verify:
- The window opens with a **Disk:** cycle gadget listing only **partitionable** disks (the `uaehf.device` scratch disks; NOT the floppy/dir-drive).
- Cycling to the **scratch_rdb** disk shows its **partition table** (DH4 / DH5 rows with cylinder ranges + MB) and a **status** line "RDB OK  2 partitions  …".
- The **disk-map bar** shows two proportional segments for that disk; the blank disk shows an empty/framed bar and status "no valid RDB on this disk".
- **Rescan** re-reads. Close gadget exits cleanly with no Guru.

- [ ] **Step 3: Record result**

If the picker is correctly filtered and the RDB disk's partitions + bar display, Plan 3a is verified. Cross-check against `rdbtool amiga_scratch/scratch_rdb.hdf info` on the host.

---

## Definition of done (Plan 3a)

- [ ] StackSwap installed; host + m68k builds clean; `entry-order OK`.
- [ ] `disc_is_partitionable` host-tested; picker lists only real disks.
- [ ] GadTools window opens (pubscreen + own-screen fallback) and closes cleanly.
- [ ] Selecting a disk reads its RDB and displays the partition table, status, and proportional bar.
- [ ] `HDPart scan` CLI still works; host tests pass.

## Self-review notes (author)

- **Spec coverage:** Implements the read half of spec §3 (main window: device picker, disk-map bar, partition table, status) and §4 `gui.c` + `model` display, reusing Plan 1 `rdb_parse` and Plan 2 `device.c`/`discover.c`. Destructive actions (New/Delete/Edit/Init/Save, spec §3.3/§6) are present-but-ghosted and deferred to Plan 3b. Safety (§6) is trivially satisfied in 3a: nothing is ever written.
- **Type consistency:** `gui_run`/`gui_rescan`/`gui_select_device`/`gui_draw_bar` signatures consistent between forward decls, stubs, and implementations. `DiscDisk.partitionable` added in header and set in `probe_one`. `disc_is_partitionable(const char*, uint32_t)` consistent across header/impl/tests. Reuses `dev_open`/`dev_block_io`/`rdb_parse`/`RdbModel`/`disc_blocks_to_mb` with their established signatures.
- **No placeholders:** every code step is complete; the listview is backed by a static Exec list of nodes that persists for the gadget's lifetime; `g_devmap` maps filtered cycle positions to `g_disks` indices.
- **Known risks (verify in emulator, iterate):** GadTools gadget geometry/positioning and the custom bar coordinates are best-effort and will be nudged against the real Topaz-8 rendering; listview label refresh via `GT_SetGadgetAttrs(GTLV_Labels)` must detach/reattach the list (set to NULL is not required when replacing, but the window must own the list memory — it does, via statics).
```
