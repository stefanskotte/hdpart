# HDPart — Plan 3b: Edit / Init / Save (the write path) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn the read-only viewer (Plan 3a) into a full partition editor: enable New / Delete / Edit (a GadTools editor dialog with name, MB field, draggable slider, and filesystem cycle), Init Disk (fresh empty RDB from device geometry), and Save (write the RDB to disk) — all under the spec's safety model: nothing is written until Save, Save and Init confirm via a requester, and every write is read back and verified.

**Architecture:** The GUI's `g_model` becomes an editable working copy with a dirty flag; the pure `rdb.c` engine gains a few host-tested edit helpers (largest-free-gap, add-at, resize). All editing mutates `g_model` in memory; the only disk write is `Save`, which serializes via the existing `dev_block_io` and then re-reads to verify. Confirmation dialogs use `EasyRequest` (V37). The partition editor is a modal GadTools window (STRING + INTEGER + SLIDER + CYCLE gadgets); the slider and MB field stay in sync, and values are read on V37 via the gadgets' `StringInfo` (since `GT_GetGadgetAttrs` is V39-only).

**Tech Stack:** C (freestanding m68k + hosted tests for the engine helpers), intuition (`EasyRequest`), gadtools (STRING/INTEGER/SLIDER/CYCLE kinds), the existing engine/device/discovery modules, FS-UAE on Kickstart 2.04, host `rdbtool` for write verification.

---

## Prerequisites & conventions

Plans 1, 2, 3a complete and verified on KS 2.04. `src/gui.c` has the read-only GUI (device picker, partition listview, disk-map bar, status). `rdb.c` has `rdb_init_model`, `rdb_add_partition`, `rdb_delete_partition`, `rdb_validate`, `rdb_serialize`, `rdb_parse`, `rdb_mb_to_cyls`, `rdb_cyls_to_mb`. `device.c` has `dev_open/dev_close/dev_geometry/dev_block_io`. `support.c` provides division.

Amiga build prelude:
```bash
export AMIGA_BIN="/Users/sfs/.vscode/extensions/bartmanabyss.amiga-debug-1.8.2/bin/darwin"
export PATH="$AMIGA_BIN/opt/bin:$AMIGA_BIN:$PATH"
```
Host tests: `./tests/run-host-tests.sh`. Build: `make` → `out/HDPart.exe`. Stage: `make hd`. Test config: **HDPart-204-devtest**. Cross-verify writes on the host with `rdbtool amiga_scratch/scratch_*.hdf info`.

**Hard-won facts (do not relearn):** ~4 KB Shell stack mitigated by StackSwap (keep big structs static anyway); avoid 64-bit int math (32-bit `/`,`%` provided); `GT_GetGadgetAttrs` is V39 — read STRING/INTEGER values via `((struct StringInfo *)gad->SpecialInfo)->Buffer` / `->LongInt`, and read the SLIDER level from the IntuiMessage `Code`; `EasyRequest` returns 1..N left-to-right with the **rightmost gadget returning 0** (so put the safe/Cancel option rightmost).

**Safety invariant (must hold throughout):** the ONLY code path that writes to the disk is `gui_save()`. Init/New/Delete/Edit mutate `g_model` in memory only. Every disk write is followed by a read-back verify before reporting success.

## File structure (this plan)

```
src/
  rdb.h / rdb.c   MOD  edit helpers: rdb_largest_free_gap, rdb_add_partition_at, rdb_set_partition
  gui.c           MOD  editable model state, button enable/disable, listview selection,
                       EasyRequest confirms, Save (+verify), Init, Delete, New, Edit dialog
tests/
  test_rdb.c      MOD  tests for the new edit helpers
```

---

## Milestone 3b.1 — RDB engine edit helpers (host-tested)

### Task 3b.1.1: rdb_largest_free_gap

**Files:** Modify `src/rdb.h`, `src/rdb.c`, `tests/test_rdb.c`

- [ ] **Step 1: Write the failing test** (append to `tests/test_rdb.c`, call from `main()`)

```c
static void test_largest_free_gap(void)
{
    RdbModel m;
    uint32_t s = 0, e = 0;
    rdb_init_model(&m, 996, 16, 63);            /* lo_cyl=2, hi_cyl=995 */
    /* empty disk: whole partitionable area is the gap */
    CHECK(rdb_largest_free_gap(&m, &s, &e) == 1);
    CHECK(s == 2 && e == 995);
    /* one partition at the front leaves the tail as the gap */
    rdb_add_partition(&m, "DH0", 100, RDB_DOSTYPE_FFS_INTL);   /* 2..N */
    CHECK(rdb_largest_free_gap(&m, &s, &e) == 1);
    CHECK(s == m.parts[0].high_cyl + 1 && e == 995);
    /* fill the rest; no gap remains */
    {
        uint32_t cyls = 995 - (m.parts[0].high_cyl) ;
        uint32_t mb = rdb_cyls_to_mb(cyls, m.cyl_blocks, m.block_bytes);
        rdb_add_partition(&m, "DH1", mb, RDB_DOSTYPE_FFS_INTL);
    }
    CHECK(rdb_largest_free_gap(&m, &s, &e) == 0);
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `cd /Users/sfs/Devel/party && ./tests/run-host-tests.sh`
Expected: FAIL — `rdb_largest_free_gap` undefined.

- [ ] **Step 3: Declare + implement**

`src/rdb.h` (near the other partition ops):
```c
/* Find the largest unallocated cylinder range within [lo_cyl, hi_cyl].
   On success writes start/end (inclusive) and returns 1; returns 0 if the
   disk is full. Handles unsorted/overlapping partitions defensively. */
int rdb_largest_free_gap(const RdbModel *m, uint32_t *start, uint32_t *end);
```

`src/rdb.c`:
```c
int rdb_largest_free_gap(const RdbModel *m, uint32_t *start, uint32_t *end)
{
    uint32_t cur = m->lo_cyl, best_s = 0, best_e = 0, best_len = 0;
    while (cur <= m->hi_cyl) {
        uint32_t next_start = m->hi_cyl + 1;
        int covering = -1, i;
        for (i = 0; i < m->num_parts; i++) {
            const RdbPartition *p = &m->parts[i];
            if (p->low_cyl <= cur && cur <= p->high_cyl) { covering = i; break; }
            if (p->low_cyl > cur && p->low_cyl < next_start) next_start = p->low_cyl;
        }
        if (covering >= 0) { cur = m->parts[covering].high_cyl + 1; continue; }
        {
            uint32_t gs = cur, ge = (next_start > 0 ? next_start - 1 : 0);
            if (ge > m->hi_cyl) ge = m->hi_cyl;
            if (ge >= gs) {
                uint32_t len = ge - gs + 1;
                if (len > best_len) { best_len = len; best_s = gs; best_e = ge; }
            }
        }
        cur = next_start;
    }
    if (best_len == 0) return 0;
    *start = best_s; *end = best_e;
    return 1;
}
```

- [ ] **Step 4: Run to verify pass**; **Step 5: Commit**

```bash
cd /Users/sfs/Devel/party && ./tests/run-host-tests.sh
git add src/rdb.h src/rdb.c tests/test_rdb.c
git commit -m "feat(rdb): rdb_largest_free_gap + tests"
```

### Task 3b.1.2: rdb_add_partition_at + rdb_set_partition

**Files:** Modify `src/rdb.h`, `src/rdb.c`, `tests/test_rdb.c`

- [ ] **Step 1: Write the failing test** (append, call from `main()`)

```c
static void test_add_at_and_set(void)
{
    RdbModel m;
    int r, idx;
    rdb_init_model(&m, 996, 16, 63);
    /* add at an explicit start cylinder */
    idx = rdb_add_partition_at(&m, "DH0", 100, 50, RDB_DOSTYPE_FFS_INTL);
    CHECK(idx == 0);
    CHECK(m.parts[0].low_cyl == 100);
    CHECK(m.parts[0].high_cyl == 100 + rdb_mb_to_cyls(50, m.cyl_blocks, m.block_bytes) - 1);
    /* resize partition 0 to 30 MB, keeping its start */
    r = rdb_set_partition(&m, 0, "WORK", 30, RDB_DOSTYPE_FFS_INTL);
    CHECK(r == RDB_OK);
    CHECK(m.parts[0].low_cyl == 100);
    CHECK(strcmp(m.parts[0].name, "WORK") == 0);
    CHECK(m.parts[0].high_cyl == 100 + rdb_mb_to_cyls(30, m.cyl_blocks, m.block_bytes) - 1);
    /* a resize that would overflow the disk is rejected, leaving the model valid */
    r = rdb_set_partition(&m, 0, "WORK", 100000, RDB_DOSTYPE_FFS_INTL);
    CHECK(r == RDB_ERR_NO_SPACE);
    CHECK(rdb_validate(&m) == RDB_OK);
    /* add_at that overlaps an existing partition is rejected */
    idx = rdb_add_partition_at(&m, "DH1", 100, 10, RDB_DOSTYPE_FFS_INTL);
    CHECK(idx == RDB_ERR_OVERLAP);
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `cd /Users/sfs/Devel/party && ./tests/run-host-tests.sh`
Expected: FAIL — functions undefined.

- [ ] **Step 3: Declare + implement**

`src/rdb.h`:
```c
/* Add a partition at an explicit start cylinder, size in MB. Returns the new
   index (>=0) or a negative RDB_ERR_* (e.g. RDB_ERR_OVERLAP / RDB_ERR_NO_SPACE
   / RDB_ERR_DUP_NAME / RDB_ERR_TOO_MANY). */
int rdb_add_partition_at(RdbModel *m, const char *name, uint32_t start_cyl,
                         uint32_t size_mb, uint32_t dos_type);

/* Update partition `index` in place: rename, set dos_type, and resize to
   size_mb keeping its low_cyl fixed (high_cyl recomputed). Validates the whole
   model afterward; on a validation failure the partition is restored and an
   RDB_ERR_* is returned. RDB_OK on success. */
int rdb_set_partition(RdbModel *m, int index, const char *name,
                      uint32_t size_mb, uint32_t dos_type);
```

`src/rdb.c` (reuses the existing static `copy_name` and `rdb_validate`):
```c
int rdb_add_partition_at(RdbModel *m, const char *name, uint32_t start_cyl,
                         uint32_t size_mb, uint32_t dos_type)
{
    uint32_t cyls, end;
    RdbPartition *p;
    if (m->num_parts >= RDB_MAX_PARTS) return RDB_ERR_TOO_MANY;
    if (!name || !name[0])             return RDB_ERR_BAD_NAME;
    if (name_taken(m, name, -1))       return RDB_ERR_DUP_NAME;
    cyls = rdb_mb_to_cyls(size_mb, m->cyl_blocks, m->block_bytes);
    if (cyls == 0) cyls = 1;
    end = start_cyl + cyls - 1;
    if (start_cyl < m->lo_cyl || end > m->hi_cyl) return RDB_ERR_NO_SPACE;

    p = &m->parts[m->num_parts];        /* tentative */
    copy_name(p->name, name);
    p->low_cyl = start_cyl; p->high_cyl = end;
    p->dos_type = dos_type; p->num_buffers = 30; p->boot_pri = 0; p->bootable = 0;
    m->num_parts++;
    if (rdb_validate(m) != RDB_OK) { m->num_parts--; return RDB_ERR_OVERLAP; }
    return m->num_parts - 1;
}

int rdb_set_partition(RdbModel *m, int index, const char *name,
                      uint32_t size_mb, uint32_t dos_type)
{
    RdbPartition saved, *p;
    uint32_t cyls;
    int v;
    if (index < 0 || index >= m->num_parts) return RDB_ERR_RANGE;
    if (!name || !name[0])                  return RDB_ERR_BAD_NAME;
    if (name_taken(m, name, index))         return RDB_ERR_DUP_NAME;
    p = &m->parts[index];
    saved = *p;
    cyls = rdb_mb_to_cyls(size_mb, m->cyl_blocks, m->block_bytes);
    if (cyls == 0) cyls = 1;
    copy_name(p->name, name);
    p->dos_type = dos_type;
    p->high_cyl = p->low_cyl + cyls - 1;
    if (p->high_cyl > m->hi_cyl) { *p = saved; return RDB_ERR_NO_SPACE; }
    v = rdb_validate(m);
    if (v != RDB_OK) { *p = saved; return v; }
    return RDB_OK;
}
```

- [ ] **Step 4: Run to verify pass**; **Step 5: Commit**

```bash
cd /Users/sfs/Devel/party && ./tests/run-host-tests.sh
git add src/rdb.h src/rdb.c tests/test_rdb.c
git commit -m "feat(rdb): rdb_add_partition_at + rdb_set_partition (resize in place) + tests"
```

### Task 3b.1.3: build the engine helpers for m68k

- [ ] **Step 1: Build + confirm no 64-bit refs**

Run:
```bash
export AMIGA_BIN="/Users/sfs/.vscode/extensions/bartmanabyss.amiga-debug-1.8.2/bin/darwin"
export PATH="$AMIGA_BIN/opt/bin:$AMIGA_BIN:$PATH"
cd /Users/sfs/Devel/party && make 2>&1 | grep -iE "entry-order|undefined|error"
m68k-amiga-elf-objdump -r obj/rdb.o | grep -iE '__udivdi3|__muldi3|__umoddi3' || echo NONE
```
Expected: clean build, `entry-order OK`, `NONE`. (No commit; covered by the next milestone's.)

---

## Milestone 3b.2 — Editable model state, selection, button enable/disable

### Task 3b.2.1: state additions + gui_update_buttons + gui_refresh_parts refactor

**Files:** Modify `src/gui.c`

- [ ] **Step 1: Add state + helpers**

Add module statics (after `g_have_model`):
```c
static int        g_dirty;            /* unsaved edits in g_model */
static DeviceInfo g_geo;              /* geometry of the selected device */
static char       g_cur_driver[40];   /* selected device driver/unit (for Save) */
static uint32_t   g_cur_unit;
static int        g_sel_part = -1;    /* selected partition index, or -1 */
```

Add `#include <intuition/intuition.h>` is already present. Add a button-state helper and a refresh helper ABOVE `gui_select_device`:
```c
static void gui_update_buttons(void)
{
    int hasModel = g_have_model;
    int hasGeo   = g_geo.has_media;
    int hasSel   = (g_sel_part >= 0 && g_sel_part < g_model.num_parts);
    if (!g_win) return;
    GT_SetGadgetAttrs(g_gad[GID_SAVE],   g_win, 0, GA_Disabled, (ULONG)!(hasModel && g_dirty), TAG_END);
    GT_SetGadgetAttrs(g_gad[GID_NEW],    g_win, 0, GA_Disabled, (ULONG)!hasModel, TAG_END);
    GT_SetGadgetAttrs(g_gad[GID_INIT],   g_win, 0, GA_Disabled, (ULONG)!hasGeo,   TAG_END);
    GT_SetGadgetAttrs(g_gad[GID_DELETE], g_win, 0, GA_Disabled, (ULONG)!hasSel,   TAG_END);
    GT_SetGadgetAttrs(g_gad[GID_EDIT],   g_win, 0, GA_Disabled, (ULONG)!hasSel,   TAG_END);
}

/* Rebuild the partition listview + status text + bar from g_model. */
static void gui_refresh_parts(void)
{
    int i;
    NewList(&g_partlist);
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
        { int p = 0; s_cat(g_statusbuf, &p, g_dirty ? "MODIFIED  " : "RDB OK  ");
          p += u2s(g_statusbuf + p, (ULONG)g_model.num_parts);
          s_cat(g_statusbuf, &p, " partitions  ");
          p += u2s(g_statusbuf + p, g_model.cylinders); s_cat(g_statusbuf, &p, " cyl");
          g_statusbuf[p] = 0; }
    } else {
        int p = 0; s_cat(g_statusbuf, &p, "no valid RDB on this disk"); g_statusbuf[p] = 0;
    }
    if (g_win && g_gad[GID_PARTS])
        GT_SetGadgetAttrs(g_gad[GID_PARTS], g_win, 0, GTLV_Labels, (ULONG)&g_partlist, TAG_END);
    if (g_win && g_gad[GID_STATUS])
        GT_SetGadgetAttrs(g_gad[GID_STATUS], g_win, 0, GTTX_Text, (ULONG)g_statusbuf, TAG_END);
    gui_draw_bar();
}
```

- [ ] **Step 2: Rewrite gui_select_device to capture geometry + reset state + use gui_refresh_parts**

Replace the body of `gui_select_device` with:
```c
void gui_select_device(int idx)
{
    DeviceHandle *h;
    DiscDisk *d;
    int di, n;

    g_have_model = 0;
    g_dirty = 0;
    g_sel_part = -1;
    g_geo.has_media = 0;

    if (g_ndisks == 0 || idx < 0) {
        gui_refresh_parts();
        gui_update_buttons();
        return;
    }
    di = g_devmap[idx];
    d  = &g_disks[di];
    for (n = 0; n < 40 && d->driver[n]; n++) g_cur_driver[n] = d->driver[n];
    g_cur_driver[n] = 0;
    g_cur_unit = d->unit;

    h = dev_open(d->driver, d->unit);
    if (h) {
        dev_geometry(h, &g_geo);                       /* for Init + display */
        if (rdb_parse(&g_model, dev_block_io, h) == RDB_OK) g_have_model = 1;
        dev_close(h);
    }
    gui_refresh_parts();
    gui_update_buttons();
}
```
(Remove the old in-function row/status building — it now lives in `gui_refresh_parts`.)

- [ ] **Step 3: Track listview selection + call gui_update_buttons after gui_rescan**

In `gui_run`'s event loop `IDCMP_GADGETUP` case, add a branch for the listview:
```c
                case IDCMP_GADGETUP:
                    if (gad->GadgetID == GID_DEVICE) gui_select_device((int)code);
                    else if (gad->GadgetID == GID_RESCAN) gui_rescan();
                    else if (gad->GadgetID == GID_PARTS) { g_sel_part = (int)code; gui_update_buttons(); }
                    break;
```
In `gui_rescan`, after the `if (n > 0) gui_select_device(0);` line, add `else { g_have_model = 0; gui_update_buttons(); }` so the buttons are correct when no disks are found. And add a `gui_update_buttons();` call at the very end of `gui_rescan` (after the select) so the initial state is correct.

- [ ] **Step 4: Build + host tests + stage**

Run:
```bash
export AMIGA_BIN="/Users/sfs/.vscode/extensions/bartmanabyss.amiga-debug-1.8.2/bin/darwin"
export PATH="$AMIGA_BIN/opt/bin:$AMIGA_BIN:$PATH"
cd /Users/sfs/Devel/party && make 2>&1 | grep -iE "entry-order|undefined|error" && ./tests/run-host-tests.sh 2>&1 | tail -2 && make hd 2>&1 | tail -1
```
Expected: clean build; host tests pass; staged.

- [ ] **Step 5: Commit**

```bash
cd /Users/sfs/Devel/party
git add src/gui.c
git commit -m "feat(gui): editable model state, geometry capture, listview selection, button enable/disable"
```

- [ ] **Step 6: FS-UAE check (human)** — run the GUI; confirm: selecting the RDB disk enables Delete/Edit only after you click a partition row; Save is disabled (nothing dirty); Init Disk is enabled; the blank disk shows Init enabled, New/Save disabled. Close exits cleanly.

---

## Milestone 3b.3 — Confirm requester + Save (write + verify) + Init Disk

### Task 3b.3.1: EasyRequest helpers + Save + Init

**Files:** Modify `src/gui.c`

- [ ] **Step 1: Add the requester helpers**

Add near the top of `src/gui.c` (after includes): `#include <proto/intuition.h>` is present. Add helpers ABOVE `gui_run`:
```c
/* Yes/No confirm: returns 1 if the user chose the action (left) button. The
   safe option (Cancel) is rightmost so EasyRequest returns 0 for it. */
static int gui_confirm(const char *title, const char *body)
{
    struct EasyStruct es;
    es.es_StructSize  = sizeof(es);
    es.es_Flags       = 0;
    es.es_Title       = (UBYTE *)title;
    es.es_TextFormat  = (UBYTE *)body;
    es.es_GadgetFormat= (UBYTE *)"Proceed|Cancel";
    return (int)EasyRequest(g_win, &es, 0) == 1;
}

/* Simple info message (single OK gadget). */
static void gui_msg(const char *title, const char *body)
{
    struct EasyStruct es;
    es.es_StructSize  = sizeof(es);
    es.es_Flags       = 0;
    es.es_Title       = (UBYTE *)title;
    es.es_TextFormat  = (UBYTE *)body;
    es.es_GadgetFormat= (UBYTE *)"OK";
    EasyRequest(g_win, &es, 0);
}
```
Note: `es_TextFormat` is a printf-style string processed by exec RawDoFmt; pass plain text with no `%` (or escape). We pass fixed strings that include the device name pre-formatted into a buffer where needed (see Save).

- [ ] **Step 2: Add Save (write + read-back verify)**

Add ABOVE `gui_run`:
```c
static char g_msgbuf[120];

static int gui_save(void)
{
    DeviceHandle *h;
    static RdbModel chk;          /* static: keep off the stack */
    int ok = 0, p = 0;

    if (!g_have_model) return 0;
    if (rdb_validate(&g_model) != RDB_OK) { gui_msg("Save", "Partition layout is invalid."); return 0; }

    /* Build a confirm message naming the device. */
    s_cat(g_msgbuf, &p, "Write the partition table to\n");
    { int k; for (k = 0; g_cur_driver[k] && p < 100; k++) g_msgbuf[p++] = g_cur_driver[k]; }
    s_cat(g_msgbuf, &p, " unit ");
    p += u2s(g_msgbuf + p, g_cur_unit);
    s_cat(g_msgbuf, &p, " ?\nThis overwrites the disk's RDB.");
    g_msgbuf[p] = 0;
    if (!gui_confirm("Save", g_msgbuf)) return 0;

    h = dev_open(g_cur_driver, g_cur_unit);
    if (!h) { gui_msg("Save", "Could not open the device."); return 0; }
    if (rdb_serialize(&g_model, dev_block_io, h) == RDB_OK) {
        /* read back and verify the partition count + first/last cylinders */
        if (rdb_parse(&chk, dev_block_io, h) == RDB_OK &&
            chk.num_parts == g_model.num_parts) {
            int i; ok = 1;
            for (i = 0; i < chk.num_parts; i++)
                if (chk.parts[i].low_cyl  != g_model.parts[i].low_cyl ||
                    chk.parts[i].high_cyl != g_model.parts[i].high_cyl) { ok = 0; break; }
        }
    }
    dev_close(h);

    if (ok) { g_dirty = 0; gui_msg("Save", "Saved and verified."); }
    else      gui_msg("Save", "WRITE FAILED or verify mismatch.\nThe disk RDB may be inconsistent.");
    gui_refresh_parts();
    gui_update_buttons();
    return ok;
}
```

- [ ] **Step 3: Add Init Disk**

```c
static void gui_init_disk(void)
{
    if (!g_geo.has_media || g_geo.cylinders == 0) {
        gui_msg("Init Disk", "No media / geometry on this device."); return;
    }
    if (!gui_confirm("Init Disk",
        "Replace the in-memory partition table with a\nfresh empty one for this disk?\n"
        "(Nothing is written until you press Save.)"))
        return;
    rdb_init_model(&g_model, g_geo.cylinders, g_geo.heads, g_geo.sectors);
    g_have_model = 1;
    g_dirty = 1;
    g_sel_part = -1;
    gui_refresh_parts();
    gui_update_buttons();
}
```

- [ ] **Step 4: Wire Save + Init into the event loop**

In `gui_run`'s `IDCMP_GADGETUP` case, add:
```c
                    else if (gad->GadgetID == GID_SAVE) gui_save();
                    else if (gad->GadgetID == GID_INIT) gui_init_disk();
```

- [ ] **Step 5: Build + host tests + stage + commit**

```bash
export AMIGA_BIN="/Users/sfs/.vscode/extensions/bartmanabyss.amiga-debug-1.8.2/bin/darwin"
export PATH="$AMIGA_BIN/opt/bin:$AMIGA_BIN:$PATH"
cd /Users/sfs/Devel/party && make 2>&1 | grep -iE "entry-order|undefined|error" && ./tests/run-host-tests.sh 2>&1 | tail -1 && make hd 2>&1 | tail -1
git add src/gui.c
git commit -m "feat(gui): EasyRequest confirms; Save (write RDB + read-back verify); Init Disk"
```

- [ ] **Step 6: FS-UAE check (human)** — on the **scratch_rdb** disk: select it (Save disabled), press **Init Disk** → confirm → the list empties and status shows MODIFIED, Save enabled. Press **Save** → confirm → "Saved and verified". On the host: `rdbtool amiga_scratch/scratch_rdb.hdf info` now shows an empty RDB (0 partitions). (This proves the write+verify path end to end. New/Delete/Edit come next to repopulate.)

---

## Milestone 3b.4 — Delete selected partition

### Task 3b.4.1: gui_delete

**Files:** Modify `src/gui.c`

- [ ] **Step 1: Add gui_delete**

```c
static void gui_delete(void)
{
    if (g_sel_part < 0 || g_sel_part >= g_model.num_parts) return;
    if (!gui_confirm("Delete Partition",
        "Remove the selected partition from the\nin-memory table? (Save to apply.)"))
        return;
    rdb_delete_partition(&g_model, g_sel_part);
    g_sel_part = -1;
    g_dirty = 1;
    gui_refresh_parts();
    gui_update_buttons();
}
```

- [ ] **Step 2: Wire into the event loop** — add to `IDCMP_GADGETUP`:
```c
                    else if (gad->GadgetID == GID_DELETE) gui_delete();
```

- [ ] **Step 3: Build + host tests + stage + commit**

```bash
export AMIGA_BIN="/Users/sfs/.vscode/extensions/bartmanabyss.amiga-debug-1.8.2/bin/darwin"
export PATH="$AMIGA_BIN/opt/bin:$AMIGA_BIN:$PATH"
cd /Users/sfs/Devel/party && make 2>&1 | grep -iE "entry-order|undefined|error" && ./tests/run-host-tests.sh 2>&1 | tail -1 && make hd 2>&1 | tail -1
git add src/gui.c && git commit -m "feat(gui): delete selected partition (in-memory, dirty)"
```

- [ ] **Step 4: FS-UAE check (human)** — select the RDB disk, click DH5, press **Delete** → confirm → DH5 disappears, status MODIFIED, Save enabled. (Don't Save unless you want to.)

---

## Milestone 3b.5 — Partition editor dialog + Edit

The editor is a modal GadTools window on the same screen. It returns 1 if the user pressed Ok (and the model was updated), 0 on Cancel.

### Task 3b.5.1: gui_edit_dialog

**Files:** Modify `src/gui.c`

- [ ] **Step 1: Add the dialog**

Add ABOVE `gui_run` (uses g_vi/g_scr from the main window's screen):
```c
/* Filesystem cycle choices (phase 1: FFS Intl only label; dos types parallel). */
static const char *const kFsLabels[] = { "FFS International (DOS\\3)", 0 };
static const uint32_t     kFsTypes[]  = { RDB_DOSTYPE_FFS_INTL };

/* Edit partition `index` of g_model via a modal dialog. Returns 1 if applied. */
static int gui_edit_dialog(int index)
{
    struct Window *dw;
    struct Gadget *dglist = 0, *g;
    struct Gadget *gName = 0, *gSize = 0, *gSlide = 0;
    struct NewGadget ng;
    RdbPartition *pt = &g_model.parts[index];
    static char nameBuf[32];
    uint32_t startCyl = pt->low_cyl;
    uint32_t maxEndExclusive;   /* first cylinder not available to this part */
    uint32_t maxCyls, maxMB, curMB;
    int done = 0, applied = 0;
    int dt = g_topb, dl = g_leftb;
    int i, k;

    /* compute the space available to this partition (start..next part start-1 or hi_cyl) */
    maxEndExclusive = g_model.hi_cyl + 1;
    for (i = 0; i < g_model.num_parts; i++) {
        if (i == index) continue;
        if (g_model.parts[i].low_cyl >= startCyl &&
            g_model.parts[i].low_cyl < maxEndExclusive)
            maxEndExclusive = g_model.parts[i].low_cyl;
    }
    maxCyls = (maxEndExclusive > startCyl) ? (maxEndExclusive - startCyl) : 1;
    maxMB   = rdb_cyls_to_mb(maxCyls, g_model.cyl_blocks, g_model.block_bytes);
    if (maxMB < 1) maxMB = 1;
    curMB   = rdb_cyls_to_mb(pt->high_cyl - pt->low_cyl + 1, g_model.cyl_blocks, g_model.block_bytes);
    if (curMB < 1) curMB = 1;
    if (curMB > maxMB) curMB = maxMB;

    for (k = 0; k < 31 && pt->name[k]; k++) nameBuf[k] = pt->name[k];
    nameBuf[k] = 0;

    g = CreateContext(&dglist);
    if (!g) return 0;
    ng.ng_TextAttr = &g_font; ng.ng_VisualInfo = g_vi; ng.ng_Flags = 0;

    ng.ng_LeftEdge = dl + 90; ng.ng_TopEdge = dt + 6; ng.ng_Width = 180; ng.ng_Height = 14;
    ng.ng_GadgetText = (UBYTE *)"Name"; ng.ng_GadgetID = 1;
    g = CreateGadget(STRING_KIND, g, &ng, GTST_String, (ULONG)nameBuf, GTST_MaxChars, 31, TAG_END);
    gName = g;

    ng.ng_TopEdge = dt + 24; ng.ng_Width = 80;
    ng.ng_GadgetText = (UBYTE *)"Size (MB)"; ng.ng_GadgetID = 2;
    g = CreateGadget(INTEGER_KIND, g, &ng, GTIN_Number, curMB, GTIN_MaxChars, 7, TAG_END);
    gSize = g;

    ng.ng_LeftEdge = dl + 90; ng.ng_TopEdge = dt + 42; ng.ng_Width = 180; ng.ng_Height = 12;
    ng.ng_GadgetText = (UBYTE *)""; ng.ng_GadgetID = 3;
    g = CreateGadget(SLIDER_KIND, g, &ng, GTSL_Min, 1, GTSL_Max, (ULONG)maxMB,
                     GTSL_Level, (ULONG)curMB, TAG_END);
    gSlide = g;

    ng.ng_TopEdge = dt + 58; ng.ng_Width = 180; ng.ng_Height = 14;
    ng.ng_GadgetText = (UBYTE *)"FS"; ng.ng_GadgetID = 4;
    g = CreateGadget(CYCLE_KIND, g, &ng, GTCY_Labels, (ULONG)kFsLabels, GTCY_Active, 0, TAG_END);

    ng.ng_LeftEdge = dl + 10; ng.ng_TopEdge = dt + 80; ng.ng_Width = 70; ng.ng_Height = 14;
    ng.ng_GadgetText = (UBYTE *)"Ok"; ng.ng_GadgetID = 10;
    g = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);
    ng.ng_LeftEdge = dl + 200; ng.ng_GadgetText = (UBYTE *)"Cancel"; ng.ng_GadgetID = 11;
    g = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);
    if (!g) { FreeGadgets(dglist); return 0; }

    dw = OpenWindowTags(0,
        WA_Left, 120, WA_Top, 60,
        WA_Width, dl + 300 + g_scr->WBorRight, WA_Height, dt + 102 + g_scr->WBorBottom,
        WA_Title, (ULONG)"Edit Partition",
        WA_Gadgets, (ULONG)dglist,
        WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_REFRESHWINDOW | BUTTONIDCMP | STRINGIDCMP | INTEGERIDCMP | SLIDERIDCMP,
        WA_Flags, WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_ACTIVATE | WFLG_SMART_REFRESH,
        g_pub ? WA_PubScreen : WA_CustomScreen, (ULONG)g_scr,
        TAG_END);
    if (!dw) { FreeGadgets(dglist); return 0; }
    GT_RefreshWindow(dw, 0);

    while (!done) {
        struct IntuiMessage *im;
        WaitPort(dw->UserPort);
        while ((im = GT_GetIMsg(dw->UserPort)) != 0) {
            ULONG cl = im->Class; UWORD cd = im->Code;
            struct Gadget *ig = (struct Gadget *)im->IAddress;
            GT_ReplyIMsg(im);
            if (cl == IDCMP_CLOSEWINDOW) { done = 1; }
            else if (cl == IDCMP_REFRESHWINDOW) { GT_BeginRefresh(dw); GT_EndRefresh(dw, TRUE); }
            else if (cl == IDCMP_MOUSEMOVE || cl == IDCMP_GADGETDOWN || cl == IDCMP_GADGETUP) {
                if (ig == gSlide) {
                    /* slider level arrives in Code; mirror to the integer field */
                    GT_SetGadgetAttrs(gSize, dw, 0, GTIN_Number, (ULONG)cd, TAG_END);
                } else if (cl == IDCMP_GADGETUP && ig == gSize) {
                    /* integer changed; clamp and mirror to the slider */
                    LONG v = ((struct StringInfo *)gSize->SpecialInfo)->LongInt;
                    if (v < 1) v = 1; if ((ULONG)v > maxMB) v = (LONG)maxMB;
                    GT_SetGadgetAttrs(gSize, dw, 0, GTIN_Number, (ULONG)v, TAG_END);
                    GT_SetGadgetAttrs(gSlide, dw, 0, GTSL_Level, (ULONG)v, TAG_END);
                } else if (cl == IDCMP_GADGETUP && ig->GadgetID == 10) {        /* Ok */
                    LONG mb = ((struct StringInfo *)gSize->SpecialInfo)->LongInt;
                    char *nm = (char *)((struct StringInfo *)gName->SpecialInfo)->Buffer;
                    if (mb < 1) mb = 1; if ((ULONG)mb > maxMB) mb = (LONG)maxMB;
                    if (rdb_set_partition(&g_model, index, nm, (uint32_t)mb, kFsTypes[0]) == RDB_OK) {
                        applied = 1; done = 1;
                    } else {
                        gui_msg("Edit", "Invalid name or size (overlaps / out of range).");
                    }
                } else if (cl == IDCMP_GADGETUP && ig->GadgetID == 11) { done = 1; } /* Cancel */
            }
        }
    }

    CloseWindow(dw);
    FreeGadgets(dglist);
    if (applied) g_dirty = 1;
    return applied;
}
```

- [ ] **Step 2: Wire Edit into the event loop**

In `gui_run`'s `IDCMP_GADGETUP` add:
```c
                    else if (gad->GadgetID == GID_EDIT) {
                        if (g_sel_part >= 0 && g_sel_part < g_model.num_parts) {
                            gui_edit_dialog(g_sel_part);
                            gui_refresh_parts();
                            gui_update_buttons();
                        }
                    }
```

- [ ] **Step 3: Build + host tests + stage + commit**

```bash
export AMIGA_BIN="/Users/sfs/.vscode/extensions/bartmanabyss.amiga-debug-1.8.2/bin/darwin"
export PATH="$AMIGA_BIN/opt/bin:$AMIGA_BIN:$PATH"
cd /Users/sfs/Devel/party && make 2>&1 | grep -iE "entry-order|undefined|error" && ./tests/run-host-tests.sh 2>&1 | tail -1 && make hd 2>&1 | tail -1
git add src/gui.c && git commit -m "feat(gui): modal partition editor dialog (name/MB/slider/FS) + Edit"
```

- [ ] **Step 4: FS-UAE check (human)** — select RDB disk, click DH4, press **Edit**: a dialog opens; dragging the slider updates the MB field and vice-versa; change the name, press **Ok** → the row updates, status MODIFIED. Cancel leaves it unchanged. Bad name (duplicate) shows an error and keeps the dialog logic sane.

---

## Milestone 3b.6 — New partition

### Task 3b.6.1: gui_new (auto-fill largest gap → editor)

**Files:** Modify `src/gui.c`

- [ ] **Step 1: Add an auto-namer + gui_new**

```c
/* Pick the lowest unused "DH<n>" name into out[32]. */
static void gui_auto_name(char *out)
{
    int n, i, used, p;
    for (n = 0; n < 100; n++) {
        used = 0;
        for (i = 0; i < g_model.num_parts; i++) {
            const char *nm = g_model.parts[i].name;
            /* compare nm == "DH<n>" */
            char cand[8]; int cp = 0;
            cand[cp++]='D'; cand[cp++]='H'; cp += u2s(cand + cp, (ULONG)n); cand[cp]=0;
            { int j=0; while (cand[j] && nm[j] && cand[j]==nm[j]) j++;
              if (cand[j]==0 && nm[j]==0) { used = 1; break; } }
        }
        if (!used) break;
    }
    p = 0; out[p++]='D'; out[p++]='H'; p += u2s(out + p, (ULONG)n); out[p]=0;
}

static void gui_new(void)
{
    uint32_t gs = 0, ge = 0, mb;
    char name[32];
    int idx;
    if (!g_have_model) return;
    if (!rdb_largest_free_gap(&g_model, &gs, &ge)) {
        gui_msg("New Partition", "No free space on this disk."); return;
    }
    gui_auto_name(name);
    mb = rdb_cyls_to_mb(ge - gs + 1, g_model.cyl_blocks, g_model.block_bytes);
    if (mb < 1) mb = 1;
    idx = rdb_add_partition_at(&g_model, name, gs, mb, RDB_DOSTYPE_FFS_INTL);
    if (idx < 0) { gui_msg("New Partition", "Could not add the partition."); return; }
    g_sel_part = idx;
    g_dirty = 1;
    gui_refresh_parts();
    gui_edit_dialog(idx);            /* let the user adjust size/name immediately */
    gui_refresh_parts();
    gui_update_buttons();
}
```

- [ ] **Step 2: Wire New into the event loop** — add to `IDCMP_GADGETUP`:
```c
                    else if (gad->GadgetID == GID_NEW) gui_new();
```

- [ ] **Step 3: Build + host tests + stage + commit**

```bash
export AMIGA_BIN="/Users/sfs/.vscode/extensions/bartmanabyss.amiga-debug-1.8.2/bin/darwin"
export PATH="$AMIGA_BIN/opt/bin:$AMIGA_BIN:$PATH"
cd /Users/sfs/Devel/party && make 2>&1 | grep -iE "entry-order|undefined|error" && ./tests/run-host-tests.sh 2>&1 | tail -1 && make hd 2>&1 | tail -1
git add src/gui.c && git commit -m "feat(gui): New partition — auto-fill largest gap then open editor"
```

---

## Milestone 3b.7 — Full FS-UAE end-to-end + rdbtool verification (human gate)

### Task 3b.7.1: Create a partition table from scratch and verify

- [ ] **Step 1: Build + stage + fresh fixtures**

```bash
export AMIGA_BIN="/Users/sfs/.vscode/extensions/bartmanabyss.amiga-debug-1.8.2/bin/darwin"
export PATH="$AMIGA_BIN/opt/bin:$AMIGA_BIN:$PATH"
cd /Users/sfs/Devel/party && make hd && ./tools/make-scratch-hdfs.sh
```

- [ ] **Step 2: End-to-end in FS-UAE (human)** — in **HDPart-204-devtest**, run `HDPart:HDPart`:
  1. Select the **blank** scratch disk → press **Init Disk** → confirm. Status shows MODIFIED, 0 partitions.
  2. Press **New** → editor opens proposing DH0 at full size → set size (e.g. 2 MB) via slider/field → Ok. Row appears.
  3. Press **New** again → DH1 fills the remaining gap → Ok.
  4. Press **Save** → confirm → "Saved and verified".
  5. Close HDPart.

- [ ] **Step 3: Verify on the host**

```bash
cd /Users/sfs/Devel/party && rdbtool amiga_scratch/scratch_blank.hdf info
```
Expected: a valid RDB with **DH0** and **DH1** at the cylinder ranges you chose, no checksum errors. This proves the full create→edit→save→verify path writes a byte-correct RDB to a real device.

- [ ] **Step 4: Edit round-trip (human)** — re-run HDPart, select that disk (now shows DH0/DH1 as RDB), Edit DH0's size, Save, and re-check with `rdbtool` that the change persisted.

---

## Definition of done (Plan 3b)

- [ ] Engine edit helpers (`rdb_largest_free_gap`, `rdb_add_partition_at`, `rdb_set_partition`) host-tested and pass.
- [ ] Buttons enable/disable correctly by state; listview selection tracked.
- [ ] Init Disk creates a fresh in-memory RDB; New auto-fills the largest gap then opens the editor; Edit resizes/renames via the slider+field dialog; Delete removes a partition — all in memory, marking dirty.
- [ ] Save confirms, writes the RDB via `dev_block_io`, reads it back, and verifies; nothing is written anywhere else.
- [ ] FS-UAE: a partition table created from scratch on the blank scratch disk is confirmed byte-correct by host `rdbtool`.
- [ ] `HDPart scan` CLI still works; host tests pass; `entry-order OK`.

## Self-review notes (author)

- **Spec coverage:** Completes spec §3.3 (edit dialog: name, MB+slider, FS) and §6 safety (confirm + read-back verify; single write point at Save; in-memory editing). Init/New/Delete/Edit/Save all implemented. Bootable/boot-priority and selectable FS types beyond FFS-Intl remain future (the FS cycle has one entry now but is structured for more). Expansion-bootnode discovery, DEVS:-file drivers, >4GB/TD64, localization remain on the spec roadmap.
- **Type consistency:** new rdb helpers use `(RdbModel*, ... uint32_t ...)` consistent with existing engine; `rdb_set_partition`/`rdb_add_partition_at` return `int` (index or negative RDB_ERR_*); GUI reads gadget values via `struct StringInfo` (`Buffer`/`LongInt`) and slider level via message `Code`; `gui_refresh_parts`/`gui_update_buttons`/`gui_confirm`/`gui_msg`/`gui_save`/`gui_init_disk`/`gui_delete`/`gui_edit_dialog`/`gui_new`/`gui_auto_name` names used consistently across definitions and event-loop call sites.
- **No placeholders:** every code step is complete. `EasyRequest` text strings are fixed (no `%`); the Save confirm builds the device name into `g_msgbuf` with the existing `u2s`/`s_cat` helpers rather than relying on `RawDoFmt` formatting.
- **Known risks (verify in emulator):** the modal dialog's slider↔integer sync relies on the slider level arriving in `Code` and reading the integer's `StringInfo->LongInt` (both V37-valid); dialog gadget geometry is best-effort and may need a nudge; `rdb_set_partition` keeps `low_cyl` fixed (resize moves only the end) — moving a partition's start is out of scope for 3b.
```
