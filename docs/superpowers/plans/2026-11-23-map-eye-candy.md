# Disk map eye candy: implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Bevelled partition slices, box-less shadowed/outlined labels, a hatched reserved RDB area and per-unit mini-maps, on the main window and the Quick setup map.

**Architecture:** The engine (`src/rdb.c`) gains a layout reader used by Scan. `DiscDisk` carries the layout, and the GUI draws it with the same pattern helpers as the big map. Every visual is a JAM1 line-pattern overlay with the four existing pens.

**Tech Stack:** C (m68k-amiga-elf GCC, Bartman toolchain), GadTools/graphics.library, host tests (`tests/run-host-tests.sh`), WinUAE suites (`make target-test`, `make target-quick`).

**Spec:** `docs/superpowers/specs/2026-11-23-map-eye-candy-design.md`. The approved trial is commit "spike: map eye candy" on branch `spike/map-gradient`; reuse its drawing code without the `SPIKE` markers.

## Global Constraints

- Four pens only (`g_pen_bg`, `g_pen_text`, `g_pen_shine`, `g_pen_fill`). No palette changes, no `ObtainBestPen`. Must work on KS 2.04.
- Line patterns (`LinePtrn`) only. No area patterns or anything in chip memory.
- Bevel: Bayer 4x4 `{0,8,2,10},{12,4,14,6},{3,11,1,9},{15,7,13,5}`, screen-x aligned; top rows white at levels 11,7,4,2,1; bottom rows black at levels 8,5,3,1; only when slice w > 2 and h >= 14.
- Labels: white; drop shadow at +1,+1 on `FILL_DOS`; 8-neighbour black outline on every other fill kind; lines at y+8 and y+18, x+5.
- RDB area: hatch when width >= 6 (black, `((x + i + y + r) & 7) == 0`), recessed bevel; "RDB" plate (bg box 30x12, black text) only when `rdb_label` and width >= 34.
- Mini-map: recessed box at row y+11, 9 high; contents at y+12, 7 high; slice top row white level 7, bottom row black level 6; 1-pixel bg gap after slices wider than 2. No RDB: plain shine bar.
- Unit row height stays `UNIT_ROW_H` 21.

## Review Focus

- A disk with more than 16 partitions: the mini-map shows the first 16 and nothing overflows. Covered by a host test in Task 1.
- A huge disk (cylinder count above 4,000,000): permille math must not overflow. Covered by a host test in Task 1.
- Save, then switch unit, then come back: the saved layout must be kept, and discarded later edits must not leak into it. Covered by the target step in Task 3.
- A blank disk (no RDB) that gets its first Save: its bar must turn into a mini-map without a rescan. Covered in Task 3 (diskA starts blank).
- PFS label readability: a permanent `map-pfs.png` screenshot in Task 3.

---

### Task 1: Layout reader in the engine

**Files:**
- Modify: `src/rdb.h`, `src/rdb.c` (beside `rdb_used_percent`)
- Test: `tests/test_rdb.c` (`test_used_percent` and a new `test_used_layout`)

**Interfaces:**
- Produces: `#define RDB_LAYOUT_MAX 16`; `typedef struct { uint16_t lo, hi; uint32_t dos_type; } RdbLayoutSeg;` (positions in 1/1000 of `Cylinders`, `hi` exclusive); `typedef struct { uint16_t rdb_end; uint8_t nseg; RdbLayoutSeg seg[RDB_LAYOUT_MAX]; } RdbLayout;`; `uint16_t rdb_permille(uint32_t cyl, uint32_t cylinders);` (0 for 0 cylinders, clamped to 1000, divides first above 4,000,000); `int rdb_used_layout(BlockIO io, void *ctx, uint32_t *pct, RdbLayout *lay);` (`lay` may be NULL). `rdb_used_percent` becomes `rdb_used_layout(io, ctx, pct, 0)`.

- [ ] **Step 1: Write the failing test** `test_used_layout`. Use the same 64x4x32 image as `test_used_percent` with partitions A (first quarter of the span, FFS Intl) and B (second quarter, `0x50465303`). Assert: `RDB_OK`; `lay.nseg == 2`; `lay.rdb_end == rdb_permille(m.lo_cyl, m.cylinders)`; `seg[0].lo == rdb_permille(m.lo_cyl, 64)`, `seg[0].hi == seg[1].lo`, `seg[1].dos_type == 0x50465303`; `pct` equals the existing expectation. No RDB gives `RDB_ERR_NO_RDB` and `nseg == 0`. Add `rdb_permille(0, 0) == 0`, `rdb_permille(64, 64) == 1000`, `rdb_permille(2000000000u, 4000000000u) == 500`. Add an image with 20 tiny partitions: `nseg == 16` and `pct` counts all 20.
- [ ] **Step 2:** `./tests/run-host-tests.sh`. Expected: compile failure on `rdb_used_layout`.
- [ ] **Step 3:** Implement as in the interfaces.
- [ ] **Step 4:** `./tests/run-host-tests.sh`. Expected: `ALL TESTS PASSED`.
- [ ] **Step 5:** Commit `feat(rdb): read the partition layout with the used percentage`.

### Task 2: Map visuals (main map and Quick setup)

**Files:**
- Modify: `src/gui.c` (`gui_pattern` area, `gui_slice_label`, `gui_map_draw`)

**Interfaces:**
- Produces (static, src/gui.c): `gui_dither_row(int x, int row, int w, int level, int pen)`, `gui_shade(int x, int y, int w, int h)`, `gui_hatch(int x, int y, int w, int h)`, `gui_text_fx(int x, int y, const char *s, int outline)`. `gui_slice_label` gains `int kind` (a `FILL_*`).

- [ ] **Step 1:** Port the trial's `gui_dither_row`, `gui_shade`, `gui_text_fx`, `gui_hatch`, the label change and the RDB block (hatch and bevel for any `rw >= 6`, plate only with `rdb_label`). Values are in Global Constraints. Update the `gui_map_draw` comment ("rdb_label says what the blank block ... is called") to say the block is now always hatched and only the plate depends on `rdb_label`.
- [ ] **Step 2:** `make` (with the Bartman PATH), then `make hd`. Expected: no warnings, `entry-order OK`.
- [ ] **Step 3:** Commit `feat(gui): bevelled map slices, shadowed labels, hatched RDB area`.

### Task 3: Units list mini-map, kept right after Save

**Files:**
- Modify: `src/discover.h` (`#include "rdb.h"`, `RdbLayout layout` after `used_pct`, plus `_Static_assert`s `offsetof(DiscDisk, layout) == 60` and `sizeof(DiscDisk) == 232` with a comment that `tests/target/run.mjs` reads these offsets), `src/discover.c` (empty layout where `used_pct = -1`; Scan calls `rdb_used_layout` with `&d->layout`), `src/gui.c` (empty layout in the Quick setup `DiscDisk` filler at `out->used_pct = -1`; `gui_mini_map`; `gui_layout_from_model`; `gui_draw_units`; the Save success branch copies the layout and percentage into `g_disks[g_unitmap[g_cur_unitidx]]` when `g_cur_unitidx >= 0`)
- Modify: `tests/target/run.mjs`

**Interfaces:**
- Consumes: Task 1's `RdbLayout`, `rdb_permille`, `rdb_used_layout`; Task 2's `gui_dither_row`, `gui_hatch`.
- Produces: `static void gui_layout_from_model(RdbLayout *l, const RdbModel *m)`, `static void gui_mini_map(int x, int y, int w, int h, const RdbLayout *l)`.

- [ ] **Step 1: Write the target checks.** Add a helper `async unitLayout(row)` in `run.mjs` that reads `g_unitmap[row]` (int) and then, at `g_disks + idx*232 + 60`, `rdb_end` (u16), `nseg` (u8 at +2) and the segments (8 bytes each from +4: u16 lo, u16 hi, u32 dos_type). In "Save: writes a valid RDB", assert the shown unit's `nseg == 3` and that each segment's `lo`/`hi` equal the host-read partitions' cylinders converted with the same permille rule (diskA was blank at Scan, so this proves the Save copy). In "Unsaved edits: Unit + Proceed", after `pickUnit(' 8 MB')`, assert `nseg` is still 3. Add a permanent `await sleep(1000); await e.shot(`${this.dir}/shots/map-pfs.png`)` at the end of "Format: fresh PFS partition".
- [ ] **Step 2:** Implement the C side as listed under Files, porting the trial's `gui_mini_map` and `gui_layout_from_model`.
- [ ] **Step 3:** `make && make hd && ./tests/run-host-tests.sh && make target-test KS=ks31`. Expected: host `ALL TESTS PASSED`; `KS 3.1 37 passed, 0 failed`.
- [ ] **Step 4:** Commit `feat(gui): Units list shows each disk's layout in miniature`.

### Task 4: Full verification and screenshots

- [ ] **Step 1:** `make target-test` (KS 2.04, 3.1, 3.2). Expected: 37/37 on each.
- [ ] **Step 2:** `make target-quick` with the `QUICK_ROM_*` variables. Expected: every run finished; `used` stays at its known 24 problems, `restart` safe.
- [ ] **Step 3:** Collect full-window screenshots for sharing (main window with FFS+PFS map and mini-maps, many partitions, Quick setup map) into `out/screenshots-0.14/`. Look at each one.
- [ ] **Step 4:** Commit any doc touch-ups (`tests/target/README.md` lists `map-pfs.png` if it lists shots).
