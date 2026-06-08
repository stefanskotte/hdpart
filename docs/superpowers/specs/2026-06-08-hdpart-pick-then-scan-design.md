# HDPart — Pick-then-Scan device flow (design)

**Date:** 2026-06-08
**Status:** Approved (brainstorming)
**Supersedes idea:** the deferred [[hdpart-lazy-device-pick-idea]] — this implements
the "backup option" (manual Scan) merged with zero-touch startup.

## Problem

Today HDPart probes every curated/extra driver across units 0–7 (geometry + RDB
read) at startup via `discover_disks`. It is slow and touches hardware the user
did not ask about; on a flaky/non-disk device it can even wedge the GUI. The user
wants control: pick the driver they know, then explicitly scan it.

## Goal

Startup touches no hardware. The `Disk:` dropdown lists driver *names* to choose
from (or load from disk); a renamed, bold **Scan** button probes the selected
driver on demand and appends the disks it finds.

## Flow

1. **Launch:** no discovery. Dropdown = `Select or load a driver` (item 0, the
   active selection) followed by the candidate driver names (curated list +
   any drivers loaded from disk). Partition list empty. Status line shows
   `Select a driver, or load one from disk, then press Scan.` `Scan` is disabled.
2. **Select a driver name:** records it as the current target. No probe. Clears
   the partition view. Enables `Scan`.
3. **`Driver...` (load from disk):** load the `.device` (unchanged loader), then
   `disc_add_extra_driver(name)`, rebuild the dropdown (the new name now appears
   as a driver entry), and select that driver entry. **No probe** (changed: it
   used to probe + preselect a disk).
4. **`Scan`:** probe the selected driver's units 0–7 (`discover_probe_driver`),
   **append** the found disks to the dropdown as `driver uN  sizeM` entries,
   auto-select the first partitionable one and show its partitions. Status reports
   the count, or `No disk found on <driver>` when none. `Scan` stays enabled while
   a driver-or-disk entry is selected (re-scans that driver).
5. **Select an already-found disk entry:** shows its partitions (reads its RDB);
   no re-probe of the whole driver.

## Dropdown model (GUI — `gui.c`)

Cycle entries become **typed**. Replace the single `g_devmap[]` (cycle→disk) with
two parallel arrays sized `DISC_MAX*2 + 1`:

```c
enum { ENT_PROMPT, ENT_DRIVER, ENT_DISK };
static int  g_entkind[DISC_MAX*2 + 1];  /* ENT_* per cycle index */
static int  g_entref [DISC_MAX*2 + 1];  /* ENT_DRIVER: candidate-name idx;
                                           ENT_DISK: g_disks[] index; ENT_PROMPT: -1 */
```

`gui_rebuild_devlabels()` rebuilds, in order:
- index 0: `ENT_PROMPT`, label `"Select or load a driver"`.
- one `ENT_DRIVER` per candidate name (`disc_candidate_drivers`), label = the
  driver name.
- one `ENT_DISK` per partitionable `g_disks[i]`, label = `"<driver> u<unit>  <mb>M"`
  (the existing disk-label format).

`g_devtext[]` capacity: bump to `DISC_MAX*2 + 1` rows so names + disks both fit;
keep the existing 48-byte row width. `g_devlabels[]` likewise `DISC_MAX*2 + 2`.

### Current-target tracking

A selection sets the "target driver" that `Scan` acts on:
- `ENT_PROMPT` → target `""` (Scan disabled), partition view cleared.
- `ENT_DRIVER` → target = that name, partition view cleared (no probe).
- `ENT_DISK` → target = that disk's driver; open the disk + `rdb_parse` into
  `g_model` and show its partitions (this is `gui_select_device`'s existing body).

`gui_select_device(int idx)` is replaced by `gui_select_entry(int cycleIdx)` that
switches on `g_entkind[cycleIdx]`. The disk case reuses today's open+geometry+
`rdb_parse` logic; the prompt/driver cases just reset model state
(`g_have_model=0`, `g_sel_part=-1`, clear `g_geo`) and refresh.

## Discovery accessor (`discover.c` / `discover.h`)

Move the `kProbeDrivers[]` table **out of** `#ifdef HDPART_AMIGA` (it is a plain
`static const char *const[]`, no OS dependency) so the accessor is host-testable.
The probe loops that use it stay under the guard.

```c
/* Fill out[] with candidate driver names: the curated probe list followed by
   user-loaded extras (deduped against the curated names). Returns the count
   (<= max). Pointers reference static storage; do not free. Plain C. */
int disc_candidate_drivers(const char *out[], int max);
```

`disc_candidate_drivers` is host-testable and does not touch hardware. It is the
only new discovery API; `disc_add_extra_driver`, `disc_extra_count`,
`discover_probe_driver` are unchanged and reused.

## Buttons (`gui.c`)

- Rename gadget id `GID_RESCAN` → `GID_SCAN`; label `"Rescan"` → `"Scan"`.
- Render bold: add `static struct TextAttr g_font_bold = { (STRPTR)"topaz.font",
  8, FSF_BOLD, 0 };` (needs `graphics/text.h` for `FSF_BOLD`) and set the Scan
  gadget's `ng_TextAttr = &g_font_bold`. Other gadgets keep `g_font`.
- `Scan` width may need a couple px more for the bold label; keep within the
  existing row (cycle 236 @ 70, `Driver...` 84 @ 312, `Scan` @ 400). Bold "Scan"
  (4 chars) still fits 56px.
- `gui_update_buttons`: `Scan` `GA_Disabled` when the target driver is `""`
  (prompt selected); enabled otherwise.

## Startup (`gui.c gui_run`)

Replace the startup `gui_rescan()` call with a no-probe initialiser: build the
dropdown from `disc_candidate_drivers` + the prompt, select index 0, set the
guidance status, leave `g_ndisks = 0`. `gui_rescan()` (full probe-everything) is
removed from the normal flow; its label-building is already factored into
`gui_rebuild_devlabels`. (No remaining caller needs the old eager
`discover_disks` path; it stays in `discover.c` for potential future use but is
no longer invoked by the GUI.)

## Event loop (`gui.c`)

- `GID_DEVICE` GADGETUP → `gui_select_entry((int)code)`.
- `GID_SCAN` GADGETUP → `gui_scan_selected()` (probe target driver, append, select
  first disk).
- `GID_DRIVER` → load + add name + select the new driver entry (no probe).

## Testing

- **Host** (`tests/test_discover.c`): `disc_candidate_drivers` returns the curated
  names; after `disc_add_extra_driver("lide.device")` (a curated dup) the count is
  unchanged (deduped); after adding a novel `"foo.device"` it appears exactly once
  and last. Verify `out`-capacity clamping.
- **Target build:** clean `make` (no warnings), including the moved `kProbeDrivers`
  and bold `TextAttr`.
- **On-target (FS-UAE):** launch → dropdown shows the prompt + driver names, no
  scan occurs, partition list empty; pick a driver → Scan enabled, view clears;
  press `Scan` → its disk(s) appear and the first is shown; `Driver...` a file →
  appears as a driver entry, selecting + Scan probes it; selecting a found disk
  shows partitions without re-probing.

## Risks / notes

| Risk | Handling |
|------|----------|
| Prompt text clips in the narrow cycle | Use the short `"Select or load a driver"`; full guidance in the status line. |
| Mixed entry kinds confuse the map/selection | `g_entkind`/`g_entref` make the kind explicit; bar/list only render for an `ENT_DISK` selection (model present). |
| Bold font unavailable | `FSF_BOLD` is algorithmic in graphics.library; always renders. |
| Old `discover_disks` left unused | Kept (no caller) rather than deleted, to avoid churn; noted here. |
| `g_devtext`/label array sizing | Bumped to `DISC_MAX*2 (+1/+2)` to hold names + disks. |
