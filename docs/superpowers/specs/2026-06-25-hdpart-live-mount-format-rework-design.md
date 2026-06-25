# HDPart — Live-Mount & Format Rework (0.9) — Design

**Date:** 2026-06-25
**Status:** Approved design, ready for implementation planning
**Spec:** this document
**Related:** `2026-06-14-hdpart-format-partition-design.md`, `2026-06-17-hdpart-embed-filesystem-design.md`, `docs/reference/mounter-comparison-findings.md`

## 1. Goal

Address three issues from the A3000 tester's field report (2026-06-24) on top of
the 0.8 LSEG seglist fix:

1. **Empty-disk UX (#2):** on a freshly-wiped disk (valid geometry, no RDB), only
   **Split** is available; **New** and **Filesystems** stay greyed until a failed
   Split happens to create the in-memory model. The user should be able to create a
   partition on a blank disk directly.
2. **Volume name (#3):** `ACTION_FORMAT` writes a volume with no user-chosen label,
   so Workbench shows an unnamed icon. The user should be prompted for a volume name.
3. **Fresh-partition mount (#4):** the current format path mounts a freshly-created
   partition under a throwaway `HDP0` name and then `RemDosEntry`s it, so no
   persistent DOS device under the real partition name (`DH5:`) survives, and the
   handler is never registered in `filesystem.resource`. The tester's prescription:
   replicate what `scsi.device` does — register the handler in `filesystem.resource`,
   `MakeDosNode`/`AddDosNode` from the partition block with the device proc started,
   then `GetDeviceProc` + `ACTION_FORMAT`. The result is a real, named, session-
   persistent device usable immediately without a reboot.

Ships as **0.9**. The oracle for the OS-call sequence is `github.com/a4091/mounter`
(`FSHDProcess`/`FSHDAdd`/`ParseFSHD` + `MakeDosNode`/`AddDosNode`), already cloned and
cross-checked.

## 2. Non-goals

- No change to the working "reformat an already-mounted partition" path (Inhibit →
  `ACTION_FORMAT` → un-Inhibit).
- No standalone "Mount without format" action (YAGNI — mounting happens as part of
  Format).
- No `FreeDosNode`/full teardown machinery; the existing "intentionally not freed,
  rare one-shot path" leak policy stands.
- No change to the on-disk RDB format (0.8 already fixed the LSEG seglist).

## 3. Design

### §1 Empty-disk model init (#2)

`gui_select_unit` ([src/gui.c]) sets `g_have_model = 1` only when `rdb_parse`
succeeds. On a blank disk that leaves geometry present (`g_geo.has_media`) but no
model, so **New**/**Filesystems** (which gate on `g_have_model`) stay disabled while
**Split** (gates on `hasGeo`) is the only lit button.

Fix: when geometry is valid but `rdb_parse` fails, initialize an empty in-memory
table:

```c
if (rdb_parse(&g_model, dev_block_io, h) == RDB_OK) {
    g_have_model = 1;
    fs_pool_merge_from_model(&g_model);
} else if (g_geo.has_media && g_geo.cylinders > 0) {
    rdb_init_model(&g_model, g_geo.cylinders, g_geo.heads, g_geo.sectors);
    g_have_model = 1;             /* New / Filesystems usable on a blank disk */
}
```

`g_dirty` stays 0 — nothing is written until the user creates a partition and Saves.
This only changes button enablement; the Split path's own lazy `rdb_init_model`
becomes redundant but harmless.

### §2 Volume-name prompt (#3)

New modal `gui_volname_dialog(const char *deflt, char *out, int outsz)` modeled on the
existing `gui_path_dialog` string-input modal: one string gadget pre-filled with the
partition's RDB name, **OK**/**Cancel**. Returns non-zero on OK.

`gui_format` calls it before `format_partition` on **both** the fresh and
reformat-existing paths, pre-filling with `p->name`. Cancel aborts the format. The
chosen string flows through the `volname` parameter `format_partition` already
accepts; the engine already prefers `volname` over `p->name` when writing the
`ACTION_FORMAT` label.

Volume-name validation: trim to the BSTR limit already used (`volbstr[34]` → 31 chars
usable); empty input falls back to the partition name.

### §3 Fresh-partition proper mount (#4, Approach A)

Reworks the **fresh** branch of `format_partition` (`src/format.c`). Path selection:

1. **Already mounted, FS family matches** → reformat the live volume (today's path,
   untouched).
2. **A DOS node with the partition's real name already exists** (stale / NDOS /
   auto-mounted at boot) → fall back to today's **ephemeral `HDP0`** mechanism, so we
   never `AddDosNode` a duplicate name over an existing node.
3. **Truly fresh** (no DOS node with that name — the wiped-disk case) → new
   Approach-A flow.

**Approach-A flow:**

1. `fsres_find(dos_type)` — `OpenResource("FileSystem.resource")`, walk
   `fsr_FileSysEntries` under `Forbid`/`Permit`, match by DosType family
   (`& 0xFFFFFF00`) and `fse_Version`. Returns an existing `FileSysEntry *` or NULL.
2. If NULL → `fsres_add_from_model(dos_type, m)`: `InternalLoadSeg` the embedded
   FSHD/LSEG (existing `embedded_loadseg`), `AllocMem` a `FileSysEntry`, populate it
   from the `RdbFileSys` fields gated by **PatchFlags** (Type/Task/Lock/Handler/
   StackSize/Priority/Startup/SegList/GlobalVec — same bit map as `fsload.c` and
   mounter's `ProcessPatchFlags`), set `fse_SegList`, then
   `Forbid → AddHead(&fsr->fsr_FileSysEntries, &fse->fse_Node) → Permit`. On failure:
   free the entry, return `FMT_ERR_NO_HANDLER`.
3. `MakeDosNode` under the **real partition name** (`p->name`) + the env vector.
4. Copy handler params from the `FileSysEntry` into the node per PatchFlags
   (`dn_SegList`, `dn_GlobalVec`, `dn_StackSize`, `dn_Priority`, `dn_Handler`, …).
5. `AddDosNode(0, ADNF_STARTPROC, dn)` — and **leave it live** (no `RemDosEntry`).
6. `GetDeviceProc("<name>:")` → `DoPkt(ACTION_FORMAT, volbstr, dos_type, …)` →
   `FreeDeviceProc`.

Result: a real, named, session-persistent `DH5:` device, usable for file copy without
a reboot, identical to the RDB auto-mount produced on the next boot (with the 0.8
seglist fix).

**Name-collision detection** (branch selector between 2 and 3): a pure helper decides
"is there a DOS device node whose name equals `p->name`?" by walking the device list
(the GUI/engine already does similar walks). The decision logic (name compare against a
BSTR list) is pure and unit-tested; the list walk itself is the OS part.

## 4. Module / interface boundaries

New, mostly host-testable pure helpers (in `format.c` or a small `fsres.c`):

- `int fsres_fse_from_model(const RdbFileSys *fs, FseFields *out)` — pure: map a
  `RdbFileSys` + PatchFlags to the `FileSysEntry` field set (no OS calls). Host-tested.
- `int dostype_family_match(uint32_t a, uint32_t b)` — pure (already implicit; make
  explicit + tested).
- `int name_in_bstr_list(const char *name, const Bstr *nodes, int n)` — pure name-
  collision predicate. Host-tested.

OS-only (verified on-target by the tester): `fsres_find`, `fsres_add` (`OpenResource`/
`AddHead`), the `MakeDosNode`/`AddDosNode`/`GetDeviceProc`/`ACTION_FORMAT` sequence.

## 5. Error handling

- Volume dialog Cancel → abort format, no side effects.
- No handler for the DosType (no resource entry, no embedded seglist) →
  `FMT_ERR_NO_HANDLER` (existing).
- `MakeDosNode` / `AddDosNode` failure → `FMT_ERR_MAKENODE` / `FMT_ERR_ADDNODE`
  (existing).
- `filesystem.resource` add: an entry whose `fse_SegList` failed to load is freed, not
  added (mirrors mounter `FSHDAdd`).
- Leak policy unchanged: persistent node + FSE are intentional; rare error-exit
  seglist leaks accepted (no `FreeDosNode` API; one-shot path).

## 6. Testing

- **Host (pure):** `fsres_fse_from_model` PatchFlags mapping, `dostype_family_match`,
  `name_in_bstr_list`. Add to `tests/test_format.c` / a new `tests/test_fsres.c`.
- **On-target (tester):** blank disk → New → assign FS → Save → Format → prompted for
  volume name → `DH5:` appears named in `Info`, copy a file to it without reboot →
  reboot → still mounted and named. Boot-partition case no longer crashes.

## 7. Risk

`filesystem.resource` mutation is system-wide until reboot; mitigated by Forbid/Permit
and mirroring mounter.c exactly. The resource entry persists by design (matches
`scsi.device`; reboot re-provides it from the RDB FSHD, deduped by version). Cannot be
host-tested — the tester is the on-target verifier, as for prior format work.

## 8. Version

Bump to **0.9** (gui.c window title + About line + Makefile `ADFVER`).
