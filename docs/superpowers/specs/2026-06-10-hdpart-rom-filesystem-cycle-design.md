# HDPart — ROM filesystem selection in the editor (design)

**Date:** 2026-06-10 (rev 2)
**Status:** Approved (brainstorming)
**Scope:** "A" only — the in-ROM (Kickstart) FastFileSystem, chosen as OFS/FFS plus
the International and Directory-Cache flags. Loading a third-party filesystem from
disk and embedding it in the RDB (FSHD/LSEG) is **deferred** ("B"); see
[[hdpart-filesystem-types]].

## Problem

The Edit/Add dialog's `FS` cycle has a single option (`FFS Intl (DOS\3)`), and the
Ok handler **hardcodes** `kFsTypes[0]` — so the dropdown does nothing. Users want
to choose the ROM filesystem the way HDToolBox does: OFS vs FFS, with
International and Directory-Cache as independent toggles.

## Background — DosType encoding (corrected)

A partition's filesystem is its RDB `de_DosType` (`'D','O','S', flags`). The low
byte is a **bitmask**, not a sequence:

- **bit 0** — FFS (1) vs OFS (0)
- **bit 1** — International mode
- **bit 2** — Directory Cache mode

So `DOS\0`=OFS, `\1`=FFS, `\2`=OFS+Intl, `\3`=FFS+Intl, `\4`=OFS+DC, `\5`=FFS+DC,
`\6`=OFS+Intl+DC, `\7`=FFS+Intl+DC. (An earlier rev of this spec wrongly treated
`\4/\5` as the Intl+DC combos.)

Availability by Kickstart (exec version):
- OFS/FFS and **International** — KS2.0+ (our V37 baseline always has them).
- **Directory Cache** — **KS3.0+ (exec V39)**; gate the DirCache toggle on
  `SysBase->LibNode.lib_Version >= 39`. All ROM filesystems run on a 68000, so no
  CPU gating is needed here (that's a B/SFS concern).

## Design (all in `gui_edit_dialog`, `src/gui.c`)

### Controls (on the existing FS row, y48 — no new row)

Replace the single FS cycle with three gadgets sharing the row:
1. **FS base cycle** — `FFS` / `OFS`, plus a trailing **`keep`** entry that
   appears *only* when the partition's current `dos_type` is not a ROM DOS type
   (its label is the rendered DosType, e.g. `PFS\3`). ~x110 w80.
2. **`Intl` checkbox** (International, bit 1). ~box x236.
3. **`Cache` checkbox** (Directory Cache, bit 2). ~box x320. Created **disabled
   when `lib_Version < 39`** (and never contributes a set bit on <V39).

The dialog stays 350 wide; short labels (`FS`, `Intl`, `Cache`) keep the row
within bounds.

### Initial state (decode the partition's DosType)

- If `dos_type` is a ROM `DOS\x` (`(dos_type & 0xFFFFFF00) == 0x444F5300` and the
  low byte ≤ 7): FS cycle = FFS/OFS from bit 0; `Intl` checked from bit 1; `Cache`
  checked from bit 2 (and only if V39+). No `keep` entry.
- Else (non-ROM, e.g. PFS3/SFS/custom): the FS cycle gains a `keep` entry (label =
  rendered DosType) and selects it; both checkboxes are **disabled** (they don't
  apply). The original `dos_type` is retained verbatim.

### Interaction

- FS cycle GADGETUP: if it moved to/from `keep`, enable/disable the two checkboxes
  accordingly (Cache stays disabled on <V39 regardless).
- Checkboxes toggle freely (GadTools handles their visual state; we read
  `GFLG_SELECTED` on Ok).

### Compute on Ok

Replace the hardcoded `kFsTypes[0]` with a computed DosType:

```
if (FS cycle == keep)  dt = pt->dos_type;            /* preserve */
else {
    dt = 0x444F5300u;
    if (fsCyc == FFS)              dt |= 1u;
    if (gIntl->Flags & GFLG_SELECTED)  dt |= 2u;
    if ((lib_Version >= 39) && (gCache->Flags & GFLG_SELECTED)) dt |= 4u;
}
```

Pass `dt` to `rdb_rename_partition` / `rdb_set_partition` (today both use
`kFsTypes[0]`). This also fixes the latent bug where the FS choice was ignored.

### Helper

`dostype_label(char *out, uint32_t t)` — render a DosType to a short human string:
bytes 0–2 if printable ASCII then `\` then the low byte as a decimal digit
(`DOS\3`, `DOS\7`, `PFS\3`, `SFS\0`); otherwise `0x%08X`. Defined early (near the
`u2s`/`s_cat` helpers) so both the editor and the partition list can use it.

### Partition-list Type column (folded in)

`gui_refresh_parts` currently prints a fixed `"FFS"` for every partition's `Type`.
Replace it with `dostype_label(pt->dos_type)` so the column reflects the real
filesystem — `DOS\3` (FFS Intl), `DOS\1` (FFS), `DOS\0` (OFS), `DOS\7` (FFS Intl
DC), `PFS\3`, `SFS\0`, … The Type column is 6 chars wide, which fits the 5-char
`XXX\d` form (the rare non-printable hex fallback may overflow into the next
column; acceptable). No column-layout change.

### No engine / layout-growth / test change

`de_DosType` round-trips already in `src/rdb.c`; `gui_new` still seeds
`RDB_DOSTYPE_FFS_INTL` (decodes to FFS + Intl on open).

## Risks / notes

| Risk | Handling |
|------|----------|
| FS choice silently ignored today | Fixed: Ok computes the DosType from the controls. |
| Editing a PFS3/SFS partition would overwrite its type | `keep` entry preserves it; checkboxes disabled. |
| DirCache deprecated/buggy & KS3.0-only | `Cache` checkbox disabled on <V39; on 3.0+ it is the OS's own option. |
| Crowded FS row | Short labels (`FS`/`Intl`/`Cache`); fits the 350-wide dialog. |
| `kFsLabels`/`kFsTypes` removed | FS-cycle create + Ok handler updated; `gui_new` uses `RDB_DOSTYPE_FFS_INTL` directly. |
