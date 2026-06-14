# HDPart — Format Partition (+ boot-device safety) — Design

Date: 2026-06-14
Status: Approved (brainstorming) — ready for implementation plan

## Goal

Add a **Format** action so HDPart can turn a freshly-created partition into a
usable, empty AmigaDOS volume **without a reboot**, and add a **boot-device /
in-use safety layer** that prevents the user from partitioning or formatting the
disk the running system depends on.

This is the engine-side counterpart to the existing in-memory `.device` driver
loader and the RDB editor. It targets the same audience (CF/SD-on-IDE retro
setups, Kickstart 2.04/V37+).

## Scope (v1)

In scope:
- Format a **single selected partition** into an empty volume, live (no reboot).
- **ROM FastFileSystem family only**: DosTypes `DOS\0`..`DOS\5` (OFS/FFS, Intl,
  DirCache), i.e. whatever is registered in `FileSystem.resource`. This matches
  what the partition editor already lets the user choose.
- **Newly-created partitions** that the OS is not already using.
- A shared **safety classifier** that guards both **Format** and **Save**
  (writing the RDB), since both are destructive to a live boot disk.

Explicitly out of scope (deferred):
- Non-ROM filesystems (PFS3/SFS/custom). Needs the not-yet-built "embed a
  filesystem (FSHD/LSEG) in the RDB" feature; until then Format refuses these
  DosTypes with a clear message. See `hdpart-filesystem-types` memory / the
  deferred "feature B".
- **Reformatting a partition the OS already has mounted** (would require
  `Inhibit`-ing and reusing the existing node rather than `AddDosNode`). v1
  refuses when the partition's DOS name is already live.
- Full/zeroing format or verify pass. v1 does the standard quick format
  (`ACTION_FORMAT` writes root block + bitmap only), exactly like `C:Format`
  without `QUICK`-vs-full distinctions beyond what the handler does.
- >4GB / TD64 addressing (unchanged project-wide limitation).

## Approach (decided)

**"Do what `C:Format` does."** After making the partition visible to the OS we
delegate the actual on-disk format to the **authoritative ROM filesystem
handler** via the `ACTION_FORMAT` DOS packet, rather than re-deriving FFS on-disk
layout ourselves. The handler writes the correct root/bitmap for its exact
Kickstart and the chosen DosType (OFS/FFS/Intl/DirCache) for free.

Rejected alternatives:
- *Hybrid* (write boot/root/bitmap blocks ourselves, then `AddDosNode` to mount):
  host-testable, but re-implements every FFS variant for a step the ROM handler
  already does perfectly, and still needs the same `AddDosNode` binding. More
  code, more risk of "not validated" volumes.
- *Block-level format + reboot*: simplest and fully self-contained, but the user
  explicitly wants the volume usable immediately without a reboot.

## Components

### 1. Safety classifier — new `src/safety.c` / `safety.h`

Pure-ish module that answers: *how live is the physical device (driver, unit)
the user is about to write?* Used by both Format and Save.

```
typedef enum { DEV_CLEAR, DEV_MOUNTED, DEV_BOOT } DevLiveness;

/* Classify the given (driver name, unit) against the running system.
   names[] / count: optional out-list of DOS device names mounted on this
   physical device (for the warning text), up to a small cap. */
DevLiveness safety_classify(const char *driver, ULONG unit,
                            char mounted_names[][8], int max_names, int *n_names);
```

Logic:
1. **Boot device:** `GetDeviceProc("SYS:", NULL)` → `dvp_Port`. Walk the DOS
   device list (`LockDosList(LDF_DEVICES|LDF_READ)` / `NextDosEntry`); the
   `DLT_DEVICE` whose `dol_Task == dvp_Port` is the boot device. Read its
   `dol_Startup` (BPTR `FileSysStartupMsg`) → `fssm_Device` (BSTR) + `fssm_Unit`.
   If that (name, unit) matches the target → `DEV_BOOT`.
2. **Mounted, not boot:** while walking the list, collect every `DLT_DEVICE`
   whose `fssm` resolves to the same (driver, unit). If any exist (and it is not
   the boot device) → `DEV_MOUNTED`, returning their DOS names.
3. Otherwise → `DEV_CLEAR`.
4. If `GetDeviceProc("SYS:")` fails or the boot device can't be resolved, fail
   safe: never downgrade below `DEV_MOUNTED` for a device that has any mounted
   volume; if truly nothing is determinable, return `DEV_MOUNTED` (warn) rather
   than `DEV_CLEAR`.

Reuses the existing `FileSysStartupMsg` parsing patterns from discovery (guard
BPTR derefs with `TypeOfMem()`, require a real `*.device` name — see
`hdpart-project` runtime-gotchas). All BPTR/`fssm` handling stays in this module.

Call sites:
- **Format:** `DEV_BOOT` → hard block (OK-only dialog). `DEV_MOUNTED` →
  confirmable warning listing the mounted volumes. `DEV_CLEAR` → normal
  destructive confirm.
- **Save (`gui_save`):** same classification. `DEV_BOOT` → hard block (refuse the
  write). `DEV_MOUNTED` → confirmable warning. `DEV_CLEAR` → existing behaviour.

### 2. Format engine — new `src/format.c` / `format.h`

```
typedef enum {
    FMT_OK = 0,
    FMT_ERR_NO_HANDLER,   /* DosType not in FileSystem.resource (non-ROM FS) */
    FMT_ERR_NAME_TAKEN,   /* DOS device name already live */
    FMT_ERR_MAKENODE,     /* MakeDosNode / env build failed */
    FMT_ERR_ADDNODE,      /* AddDosNode failed */
    FMT_ERR_FORMAT        /* ACTION_FORMAT packet failed */
} FmtResult;

/* Build env from model+partition, bind ROM FFS handler, AddDosNode live,
   Inhibit + ACTION_FORMAT(volname, dostype) + Inhibit off. */
FmtResult format_partition(const char *driver, ULONG unit,
                           const RdbModel *m, int part_index,
                           const char *volname);
```

Steps:
1. **Build the parameter packet / `DosEnvec`** from `RdbModel` + the partition:
   `de_SizeBlock` (block_bytes/4 = 128 longs for 512), `de_Surfaces` (heads),
   `de_SectorPerBlock` (1), `de_BlocksPerTrack` (sectors), `de_Reserved` (2),
   `de_LowCyl`/`de_HighCyl` (partition range), `de_NumBuffers`, `de_BufMemType`,
   `de_MaxTransfer`, `de_Mask`, `de_DosType`. (This struct-build is the
   host-testable seam.)
2. **`MakeDosNode`** (expansion.library, open v37) with the driver name + unit +
   env → `DeviceNode`.
3. **Bind handler:** `OpenResource("FileSystem.resource")`; walk
   `fsr_FileSysEntries` for `fse_DosType == dos_type`; copy `fse_SegList` into
   `dn_SegList` and apply `fse_PatchFlags` fields (GlobalVec, Stack, Priority,
   Startup, etc.). If none → `FMT_ERR_NO_HANDLER`.
4. **Name-clash:** if `FindDosEntry`/the DOS list already has this device name →
   `FMT_ERR_NAME_TAKEN`.
5. **`AddDosNode(0, ADNF_STARTPROC, dn)`** → `DHx:` appears, handler process
   starts.
6. **Format:** `Inhibit("DHx:", DOSTRUE)` → `DoPkt(handlerport, ACTION_FORMAT,
   BSTR volname, dostype)` → `Inhibit("DHx:", DOSFALSE)`. (`ACTION_FORMAT` = 1227;
   define locally — not in toolchain headers.)
7. Return `FMT_OK`; on any failure return the specific code (GUI maps to text).

Notes/risks captured for the plan:
- Memory for the env/parameter packet and BSTRs must outlive the node
  (`AddDosNode` keeps the structures). Allocate appropriately; do not free a
  successfully-added node's structures.
- The Shell-stack constraint applies (keep large buffers static/heap; see
  `hdpart-project`).
- `ACTION_FORMAT` arg1 is a **BSTR** volume name, arg2 is the DosType ULONG.

### 3. Engine preconditions (enforced in the GUI before calling)

- **Saved-first:** require `g_dirty == 0` (partition table written to the RDB) so
  the formatted partition persists across the eventual reboot. If dirty →
  prompt to Save first (Save itself runs the safety check, so a boot-device disk
  can't be saved and therefore can't be formatted — consistent).
- ROM-FFS-only DosType (else `FMT_ERR_NO_HANDLER` message).
- Name not already live (`FMT_ERR_NAME_TAKEN` message).

### 4. GUI (`src/gui.c`)

- **"Format…"** button in the partition action toolbar (selection-scoped, beside
  `Resize…`) + a **Partition menu** item, both ghosted with selection like the
  others, with a plain-key/`rA` shortcut consistent with existing items.
- **`gui_format_dialog(index)`** built on the shared `dlg_*` helpers: a
  volume-name string gadget (defaults to the partition's name), a read-only line
  showing the filesystem (from DosType via `dostype_label`), the safety
  classification rendered inline, and a clearly-destructive Format/Cancel.
  - `DEV_BOOT`: show the block message, OK-only (no Format button).
  - `DEV_MOUNTED`: show the warning + the mounted volume names; Format requires
    an explicit confirm.
  - `DEV_CLEAR`: standard destructive confirm.
- On success: status line "Formatted DHx: (empty <FS> volume)." On failure: map
  `FmtResult` to a specific message.
- **Save** path (`gui_save`) gains the same `safety_classify` gate before
  writing.

## Error handling

| Condition | Handling |
|---|---|
| Target is boot device | Hard block, both Format and Save. No override. |
| Target has other mounted volumes | Confirmable warning (lists names). |
| Partition table dirty | Refuse Format; prompt to Save first. |
| DosType not ROM FFS | Refuse: "needs embedded-FS support (not yet available)". |
| DOS name already live | Refuse: name clash. |
| MakeDosNode/AddDosNode/ACTION_FORMAT fail | Specific message; leave disk + RDB untouched as far as possible. |
| `GetDeviceProc("SYS:")` inconclusive | Fail safe → treat as `DEV_MOUNTED` (warn). |

## Testing

Host-testable (system cc, no Amiga libs — extend `tests/`):
- `DosEnvec`/parameter-packet construction from a synthetic `RdbModel`+partition
  (field-by-field).
- Safety classifier decision logic given a synthetic device list (boot match /
  mounted match / clear / inconclusive → fail-safe), with the OS calls behind a
  thin shim so the pure decision logic is exercised on the host.

On-target (FS-UAE):
- `HDPart-204-devtest` (KS2.04, scratch disks, **not** the boot device — boot is
  the WB floppy): create a partition on a scratch disk → Save → Format → confirm
  `DHx:` mounts live and is a usable empty volume (write a file from a Shell).
- `HDPart-31-devtest` (KS3.x) repeat, to confirm the FFS handler binding and
  ACTION_FORMAT across V37 and V39/40.
- Boot-device block: attempt to Save/Format the booted device → verify hard
  block.

## Out-of-scope follow-ups (noted, not built here)

- Embed a filesystem (FSHD/LSEG) from disk/file so non-ROM FS (PFS3/SFS) can be
  selected and formatted ("feature B").
- Reformat an already-mounted partition (Inhibit + reuse existing node).
