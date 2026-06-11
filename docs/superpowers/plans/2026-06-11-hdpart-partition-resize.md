# HDPart Partition Resize Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a Resize feature that grows or shrinks the selected partition into adjacent free space on either edge, via a new engine primitive and a dedicated GadTools dialog.

**Architecture:** A geometry-only engine function `rdb_resize_cyl` (validate + rollback) does the model mutation; two pure model-query helpers `rdb_gap_end_after`/`rdb_gap_start_before` compute the free runs around a partition. A new `gui_resize_dialog` derives a cylinder range from a Size-in-MB field plus an anchor (Move End / Move Start), confirms only when the change is destructive, and applies via the engine. A selection-scoped `Resize…` button launches it.

**Tech Stack:** C99 freestanding (AmigaOS GadTools GUI), host unit tests via system `cc` (`tests/test_rdb.c`), Bartman amiga-debug toolchain for the target build.

**Spec:** `docs/superpowers/specs/2026-06-11-hdpart-partition-resize-design.md`

**Deviation from spec (intentional):** the spec described `gap_end_after`/`gap_start_before` as static helpers in `src/gui.c`. This plan places them in the engine (`src/rdb.c`/`src/rdb.h`) as `rdb_gap_end_after`/`rdb_gap_start_before` so they are host-testable. Same math, same design intent, better testability.

**Build/test commands (reference):**
- Host engine tests: `./tests/run-host-tests.sh` (expects `ALL TESTS PASSED`)
- Target build (toolchain not on default PATH):
  ```
  export AMIGA_BIN="/Users/sfs/.vscode/extensions/bartmanabyss.amiga-debug-1.8.2/bin/darwin"
  export PATH="$AMIGA_BIN/opt/bin:$AMIGA_BIN:$PATH"
  make
  ```
- Stage for FS-UAE after every build: `make hd` (then verify `cmp -s out/HDPart.exe amiga_hd/HDPart`).

**Note on GUI testing:** `src/gui.c` depends on intuition/gadtools and is not host-compilable, so the GUI tasks (3–5) verify by compiling clean with `make` plus a manual FS-UAE checklist (Task 6). Only the engine (Tasks 1–2) is unit-tested. This matches the existing project — `gui.c` has no host tests.

---

## File Structure

- `src/rdb.h` — declare `rdb_resize_cyl`, `rdb_gap_end_after`, `rdb_gap_start_before`.
- `src/rdb.c` — implement the three engine functions.
- `tests/test_rdb.c` — add `test_gap_helpers()` and `test_resize_cyl()`, register both in `main()`.
- `src/gui.c` — add `GID_RESIZE` enum value, forward declaration, `gui_resize_dialog`, the `Resize…` button (creation + enable rule + event dispatch).

---

## Task 1: Engine — `rdb_resize_cyl`

**Files:**
- Modify: `src/rdb.h` (add declaration near the other partition ops, after `rdb_set_partition`)
- Modify: `src/rdb.c` (add implementation; place next to `rdb_set_partition`)
- Test: `tests/test_rdb.c` (add `test_resize_cyl`, register in `main`)

- [ ] **Step 1: Write the failing test**

Add this function to `tests/test_rdb.c` immediately before `int main(`:

```c
static void test_resize_cyl(void)
{
    RdbModel m;
    rdb_init_model(&m, 996, 16, 63);            /* lo_cyl=2, hi_cyl=995 */
    /* DH0 2..51, DH1 100..199, DH2 300..399 — gaps before/after DH1 */
    CHECK(rdb_add_partition_cyl(&m, "DH0",   2,  51, RDB_DOSTYPE_FFS_INTL) == 0);
    CHECK(rdb_add_partition_cyl(&m, "DH1", 100, 199, RDB_DOSTYPE_FFS_INTL) == 1);
    CHECK(rdb_add_partition_cyl(&m, "DH2", 300, 399, RDB_DOSTYPE_FFS_INTL) == 2);

    /* grow End edge into trailing gap: low fixed, high up */
    CHECK(rdb_resize_cyl(&m, 1, 100, 250) == RDB_OK);
    CHECK(m.parts[1].low_cyl == 100 && m.parts[1].high_cyl == 250);

    /* grow Start edge into leading gap: high fixed, low down */
    CHECK(rdb_resize_cyl(&m, 1, 60, 250) == RDB_OK);
    CHECK(m.parts[1].low_cyl == 60 && m.parts[1].high_cyl == 250);

    /* shrink End edge */
    CHECK(rdb_resize_cyl(&m, 1, 60, 120) == RDB_OK);
    CHECK(m.parts[1].low_cyl == 60 && m.parts[1].high_cyl == 120);

    /* shrink Start edge */
    CHECK(rdb_resize_cyl(&m, 1, 110, 120) == RDB_OK);
    CHECK(m.parts[1].low_cyl == 110 && m.parts[1].high_cyl == 120);

    /* overlap rejection -> model unchanged (rollback) */
    CHECK(rdb_resize_cyl(&m, 1, 110, 300) == RDB_ERR_OVERLAP);   /* hits DH2@300 */
    CHECK(m.parts[1].low_cyl == 110 && m.parts[1].high_cyl == 120);

    /* out-of-bounds rejection -> rollback */
    CHECK(rdb_resize_cyl(&m, 2, 300, 996) == RDB_ERR_NO_SPACE);  /* hi_cyl=995 */
    CHECK(m.parts[2].low_cyl == 300 && m.parts[2].high_cyl == 399);

    /* inverted range and bad index */
    CHECK(rdb_resize_cyl(&m, 1, 200, 199) == RDB_ERR_RANGE);
    CHECK(rdb_resize_cyl(&m, 99, 100, 200) == RDB_ERR_RANGE);

    /* 1-cylinder floor round-trips */
    CHECK(rdb_resize_cyl(&m, 1, 150, 150) == RDB_OK);
    CHECK(m.parts[1].low_cyl == 150 && m.parts[1].high_cyl == 150);
}
```

Register it: in `main()` add the call right after the existing `test_add_at_and_set();` line:

```c
    test_add_at_and_set();
    test_resize_cyl();
```

- [ ] **Step 2: Run test to verify it fails**

Run: `./tests/run-host-tests.sh`
Expected: FAIL to **compile** — `error: implicit declaration of function 'rdb_resize_cyl'` (the function does not exist yet).

- [ ] **Step 3: Add the declaration**

In `src/rdb.h`, immediately after the `rdb_set_partition` declaration block (the function whose comment mentions "size_mb keeping its low_cyl fixed"), add:

```c
/* Resize partition `index` to the cylinder range [low, high], keeping every
   other partition fixed. Validates the whole table (bounds / overlap / range /
   duplicate-name) via rdb_validate and rolls back to the prior extent on any
   error. Name, dos_type and flag fields are left untouched. Returns RDB_OK,
   RDB_ERR_RANGE (bad index or low > high), or rdb_validate's error. */
int rdb_resize_cyl(RdbModel *m, int index, uint32_t low, uint32_t high);
```

- [ ] **Step 4: Write the implementation**

In `src/rdb.c`, immediately after the closing brace of `rdb_set_partition`, add:

```c
int rdb_resize_cyl(RdbModel *m, int index, uint32_t low, uint32_t high)
{
    RdbPartition saved, *p;
    int v;
    if (index < 0 || index >= m->num_parts) return RDB_ERR_RANGE;
    if (low > high)                          return RDB_ERR_RANGE;
    p = &m->parts[index];
    saved = *p;
    p->low_cyl  = low;
    p->high_cyl = high;
    v = rdb_validate(m);
    if (v != RDB_OK) { *p = saved; return v; }
    return RDB_OK;
}
```

- [ ] **Step 5: Run test to verify it passes**

Run: `./tests/run-host-tests.sh`
Expected: PASS — output ends with `ALL TESTS PASSED`.

- [ ] **Step 6: Commit**

```bash
git add src/rdb.h src/rdb.c tests/test_rdb.c
git commit -m "feat(rdb): rdb_resize_cyl — resize a partition to a cylinder range with validate+rollback"
```

---

## Task 2: Engine — gap helpers `rdb_gap_end_after` / `rdb_gap_start_before`

**Files:**
- Modify: `src/rdb.h` (declare both, after `rdb_resize_cyl`)
- Modify: `src/rdb.c` (implement both, after `rdb_resize_cyl`)
- Test: `tests/test_rdb.c` (add `test_gap_helpers`, register in `main`)

- [ ] **Step 1: Write the failing test**

Add this function to `tests/test_rdb.c` immediately before `int main(`:

```c
static void test_gap_helpers(void)
{
    RdbModel m;
    rdb_init_model(&m, 996, 16, 63);            /* lo_cyl=2, hi_cyl=995 */
    CHECK(rdb_add_partition_cyl(&m, "DH0",   2,  51, RDB_DOSTYPE_FFS_INTL) == 0);
    CHECK(rdb_add_partition_cyl(&m, "DH1", 100, 199, RDB_DOSTYPE_FFS_INTL) == 1);
    CHECK(rdb_add_partition_cyl(&m, "DH2", 300, 399, RDB_DOSTYPE_FFS_INTL) == 2);

    /* DH1 sits between DH0 (..51) and DH2 (300..) */
    CHECK(rdb_gap_end_after(&m, 1)    == 300);  /* next occupied cyl after high */
    CHECK(rdb_gap_start_before(&m, 1) == 52);   /* first free cyl before low   */

    /* DH0 is first: nothing before -> lo_cyl */
    CHECK(rdb_gap_start_before(&m, 0) == 2);
    /* DH2 is last: nothing after -> hi_cyl+1 */
    CHECK(rdb_gap_end_after(&m, 2)    == 996);
}
```

Register it: in `main()` add the call right after `test_resize_cyl();`:

```c
    test_resize_cyl();
    test_gap_helpers();
```

- [ ] **Step 2: Run test to verify it fails**

Run: `./tests/run-host-tests.sh`
Expected: FAIL to compile — `implicit declaration of function 'rdb_gap_end_after'`.

- [ ] **Step 3: Add the declarations**

In `src/rdb.h`, immediately after the `rdb_resize_cyl` declaration, add:

```c
/* Exclusive end of the free run that follows partition `index`: the low_cyl of
   the nearest partition starting after parts[index].high_cyl, or hi_cyl+1 if
   none follows. Use (return - 1) as the max high_cyl for an End-edge grow. */
uint32_t rdb_gap_end_after(const RdbModel *m, int index);
/* Inclusive start of the free run that precedes partition `index`: the
   high_cyl+1 of the nearest partition ending before parts[index].low_cyl, or
   lo_cyl if none precedes. Use as the min low_cyl for a Start-edge grow. */
uint32_t rdb_gap_start_before(const RdbModel *m, int index);
```

- [ ] **Step 4: Write the implementation**

In `src/rdb.c`, immediately after `rdb_resize_cyl`, add:

```c
uint32_t rdb_gap_end_after(const RdbModel *m, int index)
{
    uint32_t end = m->hi_cyl + 1;            /* exclusive */
    uint32_t hi;
    int j;
    if (index < 0 || index >= m->num_parts) return end;
    hi = m->parts[index].high_cyl;
    for (j = 0; j < m->num_parts; j++) {
        if (j == index) continue;
        if (m->parts[j].low_cyl > hi && m->parts[j].low_cyl < end)
            end = m->parts[j].low_cyl;
    }
    return end;
}

uint32_t rdb_gap_start_before(const RdbModel *m, int index)
{
    uint32_t start = m->lo_cyl;              /* inclusive */
    uint32_t lo;
    int j;
    if (index < 0 || index >= m->num_parts) return start;
    lo = m->parts[index].low_cyl;
    for (j = 0; j < m->num_parts; j++) {
        if (j == index) continue;
        if (m->parts[j].high_cyl < lo && m->parts[j].high_cyl + 1 > start)
            start = m->parts[j].high_cyl + 1;
    }
    return start;
}
```

- [ ] **Step 5: Run test to verify it passes**

Run: `./tests/run-host-tests.sh`
Expected: PASS — `ALL TESTS PASSED`.

- [ ] **Step 6: Commit**

```bash
git add src/rdb.h src/rdb.c tests/test_rdb.c
git commit -m "feat(rdb): rdb_gap_end_after/before — free-run queries around a partition"
```

---

## Task 3: GUI — `GID_RESIZE` enum + forward declaration

**Files:**
- Modify: `src/gui.c:32-33` (enum), and the forward-declaration block (~`src/gui.c:124-127`)

- [ ] **Step 1: Add the gadget ID**

In `src/gui.c`, change the gadget-ID enum (currently ending `GID_SPLIT, GID_REFRESH }`):

```c
enum { GID_DEVICE = 1, GID_SCAN, GID_DRIVER, GID_PARTS, GID_NEW, GID_DELETE,
       GID_EDIT, GID_INIT, GID_SAVE, GID_STATUS, GID_UNIT, GID_SPLIT, GID_REFRESH,
       GID_RESIZE };
```

- [ ] **Step 2: Add the forward declaration**

In `src/gui.c`, in the forward-declaration block (right after the `gui_refresh_current` declaration added previously), add:

```c
static int  gui_resize_dialog(int index);     /* grow/shrink the selected partition */
```

- [ ] **Step 3: Verify it compiles**

Run:
```bash
export AMIGA_BIN="/Users/sfs/.vscode/extensions/bartmanabyss.amiga-debug-1.8.2/bin/darwin"
export PATH="$AMIGA_BIN/opt/bin:$AMIGA_BIN:$PATH"
make
```
Expected: builds to `entry-order OK: _start is first in .text`. (A `gui_resize_dialog defined but not used`-style warning is acceptable here since it is only declared, not yet defined; if `-Werror` turns the unused declaration into a failure it will not — a forward declaration alone does not warn.)

- [ ] **Step 4: Commit**

```bash
git add src/gui.c
git commit -m "feat(gui): add GID_RESIZE id and gui_resize_dialog forward decl"
```

---

## Task 4: GUI — `gui_resize_dialog`

**Files:**
- Modify: `src/gui.c` — add the function. Place it immediately before `gui_split` (so the `numstr`/`u2s`/`s_cat`/`gui_request`/`gui_msg` helpers it uses are already defined above it, as they are for `gui_split`).

- [ ] **Step 1: Write the dialog function**

Insert this complete function just above `static void gui_split(void)`:

```c
/* Resize the selected partition: Size (MB) + anchor (Move End / Move Start)
   -> a new cylinder range, applied via rdb_resize_cyl. Confirms only when the
   change is destructive (shrink, or any Start-edge move). Returns 1 if applied
   (caller need not act — the dialog refreshes the view itself), else 0. */
static int gui_resize_dialog(int index)
{
    struct Window *dw;
    struct Gadget *dglist = 0, *g, *gAnchor = 0, *gSize = 0, *gMaxHint = 0, *gRead = 0;
    struct NewGadget ng;
    RdbPartition *pt;
    int dt = g_topb, dl = g_leftb;
    int done = 0, applied = 0, anchor = 0;          /* 0 = Move End, 1 = Move Start */
    uint32_t oldLow, oldHigh, cylBlocks, blockBytes;
    uint32_t gapAfter, gapBefore, maxCyls, maxMB, curMB, n;
    static const char *anchorLabels[3];
    static char maxBuf[20], readBuf[48], freeBuf[48];

    if (!g_have_model || index < 0 || index >= g_model.num_parts) return 0;
    pt = &g_model.parts[index];
    oldLow = pt->low_cyl; oldHigh = pt->high_cyl;
    cylBlocks = g_model.cyl_blocks; blockBytes = g_model.block_bytes;
    gapAfter  = rdb_gap_end_after(&g_model, index);     /* exclusive */
    gapBefore = rdb_gap_start_before(&g_model, index);  /* inclusive */

    /* current size + the max for the default (Move End) anchor */
    curMB  = rdb_cyls_to_mb(oldHigh - oldLow + 1, cylBlocks, blockBytes);
    if (curMB < 1) curMB = 1;
    maxCyls = gapAfter - oldLow;                        /* Move End: low fixed */
    maxMB   = rdb_cyls_to_mb(maxCyls, cylBlocks, blockBytes);
    if (maxMB < 1) maxMB = 1;
    if (curMB > maxMB) curMB = maxMB;
    n = curMB;

    anchorLabels[0] = "Move: End edge (keep start)";
    anchorLabels[1] = "Move: Start edge (keep end)";
    anchorLabels[2] = 0;

    { int p = 0; s_cat(maxBuf, &p, "max "); p += u2s(maxBuf + p, maxMB);
      s_cat(maxBuf, &p, " MB"); maxBuf[p] = 0; }
    { int p = 0; uint32_t fb, fa;
      fb = (oldLow > gapBefore) ? rdb_cyls_to_mb(oldLow - gapBefore, cylBlocks, blockBytes) : 0;
      fa = (gapAfter > oldHigh + 1) ? rdb_cyls_to_mb(gapAfter - 1 - oldHigh, cylBlocks, blockBytes) : 0;
      s_cat(freeBuf, &p, "Free before "); p += u2s(freeBuf + p, fb);
      s_cat(freeBuf, &p, " MB   after "); p += u2s(freeBuf + p, fa);
      s_cat(freeBuf, &p, " MB"); freeBuf[p] = 0; }

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wincompatible-pointer-types"
    g = CreateContext(&dglist);
#pragma GCC diagnostic pop
    if (!g) return 0;
    ng.ng_TextAttr = &g_font; ng.ng_VisualInfo = g_vi; ng.ng_Flags = 0;

    /* free before/after context line (read-only) */
    ng.ng_LeftEdge = dl + 10; ng.ng_TopEdge = dt + 6; ng.ng_Width = 320; ng.ng_Height = 12;
    ng.ng_GadgetText = 0; ng.ng_GadgetID = 0;
    g = CreateGadget(TEXT_KIND, g, &ng, GTTX_Text, (ULONG)freeBuf, TAG_END);

    /* anchor cycle */
    ng.ng_LeftEdge = dl + 100; ng.ng_TopEdge = dt + 24; ng.ng_Width = 230; ng.ng_Height = 14;
    ng.ng_GadgetText = (UBYTE *)"Anchor"; ng.ng_GadgetID = 1;
    g = CreateGadget(CYCLE_KIND, g, &ng, GTCY_Labels, (ULONG)anchorLabels, GTCY_Active, 0, TAG_END);
    gAnchor = g;

    /* size (MB) */
    ng.ng_LeftEdge = dl + 100; ng.ng_TopEdge = dt + 44; ng.ng_Width = 90; ng.ng_Height = 14;
    ng.ng_GadgetText = (UBYTE *)"Size (MB)"; ng.ng_GadgetID = 2;
    g = CreateGadget(INTEGER_KIND, g, &ng, GTIN_Number, (ULONG)n, GTIN_MaxChars, 7, TAG_END);
    gSize = g;

    /* max hint */
    ng.ng_LeftEdge = dl + 200; ng.ng_Width = 130; ng.ng_GadgetText = 0; ng.ng_GadgetID = 0;
    g = CreateGadget(TEXT_KIND, g, &ng, GTTX_Text, (ULONG)maxBuf, TAG_END);
    gMaxHint = g;

    /* live cyl readout (filled by RZ_RECOMPUTE below) */
    ng.ng_LeftEdge = dl + 10; ng.ng_TopEdge = dt + 64; ng.ng_Width = 320; ng.ng_Height = 12;
    ng.ng_GadgetText = 0; ng.ng_GadgetID = 0;
    g = CreateGadget(TEXT_KIND, g, &ng, GTTX_Text, (ULONG)"", TAG_END);
    gRead = g;

    /* persistent caveat */
    ng.ng_LeftEdge = dl + 10; ng.ng_TopEdge = dt + 80; ng.ng_Width = 320; ng.ng_Height = 12;
    g = CreateGadget(TEXT_KIND, g, &ng, GTTX_Text,
                     (ULONG)"Resize edits the table only - reformat to use new space.", TAG_END);

    /* Ok / Cancel */
    ng.ng_LeftEdge = dl + 10; ng.ng_TopEdge = dt + 98; ng.ng_Width = 70; ng.ng_Height = 14;
    ng.ng_GadgetText = (UBYTE *)"Ok"; ng.ng_GadgetID = 10;
    g = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);
    ng.ng_LeftEdge = dl + 200; ng.ng_GadgetText = (UBYTE *)"Cancel"; ng.ng_GadgetID = 11;
    g = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);
    if (!g) { FreeGadgets(dglist); return 0; }

    {   int dwW = dl + 350 + g_scr->WBorRight;
        int dwH = dt + 120 + g_scr->WBorBottom;
        int dwL = g_win->LeftEdge + (g_win->Width  - dwW) / 2;
        int dwT = g_win->TopEdge  + (g_win->Height - dwH) / 2;
        if (dwL < 0) dwL = 0;
        if (dwT < 0) dwT = 0;
        dw = OpenWindowTags(0,
            WA_Left, dwL, WA_Top, dwT, WA_Width, dwW, WA_Height, dwH,
            WA_Title, (ULONG)"Resize partition", WA_Gadgets, (ULONG)dglist,
            WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW | BUTTONIDCMP | INTEGERIDCMP | CYCLEIDCMP,
            WA_Flags, WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_ACTIVATE | WFLG_SMART_REFRESH,
            g_pub ? WA_PubScreen : WA_CustomScreen, (ULONG)g_scr,
            TAG_END);
    }
    if (!dw) { FreeGadgets(dglist); return 0; }
    GT_RefreshWindow(dw, 0);

    /* helper to recompute max + clamp n + refresh the readout for the current anchor */
#define RZ_RECOMPUTE() do {                                                       \
        if (anchor == 0) maxCyls = gapAfter - oldLow;                             \
        else             maxCyls = oldHigh - gapBefore + 1;                       \
        maxMB = rdb_cyls_to_mb(maxCyls, cylBlocks, blockBytes);                   \
        if (maxMB < 1) maxMB = 1;                                                 \
        if (n < 1) n = 1; if (n > maxMB) n = maxMB;                               \
        { int p = 0; s_cat(maxBuf, &p, "max "); p += u2s(maxBuf + p, maxMB);      \
          s_cat(maxBuf, &p, " MB"); maxBuf[p] = 0; }                              \
        GT_SetGadgetAttrs(gMaxHint, dw, 0, GTTX_Text, (ULONG)maxBuf, TAG_END);    \
        GT_SetGadgetAttrs(gSize, dw, 0, GTIN_Number, (ULONG)n, TAG_END);          \
        { uint32_t cyls = rdb_mb_to_cyls(n, cylBlocks, blockBytes);              \
          uint32_t nl, nh; int p = 0;                                            \
          if (cyls < 1) cyls = 1;                                                 \
          if (anchor == 0) { nl = oldLow;  nh = oldLow + cyls - 1; }              \
          else             { nh = oldHigh; nl = oldHigh - cyls + 1; }            \
          s_cat(readBuf, &p, "-> cyls "); p += u2s(readBuf + p, nl);             \
          s_cat(readBuf, &p, ".."); p += u2s(readBuf + p, nh);                   \
          s_cat(readBuf, &p, "  ("); p += u2s(readBuf + p, n);                   \
          s_cat(readBuf, &p, " MB)"); readBuf[p] = 0; }                          \
        GT_SetGadgetAttrs(gRead, dw, 0, GTTX_Text, (ULONG)readBuf, TAG_END);      \
    } while (0)

    RZ_RECOMPUTE();                      /* fill the initial readout */
    ActivateGadget(gSize, dw, 0);

    while (!done) {
        struct IntuiMessage *im;
        WaitPort(dw->UserPort);
        while ((im = GT_GetIMsg(dw->UserPort)) != 0) {
            ULONG cl = im->Class;
            struct Gadget *ig = (struct Gadget *)im->IAddress;
            UWORD code = im->Code;
            GT_ReplyIMsg(im);
            if (cl == IDCMP_CLOSEWINDOW) { done = 1; }
            else if (cl == IDCMP_REFRESHWINDOW) { GT_BeginRefresh(dw); GT_EndRefresh(dw, TRUE); }
            else if (cl == IDCMP_GADGETUP) {
                if (ig == gAnchor) { anchor = (int)code; RZ_RECOMPUTE(); }
                else if (ig == gSize) {
                    n = (uint32_t)((struct StringInfo *)gSize->SpecialInfo)->LongInt;
                    RZ_RECOMPUTE();
                } else if (ig->GadgetID == 10) {                 /* Ok */
                    uint32_t cyls = rdb_mb_to_cyls(n, cylBlocks, blockBytes);
                    uint32_t nl, nh; int destructive, go = 1;
                    if (cyls < 1) cyls = 1;
                    if (anchor == 0) { nl = oldLow;  nh = oldLow + cyls - 1; }
                    else             { nh = oldHigh; nl = oldHigh - cyls + 1; }
                    if (nl == oldLow && nh == oldHigh) { done = 1; break; }   /* no change */
                    destructive = (nl != oldLow) || (nh < oldHigh);
                    if (destructive)
                        go = gui_request("Resize partition",
                                "Resizing changes the partition's extent.\n"
                                "Reformat afterward - existing data will be lost.",
                                1, "Proceed");
                    if (go) {
                        int r = rdb_resize_cyl(&g_model, index, nl, nh);
                        if (r == RDB_OK) {
                            g_dirty = 1; applied = 1; done = 1;
                        } else {
                            gui_msg("Resize", "Could not resize (overlaps / out of range).");
                        }
                    }
                } else if (ig->GadgetID == 11) { done = 1; }     /* Cancel */
            }
        }
    }
#undef RZ_RECOMPUTE
    CloseWindow(dw);
    FreeGadgets(dglist);
    if (applied) { gui_refresh_parts(); gui_update_buttons(); }
    return applied;
}
```

- [ ] **Step 2: Verify it compiles**

Run:
```bash
export AMIGA_BIN="/Users/sfs/.vscode/extensions/bartmanabyss.amiga-debug-1.8.2/bin/darwin"
export PATH="$AMIGA_BIN/opt/bin:$AMIGA_BIN:$PATH"
make
```
Expected: builds clean to `entry-order OK: _start is first in .text`. (The function is now defined and declared; the not-yet-wired call comes in Task 5. If the compiler warns `gui_resize_dialog defined but not used`, that is expected and resolved by Task 5 — it should be a warning, not an error, matching how other static dialog fns are treated.)

- [ ] **Step 3: Commit**

```bash
git add src/gui.c
git commit -m "feat(gui): gui_resize_dialog — Size+anchor resize with destructive-only confirm"
```

---

## Task 5: GUI — `Resize…` button (create, enable, dispatch)

**Files:**
- Modify: `src/gui.c` — button creation (after the Refresh button block, ~`src/gui.c:272-281`), enable rule in `gui_update_buttons` (~`src/gui.c:159`), event dispatch (~`src/gui.c:1077`).

- [ ] **Step 1: Create the button next to Refresh**

In `src/gui.c`, find the Refresh button creation block (it ends with `g_gad[GID_REFRESH] = g;`). Immediately after that line, add:

```c
    /* Resize... button beside Refresh: grow/shrink the selected partition. */
    ng.ng_LeftEdge = 90 + g_leftb; ng.ng_TopEdge = 146 + g_topb; ng.ng_Width = 72; ng.ng_Height = 14;
    ng.ng_GadgetText = (UBYTE *)"Resize..."; ng.ng_GadgetID = GID_RESIZE;
    g = CreateGadget(BUTTON_KIND, g, &ng, GA_Disabled, TRUE, TAG_END);
    g_gad[GID_RESIZE] = g;
```

- [ ] **Step 2: Add the enable rule**

In `gui_update_buttons`, find the line that sets `GID_REFRESH`'s disabled state. The selection state is computed there as `hasSel`. Immediately after the `GID_REFRESH` `GT_SetGadgetAttrs(...)` line, add:

```c
    GT_SetGadgetAttrs(g_gad[GID_RESIZE], g_win, 0, GA_Disabled, (ULONG)!hasSel, TAG_END);
```

(If `hasSel` is not in scope in that exact spot, it is the same expression used for `GID_EDIT`/`GID_DELETE` a few lines above — reuse that identifier; do not recompute.)

- [ ] **Step 3: Wire the event dispatch**

In the main event loop's GADGETUP dispatch chain, find the line `else if (gad->GadgetID == GID_REFRESH) gui_refresh_current();` and add directly after it:

```c
                    else if (gad->GadgetID == GID_RESIZE) {
                        if (g_sel_part >= 0 && g_sel_part < g_model.num_parts)
                            gui_resize_dialog(g_sel_part);
                    }
```

- [ ] **Step 4: Build and stage**

Run:
```bash
export AMIGA_BIN="/Users/sfs/.vscode/extensions/bartmanabyss.amiga-debug-1.8.2/bin/darwin"
export PATH="$AMIGA_BIN/opt/bin:$AMIGA_BIN:$PATH"
make && make hd && cmp -s out/HDPart.exe amiga_hd/HDPart && echo STAGED_OK
```
Expected: builds to `entry-order OK: _start is first in .text`, then `STAGED_OK`. No `defined but not used` warning for `gui_resize_dialog` now (it is called).

- [ ] **Step 5: Commit**

```bash
git add src/gui.c
git commit -m "feat(gui): Resize... button (selection-scoped) launches gui_resize_dialog"
```

---

## Task 6: Manual verification on FS-UAE

**Files:** none (manual on-target test). Prereq: Task 5's `make hd` staged the binary.

- [ ] **Step 1: Boot the dev-test config**

Launch FS-UAE with the `HDPart-204-devtest` config (KS2.04/WB2.04 + scratch hardfiles). From a WB Shell run `HDPart:HDPart`. Select a driver and **Scan**, pick a unit with a partition table that has free space (e.g. the 150MB scratch, or Init+Split a blank one to create partitions with a gap).

- [ ] **Step 2: Resize — grow End edge (non-destructive, no prompt)**

Select a partition that has free space after it. Click **Resize…**. Anchor defaults to *Move: End edge*. Increase Size (MB) toward the `max N MB` hint; confirm the `-> cyls A..B (S MB)` readout updates. Click **Ok**.
Expected: applies with **no confirmation requester**; the list + disk-map bar show the larger partition; **Save** becomes enabled.

- [ ] **Step 3: Resize — shrink (destructive, prompt appears)**

Select a partition, **Resize…**, lower the Size below current, **Ok**.
Expected: a Proceed/Cancel requester appears ("…existing data will be lost"). **Cancel** leaves it unchanged; repeat and **Proceed** applies the smaller size and opens a free gap on the anchored side.

- [ ] **Step 4: Resize — Move Start edge (destructive, prompt appears)**

Select a partition with free space **before** it. **Resize…**, switch Anchor to *Move: Start edge*; confirm the `max N MB` hint and free-before/after line reflect the leading gap. Grow the size and **Ok**.
Expected: confirmation appears (Start edge moved); on Proceed, `low_cyl` drops into the leading gap.

- [ ] **Step 5: Save + Refresh round-trip**

Click **Save**. Then click **Refresh** (re-reads from disk).
Expected: the resized extents persist and match after the re-read; Status shows the saved state.

- [ ] **Step 6: Button gating**

With no partition selected (click empty space in the list, or right after a driver change), confirm **Resize…** is ghosted; it enables only when a partition is selected.

- [ ] **Step 7: Record the result**

If all steps pass, note it. If anything misbehaves on real KS2.04 (e.g. cycle gadget not refreshing, requester Ok dead — cf. the WB2.04 ASL gotcha), capture the symptom for a follow-up fix.

---

## Self-Review notes

- **Spec coverage:** engine `rdb_resize_cyl` (Task 1); gap queries (Task 2, relocated to engine — flagged at top); dialog with anchor/size/max-hint/readout/caveat (Task 4); destructive-only confirm (Task 4, Step 1); launch button selection-scoped (Task 5); host tests incl. rollback/overlap/bounds/range/1-cyl (Tasks 1–2); manual FS-UAE matrix (Task 6). Edit's Size field left untouched (no task modifies it). YAGNI exclusions unaddressed by design.
- **Types/signatures consistent:** `rdb_resize_cyl(RdbModel*, int, uint32_t, uint32_t)`, `rdb_gap_end_after/before(const RdbModel*, int) -> uint32_t`, `gui_resize_dialog(int) -> int`, `GID_RESIZE` used in enum + create + enable + dispatch. Dialog uses confirmed helpers `s_cat`, `u2s`, `rdb_cyls_to_mb`, `rdb_mb_to_cyls`, `gui_request(title,body,1,"Proceed")` (returns 1 on Proceed), `gui_msg`, `gui_refresh_parts`, `gui_update_buttons`, `g_dirty`, `g_have_model`, `g_model`, `g_win`, `g_scr`, `g_pub`, `g_vi`, `g_font`.
- **Known watch-point for the implementer:** verify `hasSel` is the exact identifier used for `GID_EDIT`/`GID_DELETE` in `gui_update_buttons` before reusing it in Step 2 of Task 5; if the variable is named differently, use that name.
```
