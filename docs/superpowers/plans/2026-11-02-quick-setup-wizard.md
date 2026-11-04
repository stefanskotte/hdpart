# Quick Setup Wizard Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A guided Quick setup path in HDPart that takes a blank or used CF/SD card on an internal IDE port to a state ready for a Workbench install from disks.

**Architecture:** Pure layout and marker logic in a host-testable `src/quick.c` plus `src/rdb.c`. Wizard screens in `src/gui.c`, following its existing dialog pattern and reusing `format_partition`, `safety_*`, discovery and `dev_inquiry_model`. A second on-target suite, `tests/target/quick.mjs`, runs on emulated IDE because the existing suite's `uaehf.device` disks never reach this code.

**Tech Stack:** m68k C, GadTools V37, Bartman toolchain, WinUAE with GDB server via `tests/target/emu.mjs`, a small m68k guest tool.

**Spec:** `docs/superpowers/specs/2026-11-02-quick-setup-wizard-design.md`. Approved mockups: `.superpowers/brainstorm/3891-1793654791/content/wizard-flow.html` (local, not committed).

## Global Constraints

- Runs on KS 2.05 and up. GadTools V37 only, Workbench screen, topaz 8, DrawInfo pens, as the rest of `src/gui.c`.
- No automatic popup, except the setup-pending case, whose only trigger is the marker.
- No technical terms on wizard screens: no MaxTransfer, mask, cylinders, RDB or DosType.
- FFS International (`RDB_DOSTYPE_FFS_INTL`) on every partition. MaxTransfer `RDB_DEFAULT_MAXTRANSFER`; mask, buffers and buffer memory at HDPart's defaults.
- DH0 volume "System", 500 MB, bootable. Then DH1 upward in the order Work, Games, Demos, with those volume names.
- At most the first 4 GiB on every Kickstart. At most 2 GiB per partition, unless Task 1 proves otherwise.
- Commodore ROMs and disks never enter the repo. Tests find them through environment variables.
- Never `git add` the repo root's `Workbench*.zip` files or `.vscode/settings.json`.
- Standard run for any task that changes `src/`, with the toolchain on PATH (see `tests/target/README.md`): `make && make hd && ./tests/run-host-tests.sh && ./tests/verify-rdbtool.sh && make target-test`, expecting every Kickstart to report 0 failed.
- A failed test stops the task. Report it with its output; do not retry past it.

## Review Focus

- The card is swapped or removed between screens: the erase must act on the card that was confirmed, or stop. Test in Task 6.
- "Back to HDPart" after finishing: the main window must show the new table and not stale data, with nothing marked unsaved. Test in Task 6.
- A device name such as DH0 is already taken by another mounted device: the wizard stops and explains, before writing anything. Test in Task 4.
- A write or format failure partway: names the step, and leaves a card that a further run can recover. Test in Task 6 with `failNextCall`.
- A card with a damaged or foreign RDB, including one with a marker-like string but a bad checksum: no popup, treated as used. Test in Task 3 and Task 7.

---

### Task 1: Test the engine on emulated IDE and record the results

Settles two questions the spec leaves to test. No product code changes.

**Files:**
- Create: `tests/target/quick.mjs`, `tests/target/guest/volcheck.s`, `docs/reference/quick-setup-tests.md`
- Modify: `Makefile` (build volcheck, add `target-quick`), `tests/target/README.md`

**Interfaces:**
- Produces: `QuickSuite` in `quick.mjs`, built like `Suite` in `run.mjs`, taking `{key, rom, machine}` and a card file, with the card as `hardfile2=rw,:<file>,0,0,0,512,-128,,ide0`. A1200 config as in `run.mjs`. A600: `cpu_model=68000, chipset=ecs, chipset_compatible=A600, chipmem_size=4, fastmem_size=8`. A4000: `cpu_model=68040, fpu_model=68040, cpu_24bit_addressing=false, cpu_compatible=false, chipset=aga, chipset_compatible=A4000, chipmem_size=4, fastmem_size=8`. These were proven to boot during the ROM check.
- Produces: ROM paths via env vars `QUICK_ROM_<KEY>`, skipping with a printed line when unset. Keys: `ks205a`, `ks205b`, `ks30`, `ks31a600`, `ks31a1200`, `ks31a4000`, `ks314`, `ks32`.
- Produces: `volcheck <volume> <megabytes>`, guest tool that writes a pattern file of that size to the volume through dos.library, reads it back, compares, and stores magic, error and byte counts at `$100` for the harness to read, like the earlier probe.

- [ ] **Step 1: Write `volcheck.s` and the harness, and prove them on a fresh partition**

Make a blank card, use the existing UI (driver `scsi.device`, Split, Save, Format), then run volcheck with 5 MB. Expected: no error, all bytes compared.

- [ ] **Step 2: Used-card test**

On KS 3.1 A1200: create a card with DH0 and DH1 formatted, write a volcheck file, restart so they auto-mount. Without restarting again, replace the table with three partitions at different cylinders using the existing UI, Save, Format each, and run volcheck on each. Then restart and run volcheck again. Record any requester, crash, wrong volume size, or read failure. Repeat on KS 3.2.

- [ ] **Step 3: 2 GiB test**

On KS 3.1 A1200, a 4 GB card with one FFS International partition of about 3.5 GB: format, Info the volume, and run volcheck for 64 MB near the end of the partition by filling first. Repeat with a 2 GiB partition. Record whether the larger one reads back correctly. KS 2.05 too.

- [ ] **Step 4: Record results and decide**

Write `docs/reference/quick-setup-tests.md` using the labels from the ROM findings doc. State plainly: single-run flow safe, yes or no; partition limit. Unclear counts as no. If 2 GiB is proven wrong, the limit may rise only as far as proven. Add the decision to the spec under "Used Amiga cards".

- [ ] **Step 5: Commit**

```bash
git add tests/target docs/reference/quick-setup-tests.md docs/superpowers/specs Makefile
git commit -m "test: IDE harness and engine results for Quick setup"
```

### Task 2: Reliably tell internal IDE from everything else

**Files:**
- Modify: `src/safety.h`, `src/safety.c`, `src/gui.c` (sets `g_qs_is_ide`), `tests/target/quick.mjs`, `docs/reference/quick-setup-tests.md`

**Interfaces:**
- Produces: `int safety_is_internal_ide(void)` in `src/safety.c`, under `HDPART_AMIGA`. 1 only when `scsi.device` in the running system is proven to be an internal IDE driver. 0 for anything else, including doubt.

- [ ] **Step 1: Find the check**

Read the ROMs in the local ROM collection. Known: from 45.x the ID string is `IDE_scsidisk`; before that A3000 and IDE both say `scsidisk`. The A3000 SCSI driver uses SDMAC at `$DD0000` and has no IDE translator. Candidates: the IDE base addresses `$DA0000` and `$DD2020` used by the driver, or a marker in the device's own code. Prefer reading what the driver is over probing hardware registers. Record what was checked in the tests doc.

**Stop and ask the user if no check holds on every ROM in the Test step.** Do not ship a heuristic that could pass on an A3000.

- [ ] **Step 2: Write the failing on-target test, then implement**

`quick.mjs`, `Quick setup: IDE check`: read the result via a debug symbol `g_qs_is_ide` set at startup. Expect 1 on A600 2.05 (both ROMs), A1200 3.0, 3.1, 3.1.4, 3.2, A4000 3.1, 3.1.4, 3.2, and A600 3.1. Expect 0 on A3000 KS 2.04, 3.1, 3.2 and A500+ 2.04 (no `scsi.device`) and on A4000T's `NCR scsi.device` when only that is present, if the emulator can present it.

- [ ] **Step 3: Run the tests**

Run: `node tests/target/quick.mjs` with the ROM env vars set. Expected: all pass, none skipped except as recorded.

- [ ] **Step 4: Commit**

```bash
git add src/safety.c src/safety.h src/gui.c tests/target/quick.mjs docs/reference/quick-setup-tests.md
git commit -m "feat: detect an internal IDE scsi.device"
```

### Task 3: Layout arithmetic and setup-pending marker

**Files:**
- Create: `src/quick.c`, `src/quick.h`, `tests/test_quick.c`
- Modify: `src/rdb.h`, `src/rdb.c`, `tests/test_rdb.c`, `tests/run-host-tests.sh`, `src/gui.c` (`gui_save` clears the marker)

**Interfaces:**
- Produces in `quick.h`: `enum { QS_WORK = 1, QS_GAMES = 2, QS_DEMOS = 4 };`
- Produces: `int quick_layout(RdbModel *m, uint32_t mask)`. `m` is already `rdb_init_model`-ed with the card's geometry. Clears its partitions and fills them. Returns `RDB_OK` or `QS_ERR_TOO_SMALL`.
- Produces: `const char *quick_volume_name(uint32_t mask, int part_index)`, "System", then the ticked names in order.
- Produces in `RdbModel`: `uint8_t setup_pending`, 0 for none, else `0x80 | mask`.
- Produces: `int quick_pending_valid(const RdbModel *m)`. 1 only when `setup_pending` is set and the table matches it: partition count is 1 plus the ticked count, names DH0 upward, DH0 bootable, all FFS International.

Rules for `quick_layout`:

- Usable blocks: card blocks capped at 8388608 (4 GiB at 512 bytes). Set `m->hi_cyl` so no partition ends past that.
- Refuse below 128 MB usable.
- System size: `min(500 MB, usable / 2)`.
- Each ticked partition: remaining cylinders divided evenly, capped at 2 GiB or the limit from Task 1. The last ticked one absorbs remainder cylinders within the cap.
- `m->hi_cyl` caps only. `m->cylinders` stays the card's own.
- DH0 `bootable = 1`, `boot_pri = 0`.

Marker in the RDSK block: ControllerVendor at offset 188 is `"HDPART  "`, ControllerProduct at offset 196 is `"QUICKSETUP      "`, ControllerRevision at offset 212 is `'0' + mask` followed by three spaces. These sit inside the 64 checksummed longs (`RDB_SUMMEDLONGS` covers 256 bytes). `rdb_parse` sets `setup_pending` only on an exact match with a valid checksum. `rdb_serialize` zeroes the block and writes these only when `setup_pending` is set, so tables from other tools never carry it.

- [ ] **Step 1: Write failing host tests**

```c
/* test_quick.c, cylinder size 1008 blocks unless noted */
static void test_layout_8gb_two_ticked(void)
{   /* usable 4096 MB: System 500, Work and Games share the rest evenly */
    /* CHECK num_parts == 3; names DH0, DH1, DH2; parts[0].bootable == 1;
       parts[1].bootable == 0; sizes of parts[1] and parts[2] within one cylinder;
       last cylinder end < 8388608 blocks; dos_type == RDB_DOSTYPE_FFS_INTL on all;
       maxtransfer == RDB_DEFAULT_MAXTRANSFER on all; rdb_validate == RDB_OK */
}
static void test_layout_partition_cap(void)
{   /* 8 GB, QS_WORK only: parts[1] is at most 2 GiB and the remainder stays unused */
}
static void test_layout_system_only(void)
{   /* mask 0: one partition, about 500 MB */
}
static void test_layout_small_cards(void)
{   /* 128 MB, all ticked: System is about 64 MB, valid; 64 MB returns QS_ERR_TOO_SMALL */
}
static void test_volume_names(void)
{   /* mask QS_WORK|QS_DEMOS: System, Work, Demos */
}
```

Add to `test_rdb.c`: the marker round-trips through `rdb_serialize` and `rdb_parse`; a table with `setup_pending == 0` writes zero bytes at offsets 188-215; a block with the marker text and a bad checksum does not set it (`rdb_parse` must fail or leave it 0); `quick_pending_valid` is 0 when the partition count or a name disagrees.

- [ ] **Step 2: Run to see them fail**

Run: `./tests/run-host-tests.sh`. Expected: fails to compile, `quick_layout` undefined.

- [ ] **Step 3: Implement, add `tests/test_quick.c` to `run-host-tests.sh`, and set `g_model.setup_pending = 0` at the top of `gui_save`**

- [ ] **Step 4: Run the standard run**

Expected: host tests pass, `make target-test` unchanged at 0 failed.

- [ ] **Step 5: Commit**

```bash
git add src/quick.c src/quick.h src/rdb.c src/rdb.h src/gui.c tests/test_quick.c tests/test_rdb.c tests/run-host-tests.sh
git commit -m "feat: Quick setup layout and setup-pending marker"
```

### Task 4: Entry point, card screen and refusals

**Files:**
- Modify: `src/gui.c`, `tests/target/quick.mjs`

**Interfaces:**
- Consumes: `safety_is_internal_ide`, `safety_classify`, `discover_probe_driver`, `dev_inquiry_model`, `fsres_dosnode_exists`, `rdb_parse`.
- Produces: `static void gui_quick_setup(void)` in `gui.c`, the wizard's entry, running one screen function per step. `GID_QUICK` added at the end of the gadget enum. A menu item in the Disk menu, added at the end so `ID_*` indices do not shift. A `static int g_qs_screen` debug symbol set to the current screen number, 0 when closed.
- Produces: `static int qs_find_card(DiscDisk *out)`, returning the card or an index into a table of refusal messages.

Layout: `gui_draw_details` shows the mockup's panel with a real button while no driver has been picked. The button is hidden once one is, in the same way the Filesystems button is managed. Once a driver has been picked, the panel stays gone for that session.

- [ ] **Step 1: Write failing on-target tests**

`Quick setup: button shows until a driver is picked`; `Quick setup: blank card shows Empty`; `used Amiga card lists its partitions`; `PC card says Not an Amiga disk`; `8 GB card says this Amiga can use 4 GB`; refusals, each asserting no write to the card by comparing its hash: `no card`, `two drives`, `booted from this card`, `A3000`, and `DH0 taken`, where another device already holds that name.

- [ ] **Step 2: Implement, then run**

Run: `node tests/target/quick.mjs`. Expected: all pass. Take screenshots to `shots/quick-*.png` and check them by eye against the mockup.

- [ ] **Step 3: Run the standard run, then commit**

```bash
git add src/gui.c tests/target/quick.mjs
git commit -m "feat(gui): Quick setup entry, card screen and refusals"
```

### Task 5: Partition choice and last-check screens

**Files:**
- Modify: `src/gui.c`, `tests/target/quick.mjs`

**Interfaces:**
- Consumes: `quick_layout`, `quick_volume_name`.
- Produces: `static int qs_choose(const DiscDisk *card, RdbModel *m, uint32_t *mask)` returning next, back or cancel, and `static int qs_confirm(const DiscDisk *card, const RdbModel *m, uint32_t mask)` likewise. Neither writes to the card.
- Reuse: the disk map drawing, by giving `gui_draw_bar` a helper that takes a model and rectangle, so the wizard map cannot drift from the main one. No RDB label on this map: the mockup's blank block is unlabelled for the wizard, per the no-terms rule.

Exact copy from the mockup: "What else do you want on the card?"; "System is always included, and is what the Amiga starts from."; "Only System. The rest of the card stays unused."; "The rest of the card is shared evenly between your choices."; "Each partition is limited to 2 GB, so <n> MB stays unused."; the button "Erase and set up".

- [ ] **Step 1: Write failing on-target tests**

`choose: ticking updates the layout`, reading the model via symbols; `choose: Back and Cancel write nothing`; `confirm: table lists device, volume, size`; `confirm: Cancel writes nothing`.

- [ ] **Step 2: Implement, run, check screenshots against the mockup, run the standard run**

- [ ] **Step 3: Commit**

```bash
git add src/gui.c tests/target/quick.mjs
git commit -m "feat(gui): Quick setup partition choice and confirmation"
```

### Task 6: Working and finished screens

**Files:**
- Modify: `src/gui.c`, `tests/target/quick.mjs`

**Interfaces:**
- Consumes: `rdb_serialize`, the read-back in `gui_save` (factor it into `static int qs_write_verified(RdbModel *m)` used by both), `format_partition`.
- Produces: `static int qs_run(const DiscDisk *card, RdbModel *m, uint32_t mask)`, doing the steps with the checklist drawn live. Each step's failure names it and says what to do next.
- Before the first write, re-read the card's `dev_inquiry_model` and size and stop if they no longer match what was confirmed.
- After each format, lock the volume by name and `Info()` it. A failure fails the step.
- Restart button: `ColdReboot()` after closing the window. Back to HDPart: reloads the unit into the main window and leaves `g_dirty` 0.

- [ ] **Step 1: Write failing on-target tests**

`run: blank card ends with a valid table, verified from the host`, using `readRdb` to check three partitions, DH0 bootable, MaxTransfer `0x1FE00`; `run: every volume exists with the right name`; `run: volcheck passes on each`; `finished: Back to HDPart shows the table, not dirty`; `run: card swapped after confirm stops before writing`, by changing the probed model through memory; `run: a failed write names the step`, and `a failed format names the step and a re-run recovers`, both using `failNextCall`; `Restart reboots, and DH0 auto-mounts as System`.

- [ ] **Step 2: Implement, run, run the standard run**

- [ ] **Step 3: Commit**

```bash
git add src/gui.c tests/target/quick.mjs
git commit -m "feat(gui): Quick setup run with verification and finish"
```

### Task 7: The restart path and popup

Build this if Task 1 found the single-run flow is not proven safe, which the code suggests. If it is proven safe, skip the task and record that in the spec.

**Files:**
- Modify: `src/gui.c`, `tests/target/quick.mjs`

**Interfaces:**
- Consumes: `setup_pending`, `quick_pending_valid`, `qs_find_card`, `qs_write_verified`, `quick_volume_name`.
- Produces: `static void gui_quick_resume_check(void)`, called once at startup after the main window opens. It does nothing unless `safety_is_internal_ide()`, `qs_find_card` finds exactly one acceptable card, and `quick_pending_valid` holds on its table.
- In `qs_run`: when any of the card's blocks are mounted (`safety_classify` returns `DEV_MOUNTED`), write the table with `setup_pending = 0x80 | mask`, show "Restart with this same disk to finish setting up the card", offer Restart. Do not format.
- Resume screen: "Finish setting up this card? System, Work, Games. Everything on them will be erased." Continue formats all, then rewrites the table with `setup_pending = 0`. Cancel only rewrites the table with `setup_pending = 0`.

- [ ] **Step 1: Write failing on-target tests**

`used card: first run writes table, formats nothing, offers Restart`; `after restart: popup opens by itself`; `resume: Continue formats all, marker gone, no popup on next boot`; `resume: Cancel clears marker, no popup on next boot`; `resume: a format failure keeps the marker and the next run finishes`.

No popup on each of: blank card; PC card; table from HDPart's normal Save, including one saved over a marked table; finished wizard card; a marked table edited so it no longer matches; a marker with a bad checksum; A3000 config with a marked table; card present but booted from. Assert `g_qs_screen == 0` after the window opens, and a card hash unchanged.

- [ ] **Step 2: Implement, run, run the standard run**

- [ ] **Step 3: Commit**

```bash
git add src/gui.c tests/target/quick.mjs
git commit -m "feat(gui): Quick setup restart path with marker-only popup"
```

### Task 8: Coverage on every driver and the 4 GiB proof

**Files:**
- Modify: `tests/target/quick.mjs`, `Makefile`, `tests/target/README.md`

- [ ] **Step 1: Add the matrix**

Run the Task 4 to 7 steps on every key from Task 1. Cards: blank 4 GB and 8 GB, as sparse files made with `truncate`.

- [ ] **Step 2: Add the 4 GiB proof**

On the 8 GB card after a full run including volcheck, scan from byte 4 GiB to the end on the host. Expected: all zero. Also check that the last partition ends below 4 GiB with `readRdb`.

- [ ] **Step 3: Run**

Run: `make target-quick` with the ROM variables set. Expected: all pass. Any configuration that cannot run is printed and listed in the tests doc, not silently skipped.

- [ ] **Step 4: Commit**

```bash
git add tests/target Makefile docs/reference/quick-setup-tests.md
git commit -m "test: Quick setup on every IDE driver, with 4 GiB proof"
```

### Task 9: End to end with real Install disks, then ship-ready

**Files:**
- Create: `tests/target/install.mjs`
- Modify: `README.md`, `tests/target/README.md`, `src/gui.c` and `Makefile` (version 0.12 in the three usual spots), `docs/reference/quick-setup-tests.md`

**Interfaces:**
- Consumes: `QuickSuite`, env vars `QUICK_ADF_205`, `QUICK_ADF_31`, `QUICK_ADF_314`, `QUICK_ADF_32`, each a directory of ADFs.

Sources: 2.05 from the repo root `Workbench v2.05*.zip`, unpacked to a temp directory. 3.1 and later from the local Amiga collection (`roms/AmigaOS-3.1.4-A1200/`, `os-install-updates/workbench-3.2-adf/`; 3.1 is inside the OS 3.5 image in `AmigaOS_ALL.rar`, at `OS-Version3.1/Install3.1`).

The Installer has no gadget IDs to read. Drive it by mouse positions recorded per OS version, with a screenshot saved at each step so a person can see where it went wrong.

- [ ] **Step 1: For each of 2.05, 3.1, 3.1.4, 3.2**

Wizard makes the card. Follow the finished screen's own instructions: swap to Install, Restart, run the installer to System (DH0:), swapping disks as it asks. Remove the floppy, restart.

Expected: Workbench comes up from the card with no floppy (the Workbench task is present and `SYS:` is on `System`), and volcheck passes on the others. If the installer behaves differently from the finished screen's steps, fix the wording on that screen, not the test.

- [ ] **Step 2: Update docs and version**

Version 0.12 in `WA_Title`, the About text, `ADFVER`. README: what Quick setup is for and who it is not for. Tests README: how to run `target-quick` and `install.mjs`, and where the disks come from.

- [ ] **Step 3: Run everything**

Run: the standard run, `make target-quick`, and `node tests/target/install.mjs`. Expected: all pass.

- [ ] **Step 4: Commit, and stop**

```bash
git add tests/target README.md src/gui.c Makefile docs/reference/quick-setup-tests.md
git commit -m "feat: Quick setup ready for 0.12, proven with real Install disks"
```

Do not merge or tag. Merge and release are the user's call, after their real-hardware check.
