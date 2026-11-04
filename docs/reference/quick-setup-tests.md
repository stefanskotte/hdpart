# Quick setup: the engine on emulated IDE

Date: 2026-11-03. HDPart 0.11.2, unchanged. Everything here ran with `tests/target/quick.mjs`, on WinUAE, with the card on the
emulated internal IDE port (`hardfile2=...,ide0`) and HDPart driving `scsi.device` unit 0.

Labels, as in `rom-findings-maxtransfer-mask-buffers.md`:

- **OBSERVED**: read in the code.
- **INFERRED**: reasoned from the code, not executed.
- **EMULATED**: ran on WinUAE. This proves ROM and HDPart logic against WinUAE's drive model, not real drive behaviour.
- **UNVERIFIED**: not established by anything here.

Everything marked EMULATED still needs the real-hardware check before release.

## Decisions

**Single-run flow safe: NO.** Rewriting the table of a used card and formatting without a restart broke the card on KS 3.1 and on
KS 3.2, so the wizard uses the restart path. **EMULATED.** The restart path itself worked on both.

**Partition limit: 2 GiB stands.** A partition of 2 GiB read back correctly up to its last block on KS 3.1 and KS 2.05, so the limit is
proven, not just assumed. **EMULATED.** A 3584 MB partition also read back correctly on both, so the FAQ's 2 GiB for FFS 34-40 did not
show up as a filesystem failure in these tests. That alone does not raise the limit. See "What this does not prove".

## Used card: the single-run flow

Test: `used`, 256 MB card. Two formatted partitions with a file on each, restart so they auto-mount, read both files back. Then, with no
further restart: delete both in the UI, Split into three, Save (it warns about mounted volumes and proceeds), Format each, `volcheck` 5 MB on
each. Then restart and check again.

Both ROMs gave the same result. **EMULATED**, 24 problems recorded on each.

| Partition | KS 3.1 A1200 (cylinders) | KS 3.2 A1200 (cylinders) |
|---|---|---|
| Old DH0, DH1 | 2-259, 260-518 | 2-462, 463-923 |
| New DH0, DH1, DH2 | 2-173, 174-345, 346-518 | 2-308, 309-615, 616-923 |

What happened, in order:

| Step | Result |
|---|---|
| Format new DH0 | Reports success. Goes through the old DH0 handler, which still has the old bounds |
| Format new DH1 | The confirmation says "mounted as DH0:", and the status line says "Formatted DH1:". It reformats old DH0 a second time. Nothing is written at new DH1's first block |
| Format new DH2 | Says "mounted as DH1:". Reformats old DH1. Nothing is written at new DH2's first block |
| `volcheck` DH0 | Passes, but Info says 260062 blocks (KS 3.2: 261385) on a partition of 173374 (174067): the old size |
| `volcheck` DH1 | Passes, but Info gives the old size, and the file is on the card at blocks 392684-403065 (KS 3.2: 393283-403664), outside new DH1 and inside new DH2 |
| `volcheck` DH2 | Requester "Please insert volume DH2 in any drive". After Cancel, IoErr 218 (device not mounted) |
| Restart | The three handlers start with the right bounds, and none has a usable volume |
| DH0, DH1, DH2 after restart | Requester "Not a DOS disk in device DHn" on each. After Cancel, IoErr 225. The files written before the restart are gone from the user's view |

No crash, and the table on the card stayed valid. The failure is worse than a crash for a beginner: every Format reports success, and the card is
unusable after the restart.

Cause: `format_partition` (`src/format.c`) treats any cylinder overlap with a live mount as "reformat that volume" and uses the old handler. **OBSERVED**, and
this run confirms it behaves as read. `safety_partition_mounted` matches the first overlap, which is why new DH1 maps onto old DH0.

Screenshots of the requesters: `out/target-quick/<key>/used/shots/`, not committed.

## Used card: the restart path

Test: `restart`. The same old card, but restart straight after Save, then Format each, `volcheck` each, restart and read back.

| Check | KS 3.1 A1200 | KS 3.2 A1200 |
|---|---|---|
| New table auto-mounts as DH0, DH1, DH2, each matched to itself | yes | yes |
| Format through those mounts | ok | ok |
| Info size equals the new partition, less 2 reserved blocks | yes | yes |
| 5 MB written, read and compared, and on the card inside the partition | yes | yes |
| Read back after another restart | yes | yes |
| Requesters | none | none |

**EMULATED.** Not run on A600 or A4000 machines, nor with a marker, which does not exist yet.

## Fresh partition on a blank card

Test: `fresh`, 64 MB card. Split in two, Save, Format, `volcheck` 5 MB on each. All bytes compared in the guest. On the host, every sector is
on the card, undamaged and inside its partition. No requesters. **EMULATED** on all of these:

| Key | Machine | ROM | scsi.device |
|---|---|---|---|
| `ks205a` | A600 | KS 2.05 `kickstart205.rom` | 37.55 |
| `ks205b` | A600 | KS 2.05 37.300 A600HD | 37.34 |
| `ks30` | A1200 | KS 3.0 | 37.64 |
| `ks31a600` | A600 | KS 3.1 40.63 | 40.5 |
| `ks31a1200` | A1200 | KS 3.1 `amiga-os-310-a1200.rom` | 40.12 |
| `ks31a4000` | A4000 | KS 3.1 40.70 | 40.20 |
| `ks314` | A1200 | KS 3.1.4 | 45.7 |
| `ks32` | A1200 | KS 3.2 `kicka1200.rom`, exec 47.7 | 47.4 |

All eight machine configurations ran. Note that `ks205a` here is the 37.55 ROM and `ks205b` the 37.34 one.

## Partition size

Test: `limit` and `offset`, on a blank 4,000,000,000-byte sparse card with FFS International (DOS\3), MaxTransfer 0x1FE00, Mask 0x7FFFFFFE.

### How data reached the end of the partition

Files go after the root block in the middle of the partition, up to its end, then wrap to its start. So:

1. Filler files, written through dos.library like any file, up to half the volume less 88 MB and an allowance for FFS's own blocks. These are written in
   512 MB files and not read back at this stage.
2. A 64 MB file, written, read back and compared in the guest.
3. A 128 MB file that runs over the end of the partition and wraps to its start, also compared.
4. On the host, every sector of every file is found in the hardfile by its stamp: count, pattern and position. This is what shows where the data truly landed.
5. Restart: Info, then the 64 MB and 128 MB files read and compared again, and the first 512 MB filler too on the 68020 runs.

To make this practical, the runs use `cpu_speed=max`, and `volcheck` writes 256 KB at a time. On the KS 2.05 A600 runs the filler was written on a 68020 machine with the same
ROM, chipset and memory, because a 68000 at full speed writes about 0.4 MB/s against 1.5. That machine then restarted, and everything from step 2 on ran on the real A600
configuration (68000). The filler on those runs was checked on the host only. The filesystem and driver code is the same ROM code either way.

### Results

| Machine | Partition | Size in bytes | Ends at byte | 64 MB file ended | 128 MB file wrapped | Read back after restart |
|---|---|---|---|---|---|---|
| KS 3.1 A1200 (FFS 40.1) | 3584 MB at the start | 3,758,211,072 | 3,759,243,264 | 24.0 MB before the end | yes, last block to first area | correct |
| KS 3.1 A1200 | 2048 MB at the start | 2,147,991,552 | 2,149,023,744 | 24.0 MB before the end | yes | correct |
| KS 3.1 A1200 | 2048 MB after a 1760 MB one | 2,147,991,552 | 3,994,583,040 | 24.0 MB before the end | yes | correct |
| KS 2.05 A600 (`ks205a`, scsi.device 37.55) | 3584 MB at the start | 3,758,211,072 | 3,759,243,264 | 24.0 MB before the end | yes | correct |
| KS 2.05 A600 | 2048 MB at the start | 2,147,991,552 | 2,149,023,744 | 24.0 MB before the end | yes | correct |
| KS 2.05 A600 | 2048 MB after a 1760 MB one | 2,147,991,552 | 3,994,583,040 | 24.0 MB before the end | yes | correct |

All **EMULATED**. The 2048 MB partition is slightly over 2 GiB because HDPart rounds to cylinders. In every run the highest data on the card was the partition's very last block.

`Info` matched the partition size on all runs, and the volume stayed validated (state 82) after restart. No requesters, no damaged sectors, nothing outside the partition, and the table was valid afterwards. Host scan checked with a
one-byte corruption of a copy, which it reported as one bad sector.

## Telling internal IDE from everything else

`safety_is_internal_ide()` in `src/safety.c`, the wizard's main gate. It returns 1 only when the `scsi.device` in exec's device list is proven to be a ROM-resident
internal IDE driver. It reads memory only and touches no hardware. `gui_run` stores the answer in `g_qs_is_ide` at startup, which is what the test reads.

**The check.** Every IDE build loads its hardware base into A4 during init, with one `lea abs.l,a4` (`49F9 aaaa aaaa`). **OBSERVED** in the disassembly of 3.1 A1200 $FB50A8 and A4000 $F87E9C. A3000 3.1 and later and the NCR driver use the same instruction ($F87B40, 3.2 A4000T $F898F0), but A3000 2.0 and 2.04 use `movea.l #$DD0000,a4` (`287C 00DD 0000`). So the IDE bases count only as that `lea`, and the SCSI bases count as a long in any instruction. The base says what the driver drives:

| Base | Hardware | Driver | Answer |
|---|---|---|---|
| `$DA2000` | Gayle IDE | A600, A1200, also the shared A500/A2000/CDTV 3.2 ROM and CD32 | 1 |
| `$DD2020` | A4000 and A4000T IDE | A4000, `scsi.device` on A4000T | 1 |
| `$DD0000` | A3000 SDMAC | A3000 | 0 |
| `$DD0040` | A4000T NCR | `NCR scsi.device` | 0 |

Answer 1 needs exactly one IDE `lea`, and neither SCSI address as a long anywhere in the module, at any even offset. The ID string is not used: before 45.x, IDE and A3000 both say `scsidisk`.

**Which code gets read.** All of these must hold, or the answer is 0:

1. `scsi.device` is in exec's device list. This is the running driver, not whatever a ROM contains.
2. Its BeginIO and AbortIO vectors are absolute jumps to `$F80000-$FFFFFF`.
3. A RomTag in exec's ResModules, also in that range and valid, is named `scsi.device` and contains both targets between its start and `rt_EndSkip`.

Only that module is scanned, so the A4000T's NCR module next to it cannot affect the answer.

**What this means for other drivers:**

| Situation | Answer | Why |
|---|---|---|
| Driver loaded from disk with LoadModule, including a genuine IDE one on an A1200 | 0 | Its vectors are in RAM. **EMULATED** below. A beginner's wizard does not need to trust it, and the user can still partition by hand |
| A ROM update that replaces `scsi.device` from disk | 0 | The same case. **INFERRED** |
| A whole ROM mapped onto `$F80000` (MapROM, an MMU tool, A3000 SuperKickstart) | Decided by that ROM's driver, so an A3000 ROM gives 0 and an A1200 ROM on an A1200 gives 1 | The check reads what is really at those addresses. **INFERRED**; the emulator boots these ROMs at `$F80000` directly, not through a mapping tool |
| A program that patches BeginIO or AbortIO (IDEfix-style tools) | 0 | The vector leaves the module. **INFERRED**: same comparison as the LoadModule case, but no such tool was run |
| SetPatch, including its `scsi.device` LED patch | Unaffected | BeginIO and AbortIO were read after SetPatch ran and stayed in ROM. **EMULATED** below, with the 3.1.4 and the 3.2 SetPatch. Whether the emulator's drive made the LED patch act was not checked |

Refusing a LoadModule'd IDE driver is the price of the rule. Wizard screen text should say the wizard does not recognise this setup and point to the normal window.

### Every ROM in the collection

`tests/target/romsweep.c` runs the shipped `safety_driver_is_ide` on the `scsi.device` and `NCR scsi.device` RomTags of every ROM image under the directories it is given, and fails if the answer disagrees with the base it finds on its own. **OBSERVED**. It covered four directories: the local ROM collection, the FS-UAE Kickstarts directory, the OS install set's `roms` directory, and the `ROMs` drawer of Update3.2.2. Files are counted as found, so the same ROM under several names or directories counts more than once; "Different modules" removes that.

| Base found | Module | ID string | Image built for | Files | Different modules | Answer |
|---|---|---|---|---|---|---|
| A4000 IDE | `scsi.device` | IDE_scsidisk 45.7 | $F80000 | 1 | 1 | 1 |
| A4000 IDE | `scsi.device` | IDE_scsidisk 47.4 | $F80000 | 10 | 3 | 1 |
| A4000 IDE | `scsi.device` | scsidisk 37.64 | $F80000 | 1 | 1 | 1 |
| A4000 IDE | `scsi.device` | scsidisk 40.12 | $F80000 | 9 | 2 | 1 |
| A4000 IDE | `scsi.device` | scsidisk 40.20 | $F80000 | 4 | 2 | 1 |
| A4000 IDE | `scsi.device` | scsidisk 40.4 | $F80000 | 3 | 1 | 1 |
| Gayle IDE | `scsi.device` | IDE_scsidisk 45.7 | $F80000 | 1 | 1 | 1 |
| Gayle IDE | `scsi.device` | IDE_scsidisk 47.4 | $F80000 | 10 | 6 | 1 |
| Gayle IDE | `scsi.device` | scsidisk 37.34 | $200000 | 8 | 1 | 1 |
| Gayle IDE | `scsi.device` | scsidisk 37.34 | $F80000 | 2 | 1 | 1 |
| Gayle IDE | `scsi.device` | scsidisk 37.55 | $F80000 | 4 | 1 | 1 |
| Gayle IDE | `scsi.device` | scsidisk 37.64 | $F80000 | 3 | 1 | 1 |
| Gayle IDE | `scsi.device` | scsidisk 40.12 | $200000 | 4 | 1 | 1 |
| Gayle IDE | `scsi.device` | scsidisk 40.12 | $F80000 | 8 | 1 | 1 |
| Gayle IDE | `scsi.device` | scsidisk 40.20 | $200000 | 1 | 1 | 1 |
| Gayle IDE | `scsi.device` | scsidisk 40.4 | $F80000 | 7 | 1 | 1 |
| Gayle IDE | `scsi.device` | scsidisk 40.5 | $F80000 | 3 | 1 | 1 |
| NCR | `NCR scsi.device` | (no ID string) | $F80000 | 1 | 1 | 0 |
| NCR | `NCR scsi.device` | A4000T_scsidisk 47.4 | $F80000 | 5 | 3 | 0 |
| SDMAC | `scsi.device` | scsidisk 36.38 | $F80000 | 11 | 1 | 0 |
| SDMAC | `scsi.device` | scsidisk 37.19 | $F80000 | 9 | 1 | 0 |
| SDMAC | `scsi.device` | scsidisk 37.55 | $F80000 | 1 | 1 | 0 |
| SDMAC | `scsi.device` | scsidisk 40.12 | $F80000 | 2 | 1 | 0 |
| SDMAC | `scsi.device` | scsidisk 40.4 | $F80000 | 12 | 2 | 0 |
| SDMAC | `scsi.device` | scsidisk 47.4 | $F80000 | 5 | 2 | 0 |

285 images read, 125 modules, 0 problems.

Totals: SDMAC 40, NCR 6, IDE 79. The local ROM collection alone gives SDMAC 22, NCR 3, IDE 50. A count of exact 512K files in that collection gives 21, 3 and 48, which leaves out the two 37.34 KickIt images with an 8-byte header and the 526572-byte `kickstart.rom`, whose SDMAC driver carries ID 37.55.

- Images built for `$200000` are ReKick and similar. The table gives the answer for their code. When running, their vectors are outside `$F80000-$FFFFFF`, so the answer is 0. **INFERRED**.
- Not read: byte-swapped images and split hi/lo files. 2.05 37.299 has the name as a string but no such RomTag, and A500+ 2.04 has neither.

### On the emulator

Test: `ide`. Boots HDPart on each machine and reads `g_qs_is_ide`. The driver column is read from exec's device list by the harness, not from the ROM. All **EMULATED**.

| Key | Machine | Driver running | BeginIO | Expected | Got |
|---|---|---|---|---|---|
| `ks205a` | A600, 2.05 | scsidisk 37.55 | $F868EA | 1 | 1 |
| `ks205b` | A600, 2.05 37.300 | scsidisk 37.34 | $F868BE | 1 | 1 |
| `ks31a600` | A600, 3.1 | scsidisk 40.5 | $FB5FA0 | 1 | 1 |
| `ks30` | A1200, 3.0 | scsidisk 37.64 | $F87E02 | 1 | 1 |
| `ks31a1200` | A1200, 3.1 | scsidisk 40.12 | $FB5724 | 1 | 1 |
| `ks314` | A1200, 3.1.4 | IDE_scsidisk 45.7 | $FB5C3E | 1 | 1 |
| `ks32` | A1200, 3.2 | IDE_scsidisk 47.4 | $FB5A22 | 1 | 1 |
| `ks31a4000` | A4000, 3.1 | scsidisk 40.20 | $F88304 | 1 | 1 |
| `ks314a4000` | A4000, 3.1.4 | IDE_scsidisk 45.7 | $F86312 | 1 | 1 |
| `ks32a4000` | A4000, 3.2 | IDE_scsidisk 47.4 | $F8634A | 1 | 1 |
| `a4000tks31` | A4000T, 3.1 40.70 | scsidisk 40.20 | $F85EFC | 1 | 1 |
| `a4000tks32` | A4000T, 3.2 | IDE_scsidisk 47.4 | $F8634A | 1 | 1 |
| `a3000ks204` | A3000, 2.04 | scsidisk 37.19 | $FB0C82 | 0 | 0 |
| `a3000ks31` | A3000, 3.1 | scsidisk 40.12 | $F881A6 | 0 | 0 |
| `a3000ks32` | A3000, 3.2 | scsidisk 47.4 | $F8629E | 0 | 0 |
| `a500pks204` | A500+, 2.04 | none | - | 0 | 0 |
| `a3000roma4000` | A3000 with the A4000 3.1 ROM | none | - | 0 | 0 |
| `a3000roma1200` | A3000 with the A1200 3.1 ROM | none | - | 0 | 0 |
| `ks314setpatch` | A1200, 3.1.4, SetPatch | IDE_scsidisk 45.7 | $FB5C3E | 1 | 1 |
| `ks32setpatch` | A1200, 3.2, SetPatch (LED patch on by default) | IDE_scsidisk 47.4 | $FB5A22 | 1 | 1 |
| `ks31softload` | A1200, 3.1 ROM, IDE 45.7 LoadModule'd | IDE_scsidisk 45.7 | $228896 | 0 | 0 |
| `a3000softload` | A3000, 3.1 ROM, A3000 47.4 LoadModule'd | scsidisk 47.4 | $7828872 | 0 | 0 |

Checks that the test can fail: with `safety_is_internal_ide` forced to return 1, `a3000ks31` and `ks31softload` both fail with "expected 0, got 1".

### What this does not prove

- **An A3000 ROM for OS 3.1.4.** None is in the collection, so that build was neither read nor run. It gives 1 only if it has an IDE `lea` and neither SCSI address, which an SDMAC driver would not. **UNVERIFIED**. The 3.2 A3000 ROM was run, and the 3.2 A3000 module from disk gave 0 too.
- **An A4000T with only the NCR driver.** WinUAE presents IDE and NCR as one board on `chipset_compatible=A4000T`, and could not present NCR alone. `scsi.device` there is the IDE driver and gives 1, correctly, since that port exists. `NCR scsi.device` did not appear in the device list on either ROM, even with the board present, so its answer rests on the ROM sweep and on its different name. With no `scsi.device` at all, the answer is 0.
- **An A3000 soft-kicked with another machine's ROM on real hardware.** On WinUAE, both the A4000 and the A1200 3.1 ROMs left no `scsi.device`, because their init probes find no IDE hardware. The A4000 probe reads task-file registers at `$DD2020`, which a real A3000 decodes differently. **UNVERIFIED**. If such a driver did come up it would answer 1, but it talks to IDE registers only, so it could not reach a disk on the SCSI bus. The wizard then finds no card and refuses.
- **Third-party drivers named `scsi.device` in ROM or an accelerator's own ROM at another address.** They give 0 unless they carry the exact instruction and a `scsi.device` RomTag in that range.
- **Real machines.** **UNVERIFIED**, as everything here.

## Other things seen

- **Geometry differs by Kickstart on the same size of card.** A 256 MB card got 519 cylinders on KS 3.1 and 924 on KS 3.2, so a card's cylinder numbers are not portable
  arithmetic. Layout code should work from the geometry it is given. **EMULATED**.
- The Format confirmation already names the wrong volume in the single-run case ("Format DH1 ... mounted as DH0:"), so normal-UI users rewriting a table can hit this today. It is outside the wizard.

## What this does not prove

- Real drives or CF cards. **UNVERIFIED**. WinUAE's drive model accepts what a real card might not.
- A completely full volume, the disk validator on a big volume, and bad block handling. **UNVERIFIED**. Volumes here were filled to about 55%.
- Programs above a 2 GiB partition. The FAQ warns that old installers do 32-bit free-space sums and fail on a system partition over 2 GiB. **UNVERIFIED**, and the reason not to raise the limit on this evidence. The Workbench installers are tested later in this project.
- Data past the top of this card. Nothing was written above 4,000,000,000 bytes, so the 4 GiB limit and any wrap there were not exercised by `limit` and `offset`. See "The 4 GiB proof" for the 8 GB card.
- Partition size on KS 3.0, 3.1.4 and 3.2. The size tests ran on KS 3.1 and KS 2.05 only, as specified.
- Single-run flow on KS 2.05, 3.0, 3.1.4 or A600 and A4000 machines. Two ROMs failing is enough to rule it out.

## The wizard's run

`run` on `ks205a`, `ks31a600`, `ks31a1200`, `ks31a4000`, `ks314` and `ks32`, each scenario with its own boot. All **EMULATED**. Each card's table and volumes are checked from the host and through the device and volume names.

**Restart with the floppy out.** Between the press of Restart and `ColdReboot`, breakpoints on these in the libraries' jump tables: `OpenLibrary`, `OldOpenLibrary`, `OpenDevice`, `OpenResource`, `Open`, `Lock`, `LoadSeg`, `NewLoadSeg`, `ErrorReport`, `OpenFont`, `EasyRequestArgs`, `BuildEasyRequestArgs`, `AutoRequest`. HDPart's process calls none of them on that path, and any caller but these two fails the test. Two calls come from other tasks. `OpenLibrary` by the test's own `volcheck`, and `OpenFont` by `input.device`, which is Intuition handling the press: graphics' `OpenFont` only looks among fonts already in memory, so it can't load anything. **INFERRED** for that last part. This does not test a real floppy being taken out: the emulated boot volume stayed in place.

**Flush.** A flush packet goes to every volume at the end of the run and again on Restart. With the emulator cut off at the point of `ColdReboot`, the card alone mounts all volumes validated. Formatting writes through, so that test would very likely pass with no flush too: it proves the end state, not that the flush was needed. **INFERRED.**

**Not covered:** a drive's own write cache. Real cards may hold writes the emulator's drive doesn't. **UNVERIFIED.**

## The restart path and the popup

`resume` on `ks205a`, `ks31a600`, `ks31a1200`, `ks31a4000`, `ks314` and `ks32`, each scenario with its own boot. All **EMULATED**.

**First run on a used card.** The table goes on with the marker, the read-back compares it field by field, and the driver is asked to let go of it (`CMD_UPDATE`) before the offer appears. Nothing is formatted: a breakpoint on `format_partition` is never hit, and everything past the table's blocks has the same hash. The only way on from the offer is Restart, since the old volumes are live over a table that no longer describes them. Between the press and `ColdReboot` the same watch as the Finished screen finds nothing loading.

**Resume mode.** After the restart every partition is auto-mounted through the marked table. Only the popup's Continue may format through such a mount, and only if it has this partition's name, driver, unit, bounds, cylinder size and filesystem, and nothing else stands on those blocks or under that name. Each of these was changed in guest memory on the live `DH1` after `System` was formatted: the step fails, `Work` and `Games` are not formatted, the marker stays and the next start finishes. One of them, a device whose driver name can't be read, is not listed as a mount at all. It went through to the format until the guard also looked for any device of that name.

**Real failures.** `ACTION_FORMAT`'s answer is forced to failure at `DoPkt`, inside `format_partition`, so the mount is really live, unlike the fake in `run-failformat`. Blank card, second format, no restart: a further run sees the mounts, marks the table, Restart, popup, Continue, volumes valid on the host. Blank card, first format, restart first: no popup as there is no marker, then the same. Resume, second format: marker stays through a power cycle, popup again, finished.

**The marker that can't be cleared.** A write failure leaves the marker and a read-back failure may not, so the box says "may". With the marker there, the next start brings the popup. Its wording stays true: Continue would erase them, and Cancel was run and keeps the volumes, valid on the host.

**Cards that do not bring the popup,** each with `g_qs_screen` 0, one run of the check, no wizard window, the main window untouched and the card's hash unchanged: blank; PC; another tool's table; the same layout with no marker; HDPart's normal Save; a Save over a marked table; a finished wizard card; after Cancel; a marked card it started from; a marked card with a second drive; a marked card with DH3 taken; a marked table with its bounds, start, name, filesystem, MaxTransfer, bootable flag, partition count, marker digit, geometry or marker padding edited with the checksum fixed; a bad checksum; the marker outside what the checksum covers. On `ks31a1200` also a marked table on `a3000ks31` and on `ks31softload`, both answering 0 from the IDE check. The unedited table does bring it, which is what makes each of those mean something.

**Input during the startup check.** The check runs with the main window up, so a press can land in it. The test holds the check open by sending HDPart through `Delay` on the way into its last read, so the press lands inside it every time. With no popup it is queued and acted on after the check: the wizard opens once. On a marked card the popup still asks and waits and the press is not taken for its answer, and anything queued behind the popup (Quick setup, Driver, a shortcut key) is dropped when it is over, so it can't open the erase screen on a card that was just dealt with. The status line says "Looking for your card..." during the check. The check takes about 0.2 to 1.5 s of wall time on the emulator, depending on the machine and how many drives answer. The harness now waits for the check to end before arming a breakpoint, or the check itself hits it (`run-unit1` failed that way on two machines).

**Files on the card.** The popup adds "This card already has files on it." when a partition of the marked table has a valid root block with an entry in its directory. It looks only at the new table's partitions, so files in an old layout whose root blocks are elsewhere do not count. Tested with a real file on Work, then the same card formatted and empty (no line), a retry after a failed format (no line) and failed reads (no line, the popup otherwise the same).

**Not covered.** A real DH0 held by another disk with a marked card: the ROM mounts the card first, so the card gets DH0 and the other disk is renamed, and that is rightly a popup. A card's own write cache: `scsi.device` is asked to update, and no command reaches the drive's cache that we know of. **UNVERIFIED.** How long the check at startup takes on a real port with an empty position: it probes both units on every start with the IDE check passing. Not noticeable here. **UNVERIFIED** on hardware.

## The 4 GiB proof

`run` scenario `proof4g`, 8,000,000,000-byte sparse card: a full run, 5 MB `volcheck` on each volume, every sector found inside its partition, restart, the volumes mount validated, 1 MB more on each. With the emulator stopped, the host reads from byte 4 GiB to the end. **EMULATED.**

| Key | Geometry the wizard used | Last partition ends at byte | Bytes scanned from 4294967296 | Result |
|---|---|---|---|---|
| `ks31a1200` | 15500 x 16 x 63 | 4294950912, 16384 below 4 GiB | 3705032704 | all zero |
| `ks32` | 62003 x 4 x 63 | 4294950912, 16384 below 4 GiB | 3705032704 | all zero |

The scan can fail. Each run first makes a throwaway card with `truncate` and checks the scan on it: blank gives nothing, a byte at 4 GiB - 1 gives nothing, a byte in the last place and a byte at exactly 4 GiB are each found at their offset.

This shows nothing was written up there in these runs, with only a few MB of files on each volume. Filling the top partition is `limit`, `offset` and `run-synth`, not this. Their last results are from before this code. **UNVERIFIED** on real cards.

## Every ROM, to the refusal or the card screen

`card` with `QUICK_CARD_ONLY=blank`, on every key below. A refused machine runs its one refusal scenario whatever that variable says. Every refusal checks by hash that the card was not written. All **EMULATED**, all passed, and none could not run.

| Key | Result | Figures or reason |
|---|---|---|
| `ks205a`, `ks205b`, `ks30`, `ks31a600`, `ks31a1200` | Card screen, 255 MB | 519 x 16 x 63, drive 523908 blocks |
| `ks31a4000` | Card screen, 255 MB | 519 x 16 x 63 (the wizard's own figures), drive 523907 blocks |
| `ks314`, `ks32`, `ks314a4000`, `ks32a4000` | Card screen, 256 MB | 924 x 9 x 63, drive 524288 blocks |
| `a3000ks204`, `a3000ks31`, `a3000ks32` | Refused, nothing written | "can't confirm this setup" |
| `ks31softload`, `a3000softload` | Refused, nothing written | "can't confirm this setup" |
| `a500pks204` | Refused, nothing written | "no IDE port with a card on it" |
| `a3000roma4000`, `a3000roma1200` | Refused, nothing written | "no IDE port with a card on it": these leave no `scsi.device` on the emulator |

These agree with the IDE check table above and with the geometry section below. `a3000roma4000` and `a3000roma1200` were not in `card` before. They take their ROMs from `QUICK_ROM_A3000ROMA4000` and `QUICK_ROM_A3000ROMA1200`, here the 3.1 A4000 and A1200 ROMs.

Other `card` scenarios were not repeated on these keys. Not run under `card`: `a4000tks31`, `a4000tks32`, `ks314setpatch` and `ks32setpatch`, which the test does not accept. They have only the `ide` results above.

## Why `run-dirty` failed on `ks31a4000`

A fault in the test is the supported explanation, not a proven one for the original failures. About 1 run in 3 failed with "timed out waiting for dialog to close".

What was shown: the mechanism below, under an injected delay. The old wait failed 5 of 5, the fix passed 3 of 3, and the control with no press still timed out. The natural 1-in-3 failure was never reproduced on untouched code.

The original failing run's log and screenshot were inspected. At the timeout the open windows are "Quick setup: your card" and the main window, and the screenshot shows the wizard on its card screen. That fits the wizard having moved on while the test waited, but that run did not log the harness's looks, so it cannot show why the wait missed the close.

`clickClose` took a dialog as still open while a window stood at its address with the same first gadget. The prompt "Quick setup" closes, and "Quick setup: your card" opens on the same memory. Measured on `ks31a4000`: both at `$2cf3fc`, first gadget `$2cf4f4` for both. If the harness's first look falls between the two windows, it sees the close. If not, it waits 20 s for a dialog that has already gone, with the wizard sitting correctly on its card screen.

| What was run on `ks31a4000` | Result |
|---|---|
| Original wait, first look at once (log lines added) | 3 of 3 passed. Each saw the gap |
| Original wait, first look 300 ms later | 5 of 5 failed: 3 with a wait added after the press, 2 with one added before the first look. Every look logged shows the new window at the old address. No press was sent after Proceed |
| The fix, first look 300 ms later | 3 of 3 passed |
| The fix, button deliberately not pressed | Fails with the same timeout, so the mark does not pass by itself |
| The fix, final code | 8 of 8 passed in a row |

The same 300 ms delay also fails `ks31a1200` on the original wait, and passes with the fix. `ks31a600` and `ks32` passed with the delay. So this is not specific to the A4000. Why it showed there: its gap is short, since the machine is fast. That reason and the rate of 1 in 3 are **INFERRED** from these runs, as the gap itself was not timed.

The pointer was not the cause: 82 presses logged across those failing and passing runs, none more than 2 pixels off target, with the old one-read exit in `moveTo`.

The fix marks the dialog in `Window.UserData`, which HDPart does not use. A new window starts without the mark. `moveTo` now needs two equal reads as well, and each press is logged to `press.log` beside the emulator log, but that is a separate hardening and did not fix this.

`pickMenu` (`tests/target/emu.mjs`) still exits after one position read: its loop that drags to the menu item did not get the two-read change.

## What ran on the final code

All **EMULATED**, source unchanged from 75ded88, harness at 441a6f6.

| Test | Result |
|---|---|
| `tests/run-host-tests.sh`, `tests/verify-rdbtool.sh` | pass |
| `make target-test` on `ks204`, `ks31`, `ks32` | 37 of 37 on each |
| `resume` on `ks31a600`, `ks314`, `ks205a`, `ks32`, `ks31a4000` | 50 of 50 scenarios on each |
| `resume` on `ks31a1200` | 52 of 52, with the two runs on `a3000ks31` and `ks31softload` |

`resume-fileread` passed inside the full `resume` on all six. Its earlier failure's cause was never proved, and these passes do not prove it either.

Run before the `moveTo` and `clickClose` changes, on the same source: `proof4g`, the `card` table above, and `run` on `ks30` (37.64, 15 of 15 scenarios).

`limit` and `offset` now count as passed only when the run's own `ok` holds. They had counted any run that finished. Their stored results from the runs above all have `ok` true, so those results stand. They were not run again.

**Not run:** the full `make target-quick` matrix, `run` on keys other than `ks30` since these changes (only `proof4g` and `run-dirty`), 4 GB cards in this round, and any real hardware.

## The 0.12 final round

Date: 2026-11-04. All **EMULATED** except the host tests. Source: the 0.12 fixes, in one commit on top of 5cbe0ba.

The fixes: the layout stops one cylinder short when a cylinder size divides 4 GiB exactly, so a transfer's 32-bit end offset can't wrap to 0. 1008-block cylinders give the same table as before. The line on the choice screen says the first 4 GB is shared when a card is bigger than that. `qs_read_root` refuses a table that would overflow. A failed `CreateContext` on the card screen now says so. The driver cycle gadget is set again after the popup.

The new host tests failed before the fix on 16 x 32 and 32 x 64 and pass after it. The 1008-block test passed on both, so its figures are the old code's.

| Test | Result |
|---|---|
| `tests/run-host-tests.sh`, `tests/verify-rdbtool.sh` | pass |
| `make target-test` on `ks204`, `ks31`, `ks32` | 37 of 37 on each |
| `limit` on `ks31a1200`: 3584 MB and 2048 MB | both read back correctly, 0 problems |
| `offset` on `ks31a1200`: 2048 MB starting at 1760 MB | reads back correctly, 0 problems |
| `run` on `ks31a1200` | 16 of 16 scenarios, `proof4g` among them |
| `run` on `ks205a` (37.55) | 15 of 15 scenarios |
| `resume` on `ks32` | 48 of 48 scenarios |
| `run` scenario `proof4g` on `ks32` | passed |
| `card` scenario `confirm8` on `ks31a1200` | passed. Now also asserts the new line, on an 8 GB card with Work and Games ticked, and a screenshot shows it inside its box |

The 48 scenarios on `ks32` are the same list as the last round's log, which also has 48 where "What ran on the final code" says 50. Nothing was dropped: that count looks wrong, and was left as written.

`proof4g` gave the same figures as above on both keys: the last partition ends at byte 4294950912, 16384 below 4 GiB, and 3705032704 bytes from 4 GiB to the end are all zero.

No emulated run reached the changed case of the layout: these machines use 16 x 63 and 4 x 63 cylinders, which do not divide 4 GiB. Only the host tests cover it. The refusal of an overflowing table and the `CreateContext` message have no test that forces them. The gadget reset only ran as part of `resume`, with nothing checking the gadget.

**Not run in this round:** the full `make target-quick` matrix, `run` and `resume` on other keys.

## No coverage

Nothing here has tested these. All **UNVERIFIED**.

- A wizard `run` on `scsi.device` 37.34 (`ks205b`).
- A real Install disk run on a wizard-made card.
- A boot from a wizard-made card with no floppy.
- Restart with the floppy actually swapped.
- A lone master that also answers as unit 1, as some real IDE adapters do (a "ghost slave"). It would give a refusal. Not checked on hardware.

## Drive geometry on the A4000

Measured from `TD_GETGEOMETRY` in WinUAE, card on emulated IDE. **EMULATED.** Real drives and adapters may report differently.

| ROM, driver | Card | Cylinders | dg_CylSectors | dg_Heads | dg_TrackSectors | dg_TotalSectors | Cylinders x CylSectors |
|---|---|---|---|---|---|---|---|
| 3.1 A4000, 40.20 | 256 MiB | 924 | 63 | 1 | 63 | 523907 | 58212, about 1/9 of the card |
| 3.1 A4000, 40.20 | 4 GB | 31001 | 63 | 1 | 63 | 7812251 | 1953063, about 1/4 |
| 3.1 A4000, 40.20 | 8 GB | 62003 | 63 | 1 | 63 | 15624755 | 3906189, about 1/4 |
| 3.1.4 A4000, 45.7 | 256 MiB | 924 | 567 | 9 | 63 | 524288 | 523908 |
| 3.1.4 A4000, 45.7 | 4 GB | 31001 | 252 | 4 | 63 | 7812500 | 7812252 |
| 3.2 A4000, 47.4 | 256 MiB | 924 | 567 | 9 | 63 | 524288 | 523908 |
| 3.2 A4000, 47.4 | 4 GB | 31001 | 252 | 4 | 63 | 7812500 | 7812252 |

So 40.20 reports the right cylinder count for 9 or 4 heads, but gives 1 head and a cylinder size of 63. Its cylinder size is wrong as well, so dg_CylSectors cannot repair it.

On 3.1 A1200, `TD_GETGEOMETRY` leaves the structure at zero, and the 16 x 63 figures seen there come from the READ CAPACITY fallback in `dev_geometry`. The `limit` and `offset` tests above ran through that path.

**Wizard:** when cylinders x heads x sectors differs from the total by more than total/64 either way, it uses `disc_synth_chs(total)`, 16 x 63. This is safe only if the driver addresses by offset, and `card` scenario `synth` tests that on 3.1 A4000. **EMULATED.** An 8 GB card holds a table with those figures and three volumes formatted on the Amiga, the last ending at block 8388575, the top of what the wizard uses. Every root block sits where the table puts it, every file sector is inside its partition, data reaches the last block, nothing is written past it, and the files read back after a restart.

The same proof on the wizard's own table, `run` scenario `run-synth`, 3.1 A4000, 8 GB card, System, Work, Games and Demos, table and volumes made by the wizard. **EMULATED.** The last partition ends at block 8388575. Files on all four volumes, and 503 MB of filler, then 64 MB and 128 MB files on Demos. On the host: every file sector whole and inside its partition, the highest at block 8388575, all zero above it. Everything but the filler read back after a restart.

**Main window: not changed, and it has this gap.** It uses the driver's figures as they are. On 3.1 A4000, a blank 256 MiB card shows as "924 cyl" with 63 blocks per cylinder, so it offers 28 MB of 256 MB, and a 4 GB card offers 953 MB. **EMULATED**, read from the model after Scan on a blank card. Nothing is lost, but the space is unusable from there. A table saved with the real figures on such a machine reads back normally, since it carries its own.
