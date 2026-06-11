# HDPart — Partition Resize (grow/shrink either edge)

**Date:** 2026-06-11
**Status:** Approved design, pending implementation plan
**Component:** `src/rdb.{c,h}` (engine), `src/gui.c` (dialog + launch button), `tests/` (host tests)

## Summary

Add an explicit **Resize** feature to HDPart: grow or shrink the selected
partition into the free space adjacent to it, on either the Start or the End
edge. The work is split into a small geometry-only engine function
(`rdb_resize_cyl`) and a dedicated GadTools dialog (`gui_resize_dialog`) reached
from a new selection-scoped **Resize…** button.

Resize is a **table edit only** — it rewrites the partition's cylinder range in
the RDB and does not touch the filesystem inside. The user must reformat the
partition afterward to use the new extent, and any data in cylinders that are
removed (shrink) or relocated (Start-edge move) is lost. This is the same
contract HDToolBox used: partition, then format. Data-preserving resize
(filesystem bitmap/root-block surgery) is explicitly out of scope.

## Background / current state

The engine already does a constrained form of resize:

- `rdb_set_partition(m, index, name, size_mb, dos_type)` (src/rdb.c) resizes by
  keeping `low_cyl` fixed and recomputing `high_cyl`, then runs `rdb_validate`
  (bounds / overlap / range / duplicate-name) and rolls back on error.
- The Edit dialog (src/gui.c) exposes a **Size (MB)** field that calls
  `rdb_set_partition` only when the size changed, clamped to the gap that
  immediately follows the partition (`maxEndExclusive` = next partition's
  `low_cyl`, or `hi_cyl + 1`).

So **grow-right and shrink-right already work** from Edit, but only on the End
edge, and without an explicit data-loss confirmation. This feature generalizes
that to both edges in a purpose-built dialog and adds the safety prompt.

`rdb_validate` (src/rdb.c) is the single source of truth for table legality:
in-bounds (`lo_cyl..hi_cyl`), `low_cyl <= high_cyl`, non-empty name, no
duplicate names, no overlaps. Any new resize path relies on it.

## Engine

New geometry-only function, declared in `src/rdb.h`, implemented in `src/rdb.c`:

```c
/* Resize partition `index` to the cylinder range [low, high], keeping every
   other partition fixed. Validates the whole table (bounds / overlap / range /
   duplicate-name) via rdb_validate and rolls back to the prior extent on any
   error. Name, dos_type and all flag fields are left untouched.
   Returns RDB_OK, or RDB_ERR_RANGE (index out of range, or low > high),
   or whatever rdb_validate reports (RDB_ERR_NO_SPACE / RDB_ERR_OVERLAP). */
int rdb_resize_cyl(RdbModel *m, int index, uint32_t low, uint32_t high);
```

Implementation mirrors `rdb_set_partition`'s save/restore discipline:

1. Range-check `index`; reject `low > high` with `RDB_ERR_RANGE`.
2. Save `RdbPartition saved = *p`.
3. Set `p->low_cyl = low; p->high_cyl = high;` (only these two fields).
4. `v = rdb_validate(m);` if `v != RDB_OK` restore `*p = saved` and return `v`.
5. Return `RDB_OK`.

`rdb_set_partition` is left unchanged. `rdb_resize_cyl` is the general primitive;
the dialog derives `low`/`high` from the user's *Size + anchor* choice:

- **Move End** (keep Start): `low = p->low_cyl;  high = low + cyls - 1`
- **Move Start** (keep End): `high = p->high_cyl; low = high - cyls + 1`

where `cyls = max(1, rdb_mb_to_cyls(size_mb, ...))`. The Start-edge formula
guards against unsigned underflow (`cyls` is already clamped so `low` cannot go
below the gap floor because the dialog clamps `size_mb` first; the engine still
validates as a backstop).

## GUI — Resize dialog

New `static void gui_resize_dialog(int index)` in `src/gui.c`, modeled on the
existing Split and Edit dialogs (own GadTools window centered over the main
window, GadTools cycle/integer/text gadgets, `g_font`, `g_vi`, `g_pub`/`g_scr`).

Gadget layout (top to bottom):

- **Title bar:** `Resize <name>` (the partition's device name).
- **Context line** (TEXT): `Free before: X MB    Free after: Y MB`, computed
  from the adjacent gaps (see Gap helpers). Read-only, refreshed when nothing
  else changes it — static after open.
- **Anchor** (CYCLE_KIND, `GID`-local): two entries —
  `Move: End edge (keep start)` (default, index 0) and
  `Move: Start edge (keep end)` (index 1).
- **Size (MB)** (INTEGER_KIND): default = current size in MB; `GTIN_MaxChars` 7.
  Live-clamped on GADGETUP to `[1, maxMB]` where `maxMB` depends on the anchored
  edge's adjacent gap (recomputed when the anchor flips).
- **Max hint** (TEXT): `max N MB`, updated when the anchor changes.
- **Live readout** (TEXT): `-> cyls A..B  (S MB)`, recomputed whenever size or
  anchor changes.
- **Caveat line** (TEXT, always shown): `Resize edits the table only - reformat
  to use new space.`
- **Ok** / **Cancel** buttons.

Interaction:

- Changing **Anchor** recomputes `maxMB` for that edge, re-clamps the Size field,
  and updates the max hint + readout.
- Changing **Size** re-clamps to `[1, maxMB]` and updates the readout.
- **Cancel** / close: no change.
- **Ok**: compute `low`/`high` from anchor+size, run the Apply logic below.

### Gap helpers

Factor the gap math (currently inline in the Edit dialog) into two helpers in
`src/gui.c`, used for both the Size clamp and the free-before/after labels:

```c
/* exclusive end of the free run that starts at parts[index].high_cyl+1:
   the next partition's low_cyl, or hi_cyl+1 if none follows. */
static uint32_t gap_end_after(int index);
/* inclusive start of the free run that ends at parts[index].low_cyl-1:
   the previous partition's high_cyl+1, or lo_cyl if none precedes. */
static uint32_t gap_start_before(int index);
```

From these:

- **Move End** max high = `gap_end_after(index) - 1` → `maxCyls = maxHigh - low + 1`.
- **Move Start** min low = `gap_start_before(index)` → `maxCyls = high - minLow + 1`.
- Free after (MB) = cyls in `[high+1, gap_end_after-1]`.
- Free before (MB) = cyls in `[gap_start_before, low-1]`.

"Previous / next" are by cylinder position, not array order — the helpers scan
all partitions for the nearest `low_cyl`/`high_cyl` on each side, the same way
the Edit dialog's `maxEndExclusive` loop does today.

### Apply logic (destructive-only confirm)

On **Ok**, with computed `new_low`, `new_high` and the partition's current
`old_low`, `old_high`:

```
destructive = (new_low != old_low) || (new_high < old_high)
```

- `destructive == false` → a pure End-edge grow into trailing free space
  (start unchanged, end moved outward). Existing file data stays in place; only
  the newly added tail needs a reformat to become usable. **Apply silently.**
- `destructive == true` → shrink (either edge) or any Start-edge move. Show a
  modal `gui_request` Proceed/Cancel:

  > **Resize <name>**
  > Resizing changes the partition's extent.
  > Reformat afterward — existing data will be lost.
  > [ Proceed ]  [ Cancel ]

  Cancel returns to the Resize dialog (no change). Proceed continues.

Then:

1. `r = rdb_resize_cyl(&g_model, index, new_low, new_high);`
2. On `RDB_OK`: `g_dirty = 1`; `gui_refresh_parts()`; `gui_update_buttons()`;
   close the dialog. The disk-map bar and list reflect the new extent
   immediately; **Save** lights up (unsaved edits).
3. On error: `gui_msg("Resize", "...")` describing the failure (overlap / out of
   range), leave the dialog open.

If `new_low == old_low && new_high == old_high` (no change), close without
touching `g_dirty`.

## GUI — launch point

Add a selection-scoped **Resize…** button on the existing under-the-list row
(the row that currently holds **Refresh** at `y = 146`, which is otherwise
empty), e.g. `x = 90 + g_leftb, y = 146 + g_topb, w = 72, h = 14`, id
`GID_RESIZE`. The bottom action row (New / Delete / Edit / Split… / Init Disk /
Save) is full at the 460px content width, so the under-list row is the natural
home and sits directly beneath the list + disk-map bar where partitions live.

- New enum value `GID_RESIZE` in the gadget-ID enum.
- Create it alongside the Refresh button; store in `g_gad[GID_RESIZE]`.
- In `gui_update_buttons`, enable iff a partition is selected
  (`GA_Disabled = !hasSel`), matching Edit/Delete.
- In the main event loop's GADGETUP dispatch:
  `else if (gad->GadgetID == GID_RESIZE) { if (g_sel_part valid) gui_resize_dialog(g_sel_part); }`
  (refresh/buttons handled inside the dialog on apply).

`g_gad[]` is sized `[16]`; `GID_RESIZE` is the next id after `GID_REFRESH` and
fits.

## Edge cases

- **No adjacent free space on the anchored edge:** `maxMB` equals the current
  size; the user can still shrink (Size down to 1 cylinder) but not grow that
  direction. Flipping the anchor lets them target the other side.
- **Single partition / blank-ish disk:** gaps are bounded by `lo_cyl`/`hi_cyl`;
  helpers return those when no neighbor exists.
- **1-cylinder floor:** Size clamps to `>= 1` cylinder; `rdb_resize_cyl`
  rejects `low > high` as a backstop.
- **Unsigned underflow** on Move-Start with an over-large size: prevented by the
  Size clamp to `maxMB`; engine `rdb_validate` is the final guard.
- **Selection cleared / model reloaded** (driver change, Refresh, Scan): the
  button disables via `gui_update_buttons` because `g_sel_part` resets.

## Out of scope (YAGNI)

- **Move** (slide a partition without changing its size) — purely destructive,
  no real benefit here.
- **Drag-to-resize** on the disk-map bar — considered, rejected for cost/precision.
- **Data-preserving resize** — would require filesystem-specific bitmap/root-block
  rewriting (FFS vs PFS3 vs SFS); large, risky, separate project.
- Changing `rdb_set_partition` to be `low_cyl`-aware — left as-is; the Edit
  dialog's Size field keeps its current grow-right/shrink behavior. The new
  dialog is the fuller geometry tool.

## Testing

**Host tests** (`tests/`, system cc, no toolchain) for `rdb_resize_cyl` against a
known model:

- Grow End edge into a trailing gap → `high` increases, `low` unchanged, `RDB_OK`.
- Grow Start edge into a leading gap → `low` decreases, `high` unchanged, `RDB_OK`.
- Shrink End edge → `high` decreases; freed gap appears after.
- Shrink Start edge → `low` increases; freed gap appears before.
- Overlap rejection: resize that would collide with a neighbor → `RDB_ERR_OVERLAP`,
  model **unchanged** (rollback verified field-by-field).
- Bounds rejection: `high > hi_cyl` or `low < lo_cyl` → `RDB_ERR_NO_SPACE`,
  rollback verified.
- `low > high` → `RDB_ERR_RANGE`.
- 1-cylinder partition (min size) round-trips.

No `rdb_serialize` change, so the `rdbtool` byte-for-byte cross-check is
unaffected; a save after resize still validates through the existing path.

**Manual FS-UAE** (`HDPart-204-devtest`, per [[hdpart-project]] workflow; `make`
then `make hd`):

- Resize a partition larger on the End edge into a trailing gap → no confirm,
  applies; list + bar + Save reflect it.
- Shrink it → confirm requester appears; Proceed applies, Cancel doesn't.
- Grow/shrink on the Start edge → confirm appears (Start moved); applies.
- Save, then Refresh → re-read from disk matches the new extent.
- With no partition selected, the Resize… button is ghosted.

## Files touched

- `src/rdb.h` — declare `rdb_resize_cyl`.
- `src/rdb.c` — implement `rdb_resize_cyl`.
- `src/gui.c` — `gui_resize_dialog`, `gap_end_after`/`gap_start_before`,
  `GID_RESIZE` enum + button + enable rule + event dispatch.
- `tests/` — host tests for `rdb_resize_cyl`.
