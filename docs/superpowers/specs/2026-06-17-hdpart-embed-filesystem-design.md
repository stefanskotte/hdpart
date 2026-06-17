# HDPart — Embed Filesystem in RDB (Feature B) — Design

**Date:** 2026-06-17
**Status:** Approved design, ready for implementation planning
**Spec:** this document
**Related:** `2026-06-14-hdpart-format-partition-design.md`, `2026-06-10-hdpart-rom-filesystem-cycle-design.md`, `2026-06-08-hdpart-load-driver-from-file-design.md`

## 1. Goal

Let HDPart embed a non-ROM filesystem handler (PFS3, SFS, ...) into a disk's RDB so
the partitioned disk boots and mounts that filesystem on a **stock Amiga** where the
handler is not otherwise installed. Expose embedded filesystems in the Edit dialog's
FS-cycle alongside the ROM FFS/OFS types, and let Format use an embedded filesystem
even when its handler is not currently live in the running machine.

This is "Feature B" from the deferred list (load a filesystem from disk and embed
FSHD/LSEG blocks in the RDB). It complements the existing in-memory `.device` driver
loader and the live-format machinery shipped in v0.5.

### User-facing capabilities

1. **Load a filesystem and use it in the FS dropdown** — a loaded filesystem (e.g.
   PFS\3) appears in the Edit dialog's FS-cycle next to FFS/OFS.
2. **Embed the filesystem into the disk's RDB** — written as `FSHD` + `LSEG` blocks on
   Save, so the disk mounts the FS on its own on a real Amiga.
3. **Format with the embedded filesystem** — Format works with the embedded FS even if
   that handler was never installed on the running Amiga.

## 2. Decisions (settled during brainstorming)

- **Filesystem sources:** (a) **from a file** (an `L:`-style handler binary, e.g.
  `L:PFSFileSystem`); (b) **copy from another disk's RDB** (read its FSHD+LSEG and
  import verbatim). The "harvest from the live `FileSystem.resource`" source was
  considered and **excluded** — both supported sources are disk-based.
- **Management UX:** a **dedicated Filesystems dialog** (opened from the Disk menu and a
  button), not an inline button in the Edit dialog. The dialog manages the disk's set of
  embedded filesystems; the Edit FS-cycle merely reads that set. Load once, use for every
  partition.
- **Format binding:** add an **embedded-FS binding source (Strategy 3)** to
  `bind_handler`, so Format reconstructs a live seglist from the embedded FSHD/LSEG and
  formats with it. This makes HDPart self-contained and is the only way to genuinely
  cold-test embedding.
- **Model semantics:** all embedding edits the in-memory `RdbModel`, is dirty-tracked,
  and is applied **on Save** — identical to how partition edits behave. The one
  immediate-IO exception is *copying* from another disk, which reads that other disk at
  the moment of import but only writes into the in-memory target model.
- **Caps / allocator:** `RDB_MAX_FS = 8`. The RDB block allocator is **simple and
  linear** — Save rewrites the whole reserved RDB region every time (RDSK, then PART
  blocks, then FSHD/LSEG), with no free-list. This matches the existing wholesale rewrite
  of RDSK+PART.

## 3. Key technical insight (shapes the whole design)

Embedding from a file does **not** use `LoadSeg`. The RDB stores a filesystem as its
**raw AmigaDOS hunk-format load file**, chunked verbatim into `LSEG` blocks; the OS later
reconstructs it via `InternalLoadSeg`. Therefore:

- **Embed from file** = read the file's raw bytes, validate the `HUNK_HEADER` magic
  (`0x000003F3`), and split the bytes into `LSEG` blocks. No seglist walking, no
  un-relocation problem.
- **Format Strategy 3** goes the other direction: feed the concatenated `LSEG` bytes to
  `InternalLoadSeg` (with an in-memory read hook) to obtain a runnable seglist to format
  with.

The driver-loader's `LoadSeg` hunk-walk (`src/driver.c`) is the analog only for the
Strategy-3 reconstruction direction, not for embedding.

## 4. Architecture (four layers, bottom-up)

```
GUI (gui.c)            Filesystems dialog + Edit FS-cycle
  |
FS acquisition         fsload.c  (from file / copy from disk)
  |
Format (format.c)      bind_handler Strategy 3 (InternalLoadSeg from embedded LSEG)
  |
RDB engine (rdb.c)     FSHD/LSEG model, parse, serialize, block allocator
```

The RDB engine layer is mandatory even for a minimum viable feature: today
`rdb_serialize` writes `RDB_o_FileSysHdr = NULLPTR` unconditionally, so saving a disk that
already carries an embedded filesystem **silently destroys it**. The read+write FSHD
support fixes that bug as a side effect.

## 5. RDB engine (`src/rdb.c`, `src/rdb.h`)

### 5.1 On-disk block formats (512-byte blocks, standard RDB layout)

**FSHD — FileSysHeaderBlock** (one per embedded filesystem):

| Field            | Offset | Notes                                            |
|------------------|--------|--------------------------------------------------|
| `fhb_ID`         | 0      | `'FSHD'` = `0x46534844`                           |
| `fhb_SummedLongs`| 4      | 64                                               |
| `fhb_ChkSum`     | 8      | block checksum (existing helper)                 |
| `fhb_HostID`     | 12     | 7                                                |
| `fhb_Next`       | 16     | block# of next FSHD or `NULLPTR`                 |
| `fhb_Flags`      | 20     | 0                                               |
| `fhb_DosType`    | 32     | the filesystem DosType (e.g. `0x50465303`)        |
| `fhb_Version`    | 36     | `ver<<16 | patch`                                |
| `fhb_PatchFlags` | 40     | which `dn_*` fields below are valid               |
| `fhb_Type`..`fhb_GlobalVec` | 44.. | partial DeviceNode image: `Type, Task, Lock, Handler, StackSize, Priority, Startup, SegListBlocks, GlobalVec` |

`fhb_SegListBlocks` holds the block# of the first `LSEG`.

**LSEG — LoadSegBlock** (hunk image, chained):

| Field            | Offset | Notes                                            |
|------------------|--------|--------------------------------------------------|
| `lsb_ID`         | 0      | `'LSEG'` = `0x4C534547`                           |
| `lsb_SummedLongs`| 4      | 128                                              |
| `lsb_ChkSum`     | 8      | block checksum                                   |
| `lsb_HostID`     | 12     | 7                                                |
| `lsb_Next`       | 16     | block# of next LSEG or `NULLPTR`                 |
| `lsb_LoadData`   | 20     | 123 longs = **492 bytes** of raw hunk payload     |

### 5.2 In-memory model (`rdb.h`)

```c
typedef struct {
    uint32_t dos_type;        /* fhb_DosType */
    uint32_t version;         /* fhb_Version (ver<<16 | patch) */
    uint32_t patch_flags;     /* fhb_PatchFlags */
    uint32_t dn_type, dn_task, dn_lock, dn_handler;
    uint32_t dn_stack, dn_pri, dn_startup, dn_globalvec;
    uint32_t seg_len;         /* bytes of hunk image */
    uint8_t *seg_data;        /* heap-owned hunk image; freed on teardown */
    char     name[RDB_NAME_LEN]; /* display, e.g. "PFSFileSystem" */
    uint8_t  source;          /* RDB_FS_EMBEDDED / _FILE / _COPIED (display only) */
} RdbFileSys;
```

Added to `RdbModel`:

```c
    RdbFileSys fs[RDB_MAX_FS];   /* RDB_MAX_FS = 8 */
    int        num_fs;
```

`seg_data` is the **only heap-owned field** in the model (everything else is flat today).
The model gains an explicit teardown (`rdb_model_free` or similar) that frees every
`fs[].seg_data`. To keep `rdb.c` host-testable under plain `cc`, allocation goes through a
tiny shim: `malloc/free` in host tests, `AllocMem/FreeMem` on-target.

### 5.3 Parse (`rdb_parse`)

After the existing PART walk: read `RDB_o_FileSysHdr` (offset 32 of RDSK). Walk the FSHD
chain (`fhb_Next`); for each FSHD up to `RDB_MAX_FS`, copy the `dos_type/version/
patch_flags/dn_*` fields, then follow `fhb_SegListBlocks` through the LSEG chain
concatenating `lsb_LoadData` into a freshly allocated `seg_data` (`seg_len` accumulated).
Guard both chains with a block-count limit (reuse the PART-walk guard pattern) and validate
each block's ID + checksum.

### 5.4 Serialize (`rdb_serialize`) + block allocation

Replace the fixed RDSK@0 / PART@1..n layout with a running allocator over the reserved RDB
region (`rdb_blocks_lo..rdb_blocks_hi`, sized by `rdb_init_model` to
`RDB_RESERVED_CYLS * cyl_blocks`, typically ~2015 blocks):

1. RDSK at block 0.
2. PART blocks next (as today).
3. For each `fs[i]`: one FSHD block, then its LSEG chain (each carrying ≤492 bytes).
   Wire `RDB_o_FileSysHdr` → first FSHD; each `fhb_Next`; each `fhb_SegListBlocks` /
   `lsb_Next`.
4. Set `HighRDSKBlock` to the last used block.

**Capacity guard:** compute the total block count up front; if FSHD+LSEG do not fit the
reserved region, fail with a new `RDB_ERR_NO_RDB_SPACE` and write nothing (no partial
write). A typical PFS3 (~50–60K) needs ~120 LSEG blocks — comfortably inside ~2015.

### 5.5 Constants

```c
#define ID_FSHD 0x46534844u   /* 'FSHD' */
#define ID_LSEG 0x4C534547u   /* 'LSEG' */
#define RDB_MAX_FS 8
#define FSHD_SUMMEDLONGS 64u
#define LSEG_SUMMEDLONGS 128u
```

New error code `RDB_ERR_NO_RDB_SPACE`.

## 6. FS acquisition (`src/fsload.c`, `src/fsload.h` — new module)

Both entry points produce an `RdbFileSys` appended to the model (dirty-tracked, written on
Save).

### 6.1 From a file — `fsload_from_file(path, RdbFileSys *out)`

- Read the whole file into a heap buffer (`AllocMem`). Bounded by a max size (e.g. 512K)
  to refuse oversized/garbage input (`FSL_ETOOBIG`).
- Validate the first longword is `HUNK_HEADER` = `0x000003F3`; else `FSL_ENOTLOADFILE`.
- Store the raw bytes as `seg_data` / `seg_len`. Derive `name` from the file part.
- A load file does not carry its DosType, so default `dos_type` to a placeholder and let
  the user set it in the Filesystems dialog. Default handler params mirror `format.c`
  Strategy-2 defaults: `patch_flags` covering StackSize+Priority+GlobalVec, `dn_stack =
  4096`, `dn_pri = 10`, `dn_globalvec = (uint32_t)-1` (non-BCPL).

### 6.2 Copy from another disk's RDB — `fsload_from_disk(driver, unit, ...)`

- Transiently open the chosen device+unit (reuse `device.c` / `discover.c` plumbing),
  `rdb_parse` into a scratch model, and import the chosen embedded FS verbatim —
  `dos_type`, `version`, all `dn_*`, and `seg_data` (deep-copied). Close the transient
  device. Only the in-memory target model is mutated.
- If the source disk carries more than one embedded FS, the GUI presents a sub-pick.

### 6.3 DosType editing

The Filesystems dialog lets the user set/confirm a row's DosType (hex field plus a small
preset cycle: `PFS\3` `0x50465303`, `PDS\3` `0x50445303`, `SFS\0` `0x53465300`, `SFS\2`
`0x53465302`). File-sourced entries need this; copy-from-disk entries arrive pre-filled.

### 6.4 Error codes

`FSL_OK`, `FSL_EOPEN`, `FSL_EREAD`, `FSL_ETOOBIG`, `FSL_ENOTLOADFILE`, `FSL_ENOMEM`,
`FSL_EFULL` (pool at `RDB_MAX_FS`), `FSL_ENOFS` (source disk has no embedded FS), each with
a text mapping for the GUI (mirrors `drv_err_text`).

## 7. Format integration (`src/format.c`)

Add **Strategy 3** to `bind_handler`, tried **after** Strategy 1 (live mount) and
Strategy 2 (`FileSystem.resource`), so an already-running handler is still preferred:

1. Find the model's `RdbFileSys` whose `dos_type` family (`& 0xFFFFFF00`) matches the
   requested type.
2. Reconstruct a live seglist from `seg_data` via `InternalLoadSeg` (dos.library) with an
   in-memory read hook feeding bytes from `seg_data`.
3. Populate `dn_SegList` plus `dn_GlobalVec` / `dn_StackSize` / `dn_Priority` from the FSHD
   params (defaults applied as in Strategies 1/2), then continue down the existing
   `MakeDosNode` / `ACTION_FORMAT` path.

**Lifetime:** the reconstructed seglist is owned by the resulting mount — do **not**
`UnLoadSeg` after a successful bind (same contract as the driver loader's `InitResident`).

`format_build_envec` and the pure host-tested helpers are unchanged. The reconstruction is
an OS wrapper (not host-testable), consistent with the rest of `format.c`.

The Strategy-3 lookup needs the current `RdbModel`; thread the model (or its `fs[]`) into
`format_partition` / `bind_handler`.

## 8. GUI (`src/gui.c`)

### 8.1 Entry points

- A `Filesystems…` item in the **Disk** menu, command key `rA-Y` (F is Refresh, I is Init,
  so Y avoids collisions).
- A small `Filesystems…` button, ghosted until a disk is scanned (model-scoped, like Init
  / partition actions). Menu and button ghost in lockstep via `gui_update_buttons` /
  `gui_menu_enable`.

### 8.2 Filesystems dialog

Built on the shared `dlg_center` / `dlg_open` / `dlg_refresh` / `dlg_close` scaffolding.

```
Filesystems on DH  (this disk's RDB)
+---------------------------------------------+
| > DosType  Name           Source     Size   |
|   PFS\3    PFSFileSystem   embedded   58K    |
|   SFS\0    SmartFilesys    file       44K    |
+---------------------------------------------+
DosType [0x50465303] ( PFS\3  v)      <- edits selected row
[Add from file..] [Copy from disk..] [Remove]
                                        [Done]
```

- **Listview** over `model.fs[]` with a `>` selection-marker column (V37-safe; `GTLV_Selected`
  is V39+ and a no-op on V37, same pattern as the partition list).
- **Add from file…** → ASL requester, initial drawer `L:`, **no pattern filter** (WB2.04
  ASL Ok-button gotcha), `ASLFR_Screen` set to our screen → `fsload_from_file` → append →
  mark dirty.
- **Copy from disk…** → Driver + Unit pick (reuse the cycle widgets and `discover`
  plumbing) → `fsload_from_disk`; sub-pick if the source has >1 embedded FS → append →
  mark dirty.
- **DosType** hex string gadget + preset cycle edits the selected row (mainly to assign a
  type to file-sourced entries).
- **Remove** drops the selected `fs[]` entry (frees `seg_data`), marks dirty. If any
  partition currently uses that DosType, confirm first (after removal it falls back to
  live/resource binding, or fails to mount on a stock machine).
- **Done** closes; all changes are in the model, written on Save.

### 8.3 Edit dialog FS-cycle

The FS-cycle label array becomes: `FFS`, `OFS`, one entry per `model.fs[]` (friendly name
via `dostype_label`), then the existing read-only `keep` entry for any non-ROM type not in
the pool. Selecting a pool entry sets `dos_type` to that filesystem's type and disables the
Intl/Cache checkboxes (those bits only apply to `DOS\x`). The list Type column already
renders friendly names via `fstype_label` / `dostype_label`.

## 9. Testing

### 9.1 Host tests (`tests/`, plain `cc`)

- FSHD/LSEG `serialize` → `parse` round-trip (single FS and multiple).
- Capacity guard returns `RDB_ERR_NO_RDB_SPACE` and writes nothing when oversized.
- File validation: `HUNK_HEADER` accept, non-hunk reject, too-big reject.
- Allocator layout (RDSK / PART / FSHD / LSEG block numbering and chain pointers).
- Preservation: a model parsed from a disk with an embedded FS re-serializes identically
  (the silent-drop regression).

### 9.2 amitools cross-check (`tests/verify-rdbtool.sh`)

Extend so `rdbtool` reads back an embedded filesystem and confirms the stored hunk image is
byte-identical (`rdbtool` can list and export filesystems).

### 9.3 On-target (FS-UAE — the authoritative test per project history)

1. Add PFS3 from file → set DosType → Save → Refresh/reopen → FS still listed (real-RDB
   parse round-trip); cross-check with `rdbtool`.
2. New partition as `PFS\3` → Format → mounts as a working PFS volume **without PFS
   pre-installed** (exercises Strategy 3). This is the feature-3 "Format tested" gate.
3. Copy-from-disk: a scratch disk with PFS embedded → import into the target disk.
4. Regression: Save a disk that already had an embedded FS → it is preserved, not nulled.

## 10. Phasing (one spec, sequenced into independently testable phases)

- **P1 — Engine read + write:** FSHD/LSEG model, `rdb_parse`, `rdb_serialize`, linear
  allocator, capacity guard, model teardown, host tests. Also fixes the silent-drop bug.
- **P2 — Acquisition:** `fsload.c` (from file + copy from disk).
- **P3 — Format Strategy 3:** embedded-FS binding via `InternalLoadSeg`.
- **P4 — GUI:** Filesystems dialog + Edit FS-cycle wiring.
- **P5 — On-target verification** and memory update.

## 11. Risks and open questions

- **Executing reconstructed code:** Strategy 3 runs a freshly `InternalLoadSeg`'d handler.
  Mitigation: only on an explicit Format action; `InternalLoadSeg` is the OS's own
  mechanism; the seglist is owned by the mount afterward.
- **Cold-test environment:** the FS-UAE auto-mount behaviour may make a genuine
  "PFS-not-live" test awkward (same constraint noted for the v0.5 Format work). May require
  a hand-built scratch disk whose filesystem is genuinely absent from the running system.
- **DosType for file-sourced FS:** a load file does not carry its DosType; the user must set
  it. The preset cycle plus the captured reference values reduce the chance of error, but a
  wrong DosType yields a non-mounting partition.
- **`InternalLoadSeg` availability/ABI:** confirm the exact `InternalLoadSeg` calling
  convention and read-hook signature against the V37 dos.library during P3 (it is documented
  but rarely called directly).

## 12. Out of scope

- Harvesting a filesystem from the live `FileSystem.resource` (excluded by decision).
- Editing FSHD handler parameters beyond DosType in the GUI (priority/stack are taken from
  the source or sensible defaults).
- A free-list / incremental RDB block allocator (the linear full-rewrite is sufficient).
- Filesystems larger than the reserved RDB region (guarded, not supported).
