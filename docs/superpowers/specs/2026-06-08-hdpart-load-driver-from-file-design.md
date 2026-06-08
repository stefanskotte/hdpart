# HDPart — Load a Device Driver From File (design)

**Date:** 2026-06-08
**Status:** Approved (brainstorming)
**Roadmap item:** Spec "Device driver sources (later)" — case (b): disk-based
`.device` files in `DEVS:` that are NOT in the exec device list until loaded.

## Problem

`OpenDevice` — and therefore HDPart's whole discovery path
([src/discover.c](../../../src/discover.c)) — can only find drivers that are
already **resident** in exec's device list (ROM/autoconfig drivers, and
controller-ROM-installed ones such as `lide.device`). A `.device` file that
lives on disk (e.g. `DEVS:scsi.device`) and has not been mounted/loaded is
invisible to discovery. Users with such controllers currently cannot reach
their disks in HDPart at all.

## Goal

Let the user pick a `.device` file via a file requester. HDPart loads it into
the running system so `OpenDevice` can reach it, probes that one driver's units,
merges the results into the live disk list, and preselects the first unit of the
new driver that has media (reading its RDB). Targeted — no full rescan.

Non-goals (remain "later"): auto-scanning `DEVS:`/`DEVS:storage`/mountlists for
`.device` files; mountlist parsing; unloading drivers; manual driver/unit entry.

## Mechanism — new module `driver.c` / `driver.h`

OS-bound module that makes an on-disk `.device` openable on a V37 baseline.

### API

```c
/* Find a valid Resident romtag of type NT_DEVICE within [base, base+size).
   Pure (no OS calls) -> host-unit-testable with a synthetic buffer.
   Returns 1 and sets *off_out to the romtag's byte offset, or 0 if none. */
int drv_find_romtag(const unsigned char *base, uint32_t size, uint32_t *off_out);

/* Load a .device file so OpenDevice() can reach it.
   On success, copies the exec device name (romtag rt_Name) into name_out and
   returns 0. On failure returns one of DRVL_* (<0). OS-bound (LoadSeg/exec). */
int driver_load_file(const char *path, char *name_out, int name_sz);

enum {
    DRVL_OK          =  0,
    DRVL_ELOAD       = -1,  /* LoadSeg failed (file missing / not a load file) */
    DRVL_ENOROMTAG   = -2,  /* no Resident structure found */
    DRVL_ENOTDEVICE  = -3,  /* romtag is not NT_DEVICE */
    DRVL_EINIT       = -4   /* InitResident ran but device not in list after */
};
```

### `driver_load_file` algorithm

1. `seglist = LoadSeg(path)` → `DRVL_ELOAD` on 0.
2. Walk the seglist (`next = *(BPTR*)BADDR(seg)`, each segment's usable bytes =
   `*(LONG*)(BADDR(seg)-4) - 8`). For each segment call `drv_find_romtag`.
   - The romtag is validated: `rt_MatchWord == RTC_MATCHWORD` (0x4AFC) **and**
     `rt_MatchTag == (the romtag address itself)`. This self-pointer check is
     what distinguishes a real romtag from a coincidental 0x4AFC in code/data.
   - Of the romtags found, require `rt_Type == NT_DEVICE` → else
     `DRVL_ENOTDEVICE` (still `UnLoadSeg` and bail).
   - No romtag in any segment → `UnLoadSeg`, `DRVL_ENOROMTAG`.
3. **Already-loaded guard:** `Forbid()`, `FindName(&SysBase->DeviceList,
   romtag->rt_Name)`. If found → `Permit()`, **`UnLoadSeg(seglist)`** (we don't
   need this copy), copy the existing name to `name_out`, return `DRVL_OK`.
4. Else `InitResident(romtag, seglist)` while still under Forbid/Permit window
   as appropriate. For an `RTF_AUTOINIT` device this performs `MakeLibrary` +
   `AddDevice`, inserting it into the exec device list. Re-check
   `FindName(&SysBase->DeviceList, rt_Name)`; absent → `DRVL_EINIT`.
   `Permit()`. Copy `rt_Name` to `name_out`, return `DRVL_OK`.

### Notes / constraints

- **Name source:** the exec name `OpenDevice` needs is `romtag->rt_Name`, which
  can differ from the filename — always register/probe by `rt_Name`, never the
  path basename.
- **Lifetime:** once init'd, the driver stays resident for the session (and
  until reboot). We do **not** `RemDevice`/`UnLoadSeg` an init'd driver — a disk
  on it will be opened by discovery/selection, and removing an open device is
  unsafe. This matches HDToolBox-class tools; surfaced in the status line.
- **Stack/structs:** romtag scan touches only the loaded segment memory; no
  large stack structs introduced. Buffers in the GUI caller are `static`
  (4KB-stack convention, even though we run on the 64KB swapped stack).
- `seglist`/`BADDR`/`BPTR` arithmetic is 32-bit; no 64-bit math (toolchain
  constraint).

## Discovery integration — `discover.c` / `discover.h`

The probe set is currently the fixed `kProbeDrivers[]`. Add a small registry of
user-loaded driver names so they persist across `Rescan`.

```c
/* Register a user-loaded driver name so scan_probe() also probes it on every
   subsequent full discover_disks(). Deduped; silently ignored past capacity. */
void disc_add_extra_driver(const char *name);

/* Targeted probe of ONE driver: open units 0..PROBE_UNITS-1, add the ones that
   open, classify just those new entries (geometry + RDB). Appends to out[]
   starting at *count (in/out). Returns the number of entries added. */
int discover_probe_driver(DiscDisk out[], int *count, int max, const char *driver);
```

- `g_extra[]`: static array (cap 8) of `char[DISC_DRIVER_LEN]`, deduped via the
  existing name compare. `scan_probe` loops `kProbeDrivers` **then** `g_extra`.
- `discover_probe_driver` reuses the existing `add_unique` + `probe_one` statics
  (no duplication): for `u` in `0..PROBE_UNITS-1`, `dev_open`/`dev_close` to test
  presence, `add_unique`, then `probe_one` on each newly added index.
- Both new functions live under `#ifdef HDPART_AMIGA` except the registry list
  which is plain. `drv_find_romtag` is host-compiled and host-tested.

## GUI integration — `gui.c`

- **Library:** open `asl.library` v37 in `gui_open` (store base; close in
  teardown). If it fails, the new button is created **disabled** — graceful on a
  stripped system.
- **Gadget:** new `GID_DRIVER` button labelled `Driver…`, placed on the
  cycle/rescan row next to `Rescan`; that row is re-spaced to fit three controls.
- **Factor out** the cycle-label rebuild currently inline in `gui_rescan` into
  `static void gui_rebuild_devlabels(int *active_out)` so both the full-rescan
  path and the load-driver path share one implementation.
- **`gui_load_driver()`** (invoked on `GID_DRIVER` GADGETUP):
  1. `AllocAslRequestTags(ASL_FileRequest, ASLFR_TitleText, "Select a device
     driver", ASLFR_InitialDrawer, "DEVS:", ASLFR_DoPatterns, TRUE,
     ASLFR_InitialPattern, "#?.device", TAG_END)`; `AslRequest`. Cancel → return.
     Build the full path from `fr_Drawer` + `fr_File` (`AddPart`-style join);
     `FreeAslRequest` after copying out.
  2. `driver_load_file(path, name, sizeof name)`. On `<0` → `gui_msg` with the
     reason mapped from the `DRVL_*` code.
  3. On `DRVL_OK` → `disc_add_extra_driver(name)`;
     `discover_probe_driver(g_disks, &g_ndisks, DISC_MAX, name)`.
  4. `gui_rebuild_devlabels(...)` then **preselect:** scan the rebuilt
     `g_devmap`/`g_disks` for the first entry whose `driver` matches `name` and
     is partitionable; set `GTCY_Active` to that cycle index and call
     `gui_select_device(idx)` (opens it, reads RDB). If none has media → leave
     selection unchanged-but-reset and set status
     `"Driver loaded; no media on units 0-7"`.
- Status/errors use the existing `g_statusbuf` and `gui_msg` patterns.

### `DRVL_*` → message mapping

| Code | Message |
|------|---------|
| `DRVL_ELOAD` | "Could not load that file as a driver." |
| `DRVL_ENOROMTAG` | "That file has no driver (no Resident tag)." |
| `DRVL_ENOTDEVICE` | "That file is not a .device driver." |
| `DRVL_EINIT` | "Driver loaded but failed to initialise." |

## Testing

- **Host tests** (`tests/run-host-tests.sh`): add cases for `drv_find_romtag`
  against synthetic buffers — valid romtag found at offset; rejects a bare
  `0x4AFC` with a non-self `rt_MatchTag`; returns none on a buffer without one;
  honours `size` bounds (no over-read on a romtag straddling the end).
- **On-target (FS-UAE KS2.04):** copy a real `.device` (e.g. `uaehf.device` or a
  `scsi.device` build) into `DEVS:`, ensure it is NOT otherwise loaded, run
  `Driver…`, confirm the file requester opens at `DEVS:`, the driver loads, a
  unit with media is auto-selected, and its RDB/partition list renders. Verify
  re-loading the same driver (already resident) selects without error. Verify a
  later `Rescan` still lists the loaded driver's units.

## Risks

| Risk | Mitigation |
|------|------------|
| `.device` file has multiple romtags (device + filesystem) | Select the first `NT_DEVICE` romtag; ignore others. |
| Coincidental `0x4AFC` in code/data | `rt_MatchTag` self-pointer validation. |
| Non-`RTF_AUTOINIT` device (rare) | `InitResident` still runs the romtag's init; post-check via `FindName` catches anything that didn't register → `DRVL_EINIT`. |
| `asl.library` absent | `Driver…` button created disabled. |
| Init'd driver can't be removed | By design — kept resident for the session; documented in status line. |
