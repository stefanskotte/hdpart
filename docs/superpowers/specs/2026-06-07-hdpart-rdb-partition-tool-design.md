# HDPart — Amiga RDB Partition Tool (Phase 1 Design)

**Date:** 2026-06-07
**Status:** Approved (design); ready for implementation planning
**Target:** AmigaOS / Kickstart 2.04 (V37) and up, 68000+

## 1. Purpose

A modern, genuinely usable replacement for HDToolBox for preparing hard
disks on classic Amigas — in practice CF/SD cards behind IDE/SCSI adapters.
It reads, edits, and writes the **Rigid Disk Block (RDB)** structures that
describe a drive's geometry and partitions.

The defining problem it solves: **HDToolBox forces the user to guess the
correct device driver** among many possibilities. HDPart auto-discovers
devices and presents them with friendly identification (driver, unit, model,
size, status) so the user never types a driver name.

### Phase 1 scope (this spec)

1. Discover and select a device (driver + unit).
2. Read an existing RDB (or detect a blank disk).
3. Initialise a disk (write a fresh RDB with correct geometry).
4. Create / delete / edit partitions.
5. Save (commit RDB to disk).

### Explicitly out of scope (deferred to later phases)

- Custom filesystem types (PFS/SFS) and embedding filesystems on the RDB
  (FSHD / LSEG blocks). Phase 1 uses the ROM FFS only.
- Bootable / boot-priority editing (UI controls present but greyed).
- Bad-block lists, low-level format.
- Disks > 4 GB / TD64 / NSD 64-bit addressing.
- Automatic mount of partitions after save.

## 2. Platform, Toolchain, Build

- **Toolchain:** Bartman `amiga-debug` VSCode extension — `m68k-amiga-elf-gcc`
  (GCC 15.1.0), `elf2hunk`, `vasmm68k_mot`. Full AmigaOS headers present
  (`proto/*`, `inline/*`, `clib/*`, `devices/hardblocks.h`,
  `devices/scsidisk.h`). No C library, no libnix, no crt0 — pure freestanding.
- **OS-app build (new):** The existing `party` Makefile builds a bare-metal
  demo (`-nostdlib -Ttext=0`, custom-hardware). HDPart needs a normal
  AmigaDOS executable:
  - Keep `-nostdlib` (no libc available), **drop `-Ttext=0`**.
  - Provide a minimal **startup** (`startup.s`/`startup.c`) that: fetches
    `SysBase` from absolute address 4, opens `dos.library`, detects CLI vs
    Workbench start (waits for + replies the `WBStartup` message on exit),
    calls `main()`, closes libraries, returns a DOS return code.
  - Link a relocatable hunk executable via `elf2hunk` (the existing
    `--emit-relocs` + `--gc-sections` flags carry over).
  - Use **only AmigaOS APIs** through the `proto/*` inline headers:
    `AllocVec`/`FreeVec`, exec device I/O, `intuition.library`,
    `gadtools.library`, `graphics.library`, `utility.library`. String
    formatting via exec `RawDoFmt`. No printf/malloc.
- **Output binary:** `out/HDPart`.
- **Location:** Repurpose the `party` directory. The original demo has been
  snapshotted to git (`git init` + baseline commit) before restructuring.

## 3. UI Design (approved)

Single-window application (GParted / macOS Disk Utility influence) rendered
as a standard Workbench 2.x GadTools GUI, opening on the default public
screen (Workbench), font-sensitive, with a fallback to its own screen if no
public screen is available.

### 3.1 Main window

- **Device line:** cycle/dropdown gadget showing the selected device
  (`scsi.device unit 0 — SanDisk CF 490MB`) + `Rescan` button.
- **Disk-map bar:** proportional horizontal bar; one segment per partition
  plus unused space; the selected partition is highlighted.
- **Geometry readout:** `N cyl × H heads × S sectors • 512 b/blk`, with the
  partitionable cylinder range at the ends of the bar.
- **Partition table** (list): columns `Device | File System | Start | End |
  Size | Flags`.
- **Actions:** `New`, `Delete`, `Edit…`; right side `Init Disk…`, `Save`.
- **Status line:** RDB read result, partition count, and a prominent
  **unsaved-changes** indicator. `Save` is disabled until the model is dirty.

### 3.2 Device picker dialog ("Select Disk")

- List of discovered disks: `driver unit — model size`, each tagged with
  status: `blank`, `RDB`, `in use` (mounted), `empty` (no media).
- Buttons: `Rescan`, `Manual…` (type a driver/unit by hand), `Cancel`,
  `Use Disk`.

### 3.3 Edit Partition dialog

- `Device name` (string), `File system` (cycle; default **FFS International
  (DOS\3)**), `Size (MB)` numeric + slider that **snaps to cylinder
  boundaries**.
- Live readout: start cyl, end cyl, cylinder count, max free space remaining.
- Bootable / boot-priority / buffers controls present but greyed (phase 2).

## 4. Architecture / Modules

Small, independently testable units communicating through narrow interfaces.

| Module | Responsibility | Depends on |
|--------|----------------|------------|
| `rdb.c/.h` | **Pure RDB engine.** Block checksums (RDSK/PART), parse RDB block chain → in-memory model, serialize model → blocks, geometry math (MB ↔ cylinders), overlap/bounds validation, build default `DosEnvec`. No OS/UI calls. | (none — pure C) |
| `device.c/.h` | Block I/O + discovery. Open device/unit, `TD_GETGEOMETRY`, `TD_CHANGENUM`, `CMD_READ`/`CMD_WRITE` (512-byte blocks), SCSI `INQUIRY` via `HD_SCSICMD` for model strings. Hybrid discovery: AmigaDOS device list + `expansion.library` boot nodes + curated driver-name probe. Returns `DiskInfo[]`. | exec, expansion, dos |
| `model.c/.h` | Session state: selected device, in-memory partition list, dirty flag, cancel/revert. | rdb |
| `gui.c/.h` | GadTools windows (main + device picker + edit dialog) and event loop. | intuition, gadtools, graphics, model, device |
| `startup.s` / `main.c` | Entry, library open/close, CLI/WB detection, top-level wiring. | exec, dos |

### Data flow

```
discover ──► user picks device ──► device.readRDB ──► rdb.parse ──► model
   ▲                                                                  │
   │                                                          GUI edits model
   │                                                          (sets dirty)
   └────────────── re-read & verify ◄── device.write ◄── rdb.serialize ◄── Save
```

## 5. On-disk structures (from `devices/hardblocks.h`)

- `RigidDiskBlock` (`RDSK`, id `0x5244534B`) within the first
  `RDB_LOCATION_LIMIT` (16) blocks: geometry, block-list heads, logical
  drive characteristics, optional drive identification.
- `PartitionBlock` (`PART`, id `0x50415254`): linked list via `pb_Next`;
  carries `pb_DriveName` (BSTR) and `pb_Environment` (`DosEnvec`).
- Optional `FileSysHeaderBlock` (`FSHD`) / `LoadSegBlock` (`LSEG`): **not
  written in phase 1** (ROM FFS used).
- All blocks are checksummed so longword sum == 0.

Default `DosEnvec` for a phase-1 partition: standard 512-byte blocks,
`de_DosType = 0x444F5303` (DOS\3, FFS International), reserved blocks 2,
sane `de_NumBuffers` (30), `de_MaxTransfer`, `de_Mask`, computed
`de_LowCyl`/`de_HighCyl`/`de_Surfaces`/`de_BlocksPerTrack` from device
geometry.

## 6. Safety model

- **Nothing is written until the user presses `Save`.** All editing is
  in-memory on the model.
- `Init Disk` and `Save` show an explicit confirmation requester naming the
  target device and model string.
- Devices backing **mounted volumes** are shown but flagged `in use` and
  require additional confirmation before being targeted.
- After any write, the tool **reads the blocks back and verifies checksums**
  before reporting success.
- Phase 1 confines itself to 32-bit block addressing; the RDB and partition
  metadata live in the low cylinders. >4 GB support is a flagged later
  enhancement.

## 7. Testing strategy

- **Host-side unit tests for `rdb.c`:** because the RDB engine is pure C with
  no Amiga calls, compile it with the host `cc` and assert: checksum
  correctness, parse↔serialize round-trips, geometry/MB-to-cylinder math,
  and overlap/bounds validation against fixtures. Follow TDD for this module.
- **Cross-verification with `amitools rdbtool`** (installed at
  `/opt/homebrew/bin/rdbtool`): (a) RDB generated by the engine is validated
  with `rdbtool ... info`; (b) an RDB created by `rdbtool` parses correctly
  in the engine.
- **Integration in FS-UAE:** boot a real Workbench 2.04/3.1 environment
  (user has the disks), attach a **blank scratch `.hdf`** as a second drive,
  run HDPart against its device, Save, then on the host run
  `rdbtool scratch.hdf info` to confirm, and mount the result in Workbench
  to prove it is real and usable.
- **Build gate:** `make` must produce `out/HDPart` cleanly at each step.

## 8. Risks & mitigations

| Risk | Mitigation |
|------|------------|
| OS-app startup/link with a demo-oriented toolchain | Early spike: minimal startup + "open library, OpenWindow, wait, close" smoke test before any RDB work. |
| Device discovery misses unusual 3rd-party drivers | Hybrid approach (live DOS list + boot nodes + curated probe) and a `Manual…` entry fallback. |
| Writing a wrong RDB damages real media | Emulator + scratch HDF only during development; read-back verification; explicit confirms; mounted devices flagged. |
| Geometry query unsupported by some drivers | Fall back to SCSI `READ CAPACITY` / `INQUIRY`; allow manual geometry entry if needed. |

## 9. Phase boundaries (roadmap)

- **Phase 1 (this spec):** discover → read → init → partition → save, FFS
  default, full safety + verification.
- **Phase 2+:** bootable/boot-priority UI, selectable filesystem types and
  embedding custom filesystems (FSHD/LSEG), >4 GB / TD64, bad-block handling,
  mount-after-save, custom visual theme layer.
- **Localization (later):** translate the UI via AmigaOS built-in
  `locale.library` catalogs (`OpenCatalog`/`GetCatalogStr`). Note
  `locale.library` is V38 (OS 2.1)+, above our V37 baseline, so it must be
  opened defensively with an English built-in fallback when absent. To keep
  this cheap to add, **phase 1 routes all user-facing strings through a
  single table/`STR_*` id scheme** rather than inlining literals, so the
  catalog layer drops in without touching call sites.
