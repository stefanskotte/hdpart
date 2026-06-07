# HDPart — Plan 2: Device I/O + Discovery — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give HDPart the ability to open Amiga block devices (`scsi.device`, `lide.device`, the emulator's `uaehf.device`, …), read drive geometry, read/write 512-byte blocks, identify drives via SCSI INQUIRY, and **auto-discover the disks present on the system** so the user never types a driver name — then prove it end-to-end by reading/writing real RDBs on scratch hardfiles in FS-UAE, cross-checked with `rdbtool`.

**Architecture:** Two new modules. `device.c` is a thin, focused wrapper over exec device I/O (CreateMsgPort/CreateIORequest/OpenDevice/DoIO) exposing open/close, geometry, block read/write, and an INQUIRY model string; it also provides a `BlockIO` adapter so the existing pure `rdb.c` engine can parse/serialize an RDB straight off a real device. `discover.c` builds a de-duplicated list of `DiscDisk` records by (a) scanning the AmigaDOS device list and (b) probing a curated list of known drivers/units, classifying each as no-media / blank / has-RDB / mounted. Pure helpers (BCPL-string conversion, de-dupe, blocks→MB) are unit-tested on the host; the OS-bound code is verified in FS-UAE via a temporary `scan` CLI mode and `rdbtool` cross-checks.

**Tech Stack:** C (freestanding m68k + hosted tests), exec device I/O, dos.library DosList, `devices/trackdisk.h` (TD_GETGEOMETRY), `devices/scsidisk.h` (HD_SCSICMD INQUIRY), amitools `rdbtool`, FS-UAE.

---

## Prerequisites & conventions

Plan 1 is complete: `src/rdb.c`/`rdb.h` (RDB engine with `BlockIO` typedef and `rdb_parse`), `src/device`-less build, `src/startup.c`, `src/main.c` (window + own-screen fallback), `src/support.c`, host tests, FS-UAE configs. Work on git `master`.

Amiga build prelude (toolchain not on default PATH):
```bash
export AMIGA_BIN="/Users/sfs/.vscode/extensions/bartmanabyss.amiga-debug-1.8.2/bin/darwin"
export PATH="$AMIGA_BIN/opt/bin:$AMIGA_BIN:$PATH"
```
Host tests: system `cc`. `rdbtool` at `/opt/homebrew/bin/rdbtool`. Project root `/Users/sfs/Devel/party`.

Key facts already verified against the SDK headers:
- `struct IOExtTD { struct IOStdReq iotd_Req; ULONG iotd_Count; ULONG iotd_SecLabel; }`.
- `TD_GETGEOMETRY` fills `struct DriveGeometry { ULONG dg_SectorSize, dg_TotalSectors, dg_Cylinders, dg_CylSectors, dg_Heads, dg_TrackSectors, dg_BufMemType; UBYTE dg_DeviceType, dg_Flags; UWORD dg_Reserved; }`.
- Block I/O: `io_Command = CMD_READ/CMD_WRITE`, `io_Data = buf`, `io_Length = bytes`, `io_Offset = block*512`, then `DoIO`. `io_Error == 0` on success.
- SCSI: `io_Command = HD_SCSICMD` (28), `io_Data = &struct SCSICmd`, `io_Length = sizeof(SCSICmd)`.
- `exec`: `CreateMsgPort`, `DeleteMsgPort`, `CreateIORequest`, `DeleteIORequest`, `OpenDevice`, `CloseDevice`, `DoIO`, `AllocVec`, `FreeVec` (all V37, in `inline/exec.h`).
- `dos`: `LockDosList`/`NextDosList`/`UnLockDosList` with `LDF_DEVICES|LDF_READ`; `DLT_DEVICE == 0`; device node `dol_Startup` (a BPTR to `FileSysStartupMsg { ULONG fssm_Unit; BSTR fssm_Device; BPTR fssm_Environ; ULONG fssm_Flags; }`); `fssm_Device` is a BSTR (BCPL string: length byte then chars). `BADDR(bptr) == (APTR)((ULONG)(bptr) << 2)`. `GetArgStr`, `Output`, `Write` for the CLI scan mode.

**Scope note (honored from the spec):** Plan 2 discovery uses the **DOS device list + a curated driver probe**. Enriching discovery by also walking `expansion.library`'s boot-node MountList is deferred to a later enhancement — the DOS-list + probe combination already finds mounted disks *and* blank/uninitialised disks (including the FS-UAE scratch hardfiles via `uaehf.device`). 64-bit/TD64 addressing remains out of scope (RDB lives in low blocks).

**Driver-source limitation (deferred, see spec roadmap):** the curated probe calls `OpenDevice`, which only finds drivers **already resident** in the exec device list — ROM/autoconfig drivers and controller-loaded ones like `lide.device`. **Disk-based drivers living as files in `DEVS:`** (e.g. `devs:scsi.device`) are not in the exec list until `Mount`ed/loaded, so they are NOT discovered in Plan 2. Scanning `DEVS:`/`DEVS:storage` for `.device` files and loading them before probing is a later phase. (The `Manual…` device-entry path planned for the GUI in Plan 3 is the interim escape hatch for such drivers.)

---

## File structure (this plan)

```
src/
  device.h     NEW  device I/O API + DeviceInfo
  device.c     NEW  exec device wrapper (open/close/geometry/rw/inquiry/block-io)
  discover.h   NEW  discovery API + DiscDisk + pure helpers
  discover.c   NEW  DOS-list scan + curated probe + classify/dedupe
  main.c       MOD  add temporary `scan` CLI mode (observability); GUI unchanged
tests/
  test_discover.c   NEW  host unit tests for pure helpers (bcpl->c, dedupe, blocks->mb)
  run-host-tests.sh MOD  also build+run test_discover
tools/fsuae/
  HDPart-204-devtest.fs-uae  NEW  KS2.04 + WB2.04 + scratch hardfiles (blank + RDB)
tools/
  make-scratch-hdfs.sh  NEW  create scratch .hdf fixtures with rdbtool
```

---

## Milestone 2.1 — Device I/O module

Outcome: `device.c` compiles/links into `out/HDPart.exe`; provides open/close, geometry, block read/write, and a `BlockIO` adapter usable by `rdb.c`.

### Task 2.1.1: Device API header

**Files:** Create `src/device.h`

- [ ] **Step 1: Write `src/device.h`**

```c
#ifndef HDPART_DEVICE_H
#define HDPART_DEVICE_H

#include <exec/types.h>
#include <stdint.h>

/* Geometry + identity for one drive (filled by dev_geometry / dev_inquiry_model). */
typedef struct {
    ULONG block_bytes;     /* bytes per sector (usually 512) */
    ULONG cylinders;
    ULONG heads;
    ULONG sectors;         /* sectors per track */
    ULONG total_blocks;    /* total sectors on the drive */
    int   has_media;       /* nonzero if geometry query succeeded */
    char  model[40];       /* "VENDOR PRODUCT" from INQUIRY, or "" */
} DeviceInfo;

/* Opaque open-device handle. */
typedef struct DeviceHandle DeviceHandle;

/* Open driver/unit for block I/O. Returns NULL on any failure (no media counts
   as success for OpenDevice on most drivers; use dev_geometry to detect media). */
DeviceHandle *dev_open(const char *driver, ULONG unit);
void          dev_close(DeviceHandle *h);

/* Query geometry via TD_GETGEOMETRY. Returns 0 on success, else the io_Error. */
int dev_geometry(DeviceHandle *h, DeviceInfo *out);

/* Read/write one 512-byte block. Returns 0 on success, else io_Error. */
int dev_read_block (DeviceHandle *h, ULONG block, UBYTE *buf512);
int dev_write_block(DeviceHandle *h, ULONG block, UBYTE *buf512);

/* Best-effort SCSI INQUIRY model string. Returns 0 on success (model filled),
   nonzero if the device does not support HD_SCSICMD (model set to ""). */
int dev_inquiry_model(DeviceHandle *h, char model[40]);

/* BlockIO adapter for the RDB engine (src/rdb.h). ctx must be a DeviceHandle*.
   Matches the BlockIO typedef: returns 0 on success, nonzero on I/O error. */
int dev_block_io(void *ctx, uint32_t block, uint8_t *buf, int write);

#endif /* HDPART_DEVICE_H */
```

- [ ] **Step 2: Commit**

```bash
cd /Users/sfs/Devel/party
git add src/device.h
git commit -m "feat(device): block-device I/O API header"
```

### Task 2.1.2: Device open/close + geometry + block read/write

**Files:** Create `src/device.c`; Modify `src/main.c` (temporary link-smoke ref)

- [ ] **Step 1: Write `src/device.c`**

```c
/* HDPart device I/O: a thin wrapper over exec block-device access. Uses only
   V37 exec/dos APIs. No libc. */
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/io.h>
#include <devices/trackdisk.h>
#include <devices/scsidisk.h>
#include <proto/exec.h>
#include "device.h"

struct DeviceHandle {
    struct MsgPort *port;
    struct IOExtTD *req;
    int             opened;   /* OpenDevice succeeded */
};

DeviceHandle *dev_open(const char *driver, ULONG unit)
{
    DeviceHandle *h = (DeviceHandle *)AllocVec(sizeof(*h), MEMF_CLEAR);
    if (!h) return 0;

    h->port = CreateMsgPort();
    if (!h->port) { dev_close(h); return 0; }

    h->req = (struct IOExtTD *)CreateIORequest(h->port, sizeof(struct IOExtTD));
    if (!h->req) { dev_close(h); return 0; }

    if (OpenDevice((CONST_STRPTR)driver, unit,
                   (struct IORequest *)h->req, 0) != 0) {
        dev_close(h);
        return 0;
    }
    h->opened = 1;
    return h;
}

void dev_close(DeviceHandle *h)
{
    if (!h) return;
    if (h->opened) CloseDevice((struct IORequest *)h->req);
    if (h->req)    DeleteIORequest((struct IORequest *)h->req);
    if (h->port)   DeleteMsgPort(h->port);
    FreeVec(h);
}

int dev_geometry(DeviceHandle *h, DeviceInfo *out)
{
    struct DriveGeometry dg;
    LONG err;
    int i;
    /* zero dg so undefined fields read as 0 */
    for (i = 0; i < (int)sizeof(dg); i++) ((UBYTE *)&dg)[i] = 0;

    h->req->iotd_Req.io_Command = TD_GETGEOMETRY;
    h->req->iotd_Req.io_Data    = &dg;
    h->req->iotd_Req.io_Length  = sizeof(dg);
    h->req->iotd_Req.io_Offset  = 0;
    h->req->iotd_Req.io_Actual  = 0;
    err = DoIO((struct IORequest *)h->req);

    out->block_bytes  = dg.dg_SectorSize ? dg.dg_SectorSize : 512;
    out->cylinders    = dg.dg_Cylinders;
    out->heads        = dg.dg_Heads;
    out->sectors      = dg.dg_TrackSectors;
    out->total_blocks = dg.dg_TotalSectors;
    out->has_media    = (err == 0);
    out->model[0]     = 0;
    return (int)err;
}

static int dev_rw(DeviceHandle *h, ULONG block, UBYTE *buf, UWORD cmd)
{
    h->req->iotd_Req.io_Command = cmd;
    h->req->iotd_Req.io_Data    = buf;
    h->req->iotd_Req.io_Length  = 512;
    h->req->iotd_Req.io_Offset  = block * 512UL;   /* 32-bit: RDB is low */
    h->req->iotd_Req.io_Actual  = 0;
    return (int)DoIO((struct IORequest *)h->req);
}

int dev_read_block(DeviceHandle *h, ULONG block, UBYTE *buf512)
{
    return dev_rw(h, block, buf512, CMD_READ);
}

int dev_write_block(DeviceHandle *h, ULONG block, UBYTE *buf512)
{
    return dev_rw(h, block, buf512, CMD_WRITE);
}

int dev_block_io(void *ctx, uint32_t block, uint8_t *buf, int write)
{
    DeviceHandle *h = (DeviceHandle *)ctx;
    return write ? dev_write_block(h, block, (UBYTE *)buf)
                 : dev_read_block (h, block, (UBYTE *)buf);
}

/* dev_inquiry_model is added in Task 2.2.1 */
```

- [ ] **Step 2: Add a temporary link-smoke reference so device.o links and undefined symbols surface**

In `src/main.c`, replace the existing link-smoke block
```c
    RdbModel probe;
    rdb_init_model(&probe, 100, 4, 17);   /* link-smoke: pulls in rdb.o */
    (void)probe;
    (void)wbmsg;
```
with
```c
    RdbModel probe;
    DeviceHandle *probe_dev;
    rdb_init_model(&probe, 100, 4, 17);   /* link-smoke: pulls in rdb.o */
    probe_dev = dev_open("__nonexistent.device", 0); /* link-smoke: pulls in device.o */
    if (probe_dev) dev_close(probe_dev);
    (void)probe;
    (void)wbmsg;
```
and add `#include "device.h"` near the top of `src/main.c` (after `#include "rdb.h"`).

- [ ] **Step 3: Build for m68k and confirm clean link**

Run:
```bash
export AMIGA_BIN="/Users/sfs/.vscode/extensions/bartmanabyss.amiga-debug-1.8.2/bin/darwin"
export PATH="$AMIGA_BIN/opt/bin:$AMIGA_BIN:$PATH"
cd /Users/sfs/Devel/party && make clean && make 2>&1 | grep -iE "compil|link|elf2hunk|entry-order|undefined|error"
```
Expected: compiles `src/device.c`, links cleanly (no undefined `CreateMsgPort`/`DoIO`/etc.), `entry-order OK`.

- [ ] **Step 4: Confirm host tests still pass (no shared-file regressions)**

Run: `cd /Users/sfs/Devel/party && ./tests/run-host-tests.sh 2>&1 | tail -1`
Expected: `ALL TESTS PASSED`.

- [ ] **Step 5: Commit**

```bash
cd /Users/sfs/Devel/party
git add src/device.c src/main.c
git commit -m "feat(device): open/close, TD_GETGEOMETRY, block read/write, BlockIO adapter"
```

## Milestone 2.2 — SCSI INQUIRY identification

### Task 2.2.1: dev_inquiry_model

**Files:** Modify `src/device.c`

- [ ] **Step 1: Implement `dev_inquiry_model` (append to `src/device.c`)**

```c
/* Standard SCSI INQUIRY (6-byte CDB), 36-byte response.
   Response: bytes 8..15 = vendor id, 16..31 = product id (space padded). */
int dev_inquiry_model(DeviceHandle *h, char model[40])
{
    static UBYTE cdb[6];
    static UBYTE data[36];
    static UBYTE sense[32];
    struct SCSICmd sc;
    LONG err;
    int i, n;

    model[0] = 0;
    for (i = 0; i < 6;  i++) cdb[i]  = 0;
    for (i = 0; i < 36; i++) data[i] = 0;
    cdb[0] = 0x12;   /* INQUIRY */
    cdb[4] = 36;     /* allocation length */

    sc.scsi_Data       = (UWORD *)data;
    sc.scsi_Length     = 36;
    sc.scsi_Actual     = 0;
    sc.scsi_Command    = cdb;
    sc.scsi_CmdLength  = 6;
    sc.scsi_CmdActual  = 0;
    sc.scsi_Flags      = SCSIF_READ | SCSIF_AUTOSENSE;
    sc.scsi_Status     = 0;
    sc.scsi_SenseData  = sense;
    sc.scsi_SenseLength= sizeof(sense);
    sc.scsi_SenseActual= 0;

    h->req->iotd_Req.io_Command = HD_SCSICMD;
    h->req->iotd_Req.io_Data    = &sc;
    h->req->iotd_Req.io_Length  = sizeof(sc);
    h->req->iotd_Req.io_Actual  = 0;
    err = DoIO((struct IORequest *)h->req);
    if (err != 0) return (int)err;     /* device has no SCSI passthrough */

    /* Compose "VENDOR PRODUCT", trimming trailing spaces of each field. */
    n = 0;
    for (i = 8; i < 16 && n < 39; i++) {
        if (data[i] >= 32 && data[i] < 127) model[n++] = (char)data[i];
    }
    while (n > 0 && model[n-1] == ' ') n--;
    if (n < 39) model[n++] = ' ';
    for (i = 16; i < 32 && n < 39; i++) {
        if (data[i] >= 32 && data[i] < 127) model[n++] = (char)data[i];
    }
    while (n > 0 && model[n-1] == ' ') n--;
    model[n] = 0;
    return 0;
}
```

- [ ] **Step 2: Build for m68k**

Run:
```bash
export AMIGA_BIN="/Users/sfs/.vscode/extensions/bartmanabyss.amiga-debug-1.8.2/bin/darwin"
export PATH="$AMIGA_BIN/opt/bin:$AMIGA_BIN:$PATH"
cd /Users/sfs/Devel/party && make 2>&1 | grep -iE "compil|link|elf2hunk|entry-order|undefined|error"
```
Expected: clean build (`SCSIF_AUTOSENSE` resolves from `devices/scsidisk.h`).

- [ ] **Step 3: Commit**

```bash
cd /Users/sfs/Devel/party
git add src/device.c
git commit -m "feat(device): best-effort SCSI INQUIRY model string"
```

## Milestone 2.3 — Discovery

Outcome: `discover_disks()` returns a de-duplicated, classified list. Pure helpers host-tested.

### Task 2.3.1: Discovery header + pure helpers (host-tested)

**Files:** Create `src/discover.h`; Create `tests/test_discover.c`; Modify `tests/run-host-tests.sh`

- [ ] **Step 1: Write `src/discover.h`**

```c
#ifndef HDPART_DISCOVER_H
#define HDPART_DISCOVER_H

#include <exec/types.h>

#define DISC_MAX        32
#define DISC_DRIVER_LEN 40
#define DISC_LABEL_LEN  40

typedef enum {
    DST_UNKNOWN = 0,
    DST_NOMEDIA,   /* device opened but no media / geometry failed */
    DST_BLANK,     /* media present, no valid RDB */
    DST_RDB,       /* media present, valid RDB found */
    DST_MOUNTED    /* referenced by a mounted AmigaDOS device */
} DiskStatus;

typedef struct {
    char       driver[DISC_DRIVER_LEN];  /* exec device name, e.g. "scsi.device" */
    ULONG      unit;
    ULONG      size_mb;                   /* from geometry, 0 if unknown */
    DiskStatus status;
    char       label[DISC_LABEL_LEN];     /* model string / DOS name for display */
} DiscDisk;

/* ---- pure helpers (no OS calls; unit-tested on host) ---- */

/* Convert a BCPL string (length byte followed by chars) to a C string.
   `bcpl` points at the length byte. Truncates to outsz-1. */
void disc_bcpl_to_c(const unsigned char *bcpl, char *out, int outsz);

/* Find driver+unit in list; return index or -1. Case-sensitive on driver. */
int disc_find(const DiscDisk list[], int count, const char *driver, ULONG unit);

/* Convert a block count + block size to whole MB (floor), 32-bit safe. */
ULONG disc_blocks_to_mb(ULONG total_blocks, ULONG block_bytes);

/* ---- OS-bound entry point (implemented in discover.c) ---- */
/* Fill out[] with up to `max` discovered disks; return the count. */
int discover_disks(DiscDisk out[], int max);

#endif /* HDPART_DISCOVER_H */
```

- [ ] **Step 2: Write the failing host tests `tests/test_discover.c`**

```c
/* Host unit tests for discover.c pure helpers. Built and run by run-host-tests.sh.
   The OS-bound discover_disks() is NOT exercised here (it needs AmigaOS). */
#include <stdio.h>
#include <string.h>
#include "../src/discover.h"

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); g_fail++; } } while (0)

static void test_bcpl_to_c(void)
{
    /* BCPL string: length byte 4, then "scsi" */
    unsigned char b[8] = { 4, 's', 'c', 's', 'i', 0, 0, 0 };
    char out[16];
    disc_bcpl_to_c(b, out, sizeof(out));
    CHECK(strcmp(out, "scsi") == 0);

    /* truncation to outsz-1 */
    unsigned char b2[6] = { 5, 'a','b','c','d','e' };
    char small[3];
    disc_bcpl_to_c(b2, small, sizeof(small));
    CHECK(strcmp(small, "ab") == 0);
}

static void test_find(void)
{
    DiscDisk list[2];
    memset(list, 0, sizeof(list));
    strcpy(list[0].driver, "scsi.device"); list[0].unit = 0;
    strcpy(list[1].driver, "scsi.device"); list[1].unit = 1;
    CHECK(disc_find(list, 2, "scsi.device", 0) == 0);
    CHECK(disc_find(list, 2, "scsi.device", 1) == 1);
    CHECK(disc_find(list, 2, "scsi.device", 2) == -1);
    CHECK(disc_find(list, 2, "lide.device", 0) == -1);
}

static void test_blocks_to_mb(void)
{
    /* 8192 blocks * 512 = 4 MiB */
    CHECK(disc_blocks_to_mb(8192, 512) == 4);
    /* 1,000,000 blocks * 512 = 488 MiB (floor) */
    CHECK(disc_blocks_to_mb(1000000, 512) == 488);
    CHECK(disc_blocks_to_mb(0, 512) == 0);
    CHECK(disc_blocks_to_mb(100, 0) == 0);   /* guard against /0 */
}

int main(void)
{
    test_bcpl_to_c();
    test_find();
    test_blocks_to_mb();
    if (g_fail) { printf("%d CHECK(s) FAILED\n", g_fail); return 1; }
    printf("DISCOVER TESTS PASSED\n");
    return 0;
}
```

- [ ] **Step 3: Make `tests/run-host-tests.sh` also build+run the discover tests**

Replace the contents of `tests/run-host-tests.sh` with:
```sh
#!/bin/sh
# Compile and run HDPart host unit tests with the system compiler.
set -e
cd "$(dirname "$0")/.."
cc -std=c99 -Wall -Wextra -g -o /tmp/hdpart_tests tests/test_rdb.c src/rdb.c
/tmp/hdpart_tests
cc -std=c99 -Wall -Wextra -g -o /tmp/hdpart_disc tests/test_discover.c src/discover.c
/tmp/hdpart_disc
```

- [ ] **Step 4: Run tests to verify they fail (no discover.c yet)**

Run: `cd /Users/sfs/Devel/party && ./tests/run-host-tests.sh`
Expected: first suite `ALL TESTS PASSED`, then FAIL compiling the discover suite (`src/discover.c` missing / helpers undefined).

- [ ] **Step 5: Create `src/discover.c` with ONLY the pure helpers for now**

```c
/* HDPart device discovery. This file mixes pure helpers (host-testable) with
   OS-bound scanning (added in the next task, guarded by __amigaos__-style use). */
#include "discover.h"

void disc_bcpl_to_c(const unsigned char *bcpl, char *out, int outsz)
{
    int len = bcpl ? bcpl[0] : 0;
    int i;
    if (outsz <= 0) return;
    if (len > outsz - 1) len = outsz - 1;
    for (i = 0; i < len; i++) out[i] = (char)bcpl[1 + i];
    out[len] = 0;
}

int disc_find(const DiscDisk list[], int count, const char *driver, ULONG unit)
{
    int i, j;
    for (i = 0; i < count; i++) {
        if (list[i].unit != unit) continue;
        for (j = 0; driver[j] && list[i].driver[j] == driver[j]; j++) ;
        if (driver[j] == 0 && list[i].driver[j] == 0) return i;
    }
    return -1;
}

ULONG disc_blocks_to_mb(ULONG total_blocks, ULONG block_bytes)
{
    ULONG blocks_per_mb;
    if (block_bytes == 0) return 0;
    blocks_per_mb = (1024UL * 1024UL) / block_bytes;   /* 2048 for 512 */
    if (blocks_per_mb == 0) return 0;
    return total_blocks / blocks_per_mb;
}
```

Note: `src/discover.c` must compile on BOTH host (for these helpers) and m68k. The OS-bound `discover_disks()` added in Task 2.3.2 uses AmigaOS headers; to keep the host build clean, that function is wrapped in `#ifdef HDPART_AMIGA`. The Makefile defines `HDPART_AMIGA` for the m68k build (added in Task 2.3.2 Step 2); the host test compile does not define it, so only the pure helpers compile on host.

- [ ] **Step 6: Run tests to verify they pass**

Run: `cd /Users/sfs/Devel/party && ./tests/run-host-tests.sh`
Expected: `ALL TESTS PASSED` then `DISCOVER TESTS PASSED`.

- [ ] **Step 7: Commit**

```bash
cd /Users/sfs/Devel/party
git add src/discover.h src/discover.c tests/test_discover.c tests/run-host-tests.sh
git commit -m "feat(discover): pure helpers (bcpl->c, find, blocks->mb) + host tests"
```

### Task 2.3.2: OS-bound discovery (DOS list scan + curated probe)

**Files:** Modify `src/discover.c`, `Makefile`

- [ ] **Step 1: Append the OS-bound discovery to `src/discover.c`**

Add at the TOP of `src/discover.c` (above the pure helpers), the AmigaOS includes guarded so host builds skip them:
```c
#ifdef HDPART_AMIGA
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dosextens.h>
#include <dos/filehandler.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include "device.h"
#include "rdb.h"
#endif
```

Add at the BOTTOM of `src/discover.c`:
```c
#ifdef HDPART_AMIGA

/* Curated list of common block-device drivers to probe (units 0..PROBE_UNITS-1).
   uaehf.device covers FS-UAE/WinUAE hardfiles; scsi.device is the A600/A1200
   built-in IDE and most controllers; the rest are common 3rd-party controllers. */
static const char *const kProbeDrivers[] = {
    "scsi.device", "2nd.scsi.device", "uaehf.device", "lide.device",
    "ide.device", "oktagon.device", "gvpscsi.device", "omniscsi.device",
    "a4091.device", "a3000scsi.device", "z3scsi.device", 0
};
#define PROBE_UNITS 8

/* Add (driver,unit) if not present; return index or -1 if full. */
static int add_unique(DiscDisk out[], int *count, int max,
                      const char *driver, ULONG unit)
{
    int idx = disc_find(out, *count, driver, unit);
    int n;
    if (idx >= 0) return idx;
    if (*count >= max) return -1;
    idx = (*count)++;
    out[idx].unit    = unit;
    out[idx].size_mb = 0;
    out[idx].status  = DST_UNKNOWN;
    out[idx].label[0]= 0;
    for (n = 0; n < DISC_DRIVER_LEN - 1 && driver[n]; n++)
        out[idx].driver[n] = driver[n];
    out[idx].driver[n] = 0;
    return idx;
}

/* Classify one disk by opening it, querying geometry and (if media) RDB. */
static void probe_one(DiscDisk *d)
{
    DeviceHandle *h = dev_open(d->driver, d->unit);
    DeviceInfo info;
    if (!h) { if (d->status == DST_UNKNOWN) d->status = DST_NOMEDIA; return; }

    if (dev_geometry(h, &info) == 0 && info.has_media && info.total_blocks > 0) {
        RdbModel m;
        d->size_mb = disc_blocks_to_mb(info.total_blocks, info.block_bytes);
        if (d->label[0] == 0) {
            char model[40];
            if (dev_inquiry_model(h, model) == 0 && model[0]) {
                int n; for (n = 0; n < DISC_LABEL_LEN - 1 && model[n]; n++)
                    d->label[n] = model[n];
                d->label[n] = 0;
            }
        }
        if (rdb_parse(&m, dev_block_io, h) == RDB_OK) {
            if (d->status != DST_MOUNTED) d->status = DST_RDB;
        } else {
            if (d->status != DST_MOUNTED) d->status = DST_BLANK;
        }
    } else {
        if (d->status == DST_UNKNOWN) d->status = DST_NOMEDIA;
    }
    dev_close(h);
}

/* (1) Scan mounted AmigaDOS devices for driver/unit pairs. */
static void scan_dos_devices(DiscDisk out[], int *count, int max)
{
    struct DosList *dl = LockDosList(LDF_DEVICES | LDF_READ);
    struct DosList *e;
    if (!dl) return;
    while ((e = NextDosList(dl, LDF_DEVICES | LDF_READ)) != 0) {
        struct FileSysStartupMsg *fssm;
        BPTR startup = (BPTR)e->dol_misc.dol_handler.dol_Startup;
        char driver[DISC_DRIVER_LEN];
        char dosname[DISC_LABEL_LEN];
        int idx;
        if (e->dol_Type != DLT_DEVICE || startup == 0) continue;
        fssm = (struct FileSysStartupMsg *)BADDR(startup);
        if (!fssm || fssm->fssm_Device == 0) continue;

        disc_bcpl_to_c((const unsigned char *)BADDR(fssm->fssm_Device),
                       driver, sizeof(driver));
        idx = add_unique(out, count, max, driver, fssm->fssm_Unit);
        if (idx < 0) continue;
        out[idx].status = DST_MOUNTED;
        if (out[idx].label[0] == 0) {
            disc_bcpl_to_c((const unsigned char *)BADDR(e->dol_Name),
                           dosname, sizeof(dosname));
            { int n; for (n = 0; n < DISC_LABEL_LEN - 1 && dosname[n]; n++)
                  out[idx].label[n] = dosname[n]; out[idx].label[n] = 0; }
        }
    }
    UnLockDosList(LDF_DEVICES | LDF_READ);
}

/* (2) Probe the curated driver/unit matrix for disks not already listed. */
static void scan_probe(DiscDisk out[], int *count, int max)
{
    int di;
    for (di = 0; kProbeDrivers[di]; di++) {
        ULONG u;
        for (u = 0; u < PROBE_UNITS; u++) {
            DeviceHandle *h = dev_open(kProbeDrivers[di], u);
            if (!h) continue;          /* driver/unit not present */
            dev_close(h);
            add_unique(out, count, max, kProbeDrivers[di], u);
        }
    }
}

int discover_disks(DiscDisk out[], int max)
{
    int count = 0;
    int i;
    scan_dos_devices(out, &count, max);
    scan_probe(out, &count, max);
    for (i = 0; i < count; i++)
        probe_one(&out[i]);
    return count;
}

#endif /* HDPART_AMIGA */
```

- [ ] **Step 2: Define `HDPART_AMIGA` for the m68k build**

In `Makefile`, add `-DHDPART_AMIGA` to `CCFLAGS` (so the Amiga build compiles the OS-bound code; the host test compile, which calls `cc` directly without this flag, keeps only the pure helpers). Change the `CCFLAGS = ...` line to end with `-Isrc -DHDPART_AMIGA`:
```make
CCFLAGS = -g -MP -MMD -m68000 -Os -nostdlib -ffreestanding -fomit-frame-pointer \
          -Wall -Wextra -Wno-unused-function \
          -ffunction-sections -fdata-sections -Isrc -DHDPART_AMIGA
```

- [ ] **Step 3: Build for m68k (clean link), and re-run host tests**

Run:
```bash
export AMIGA_BIN="/Users/sfs/.vscode/extensions/bartmanabyss.amiga-debug-1.8.2/bin/darwin"
export PATH="$AMIGA_BIN/opt/bin:$AMIGA_BIN:$PATH"
cd /Users/sfs/Devel/party && make clean && make 2>&1 | grep -iE "compil|link|elf2hunk|entry-order|undefined|error"
./tests/run-host-tests.sh 2>&1 | tail -2
```
Expected: clean m68k link (discover.c + device.c pulled in); host tests still pass (`DISCOVER TESTS PASSED`). The host build does NOT define `HDPART_AMIGA`, so `discover_disks` is absent there — that's fine (the host test does not call it).

- [ ] **Step 4: Commit**

```bash
cd /Users/sfs/Devel/party
git add src/discover.c Makefile
git commit -m "feat(discover): DOS-list scan + curated driver probe + RDB classification"
```

## Milestone 2.4 — `scan` CLI mode (observability)

Outcome: running `HDPart:HDPart scan` from a Shell prints the discovered disks (and the RDB partitions of any disk that has one) to the console — observable in the emulator and diffable against `rdbtool`.

### Task 2.4.1: CLI scan report

**Files:** Modify `src/main.c`

- [ ] **Step 1: Add CLI output helpers and scan mode to `src/main.c`**

At the top of `src/main.c`, add includes:
```c
#include <proto/dos.h>
#include <exec/execbase.h>
#include "discover.h"
```

Add these helpers ABOVE `hdpart_main`:
```c
/* Minimal unsigned-decimal print to a DOS file handle (no libc). */
static void put_str(BPTR fh, const char *s)
{
    long n = 0; while (s[n]) n++;
    Write(fh, (CONST APTR)s, n);
}
static void put_uint(BPTR fh, ULONG v)
{
    char buf[12]; int i = 11;
    buf[i--] = 0;
    if (v == 0) buf[i--] = '0';
    while (v && i >= 0) { buf[i--] = (char)('0' + (v % 10)); v /= 10; }
    put_str(fh, &buf[i + 1]);
}
static const char *status_name(DiskStatus s)
{
    switch (s) {
        case DST_NOMEDIA: return "no-media";
        case DST_BLANK:   return "blank";
        case DST_RDB:     return "RDB";
        case DST_MOUNTED: return "mounted";
        default:          return "unknown";
    }
}

/* Print discovered disks to the CLI. Returns process rc. */
static int run_scan(void)
{
    BPTR out = Output();
    DiscDisk disks[DISC_MAX];
    int count, i;

    put_str(out, "HDPart device scan\n------------------\n");
    count = discover_disks(disks, DISC_MAX);
    if (count == 0) { put_str(out, "(no block devices found)\n"); return 0; }

    for (i = 0; i < count; i++) {
        put_str(out, disks[i].driver);
        put_str(out, " unit ");
        put_uint(out, disks[i].unit);
        put_str(out, "  ");
        put_uint(out, disks[i].size_mb);
        put_str(out, " MB  [");
        put_str(out, status_name(disks[i].status));
        put_str(out, "]");
        if (disks[i].label[0]) { put_str(out, "  "); put_str(out, disks[i].label); }
        put_str(out, "\n");
    }
    return 0;
}
```

- [ ] **Step 2: Branch to scan mode when launched as `HDPart scan`**

Replace the temporary link-smoke block in `hdpart_main` (the `RdbModel probe; DeviceHandle *probe_dev; ...` lines added in Task 2.1.2) with a real CLI dispatch:
```c
    {
        /* CLI arg check: "scan" -> text report, else open the GUI window.
           GetArgStr returns the command line (args after the verb), or "" for WB. */
        const char *args = (const char *)GetArgStr();
        if (args && (args[0] == 's' || args[0] == 'S') &&
            args[1] == 'c' && args[2] == 'a' && args[3] == 'n')
            return run_scan();
    }
    (void)wbmsg;
```
Remove the now-unused `#include "rdb.h"` link-smoke usage if present, but KEEP `#include "rdb.h"`, `#include "device.h"`, and `#include "discover.h"` (run_scan/discovery pull rdb.o and device.o into the link).

- [ ] **Step 3: Build for m68k + stage**

Run:
```bash
export AMIGA_BIN="/Users/sfs/.vscode/extensions/bartmanabyss.amiga-debug-1.8.2/bin/darwin"
export PATH="$AMIGA_BIN/opt/bin:$AMIGA_BIN:$PATH"
cd /Users/sfs/Devel/party && make clean && make 2>&1 | grep -iE "link|elf2hunk|entry-order|undefined|error" && make hd 2>&1 | tail -1
```
Expected: clean build, `entry-order OK`, staged to `amiga_hd/HDPart`.

- [ ] **Step 4: Confirm host tests still pass**

Run: `cd /Users/sfs/Devel/party && ./tests/run-host-tests.sh 2>&1 | tail -2`
Expected: both suites pass.

- [ ] **Step 5: Commit**

```bash
cd /Users/sfs/Devel/party
git add src/main.c
git commit -m "feat: 'HDPart scan' CLI mode prints discovered disks"
```

## Milestone 2.5 — FS-UAE integration test (scratch hardfiles + rdbtool)

Outcome: scratch `.hdf` fixtures (one blank, one with a known RDB) are attached as hardfiles; `HDPart scan` lists them with correct status/size; an RDB written by HDPart is verified on the host with `rdbtool`.

### Task 2.5.1: Scratch HDF fixtures

**Files:** Create `tools/make-scratch-hdfs.sh`

- [ ] **Step 1: Write `tools/make-scratch-hdfs.sh`**

```sh
#!/bin/sh
# Create scratch hardfile fixtures for HDPart device-I/O testing.
#   scratch_blank.hdf : zeroed, no RDB  (HDPart should report "blank")
#   scratch_rdb.hdf   : a small RDB with two partitions written by rdbtool
#                       (HDPart should report "RDB"; partitions must match)
# Geometry: 64 cyl * 4 heads * 32 sec = 8192 blocks = 4 MB (matches the engine's
# rdbtool cross-check fixture so results are easy to compare).
set -e
cd "$(dirname "$0")/.."
mkdir -p amiga_scratch
BLANK=amiga_scratch/scratch_blank.hdf
RDBF=amiga_scratch/scratch_rdb.hdf
BLOCKS=8192

# 4 MB zeroed image (no RDB)
dd if=/dev/zero of="$BLANK" bs=512 count=$BLOCKS >/dev/null 2>&1

# 4 MB image with an RDB + two partitions, created by amitools rdbtool
dd if=/dev/zero of="$RDBF" bs=512 count=$BLOCKS >/dev/null 2>&1
rdbtool "$RDBF" create chs=64,4,32 + init + \
        add name=DH4 start=2 size=50% + \
        add name=DH5 size=50%
echo "=== scratch_rdb.hdf RDB ==="
rdbtool "$RDBF" info
echo "Created amiga_scratch/scratch_blank.hdf and amiga_scratch/scratch_rdb.hdf"
```

- [ ] **Step 2: Run it and confirm the RDB fixture is valid**

Run:
```bash
cd /Users/sfs/Devel/party && chmod +x tools/make-scratch-hdfs.sh && ./tools/make-scratch-hdfs.sh
```
Expected: `rdbtool ... info` lists partitions **DH4** and **DH5** with no checksum errors; both `.hdf` files exist (4 MB each). If the `rdbtool create` argument syntax differs in the installed amitools version, run `rdbtool --help` / `rdbtool <img> create --help` and adjust the `chs=`/`add` arguments to the documented form (the goal is a 64,4,32-geometry RDB with two partitions). Record the exact working command in the script.

- [ ] **Step 3: Gitignore the scratch images**

Run:
```bash
cd /Users/sfs/Devel/party && printf 'amiga_scratch/\n' >> .gitignore
```

- [ ] **Step 4: Commit**

```bash
cd /Users/sfs/Devel/party
git add tools/make-scratch-hdfs.sh .gitignore
git commit -m "test: scratch HDF fixtures (blank + RDB) via rdbtool"
```

### Task 2.5.2: Dev-test FS-UAE config (hardfiles attached)

**Files:** Create `tools/fsuae/HDPart-204-devtest.fs-uae`; Modify `Makefile` (install-fsuae already globs `tools/fsuae/*.fs-uae`)

- [ ] **Step 1: Write `tools/fsuae/HDPart-204-devtest.fs-uae`**

```
# HDPart device-I/O test: Kickstart 2.04 + Workbench 2.04, with two scratch
# hardfiles attached as real block devices (uaehf.device). After WB boots,
# open a Shell and run:  HDPart:HDPart scan
# Expect both scratch disks to appear: one "blank", one "RDB" (DH4/DH5).
# Then verify on the host:  rdbtool amiga_scratch/scratch_rdb.hdf info

[fs-uae]
amiga_model = A500+
chip_memory = 2048
kickstart_file = amiga-os-204.rom
floppy_drive_0 = /Users/sfs/Downloads/WB204/Workbench v2.04 rev 37.67 (1991)(Commodore)(Disk 1 of 4)(Workbench).adf
hard_drive_0 = /Users/sfs/Devel/party/amiga_hd
hard_drive_0_label = HDPart
hard_drive_1 = /Users/sfs/Devel/party/amiga_scratch/scratch_blank.hdf
hard_drive_2 = /Users/sfs/Devel/party/amiga_scratch/scratch_rdb.hdf
save_disk = 0
```

Note: `hard_drive_0` is the directory drive holding the HDPart binary (runs the tool); `hard_drive_1`/`hard_drive_2` are raw hardfiles, which FS-UAE exposes through `uaehf.device` units that HDPart's probe will discover. The exact unit numbers don't matter — discovery reports whatever it finds; what matters is that one shows `blank` and one shows `RDB`.

- [ ] **Step 2: Install configs**

Run: `cd /Users/sfs/Devel/party && make install-fsuae 2>&1 | tail -1`
Expected: configs (including `HDPart-204-devtest`) copied to `~/Documents/FS-UAE/Configurations/`.

- [ ] **Step 3: Commit**

```bash
cd /Users/sfs/Devel/party
git add tools/fsuae/HDPart-204-devtest.fs-uae
git commit -m "test: FS-UAE dev-test config with scratch hardfiles"
```

### Task 2.5.3: Manual integration verification (human-run gate)

**Files:** none (verification)

- [ ] **Step 1: Build + stage + make fixtures**

Run:
```bash
export AMIGA_BIN="/Users/sfs/.vscode/extensions/bartmanabyss.amiga-debug-1.8.2/bin/darwin"
export PATH="$AMIGA_BIN/opt/bin:$AMIGA_BIN:$PATH"
cd /Users/sfs/Devel/party && make hd && ./tools/make-scratch-hdfs.sh && make install-fsuae
```

- [ ] **Step 2: Run the scan in FS-UAE (human)**

In FS-UAE Launcher pick **"HDPart-204-devtest"**, boot WB 2.04, open a Shell, run:
```
HDPart:HDPart scan
```
Expected output includes a line for each discovered disk, e.g.:
```
uaehf.device unit 1   4 MB  [blank]
uaehf.device unit 2   4 MB  [RDB]
```
(plus any mounted system devices). The two scratch disks must appear — one `blank`, one `RDB` — with ~4 MB size.

- [ ] **Step 3: Cross-check the RDB the engine reads vs the host**

On the host:
```bash
cd /Users/sfs/Devel/party && rdbtool amiga_scratch/scratch_rdb.hdf info
```
Confirm the partitions HDPart reported `[RDB]` for correspond to the DH4/DH5 partitions `rdbtool` shows. (Write-path verification — HDPart writing a new RDB to a scratch disk and confirming with `rdbtool` — is part of Plan 3's Save flow; Plan 2 proves the read/discovery path.)

- [ ] **Step 4: Record result**

If the scan lists the scratch disks with correct status/size, Plan 2 is verified. If a scratch disk shows `no-media` instead of `blank`/`RDB`, note which `uaehf.device` units appeared and report — the probe unit range (`PROBE_UNITS`) or driver list may need adjustment for this FS-UAE version.

---

## Definition of done (Plan 2)

- [ ] `src/device.c` opens devices, reads geometry, reads/writes blocks, and exposes a `BlockIO` adapter the RDB engine uses.
- [ ] `src/discover.c` returns a de-duplicated, classified disk list; pure helpers pass host tests (`DISCOVER TESTS PASSED`).
- [ ] `out/HDPart.exe` builds cleanly with `-DHDPART_AMIGA`; `entry-order OK`.
- [ ] `HDPart scan` in FS-UAE lists the scratch hardfiles as `blank` and `RDB`, sizes correct, RDB partitions matching `rdbtool`.

## Self-review notes (author)

- **Spec coverage:** Implements spec §4 `device.c` (block I/O + geometry + INQUIRY) and the discovery half of the hybrid approach (DOS list + curated probe); expansion-bootnode enrichment explicitly deferred and noted. Reuses Plan 1's `rdb_parse` via `dev_block_io` (spec's "device.read RDB → rdb.parse"). Safety (§6): Plan 2 only reads during discovery; no writes to real media occur (writes happen on scratch hardfiles only, and the Save write-path is Plan 3).
- **Type consistency:** `BlockIO` signature `(void*, uint32_t, uint8_t*, int)` matches `rdb.h`; `dev_block_io` conforms. `DiscDisk`/`DeviceInfo`/`DiskStatus` names consistent across header, tests, discover.c, and main.c. `disc_find`/`disc_bcpl_to_c`/`disc_blocks_to_mb` signatures identical in header, tests, and impl.
- **No placeholders:** every code step is complete. The one version-dependent external command (`rdbtool create` syntax, Task 2.5.1 Step 2) includes an explicit fallback instruction to consult `rdbtool` help and record the working form.
- **Host/target split:** `discover.c` pure helpers compile on host (no `HDPART_AMIGA`); OS-bound code compiles only for m68k. `device.c` is m68k-only in practice (always linked into the app, never into host tests).
```
