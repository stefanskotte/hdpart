# HDPart Mounter-Findings Fixes — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`) tracking. Companion to the embed-filesystem plan; executed interleaved with it.

**Goal:** Fix the confirmed bugs/gaps surfaced by the a4091/mounter cross-check (`docs/reference/mounter-comparison-findings.md`).

**Tech Stack:** C99 (host tests via `cc`); Bartman m68k-amiga-elf GCC for target; FS-UAE for on-target.

## Global Constraints

- Target baseline KS2.04 (exec V37); 32-bit math only; freestanding C.
- `src/rdb.c` host-compiles under `cc -std=c99 -Wall -Wextra` (no -Wno-unused-function): warning-free.
- Amiga-only code behind `#ifdef HDPART_AMIGA` (Makefile passes -DHDPART_AMIGA).
- Build/stage/test: `export AMIGA_BIN="/Users/sfs/.vscode/extensions/bartmanabyss.amiga-debug-1.8.2/bin/darwin"; export PATH="$AMIGA_BIN/opt/bin:$AMIGA_BIN:$PATH"`. After `make`, `make hd` before any FS-UAE test.
- Commit only this task's files (unrelated pre-existing tree edits: .vscode/settings.json, README.md, a .fs-uae config, .playwright-mcp/ — never stage them). Commit messages end with `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`.
- Oracle reference: `/tmp/mounter/mounter.c` (clone of a4091/mounter). Findings: `docs/reference/mounter-comparison-findings.md`.

## File Structure
- `src/rdb.c`, `src/rdb.h` — Task 1 (B1), Task 2 (B2), Task 3 (C1).
- `src/format.c` — Task 4 (D2), Task 5 (D1).
- `src/device.c`, `src/device.h` — Task 6 (D3).
- `src/discover.c` — Task 7 (D4).
- `tests/test_fshd.c`, `tests/test_rdb.c` — host tests for Tasks 1–3.

---

### Task 1: B1 — validate LSEG block checksums in read_fshd_chain

**Files:** Modify `src/rdb.c`; Test `tests/test_fshd.c`.

**Interfaces:** Consumes `rdb_checksum_ok`, `be_get32`, the LSEG block layout. Produces: a corrupt LSEG (bad checksum) makes `rdb_parse` reject that filesystem rather than copying garbage.

- [ ] **Step 1: Write the failing test.** In `tests/test_fshd.c`, add a test that serializes a model with one embedded FS, then corrupts one byte of payload in an LSEG block on the RAM disk WITHOUT fixing its checksum, then parses. Today parse accepts it (only the ID is checked). Assert the corrupted FS is rejected — i.e. after the fix, `rdb_parse` returns `RDB_ERR_NO_RDB`/skips it so `r.num_fs == 0` (choose the behavior implemented in Step 3 and assert it). Use the existing `RamDisk`/`ram_io`/`fake_seg` helpers. To locate the LSEG block, read RDSK[32..35] for the FSHD block, then FSHD `FSHD_o_SegList` for the first LSEG; flip a byte in its `LSEG_o_LoadData` region and re-write the block (leaving the stored checksum stale).

```c
static void test_lseg_bad_checksum_rejected(void)
{
    RamDisk *d = (RamDisk *)calloc(1, sizeof *d);
    RdbModel m; memset(&m, 0, sizeof m);
    rdb_init_model(&m, 100, 16, 63);
    CHECK(rdb_add_partition(&m, "DH0", 20, RDB_DOSTYPE_FFS_INTL) == RDB_OK);
    m.num_fs = 1; m.fs[0].dos_type = 0x50465303u; m.fs[0].seg_len = 600;
    m.fs[0].seg_data = fake_seg(600);
    CHECK(rdb_serialize(&m, ram_io, d) == RDB_OK);
    /* find first LSEG block and corrupt a payload byte without fixing checksum */
    { uint8_t blk[512]; uint32_t fshd, lseg;
      ram_io(d, 0, blk, 0);
      fshd = (blk[32]<<24)|(blk[33]<<16)|(blk[34]<<8)|blk[35];
      ram_io(d, fshd, blk, 0);
      lseg = (blk[72]<<24)|(blk[73]<<16)|(blk[74]<<8)|blk[75]; /* FSHD_o_SegList=72 */
      ram_io(d, lseg, blk, 0);
      blk[20] ^= 0xFF;            /* LSEG_o_LoadData=20: flip a byte, leave chksum stale */
      ram_io(d, lseg, blk, 1);
    }
    RdbModel r; memset(&r, 0, sizeof r);
    rdb_parse(&r, ram_io, d);
    CHECK(r.num_fs == 0);          /* corrupt LSEG -> FS rejected, not silently copied */
    rdb_model_free(&r);
    free(m.fs[0].seg_data); free(d);
}
```
Add `test_lseg_bad_checksum_rejected();` to `main`.

- [ ] **Step 2: Run to confirm fail.** `./tests/run-host-tests.sh` → FAIL (`r.num_fs == 0` fails; today the corrupt LSEG is accepted, num_fs==1).

- [ ] **Step 3: Implement.** In `read_fshd_chain` (src/rdb.c), in BOTH LSEG passes (the count pass and the copy pass), after confirming `be_get32(lb+0) == ID_LSEG`, also require `rdb_checksum_ok(lb, be_get32(lb + 4))` (read the block's own SummedLongs at offset 4 — this also covers finding B3 for LSEG). On checksum failure, abandon this filesystem: free any `seg_data` already allocated for the current entry, do NOT increment `m->num_fs`, and `break` out of the FSHD loop (or `return RDB_OK` having simply not added it). Ensure no partial entry is left and no leak (free the in-progress `seg_data` before bailing — closes the Task-4 mid-parse-leak Minor too).

- [ ] **Step 4: Run to confirm pass.** `./tests/run-host-tests.sh` → PASS, pristine.

- [ ] **Step 5: Commit** `src/rdb.c tests/test_fshd.c`, message `fix(rdb): validate LSEG checksums on parse (mounter finding B1)`.

---

### Task 2: B2 — honor de_TableSize when reading DosEnvec in rdb_parse

**Files:** Modify `src/rdb.c`; Test `tests/test_fshd.c` (or test_rdb.c).

**Interfaces:** Consumes `env_get`, the `DE_*` index macros, `DE_TableSize` (offset 0). Produces: parsing a partition whose `de_TableSize < 16` no longer yields garbage `dos_type`/`maxtransfer`/`mask`/`boot_pri`.

- [ ] **Step 1: Write the failing test.** Build a PART block by hand (or via a helper) on a RAM disk with `de_TableSize = 11` (so `DE_DosType`=16, `DE_MaxTransfer`/`DE_Mask`/`DE_BootPri` indices are beyond the table) and non-zero garbage in the longwords at those indices, then `rdb_parse` and assert the parsed partition's `dos_type` is a safe default (e.g. `RDB_DOSTYPE_FFS_INTL` or 0 — pick what Step 3 sets) rather than the garbage. The mounter reference: it copies only `(de_TableSize+1)` env longs (`mounter.c` ~line 975-979). NOTE: the simplest robust test writes an RDSK + one PART with a controlled DosEnvec; if building a raw PART is heavy, instead unit-test a small extracted helper `env_get_bounded(blk, idx, table_size, default)` and assert it returns the default when `idx > table_size`.

- [ ] **Step 2: Run to confirm fail.**

- [ ] **Step 3: Implement.** In `rdb_parse`, read `uint32_t ts = env_get(blk, DE_TableSize)` once per PART block. For each high-index field, only use `env_get` when `DE_<field> <= ts`, else use a safe default: `dos_type` → `RDB_DOSTYPE_FFS_INTL`; `num_buffers`/`maxtransfer`/`mask`/`boot_pri` → their existing model defaults (`RDB_DEFAULT_MAXTRANSFER`, `RDB_DEFAULT_MASK`, 0). `low_cyl`/`high_cyl` are at low indices (always present in a legal RDB) so no guard needed, but clamp defensively if `ts < DE_HighCyl`. Keep a small inline helper or explicit per-field checks — match the surrounding style.

- [ ] **Step 4: Run to confirm pass.** Full suite green + pristine.

- [ ] **Step 5: Commit** `src/rdb.c` + test file, message `fix(rdb): honor de_TableSize when reading DosEnvec (mounter finding B2)`.

---

### Task 3: C1 — preserve pb_Flags (PBFF_NOMOUNT) across parse/serialize

**Files:** Modify `src/rdb.h` (add field), `src/rdb.c` (parse + serialize); Test `tests/test_fshd.c`.

**Interfaces:** Produces: `RdbPartition.flags` (uint32_t) round-trips `pb_Flags`; bootable stays bit 0; `PBFF_NOMOUNT` (bit 1) and any other set bits are preserved.

- [ ] **Step 1: Add the field.** In `src/rdb.h` `RdbPartition`, add `uint32_t flags;  /* pb_Flags: bit0=PBFF_BOOTABLE, bit1=PBFF_NOMOUNT, ... */`. Add `#define RDB_PBFF_BOOTABLE 1u` and `#define RDB_PBFF_NOMOUNT 2u`.

- [ ] **Step 2: Write the failing test.** Serialize a model where one partition has `flags = RDB_PBFF_NOMOUNT` (and bootable=0), parse it back, assert `r.parts[i].flags & RDB_PBFF_NOMOUNT` is set and survives a second serialize→parse. Today serialize writes `bootable ? 1 : 0`, dropping bit 1 → test fails.

- [ ] **Step 3: Implement.**
  - In `rdb_parse`: read `p->flags = env... ` NO — `pb_Flags` is a PartitionBlock field, not a DosEnvec field. Read it from `be_get32(blk + PART_o_Flags)` (the existing `PART_o_Flags` offset). Keep `p->bootable = (p->flags & RDB_PBFF_BOOTABLE) ? 1 : 0` for backward compat with the rest of the code.
  - In `rdb_serialize` / `write_partition_block`: write `be_put32(blk + PART_o_Flags, (p->flags & ~RDB_PBFF_BOOTABLE) | (p->bootable ? RDB_PBFF_BOOTABLE : 0))` — i.e. preserve all non-bootable bits from `flags`, and let `bootable` drive bit 0 (since the GUI toggles `bootable`). This keeps the existing bootable editing working while preserving NOMOUNT etc.
  - Ensure `rdb_init_model` / fresh partitions default `flags = 0`.

- [ ] **Step 4: Run to confirm pass.** Full suite green + pristine. Verify existing bootable tests still pass.

- [ ] **Step 5: Commit** `src/rdb.h src/rdb.c tests/test_fshd.c`, message `fix(rdb): preserve pb_Flags (PBFF_NOMOUNT) across round-trip (mounter finding C1)`.

NOTE: a future GUI task could expose NOMOUNT in the Edit dialog; out of scope here — this task only stops HDPart from DESTROYING the bit.

---

### Task 4: D2 — Strategy-1 handler clone min-stack/priority fallback

**Files:** Modify `src/format.c`.

**Interfaces:** Produces: a cloned handler from Strategy 1 never launches with `dn_StackSize == 0`.

- [ ] **Step 1: Read** `src/format.c` `bind_handler` Strategy 1 (the live-mount clone, ~lines 113-148). It currently does `if (stack > 0) dn->dn_StackSize = stack;` and `if (pri > 0) dn->dn_Priority = pri;`. Strategy 2 (~lines 163-164) already has the correct fallback (`if (dn->dn_StackSize == 0) dn->dn_StackSize = 4096; if (dn->dn_Priority == 0) dn->dn_Priority = 10;`).

- [ ] **Step 2: Implement.** After Strategy 1 sets the cloned fields and before `return 1`, apply the same fallback Strategy 2 uses: `if (dn->dn_StackSize == 0) dn->dn_StackSize = 4096;` and `if (dn->dn_Priority == 0) dn->dn_Priority = 10;`. (Use `>=`-style: a cloned stack of 0 must be replaced; priority 0 is a fine default so the 4096 stack fix is the load-bearing one.) Keep the change minimal.

- [ ] **Step 3: Build.** `make` → compiles clean. `./tests/run-host-tests.sh` → format host tests still green (this code is HDPART_AMIGA-guarded; ensure the host build is unaffected).

- [ ] **Step 4: Commit** `src/format.c`, message `fix(format): Strategy-1 clone min-stack fallback (mounter finding D2)`.

---

### Task 5: D1 — remove ephemeral HDP0 node after format (fresh path)

**Files:** Modify `src/format.c`.

**Interfaces:** Produces: the ephemeral `HDPn:` device node added for the unmounted-partition format path is removed (`RemDosEntry`) after `ACTION_FORMAT`, so no duplicate live mount lingers.

- [ ] **Step 1: Read** `src/format.c` `format_partition` (~lines 219-275): the fresh path does `pick_free_devname` → `MakeDosNode` → `AddDosNode(0, ADNF_STARTPROC, dn)` → Inhibit/ACTION_FORMAT/Inhibit, and currently returns without removing the node.

- [ ] **Step 2: Implement.** After the format completes (success or failure on this path), remove the ephemeral DOS entry so the temporary `HDPn:` does not persist as a second mount of the partition. Use the safe sequence: get the `DosList`/`DeviceNode` we added and `RemDosEntry` it under `Forbid()`/`Permit()` (or `LockDosList`/`UnLockDosList(LDF_DEVICES|LDF_WRITE)` then `RemDosEntry`). Only remove the node WE added (track the `struct DeviceNode *dn` from `MakeDosNode`/the `DosList*` returned). Do NOT remove it on the reformat-existing-mount path (that path never adds a node). Note: after `RemDosEntry`, the handler process started by `ADNF_STARTPROC` may still be running; this matches how a temporary mount is torn down — if the existing code has a teardown helper, reuse it; otherwise `RemDosEntry` is the minimum that stops new opens of `HDPn:`.

CAUTION: this is the UNVERIFIED fresh path (per `docs/.../hdpart-format-feature` memory). Keep the change conservative and clearly commented. If removing the node safely requires more than `RemDosEntry` (e.g. the started handler must be signaled), report DONE_WITH_CONCERNS describing what a full teardown needs, and implement at least the `RemDosEntry`.

- [ ] **Step 3: Build.** `make` clean; host tests green.

- [ ] **Step 4: Commit** `src/format.c`, message `fix(format): RemDosEntry ephemeral node after fresh-path format (mounter finding D1)`.

---

### Task 6: D3 — thread block_bytes through dev_rw instead of assuming 512

**Files:** Modify `src/device.c`, `src/device.h`.

**Interfaces:** Produces: `dev_rw` uses the device's geometry `block_bytes` for `io_Length` and `io_Offset` instead of the literal 512.

- [ ] **Step 1: Read** `src/device.c` `dev_geometry`/`dev_open`/`dev_rw` and `src/device.h` `DeviceHandle`. Find where geometry block size (`dg_SectorSize`) is read and where `dev_rw` hardcodes `io_Length = 512` and `io_Offset = block * 512UL`.

- [ ] **Step 2: Implement.** Store the geometry block size in `DeviceHandle` (e.g. `uint32_t block_bytes;`, defaulting to 512 if geometry doesn't report one) at open/geometry time, and use `h->block_bytes` in `dev_rw` for both `io_Length` and the `io_Offset = (ULONG)block * h->block_bytes`. Keep 512 as the fallback when `dg_SectorSize` is 0. Reference: mounter threads `md->blocksize` from geometry into every `readblock`.

- [ ] **Step 3: Build + sanity test.** `make` clean. `make hd`, and verify on FS-UAE the normal 512-byte scratch disks still scan/read identically (no regression) — OR, if no on-target run now, at minimum confirm `make` clean and host tests green and note that on-target re-verification is needed (defer to the embed-FS Task 12 on-target pass).

- [ ] **Step 4: Commit** `src/device.c src/device.h`, message `fix(device): use geometry block size in dev_rw, not hardcoded 512 (mounter finding D3)`.

---

### Task 7: D4 — media-presence pre-check before probing (discovery)

**Files:** Modify `src/discover.c` (and `src/device.c`/`.h` if a helper fits there).

**Interfaces:** Produces: discovery skips a unit with no media present (removable bay) instead of relying solely on `TD_GETGEOMETRY` erroring.

- [ ] **Step 1: Read** `src/discover.c` probe path (where `TD_GETGEOMETRY` is issued, ~lines 167-194) and the mounter's `UnitIsReady` (`/tmp/mounter/mounter.c` ~1068-1093): it sends `CMD_START` then checks `TD_CHANGESTATE` (io_Actual != 0 ⇒ no disk) before trusting the unit.

- [ ] **Step 2: Implement.** Add a small `dev_unit_ready(h)` (or inline in the probe) that issues `TD_CHANGESTATE`; if it reports media absent, classify the unit as NOMEDIA and skip the geometry/RDB probe. Keep it defensive: many fixed devices don't support `TD_CHANGESTATE` (they return an error or 0) — treat "command failed / not supported" as PRESENT (don't skip a fixed disk just because it lacks change-state). Only skip when `TD_CHANGESTATE` succeeds AND reports no disk. This must NOT regress fixed CF/SD/IDE scanning.

- [ ] **Step 3: Build + on-target note.** `make` clean; `make hd`. This is robustness for removable media and is best verified on-target; if not running FS-UAE now, confirm `make` clean + host tests green and fold the on-target check into the embed-FS Task 12 pass. Be explicit in the report that fixed-disk scanning must be re-confirmed on-target.

- [ ] **Step 4: Commit** `src/discover.c` (+ device files if touched), message `fix(discover): media-presence pre-check via TD_CHANGESTATE (mounter finding D4)`.

---

## Self-Review

Coverage: B1→Task1, B2→Task2, C1→Task3, D2→Task4, D1→Task5, D3→Task6, D4→Task7. A1–A3 are folded into the embed-filesystem plan's Tasks 7 & 9 (amended 2026-06-18). Each task ends with a build/test gate and an isolated commit. Tasks 5/6/7 touch the unverified-on-target paths and explicitly defer final on-target confirmation to the embed-FS Task 12 FS-UAE pass.
