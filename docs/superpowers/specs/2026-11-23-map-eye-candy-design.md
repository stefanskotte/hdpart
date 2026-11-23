# Disk map eye candy: design

Date: 2026-11-23. Size: a small change. A throwaway trial on the branch `spike/map-gradient` (commit "spike: map eye candy") was built, run in WinUAE and approved from screenshots; this records what it settled.

## Why

The user wants HDPart to look a bit special next to HDToolBox and friends: some eye candy that still reads at a glance and still works in the four Workbench colours.

## Goals

- **Bevelled partition slices.** Every partition slice on the disk map gets a soft 3D edge. A 4x4 ordered (Bayer) dither fades white in over the top 5 rows (levels 11, 7, 4, 2, 1 of 16) and black over the bottom 4 (8, 5, 3, 1). It's drawn JAM1 over the slice's existing fill, so the filesystem patterns (FFS solid blue, PFS checkerboard, SFS stripes, other) stay visible. The dither is aligned to the screen x, so neighbouring slices match. Free space gets no bevel.
- **Labels without boxes.** Slice labels (name and size) lose their white box. They are drawn in white straight on the fill, two text lines centred between the bevel rows: on solid FFS/OFS with a black drop shadow at +1,+1; on the patterned fills (PFS, SFS, other) with a full black outline (the 8 neighbours), because a shadow alone was unreadable on the PFS checkerboard.
- **No RDB area on the maps.** *(Changed 2026-11-24, user's decision.)* The trial drew the cylinders below `lo_cyl` as a sunk, hatched block with an "RDB" plate. To scale, it was a sliver on any real disk, and it can't be edited. So the disk map, the mini-maps and the Quick setup map now cover only the partitionable cylinders `lo_cyl..hi_cyl`, and partitions get the full width. One helper (`map_x`) maps cylinders to pixels for both drawing and clicking. Considered and left for later: a fixed-width RDB tab that opens an RDB dialog, where "reserved space for embedded filesystems" could become an advanced option. That option would rather sit in the Filesystems dialog, where the "don't fit the reserved RDB area" error shows up.
- **Units list mini-map.** The bar under each unit, which shows how much of the disk is partitioned, becomes a miniature of that disk's map: 7 rows high inside the existing recessed box (box at y+11, 9 high, within the unchanged 21-pixel row). It shows each partition in its filesystem pattern with a one-row light top and one-row dark bottom, a 1-pixel gap between slices, and the dotted free-space pattern. A disk without an RDB keeps the plain white bar.
- **The mini-map stays right.**
  - The selected unit's mini-map is drawn from the live table, so it follows edits before Save.
  - Other units use the layout read at Scan.
  - After a successful Save, the saved layout and percentage are copied into that unit's entry, so the bar is right after switching to another unit without a rescan.
- **Quick setup matches.** The wizard's disk map gets the same bevel and labels.

## Non-goals

- No palette changes or extra pens. HDPart usually draws on the Workbench screen; KS 2.04 has no `ObtainBestPen`. Everything is dithering with the four existing pens.
- No change to what the map or bars mean, to hit-testing, or to the Units row height.
- No animation.

## Anti-goals

- Filesystem types must stay recognisable at a glance on the map and in the mini-maps.
- Labels must stay readable on every fill.
- Nothing may need chip memory: line patterns only (`LinePtrn`), as the existing fills do.

## Approaches

Three ways to place the gradient were considered:

1. **Gradient on FFS slices only:** simplest, but PFS and SFS look flat next to them.
2. **Bevel on every partition, over its pattern:** consistent 3D look, patterns still readable. **Picked by the user.**
3. **Gradient everywhere, filesystem type only in the labels:** boldest look, but loses filesystem types at a glance.

For the labels, a box-less drop shadow was the user's idea. The trial showed it fails on the PFS checkerboard, so the patterned fills get an outline instead.

## The engine side

`rdb_used_percent` (src/rdb.c) already walks the partition list on Scan. A new `rdb_used_layout` returns the same percentage plus an `RdbLayout`: up to 16 segments (`lo`, `hi` exclusive, `dos_type`), positions in 1/1000 of the partitionable cylinders `lo_cyl..hi_cyl` (`rdb_span_permille`, overflow-safe like the percentage). `rdb_used_percent` becomes a wrapper around it. `DiscDisk` (src/discover.h) gains an `RdbLayout layout`, initialised empty wherever `used_pct` is set to -1. This costs about 130 bytes per disk entry (about 4 KB for `DISC_MAX` 32) and no extra disk reads.

## Testing

- Host tests: `rdb_used_layout` on images with 0, 1 and several partitions, including the 16-segment cap and a large cylinder count. Check that `rdb_used_percent` is unchanged.
- On-target (`tests/target/run.mjs`): a step that edits the table and checks the shown unit's mini-map changes before Save, then Saves, switches to another unit and back, and checks the first unit's layout is kept. Check by reading the layout from guest memory, not by pixels. Keep a screenshot of the map with a PFS partition in the suite: the trial's one-off screenshot line becomes a permanent `map-pfs.png`.
- `make target-test` on KS 2.04, 3.1 and 3.2, and `make target-quick` (the map is shared with the wizard).

## Left to the builder

- Function names and where the helpers sit in src/gui.c. The trial's are a reasonable start; drop its `SPIKE` markers.
- Whether the mini-map's partition top and bottom rows reuse `gui_dither_row` levels 7 and 6 as in the trial, or look better at other levels on real screens.
- README: refresh the main screenshot once it ships.
