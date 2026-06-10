# HDPart — ROM filesystem selection in the editor (design)

**Date:** 2026-06-10
**Status:** Approved (brainstorming)
**Scope:** "A" only — the in-ROM (Kickstart) FastFileSystem family, version-gated.
Loading a third-party filesystem from disk and embedding it in the RDB
(FSHD/LSEG) is **deferred** ("B"); see [[hdpart-filesystem-types]].

## Problem

The Edit/Add dialog's `FS` cycle has a single option (`FFS Intl (DOS\3)`), and
the Ok handler **hardcodes** `kFsTypes[0]` — so the dropdown does nothing even if
it had more entries. Users want to choose among the filesystems the running
Kickstart actually supports.

## Background (DosTypes & Kickstart gating)

A partition's filesystem is its RDB `de_DosType` (already stored/written/read by
`src/rdb.c`). The Kickstart ROM FastFileSystem handles the `DOS\x` family; which
members it knows depends on the exec version:

| Label | DosType | Available |
|-------|---------|-----------|
| OFS | `0x444F5300` (`DOS\0`) | KS2.0+ |
| FFS | `0x444F5301` (`DOS\1`) | KS2.0+ |
| OFS Intl | `0x444F5302` (`DOS\2`) | KS2.0+ |
| FFS Intl | `0x444F5303` (`DOS\3`) | KS2.0+ (default) |
| OFS Intl+DC | `0x444F5304` (`DOS\4`) | **KS3.0+ (exec V39)** |
| FFS Intl+DC | `0x444F5305` (`DOS\5`) | **KS3.0+ (exec V39)** |

HDPart's V37 baseline guarantees DOS\0–\3; DirCache (\4,\5) is gated on
`SysBase->LibNode.lib_Version >= 39`. All ROM filesystems run on a 68000, so **no
CPU gating is needed** for this scope (the 68020 gate is a B/SFS concern).

## Design (all in `gui_edit_dialog`, `src/gui.c`)

### 1. Build the FS list at dialog open

Replace the static `kFsLabels`/`kFsTypes` (single FFS-Intl entry) with arrays
built per dialog, into file-static storage sized for the max (6 ROM + 1 "keep"):

```c
static const char *fsLabels[8];   /* NULL-terminated, for GTCY_Labels */
static uint32_t    fsTypes[8];
static char        fsKeepLabel[20];
```

Build order:
1. If `pt->dos_type` is **not** one of the ROM DosTypes below, entry 0 is a
   "keep" entry: label = friendly render of the type + `" (keep)"`, value =
   `pt->dos_type`. (Render: bytes 0–2 if all printable ASCII, then `\`, then the
   low byte as a decimal digit if < 10 else two hex digits — e.g. `PFS\3`,
   `SFS\0`; otherwise `0x%08X`.)
2. ROM entries in this order: `FFS Intl (DOS\3)`, `OFS Intl (DOS\2)`,
   `FFS (DOS\1)`, `OFS (DOS\0)`.
3. If `lib_Version >= 39`: append `FFS Intl+DC (DOS\5)`, `OFS Intl+DC (DOS\4)`.
4. NULL-terminate `fsLabels`.

`fsActive` (initial cycle index) = index whose `fsTypes[i] == pt->dos_type`
(the ROM match, or the index-0 keep entry). A freshly-created partition (default
`RDB_DOSTYPE_FFS_INTL`) lands on the `FFS Intl` entry.

The FS cycle gadget is created with `GTCY_Labels=(ULONG)fsLabels`,
`GTCY_Active=(ULONG)fsActive`, and its pointer is saved (currently it is not).

### 2. Track and honor the selection

- Save the FS cycle gadget pointer (`gFsCyc`) and a running `int fsIdx = fsActive;`.
- In the event loop, on `ig == gFsCyc` GADGETUP: `fsIdx = (int)code;`.
- In the **Ok** handler, replace both `kFsTypes[0]` uses with `fsTypes[fsIdx]`,
  passed to `rdb_rename_partition` / `rdb_set_partition`. (Fixes the latent bug
  where the FS choice was ignored.)

### 3. Helpers

- A small `dostype_label(char *out, uint32_t t)` that renders a DosType to a
  human string (printable `Xxx\d` form, else `0x%08X`). Pure, could be unit-tested
  but is GUI-local; verified on-target.
- KS-version check inline: `SysBase->LibNode.lib_Version >= 39`.

### Layout / engine / tests

No change to dialog geometry (the cycle keeps its `x110 w180 y48` slot; labels
fit). No `rdb.c`/`rdb.h` change — `de_DosType` round-trips already. No host-test
change (the FS list is GUI-only); `gui_new` still defaults to `RDB_DOSTYPE_FFS_INTL`.

## Risks / notes

| Risk | Handling |
|------|----------|
| FS choice silently ignored today | Fixed: Ok now uses the selected `fsTypes[fsIdx]`. |
| Editing a PFS3/SFS partition would overwrite its type | "keep" entry preserves it; selected by default. |
| DirCache (\4,\5) is deprecated/buggy | Offered only on KS3.0+, where it is the OS's own option; user's choice. |
| Label width | Longest `FFS Intl+DC (DOS\5)` (~19 chars) fits the 180px cycle. |
| `kFsLabels`/`kFsTypes` removed | Replace usages (FS cycle create + Ok handler); `gui_new` uses `RDB_DOSTYPE_FFS_INTL` directly, unaffected. |
