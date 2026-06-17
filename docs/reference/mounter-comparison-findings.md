# a4091/mounter cross-check — findings vs HDPart

Date: 2026-06-17. Oracle: `github.com/a4091/mounter` (the a4091.device/lide.device
generic RDB mounter by Toni Wilen / Stefan Reinauer / Matt Harlum), cloned to
`/tmp/mounter`. Compared against HDPart `src/{rdb,format,device,discover}.c` and the
embed-filesystem plan. Per-area detail: `/tmp/mounter-cmp-{rdb,fshd,mount,device}.md`.

All items below were spot-verified against HDPart source (file:line cited).

## Confirmed positives (mounter validates our choices)
- **LSEG payload = 492 bytes** (`LSEG_DATASIZE = 512/4 - 5` longs in mounter.c:97) — exactly
  our `LSEG_PAYLOAD = 492` with a 5-long/20-byte header. No off-by-one.
- **Our FSHD/LSEG on-disk format is accepted** by amitools' own block parsers (Task 5
  cross-check) and matches the mounter's `struct FileSysHeaderBlock` / `LoadSegBlock` use.
- **Family-match handler binding** (`dos_type & 0xFFFFFF00`) agrees with the mounter.
- **`DEV_IOREQ_SIZE = 256`** (device.c) is the right call: the mounter uses
  `sizeof(IOExtTD)=56` safely only because it opens its own controlled driver, never an
  arbitrary user-loaded `.device` (HDPart's open-ended case — see the v0.5 IORequest bug).
- **Task 9 InternalLoadSeg is the correct choice** at format-time (dos.library open, V37):
  the mounter hand-rolls `fsrelocate` only because it runs at early boot before DOS exists.
  No need to copy `fsrelocate`. (Hardening required — see B3 below.)

## A. Fold into upcoming embed-FS tasks (Tasks 6–9)

- **A1 (Task 9, PLAN-RISK):** `InternalLoadSeg` must be paired with `InternalUnLoadSeg`
  on **every** error exit after a successful load, or the seglist leaks when a later
  format step fails. Add a `SysBase->lib_Version >= 36` guard (InternalLoadSeg is V36+;
  our floor is V37 so OK, but assert it). The plan does not mention the unload path.
- **A2 (Task 7):** use `rdb_free_sized()` (not raw `FreeMem`) for `seg_data`, and drop the
  duplicate/dead `Seek` calls in the plan's `fsload_from_file` sketch.
- **A3 (Task 7 PatchFlags):** the plan's `patch_flags = 0x180` placeholder must become the
  real `FSB_*` bit set. Use the mounter's `ProcessPatchFlags` (mounter.c:938) as the
  authority for which bits map to which `dn_*` fields (Type/Task/Lock/Handler/StackSize/
  Priority/Startup/SegListBlocks/GlobalVec). GlobalVec = -1 for non-BCPL handlers.

## B. Just-built Phase 1 engine (fix now — small, in-scope)

- **B1 (BUG) — LSEG blocks not checksum-validated.** `read_fshd_chain` (rdb.c) checks the
  FSHD checksum but only checks the `LSEG` magic ID, never the LSEG checksum (rdb.c LSEG
  loop, ~lines 34/46). The mounter validates both ID and checksum on every block. A silent
  bit-error in an LSEG would be copied into `seg_data` and re-written on Save. Add
  `rdb_checksum_ok(lb, <stored SummedLongs>)` to both LSEG passes.
- **B2 (BUG) — `rdb_parse` ignores `de_TableSize`.** rdb.c:782–786 reads `DE_DosType`
  (idx 16), `de_MaxTransfer`, `de_Mask`, `de_BootPri` unconditionally. A legacy disk with
  `de_TableSize < 16` yields garbage for those fields. The mounter copies only
  `(de_TableSize+1)` longs and leaves the rest zero. Guard each high-index `env_get` by the
  block's `de_TableSize` (rdb.c offset `DE_TableSize=0`) and default safely.
- **B3 (minor) — FSHD/LSEG checksum uses hardcoded SummedLongs.** We pass the constant
  `FSHD_SUMMEDLONGS=64`; the mounter reads the block's own `SummedLongs` (`be_get32(blk+4)`).
  Reading the stored value is more robust against odd-but-valid blocks. Low priority.

## C. Pre-existing data-fidelity bug (decide scope)

- **C1 (BUG) — `PBFF_NOMOUNT` and other `pb_Flags` bits are lost on round-trip.**
  `rdb_serialize` writes `pb_Flags = bootable ? 1 : 0` (rdb.c:499), and `RdbPartition` has
  no flags field, so parse drops every non-bootable flag. A disk with an intentionally
  unmountable reserved partition (`PBFF_NOMOUNT`, bit 1) has it cleared after any HDPart
  Save → the OS then tries to mount it. Fix: add a `flags` field to `RdbPartition`,
  preserve unknown bits, expose `PBFF_NOMOUNT`. Pre-existing, but thematically aligned with
  this feature's "don't destroy what's on the disk" goal.

## D. Shipped v0.5 format/device/discovery (decide scope)

- **D1 (BUG, fresh-path only) — ephemeral `HDP0` node not removed after format.**
  format.c:252 `AddDosNode`s an ephemeral `HDP0:` for the unmounted-partition format path,
  but never `RemDosEntry`s it afterward → a duplicate live mount of the just-formatted
  partition lingers for the rest of the session (cleared on reboot). Only on the
  fresh/unmounted path (already marked unverified on-target). Add `RemDosEntry` after
  `ACTION_FORMAT` completes. (The normal reformat-existing-mount path is unaffected.)
- **D2 (GAP) — Strategy-1 clone can leave `dn_StackSize = 0`.** format.c:144 only sets the
  stack when the source `> 0`; if a cloned live mount reports stack 0, the handler launches
  with a zero stack and can crash. Strategy 2 has the 4096 fallback (lines 163–164);
  Strategy 1 lacks it. Add the same min-stack/min-pri fallback to Strategy 1. (`pri > 0`
  dropping a valid pri=0 is cosmetic — 0 is a fine default.)
- **D3 (GAP) — `dev_rw` assumes 512-byte blocks.** device.c hardcodes `io_Length=512` and
  `io_Offset = block*512`, ignoring the `block_bytes` read from geometry. Harmless for
  CF/SD/IDE (always 512) but wrong for any non-512 medium. Thread `block_bytes` through.
- **D4 (IMPROVEMENT) — no media-presence pre-check in discovery.** The mounter issues
  `CMD_START` + `TD_CHANGESTATE` (`UnitIsReady`) before probing; we rely on
  `TD_GETGEOMETRY` erroring, which some drivers fake on empty removable bays. Adding a
  readiness pre-check would harden discovery for ZIP/removable units.
