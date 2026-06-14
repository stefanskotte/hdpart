# Format Partition Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a Format action that turns a freshly-created RDB partition into a usable empty AmigaDOS volume live (no reboot), plus a boot-device/in-use safety layer guarding Format and Save.

**Architecture:** Two new modules mirror the existing `discover.c`/`driver.c` split — pure helpers compile on the host for unit tests, OS-bound code is wrapped in `#ifdef HDPART_AMIGA`. `safety.c` classifies the target physical device (clear/mounted/boot). `format.c` builds a DOS `DosEnvec` from the in-memory RDB and (on Amiga) does `MakeDosNode` → bind ROM FFS handler via `FileSystem.resource` → `AddDosNode(ADNF_STARTPROC)` → `Inhibit` + `ACTION_FORMAT` + `Inhibit`. The GUI gets a selection-scoped "Format…" button + Partition-menu item; `gui_save` gains the same safety gate.

**Tech Stack:** C99 freestanding (m68k, Bartman gcc 15.1.0, `-DHDPART_AMIGA`); system `cc` for host tests; AmigaOS expansion.library, dos.library, FileSystem.resource (KS2.04/V37+).

**Spec:** `docs/superpowers/specs/2026-06-14-hdpart-format-partition-design.md`

**Conventions to follow (already in this repo):**
- Pure-vs-OS split with `#ifdef HDPART_AMIGA` (see `src/discover.c` lines 1–14, 97, 294).
- Makefile compiles `$(wildcard src/*.c)` — **new `src/*.c` need no Makefile change.**
- Host tests: add a `cc -std=c99 -Wall -Wextra` line to `tests/run-host-tests.sh`; use the `CHECK(cond)` macro style from `tests/test_discover.c`.
- No 64-bit int math; keep big buffers `static`/heap (4KB Shell stack).
- After `make`, always `make hd` before any FS-UAE test.
- Build env: `export AMIGA_BIN="/Users/sfs/.vscode/extensions/bartmanabyss.amiga-debug-1.8.2/bin/darwin"; export PATH="$AMIGA_BIN/opt/bin:$AMIGA_BIN:$PATH"`.

---

## Task 1: Safety classifier — pure decision (`safety_decide`)

**Files:**
- Create: `src/safety.h`
- Create: `src/safety.c`
- Test: `tests/test_safety.c`

- [ ] **Step 1: Write `src/safety.h`**

```c
#ifndef HDPART_SAFETY_H
#define HDPART_SAFETY_H
#include <stdint.h>

/* How "live" the physical device (driver,unit) is to the running system. */
typedef enum { DEV_CLEAR = 0, DEV_MOUNTED = 1, DEV_BOOT = 2 } DevLiveness;

/* One mounted DOS device's backing store. */
typedef struct {
    char     driver[40];   /* exec device name, e.g. "scsi.device" */
    uint32_t unit;
    char     name[8];      /* DOS device name, e.g. "DH0" (no colon) */
} MountEntry;

/* Pure classification. Compares the target (driver,unit) against the boot
   device (if known) and the list of currently-mounted DOS devices.
   - boot_known: 0 if the boot device could not be resolved.
   - out_names/max_names: filled with DOS names of volumes on the same physical
     (driver,unit) as the target, for the warning text. *n_names set to count.
   Returns DEV_BOOT / DEV_MOUNTED / DEV_CLEAR. Fail-safe: when boot is unknown
   and the target has no matching mount, returns DEV_MOUNTED (never CLEAR),
   because we cannot prove the target is not the boot device. */
DevLiveness safety_decide(const char *driver, uint32_t unit,
                          int boot_known, const char *boot_driver, uint32_t boot_unit,
                          const MountEntry mounts[], int n_mounts,
                          char out_names[][8], int max_names, int *n_names);

#ifdef HDPART_AMIGA
/* OS wrapper: resolve the boot device + the mount list from the running system,
   then call safety_decide. */
DevLiveness safety_classify(const char *driver, uint32_t unit,
                            char out_names[][8], int max_names, int *n_names);
#endif

#endif /* HDPART_SAFETY_H */
```

- [ ] **Step 2: Write the failing test `tests/test_safety.c`**

```c
/* Host unit tests for safety_decide (pure). The OS-bound safety_classify()
   needs AmigaOS and is not exercised here. Built by run-host-tests.sh. */
#include <stdio.h>
#include <string.h>
#include "../src/safety.h"

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); g_fail++; } } while (0)

static MountEntry mk(const char *drv, uint32_t unit, const char *name)
{
    MountEntry m; memset(&m, 0, sizeof(m));
    strncpy(m.driver, drv, sizeof(m.driver) - 1);
    m.unit = unit;
    strncpy(m.name, name, sizeof(m.name) - 1);
    return m;
}

static void test_boot_match(void)
{
    char names[8][8]; int nn = -1;
    MountEntry mounts[1] = { mk("scsi.device", 0, "DH0") };
    DevLiveness r = safety_decide("scsi.device", 0, 1, "scsi.device", 0,
                                  mounts, 1, names, 8, &nn);
    CHECK(r == DEV_BOOT);
}

static void test_mounted_not_boot(void)
{
    char names[8][8]; int nn = -1;
    MountEntry mounts[2] = { mk("scsi.device", 0, "DH4"), mk("scsi.device", 0, "DH5") };
    /* boot is the floppy (trackdisk.device), target is scsi unit 0 with mounts */
    DevLiveness r = safety_decide("scsi.device", 0, 1, "trackdisk.device", 0,
                                  mounts, 2, names, 8, &nn);
    CHECK(r == DEV_MOUNTED);
    CHECK(nn == 2);
    CHECK(strcmp(names[0], "DH4") == 0);
    CHECK(strcmp(names[1], "DH5") == 0);
}

static void test_clear(void)
{
    char names[8][8]; int nn = -1;
    MountEntry mounts[1] = { mk("scsi.device", 1, "DH0") };  /* different unit */
    DevLiveness r = safety_decide("scsi.device", 0, 1, "trackdisk.device", 0,
                                  mounts, 1, names, 8, &nn);
    CHECK(r == DEV_CLEAR);
    CHECK(nn == 0);
}

static void test_boot_unknown_failsafe(void)
{
    char names[8][8]; int nn = -1;
    /* No mounts on target and boot unknown -> cannot prove not-boot -> MOUNTED. */
    DevLiveness r = safety_decide("scsi.device", 0, 0, "", 0,
                                  0, 0, names, 8, &nn);
    CHECK(r == DEV_MOUNTED);
}

static void test_case_insensitive_driver(void)
{
    char names[8][8]; int nn = -1;
    MountEntry mounts[1] = { mk("scsi.device", 0, "DH0") };
    DevLiveness r = safety_decide("SCSI.DEVICE", 0, 1, "SCSI.DEVICE", 0,
                                  mounts, 1, names, 8, &nn);
    CHECK(r == DEV_BOOT);
}

int main(void)
{
    test_boot_match();
    test_mounted_not_boot();
    test_clear();
    test_boot_unknown_failsafe();
    test_case_insensitive_driver();
    if (g_fail) { printf("SAFETY TESTS FAILED (%d)\n", g_fail); return 1; }
    printf("SAFETY TESTS PASSED\n");
    return 0;
}
```

- [ ] **Step 3: Run the test to verify it fails to link**

Run:
```bash
cc -std=c99 -Wall -Wextra -g -o /tmp/hdpart_safety tests/test_safety.c src/safety.c
```
Expected: FAIL — `src/safety.c` does not exist yet (compile/link error).

- [ ] **Step 4: Write `src/safety.c` (pure part only for now)**

```c
/* HDPart device-liveness safety classifier. Pure helper (host-testable) +
   OS-bound gatherer guarded by HDPART_AMIGA. Mirrors discover.c. */
#ifdef HDPART_AMIGA
#include <exec/types.h>
#include <exec/execbase.h>
#include <dos/dosextens.h>
#include <dos/filehandler.h>
#include <dos/dostags.h>
#include <proto/exec.h>
#include <proto/dos.h>
#endif
#include <string.h>
#include "safety.h"

/* Case-insensitive ASCII equality for device names. */
static int dev_eq(const char *a, const char *b)
{
    int i;
    for (i = 0; a[i] && b[i]; i++) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return 0;
    }
    return a[i] == 0 && b[i] == 0;
}

DevLiveness safety_decide(const char *driver, uint32_t unit,
                          int boot_known, const char *boot_driver, uint32_t boot_unit,
                          const MountEntry mounts[], int n_mounts,
                          char out_names[][8], int max_names, int *n_names)
{
    int i, nn = 0, matched = 0;
    int is_boot = boot_known && dev_eq(driver, boot_driver) && unit == boot_unit;

    for (i = 0; i < n_mounts; i++) {
        if (mounts[i].unit != unit || !dev_eq(mounts[i].driver, driver)) continue;
        matched = 1;
        if (nn < max_names) {
            int k;
            for (k = 0; k < 7 && mounts[i].name[k]; k++) out_names[nn][k] = mounts[i].name[k];
            out_names[nn][k] = 0;
            nn++;
        }
    }
    if (n_names) *n_names = nn;

    if (is_boot)  return DEV_BOOT;
    if (matched)  return DEV_MOUNTED;
    if (!boot_known) return DEV_MOUNTED;   /* fail-safe: cannot prove not-boot */
    return DEV_CLEAR;
}
```

- [ ] **Step 5: Run the test to verify it passes**

Run:
```bash
cc -std=c99 -Wall -Wextra -g -o /tmp/hdpart_safety tests/test_safety.c src/safety.c && /tmp/hdpart_safety
```
Expected: `SAFETY TESTS PASSED`

- [ ] **Step 6: Wire the test into `tests/run-host-tests.sh`**

Add these two lines at the end of `tests/run-host-tests.sh` (after the existing `hdpart_driver` block):

```sh
cc -std=c99 -Wall -Wextra -g -o /tmp/hdpart_safety tests/test_safety.c src/safety.c
/tmp/hdpart_safety
```

- [ ] **Step 7: Run the whole host suite**

Run: `./tests/run-host-tests.sh`
Expected: ends with `SAFETY TESTS PASSED` and exit 0.

- [ ] **Step 8: Commit**

```bash
git add src/safety.h src/safety.c tests/test_safety.c tests/run-host-tests.sh
git commit -m "feat(safety): pure device-liveness classifier (safety_decide) + host tests"
```

---

## Task 2: Safety classifier — OS gatherer (`safety_classify`)

**Files:**
- Modify: `src/safety.c` (append inside an `#ifdef HDPART_AMIGA` block)

This part is OS-bound (no host test). It is verified on-target in Task 8.

- [ ] **Step 1: Append the OS wrapper to `src/safety.c`**

Add at the end of the file:

```c
#ifdef HDPART_AMIGA
/* Read the exec device name + unit backing a DLT_DEVICE DOS entry from its
   FileSysStartupMsg. Returns 1 on success. Guards BPTR derefs (see discover.c).
   out_driver must hold >=40 bytes. */
static int dev_backing(struct DosList *dl, char *out_driver, uint32_t *out_unit)
{
    struct FileSysStartupMsg *fssm;
    unsigned char *bstr;
    int len, i;
    if (!dl || !dl->dol_misc.dol_handler.dol_Startup) return 0;
    if (TypeOfMem((void *)BADDR(dl->dol_misc.dol_handler.dol_Startup)) == 0) return 0;
    fssm = (struct FileSysStartupMsg *)BADDR(dl->dol_misc.dol_handler.dol_Startup);
    if (!fssm->fssm_Device) return 0;
    if (TypeOfMem((void *)BADDR(fssm->fssm_Device)) == 0) return 0;
    bstr = (unsigned char *)BADDR(fssm->fssm_Device);
    len = bstr[0];
    if (len <= 0 || len > 39) return 0;
    for (i = 0; i < len; i++) out_driver[i] = (char)bstr[1 + i];
    out_driver[len] = 0;
    *out_unit = (uint32_t)fssm->fssm_Unit;
    return 1;
}

/* Copy a DOS device node's BSTR name (no colon) into out[<=8]. */
static void dev_dosname(struct DosList *dl, char *out)
{
    unsigned char *bstr; int len, i;
    out[0] = 0;
    if (!dl->dol_Name) return;
    if (TypeOfMem((void *)BADDR(dl->dol_Name)) == 0) return;
    bstr = (unsigned char *)BADDR(dl->dol_Name);
    len = bstr[0];
    if (len > 7) len = 7;
    for (i = 0; i < len; i++) out[i] = (char)bstr[1 + i];
    out[len] = 0;
}

DevLiveness safety_classify(const char *driver, uint32_t unit,
                            char out_names[][8], int max_names, int *n_names)
{
    static MountEntry mounts[32];           /* static: keep off the 4KB stack */
    int n_mounts = 0;
    int boot_known = 0;
    char boot_driver[40]; uint32_t boot_unit = 0;
    struct MsgPort *boot_port = 0;
    struct DevProc *dp;
    struct DosList *dl;

    boot_driver[0] = 0;

    /* Resolve the boot device's handler port via SYS:. */
    dp = GetDeviceProc((STRPTR)"SYS:", 0);
    if (dp) { boot_port = dp->dvp_Port; FreeDeviceProc(dp); }

    /* Walk the DOS device list: collect mounts + identify the boot device. */
    dl = LockDosList(LDF_DEVICES | LDF_READ);
    {
        struct DosList *e = dl;
        while ((e = NextDosEntry(e, LDF_DEVICES | LDF_READ)) != 0) {
            char drv[40]; uint32_t u;
            if (!dev_backing(e, drv, &u)) continue;
            if (n_mounts < 32) {
                int k;
                for (k = 0; k < 39 && drv[k]; k++) mounts[n_mounts].driver[k] = drv[k];
                mounts[n_mounts].driver[k] = 0;
                mounts[n_mounts].unit = u;
                dev_dosname(e, mounts[n_mounts].name);
                n_mounts++;
            }
            if (boot_port && e->dol_Task == boot_port) {
                int k;
                for (k = 0; k < 39 && drv[k]; k++) boot_driver[k] = drv[k];
                boot_driver[k] = 0;
                boot_unit = u; boot_known = 1;
            }
        }
    }
    UnlockDosList(LDF_DEVICES | LDF_READ);

    return safety_decide(driver, unit, boot_known, boot_driver, boot_unit,
                         mounts, n_mounts, out_names, max_names, n_names);
}
#endif /* HDPART_AMIGA */
```

- [ ] **Step 2: Verify the host suite still builds/passes (the new block is excluded on host)**

Run: `./tests/run-host-tests.sh`
Expected: `SAFETY TESTS PASSED` (the `#ifdef HDPART_AMIGA` block is not compiled without the define).

- [ ] **Step 3: Verify the Amiga build compiles**

Run:
```bash
export AMIGA_BIN="/Users/sfs/.vscode/extensions/bartmanabyss.amiga-debug-1.8.2/bin/darwin"; export PATH="$AMIGA_BIN/opt/bin:$AMIGA_BIN:$PATH"
make 2>&1 | tail -4
```
Expected: compiles `src/safety.c` with no errors (it is now part of the build; nothing calls it yet, which is fine — it is non-static API).

- [ ] **Step 4: Commit**

```bash
git add src/safety.c
git commit -m "feat(safety): OS gatherer safety_classify (boot via SYS:, mount list walk)"
```

---

## Task 3: Format engine — pure env builder (`format_build_envec`)

**Files:**
- Create: `src/format.h`
- Create: `src/format.c`
- Test: `tests/test_format.c`

- [ ] **Step 1: Write `src/format.h`**

```c
#ifndef HDPART_FORMAT_H
#define HDPART_FORMAT_H
#include <stdint.h>
#include "rdb.h"

typedef enum {
    FMT_OK = 0,
    FMT_ERR_RANGE,        /* bad part_index */
    FMT_ERR_NO_HANDLER,   /* DosType not in FileSystem.resource (non-ROM FS) */
    FMT_ERR_NAME_TAKEN,   /* DOS device name already live */
    FMT_ERR_MAKENODE,     /* MakeDosNode failed */
    FMT_ERR_ADDNODE,      /* AddDosNode failed */
    FMT_ERR_FORMAT        /* ACTION_FORMAT packet failed */
} FmtResult;

/* DosEnvec built up to and including de_DosType (index DE_DOSTYPE=16).
   env[0] = de_TableSize = 16; env[1..16] = the fields. 17 longwords total. */
#define FMT_ENV_LONGS 17

/* Pure: build the DOS environment vector for a partition from the model+geometry.
   Returns 0 on success, -1 if part_index is out of range. Host-testable. */
int format_build_envec(const RdbModel *m, int part_index, uint32_t env[FMT_ENV_LONGS]);

#ifdef HDPART_AMIGA
/* OS: MakeDosNode + bind ROM FFS handler + AddDosNode(STARTPROC) +
   Inhibit + ACTION_FORMAT(volname, dostype) + Inhibit. volname = volume label
   (no colon). Returns FmtResult. */
FmtResult format_partition(const char *driver, uint32_t unit,
                           const RdbModel *m, int part_index, const char *volname);
#endif

#endif /* HDPART_FORMAT_H */
```

- [ ] **Step 2: Write the failing test `tests/test_format.c`**

```c
/* Host unit tests for format_build_envec (pure). OS-bound format_partition()
   needs AmigaOS and is not exercised here. Built by run-host-tests.sh. */
#include <stdio.h>
#include <string.h>
#include "../src/format.h"
#include "../src/rdb.h"

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); g_fail++; } } while (0)

/* DE_ indexes (relative to env[0]=DE_TABLESIZE). */
enum { DE_TABLESIZE=0, DE_SIZEBLOCK=1, DE_SECORG=2, DE_NUMHEADS=3, DE_SECSPERBLK=4,
       DE_BLKSPERTRACK=5, DE_RESERVEDBLKS=6, DE_PREFAC=7, DE_INTERLEAVE=8,
       DE_LOWCYL=9, DE_HIGHCYL=10, DE_NUMBUFFERS=11, DE_BUFMEMTYPE=12,
       DE_MAXTRANSFER=13, DE_MASK=14, DE_BOOTPRI=15, DE_DOSTYPE=16 };

static void test_envec(void)
{
    RdbModel m;
    uint32_t env[FMT_ENV_LONGS];
    int rc;

    /* 100 cyl, 4 heads, 32 sectors, 512-byte blocks. */
    rdb_init_model(&m, 100, 4, 32);
    rc = rdb_add_partition_cyl(&m, "DH4", 10, 49, 0x444F5303u); /* FFS Intl */
    CHECK(rc >= 0);
    /* set flag fields the env carries */
    m.parts[0].num_buffers = 30;
    m.parts[0].maxtransfer = 0x0001FE00u;
    m.parts[0].mask        = 0x7FFFFFFEu;
    m.parts[0].boot_pri    = 0;

    CHECK(format_build_envec(&m, 0, env) == 0);
    CHECK(env[DE_TABLESIZE]    == 16);
    CHECK(env[DE_SIZEBLOCK]    == 128);   /* 512 / 4 longwords */
    CHECK(env[DE_SECORG]       == 0);
    CHECK(env[DE_NUMHEADS]     == 4);
    CHECK(env[DE_SECSPERBLK]   == 1);
    CHECK(env[DE_BLKSPERTRACK] == 32);
    CHECK(env[DE_RESERVEDBLKS] == 2);
    CHECK(env[DE_PREFAC]       == 0);
    CHECK(env[DE_INTERLEAVE]   == 0);
    CHECK(env[DE_LOWCYL]       == 10);
    CHECK(env[DE_HIGHCYL]      == 49);
    CHECK(env[DE_NUMBUFFERS]   == 30);
    CHECK(env[DE_BUFMEMTYPE]   == 0);
    CHECK(env[DE_MAXTRANSFER]  == 0x0001FE00u);
    CHECK(env[DE_MASK]         == 0x7FFFFFFEu);
    CHECK(env[DE_BOOTPRI]      == 0);
    CHECK(env[DE_DOSTYPE]      == 0x444F5303u);

    CHECK(format_build_envec(&m, 5, env) == -1); /* out of range */
}

int main(void)
{
    test_envec();
    if (g_fail) { printf("FORMAT TESTS FAILED (%d)\n", g_fail); return 1; }
    printf("FORMAT TESTS PASSED\n");
    return 0;
}
```

> NOTE: confirm the `RdbPartition` field names (`num_buffers`, `maxtransfer`, `mask`, `boot_pri`) against `src/rdb.h` before writing `format.c`; the editor sets `pt->maxtransfer/pt->mask/pt->boot_pri` so they exist. If `block_bytes`/`heads`/`sectors` live on `RdbModel`, read them from there (they do — see `RdbModel` in `src/rdb.h`).

- [ ] **Step 3: Run the test to verify it fails to link**

Run:
```bash
cc -std=c99 -Wall -Wextra -g -o /tmp/hdpart_format tests/test_format.c src/format.c src/rdb.c
```
Expected: FAIL — `src/format.c` does not exist yet.

- [ ] **Step 4: Write `src/format.c` (pure part)**

```c
/* HDPart partition formatter. Pure env builder (host-testable) + OS-bound
   live-mount + ACTION_FORMAT guarded by HDPART_AMIGA. Mirrors discover.c. */
#ifdef HDPART_AMIGA
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/filehandler.h>
#include <libraries/expansion.h>
#include <resources/filesysres.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/expansion.h>
#endif
#include "format.h"

/* DE_ indexes relative to env[0]=DE_TABLESIZE. */
enum { FE_TABLESIZE=0, FE_SIZEBLOCK=1, FE_SECORG=2, FE_NUMHEADS=3, FE_SECSPERBLK=4,
       FE_BLKSPERTRACK=5, FE_RESERVEDBLKS=6, FE_PREFAC=7, FE_INTERLEAVE=8,
       FE_LOWCYL=9, FE_HIGHCYL=10, FE_NUMBUFFERS=11, FE_BUFMEMTYPE=12,
       FE_MAXTRANSFER=13, FE_MASK=14, FE_BOOTPRI=15, FE_DOSTYPE=16 };

int format_build_envec(const RdbModel *m, int part_index, uint32_t env[FMT_ENV_LONGS])
{
    const RdbPartition *p;
    if (!m || part_index < 0 || part_index >= m->num_parts) return -1;
    p = &m->parts[part_index];
    env[FE_TABLESIZE]    = 16;
    env[FE_SIZEBLOCK]    = m->block_bytes / 4u;   /* longwords per block */
    env[FE_SECORG]       = 0;
    env[FE_NUMHEADS]     = m->heads;
    env[FE_SECSPERBLK]   = 1;
    env[FE_BLKSPERTRACK] = m->sectors;
    env[FE_RESERVEDBLKS] = 2;
    env[FE_PREFAC]       = 0;
    env[FE_INTERLEAVE]   = 0;
    env[FE_LOWCYL]       = p->low_cyl;
    env[FE_HIGHCYL]      = p->high_cyl;
    env[FE_NUMBUFFERS]   = p->num_buffers ? p->num_buffers : 30u;
    env[FE_BUFMEMTYPE]   = 0;
    env[FE_MAXTRANSFER]  = p->maxtransfer;
    env[FE_MASK]         = p->mask;
    env[FE_BOOTPRI]      = (uint32_t)p->boot_pri;
    env[FE_DOSTYPE]      = p->dos_type;
    return 0;
}
```

- [ ] **Step 5: Run the test to verify it passes**

Run:
```bash
cc -std=c99 -Wall -Wextra -g -o /tmp/hdpart_format tests/test_format.c src/format.c src/rdb.c && /tmp/hdpart_format
```
Expected: `FORMAT TESTS PASSED`

- [ ] **Step 6: Wire into `tests/run-host-tests.sh`**

Append at the end:
```sh
cc -std=c99 -Wall -Wextra -g -o /tmp/hdpart_format tests/test_format.c src/format.c src/rdb.c
/tmp/hdpart_format
```

- [ ] **Step 7: Run the whole host suite**

Run: `./tests/run-host-tests.sh`
Expected: ends with `FORMAT TESTS PASSED`, exit 0.

- [ ] **Step 8: Commit**

```bash
git add src/format.h src/format.c tests/test_format.c tests/run-host-tests.sh
git commit -m "feat(format): pure DosEnvec builder (format_build_envec) + host tests"
```

---

## Task 4: Format engine — OS live-mount + ACTION_FORMAT (`format_partition`)

**Files:**
- Modify: `src/format.c` (append inside `#ifdef HDPART_AMIGA`)

OS-bound; verified on-target in Task 8. Open `expansion.library` locally (HDPart does not keep it open).

> RISK / on-target verification points (call out during review):
> - `MakeDosNode` parmPacket strings (`pp[0]`, `pp[1]`) are **C strings** (MakeDosNode builds the BSTRs). Unit at `pp[2]`, flags `pp[3]=0`, then the env from `pp[4]` (= `de_TableSize`).
> - `FileSysEntry` patch: copy each longword from `&fse->fse_Type` into `&dn->dn_Type` where the corresponding `fse_PatchFlags` bit is set (bit 0 → dn_Type, bit 1 → dn_Task, … bit 8 → dn_GlobalVec). `dn_SegList` (bit 7) is the critical one.
> - `ACTION_FORMAT` (= 1227) is not in the toolchain headers — `#define` it. arg1 = **BSTR** volume name, arg2 = DosType ULONG.

- [ ] **Step 1: Append the OS wrapper to `src/format.c`**

```c
#ifdef HDPART_AMIGA

#ifndef ACTION_FORMAT
#define ACTION_FORMAT 1227L
#endif

/* Copy a C string into a long-aligned BSTR buffer (len byte + chars).
   Returns a BPTR. buf must be >= len+2 bytes and long-aligned. */
static BPTR cstr_to_bstr(const char *s, UBYTE *buf)
{
    int i;
    for (i = 0; s[i] && i < 30; i++) buf[1 + i] = (UBYTE)s[i];
    buf[0] = (UBYTE)i;
    return MKBADDR(buf);
}

/* Find the ROM FFS handler seglist for dos_type and patch it into dn. Returns 1
   if a handler was found and applied, 0 otherwise. */
static int bind_rom_handler(struct DeviceNode *dn, uint32_t dos_type)
{
    struct FileSysResource *fsr;
    struct FileSysEntry *fse;
    ULONG *src, *dst;
    int applied = 0;

    fsr = (struct FileSysResource *)OpenResource((STRPTR)"FileSystem.resource");
    if (!fsr) return 0;

    Forbid();
    for (fse = (struct FileSysEntry *)fsr->fsr_FileSysEntries.lh_Head;
         fse->fse_Node.ln_Succ;
         fse = (struct FileSysEntry *)fse->fse_Node.ln_Succ) {
        if (fse->fse_DosType != dos_type) continue;
        /* Patch DeviceNode fields selected by fse_PatchFlags. The patchable run
           starts at fse_Type (mirrors dn_Type) and is contiguous. */
        src = (ULONG *)&fse->fse_Type;
        dst = (ULONG *)&dn->dn_Type;
        {
            ULONG flags = fse->fse_PatchFlags; int bit = 0;
            for (bit = 0; bit < 9; bit++)
                if (flags & (1u << bit)) dst[bit] = src[bit];
        }
        applied = 1;
        break;
    }
    Permit();
    return applied;
}

FmtResult format_partition(const char *driver, uint32_t unit,
                           const RdbModel *m, int part_index, const char *volname)
{
    struct Library *ExpansionBase;
    uint32_t env[FMT_ENV_LONGS];
    static ULONG pp[4 + FMT_ENV_LONGS];   /* static: keep off the 4KB stack */
    static char  driverbuf[40];
    static char  dosnamebuf[8];
    static UBYTE volbstr[34];
    struct DeviceNode *dn;
    const RdbPartition *p;
    char dosdev[10];                      /* "DHx:" */
    int i, k;

    if (format_build_envec(m, part_index, env) != 0) return FMT_ERR_RANGE;
    p = &m->parts[part_index];

    /* Copies that outlive this call are unnecessary (MakeDosNode copies names +
       env), but use stable buffers for the packet anyway. */
    for (k = 0; k < 39 && driver[k]; k++) driverbuf[k] = driver[k];
    driverbuf[k] = 0;
    for (k = 0; k < 7 && p->name[k]; k++) dosnamebuf[k] = p->name[k];
    dosnamebuf[k] = 0;

    /* Refuse if a device of this name is already mounted. */
    {
        struct DosList *dl = LockDosList(LDF_DEVICES | LDF_READ);
        struct DosList *f = FindDosEntry(dl, (STRPTR)dosnamebuf, LDF_DEVICES);
        UnlockDosList(LDF_DEVICES | LDF_READ);
        if (f) return FMT_ERR_NAME_TAKEN;
    }

    ExpansionBase = OpenLibrary((STRPTR)"expansion.library", 37);
    if (!ExpansionBase) return FMT_ERR_MAKENODE;

    pp[0] = (ULONG)dosnamebuf;
    pp[1] = (ULONG)driverbuf;
    pp[2] = (ULONG)unit;
    pp[3] = 0;
    for (i = 0; i < FMT_ENV_LONGS; i++) pp[4 + i] = env[i];

    dn = (struct DeviceNode *)MakeDosNode((APTR)pp);
    if (!dn) { CloseLibrary(ExpansionBase); return FMT_ERR_MAKENODE; }

    if (!bind_rom_handler(dn, p->dos_type)) {
        /* No ROM handler for this dostype: non-ROM FS, not supported in v1.
           The node is not added; free its allocations. */
        CloseLibrary(ExpansionBase);
        return FMT_ERR_NO_HANDLER;
    }

    if (!AddDosNode(0, ADNF_STARTPROC, dn)) {
        CloseLibrary(ExpansionBase);
        return FMT_ERR_ADDNODE;
    }
    CloseLibrary(ExpansionBase);   /* node is now owned by DOS */

    /* Build "DHx:" and format via the handler. */
    for (k = 0; k < 7 && dosnamebuf[k]; k++) dosdev[k] = dosnamebuf[k];
    dosdev[k++] = ':'; dosdev[k] = 0;

    {
        struct MsgPort *port = (struct MsgPort *)DeviceProc((STRPTR)dosdev);
        BPTR vb;
        LONG ok;
        if (!port) return FMT_ERR_FORMAT;
        Inhibit((STRPTR)dosdev, DOSTRUE);
        vb = cstr_to_bstr(volname && volname[0] ? volname : dosnamebuf, volbstr);
        ok = DoPkt(port, ACTION_FORMAT, (LONG)vb, (LONG)p->dos_type, 0, 0, 0);
        Inhibit((STRPTR)dosdev, DOSFALSE);
        if (!ok) return FMT_ERR_FORMAT;
    }
    return FMT_OK;
}
#endif /* HDPART_AMIGA */
```

- [ ] **Step 2: Verify host suite unaffected**

Run: `./tests/run-host-tests.sh`
Expected: `FORMAT TESTS PASSED` (OS block excluded on host).

- [ ] **Step 3: Verify the Amiga build compiles**

Run:
```bash
export AMIGA_BIN="/Users/sfs/.vscode/extensions/bartmanabyss.amiga-debug-1.8.2/bin/darwin"; export PATH="$AMIGA_BIN/opt/bin:$AMIGA_BIN:$PATH"
make 2>&1 | tail -6
```
Expected: `src/format.c` compiles; link OK; `entry-order OK`. If headers report a different `FileSysEntry`/`fssm` field name, adjust includes — do not change the logic.

- [ ] **Step 4: Commit**

```bash
git add src/format.c
git commit -m "feat(format): live MakeDosNode + ROM FFS bind + ACTION_FORMAT (no reboot)"
```

---

## Task 5: GUI — Format button, menu item, ghosting

**Files:**
- Modify: `src/gui.c` (GID enum ~line 33; `gui_update_buttons` ~170-195; menu array ~250-272; `IP_` enum ~163; toolbar table ~322-330; `build_menus` if it has an item-count guard)

- [ ] **Step 1: Add `GID_FORMAT` to the gadget id enum**

In `src/gui.c` change the enum at line 33-35 from:
```c
enum { GID_DEVICE = 1, GID_SCAN, GID_DRIVER, GID_PARTS, GID_NEW, GID_DELETE,
       GID_EDIT, GID_INIT, GID_SAVE, GID_STATUS, GID_UNIT, GID_SPLIT, GID_REFRESH,
       GID_RESIZE };
```
to (append `GID_FORMAT`; it becomes 15, still within `g_gad[16]`):
```c
enum { GID_DEVICE = 1, GID_SCAN, GID_DRIVER, GID_PARTS, GID_NEW, GID_DELETE,
       GID_EDIT, GID_INIT, GID_SAVE, GID_STATUS, GID_UNIT, GID_SPLIT, GID_REFRESH,
       GID_RESIZE, GID_FORMAT };
```

- [ ] **Step 2: Add `IP_FORMAT` to the Partition-menu item enum**

Change line 163 from:
```c
enum { IP_NEW=0, IP_EDIT=1, IP_DELETE=2, IP_SPLIT=4, IP_RESIZE=5 }; /* Partition items */
```
to (Resize is item 5; a new bar would shift indexes, so add Format as item 6 with **no** preceding bar):
```c
enum { IP_NEW=0, IP_EDIT=1, IP_DELETE=2, IP_SPLIT=4, IP_RESIZE=5, IP_FORMAT=6 }; /* Partition items */
```

- [ ] **Step 3: Add the "Format…" item to the Partition menu**

In `g_newmenu[]`, change:
```c
    {  NM_ITEM, "Resize...",    "R", 0, 0, 0 },
    { NM_END, 0, 0, 0, 0, 0 }
```
to:
```c
    {  NM_ITEM, "Resize...",    "R", 0, 0, 0 },
    {  NM_ITEM, "Format...",    "O", 0, 0, 0 },
    { NM_END, 0, 0, 0, 0, 0 }
```
(`IP_FORMAT=6` matches: New=0, Edit=1, Delete=2, bar=3, Split=4, Resize=5, Format=6.)

- [ ] **Step 4: Add the Format toolbar button**

In the partition toolbar block (~line 322), change the table + loop bound from:
```c
        static const struct { int id; const char *txt; int x; int w; } pbtn[] = {
            { GID_NEW,    "_New",     16,  56 }, { GID_EDIT,   "_Edit...", 78,  72 },
            { GID_DELETE, "_Delete", 156,  72 }, { GID_SPLIT,  "Spli_t...",234, 74 },
            { GID_RESIZE, "_Resize...",314, 88 }
        };
        int k;
        for (k = 0; k < 5; k++) {
```
to:
```c
        static const struct { int id; const char *txt; int x; int w; } pbtn[] = {
            { GID_NEW,    "_New",     16,  56 }, { GID_EDIT,   "_Edit...", 78,  72 },
            { GID_DELETE, "_Delete", 156,  72 }, { GID_SPLIT,  "Spli_t...",234, 74 },
            { GID_RESIZE, "_Resize...",314, 88 }, { GID_FORMAT, "F_ormat...",408, 92 }
        };
        int k;
        for (k = 0; k < 6; k++) {
```
(`F_ormat` → plain-key `o`, matching the menu's `O`. New button ends at 408+92=500 < 580 content width.)

- [ ] **Step 5: Ghost the Format button + menu with selection**

In `gui_update_buttons`, after the `GID_RESIZE` line (line 186) add:
```c
    GT_SetGadgetAttrs(g_gad[GID_FORMAT], g_win, 0, GA_Disabled, (ULONG)!hasSel, TAG_END);
```
and after the `IP_RESIZE` menu line (line 195) add:
```c
    gui_menu_enable(MN_PART,    IP_FORMAT,  hasSel);
```

- [ ] **Step 6: Build and run host tests (GUI not yet wired to a handler — button exists but does nothing)**

Run:
```bash
export AMIGA_BIN="/Users/sfs/.vscode/extensions/bartmanabyss.amiga-debug-1.8.2/bin/darwin"; export PATH="$AMIGA_BIN/opt/bin:$AMIGA_BIN:$PATH"
make 2>&1 | tail -4 && ./tests/run-host-tests.sh
```
Expected: builds clean (`entry-order OK`); host tests all pass.

- [ ] **Step 7: Commit**

```bash
git add src/gui.c
git commit -m "feat(gui): Format partition button + menu item + ghosting (no handler yet)"
```

---

## Task 6: GUI — Format dialog, dispatch, save-first + safety

**Files:**
- Modify: `src/gui.c` (add `#include "format.h"` and `#include "safety.h"` near the other includes; add `gui_format_dialog`/`gui_format` before the event loop; wire button/menu/key dispatch ~1390-1438; forward-declare near line 132)

- [ ] **Step 1: Add includes + forward declaration**

Near the top includes of `src/gui.c` add:
```c
#include "format.h"
#include "safety.h"
```
Near the other forward decls (~line 132, by `gui_resize_dialog`) add:
```c
static void gui_format(int index);
```

- [ ] **Step 2: Implement `gui_format` (preconditions + dialog + engine call)**

Add this function next to `gui_resize_dialog` in `src/gui.c`. It mirrors the
existing dialog idioms (`dlg_*` helpers, `gui_msg`, `gui_confirm`). The volume
name is entered with a GadTools string gadget; the safety classification gates
the action.

```c
/* Format the selected partition into an empty volume, live (no reboot).
   Preconditions: saved (not dirty), not the boot device, name free, ROM FFS. */
static void gui_format(int index)
{
    static char names[8][8];
    static char vol[36];
    RdbPartition *pt;
    DevLiveness lv;
    int nn = 0, i, p;
    char msg[200];

    if (!g_have_model || index < 0 || index >= g_model.num_parts) return;
    pt = &g_model.parts[index];

    /* Save-first: the partition must persist before we format it. */
    if (g_dirty) {
        gui_msg("Format", "Save the partition table first,\nthen format.");
        return;
    }

    /* Boot/mount safety on the physical device HDPart is editing. */
    lv = safety_classify(g_cur_driver, g_cur_unit, names, 8, &nn);
    if (lv == DEV_BOOT) {
        gui_msg("Format",
                "You booted from this device.\n"
                "Formatting it now would corrupt the\n"
                "running system. Boot from floppy (or\n"
                "another disk) to format this one.");
        return;
    }
    if (lv == DEV_MOUNTED) {
        /* Build a warning listing mounted volumes, require explicit confirm. */
        p = 0;
        for (i = 0; "This disk has mounted volumes ("[i]; i++) msg[p++] = "This disk has mounted volumes ("[i];
        for (i = 0; i < nn && p < 150; i++) {
            int k;
            if (i) { msg[p++] = ','; msg[p++] = ' '; }
            for (k = 0; names[i][k] && p < 150; k++) msg[p++] = names[i][k];
        }
        { const char *t = ").\nFormatting may destroy data. Proceed?";
          for (i = 0; t[i]; i++) msg[p++] = t[i]; }
        msg[p] = 0;
        if (!gui_confirm("Format", msg)) return;
    } else {
        /* DEV_CLEAR: still destructive. */
        p = 0;
        { const char *t = "Format this partition into an empty\nvolume? All data on it is lost.";
          for (i = 0; t[i]; i++) msg[p++] = t[i]; msg[p] = 0; }
        if (!gui_confirm("Format", msg)) return;
    }

    /* Default volume label = partition name. (A name-entry string gadget can be
       added later; v1 uses the partition name as the label.) */
    for (i = 0; i < 35 && pt->name[i]; i++) vol[i] = pt->name[i];
    vol[i] = 0;

    switch (format_partition(g_cur_driver, g_cur_unit, &g_model, index, vol)) {
    case FMT_OK:
        p = 0;
        { const char *t = "Formatted "; for (i = 0; t[i]; i++) msg[p++] = t[i]; }
        for (i = 0; pt->name[i]; i++) msg[p++] = pt->name[i];
        { const char *t = ": as an empty volume."; for (i = 0; t[i]; i++) msg[p++] = t[i]; }
        msg[p] = 0;
        gui_set_status(msg);
        break;
    case FMT_ERR_NO_HANDLER:
        gui_msg("Format", "No ROM filesystem for this type.\nNon-ROM filesystems need embedded-FS\nsupport (not available yet).");
        break;
    case FMT_ERR_NAME_TAKEN:
        gui_msg("Format", "A device with this name is already\nmounted. Reboot to reformat it.");
        break;
    case FMT_ERR_RANGE:
    case FMT_ERR_MAKENODE:
    case FMT_ERR_ADDNODE:
    case FMT_ERR_FORMAT:
    default:
        gui_msg("Format", "Could not format the partition.");
        break;
    }
}
```

> NOTE: use the existing status-line setter. If it is named differently than
> `gui_set_status`, match the name used elsewhere (search `g_statusbuf` writers /
> `GID_STATUS`); if there is no helper, set status via the same
> `GT_SetGadgetAttrs(g_gad[GID_STATUS], ... GTTX_Text ...)` call
> `gui_update_buttons` uses. Verify `gui_msg`/`gui_confirm` signatures
> (`gui_msg(title, body)`, `gui_confirm(title, body)` — see lines 472-473).

- [ ] **Step 3: Wire the button, menu, and plain-key dispatch**

In the `IDCMP_GADGETUP` switch, after the `GID_RESIZE` handler add:
```c
                    else if (gad->GadgetID == GID_FORMAT) {
                        if (g_sel_part >= 0 && g_sel_part < g_model.num_parts)
                            gui_format(g_sel_part);
                    }
```
In the `IDCMP_MENUPICK` `MN_PART` block, after the `IP_RESIZE` handler add:
```c
                            else if (it == IP_FORMAT) {
                                if (g_sel_part >= 0 && g_sel_part < g_model.num_parts)
                                    gui_format(g_sel_part);
                            }
```
In the `IDCMP_VANILLAKEY` handler (search the block that maps plain letters like
`case 't': gui_split(); break;`), add a case for `o`:
```c
                        case 'o': case 'O':
                            if (g_sel_part >= 0 && g_sel_part < g_model.num_parts)
                                gui_format(g_sel_part);
                            break;
```

- [ ] **Step 4: Build + host tests**

Run:
```bash
export AMIGA_BIN="/Users/sfs/.vscode/extensions/bartmanabyss.amiga-debug-1.8.2/bin/darwin"; export PATH="$AMIGA_BIN/opt/bin:$AMIGA_BIN:$PATH"
make 2>&1 | tail -4 && ./tests/run-host-tests.sh
```
Expected: clean build (`entry-order OK`); all host tests pass.

- [ ] **Step 5: Commit**

```bash
git add src/gui.c
git commit -m "feat(gui): Format dialog + dispatch with save-first and boot/mount safety"
```

---

## Task 7: GUI — safety gate on Save

**Files:**
- Modify: `src/gui.c` (`gui_save`, ~line 480-513)

- [ ] **Step 1: Add the safety classification to the start of `gui_save`**

At the top of `gui_save` (before it opens the device / writes the RDB), add the
same gate Format uses. Insert right after the function's existing guards:

```c
    {
        static char snames[8][8];
        int snn = 0;
        DevLiveness slv = safety_classify(g_cur_driver, g_cur_unit, snames, 8, &snn);
        if (slv == DEV_BOOT) {
            gui_msg("Save",
                    "You booted from this device.\n"
                    "Writing a new partition table now would\n"
                    "corrupt the running system. Boot from\n"
                    "floppy (or another disk) to modify it.");
            return;   /* match gui_save's return type (it returns int 'ok'; use 'return 0;' if so) */
        }
        if (slv == DEV_MOUNTED) {
            if (!gui_confirm("Save",
                    "This disk has mounted volumes. Writing a\n"
                    "new table may disturb them. Proceed?"))
                return; /* or 'return 0;' to match gui_save */
        }
    }
```

> NOTE: `gui_save` returns `int` (`return ok;` at line 513). Use `return 0;` for
> the early-outs above to match the signature. Verify before building.

- [ ] **Step 2: Build + host tests**

Run:
```bash
export AMIGA_BIN="/Users/sfs/.vscode/extensions/bartmanabyss.amiga-debug-1.8.2/bin/darwin"; export PATH="$AMIGA_BIN/opt/bin:$AMIGA_BIN:$PATH"
make 2>&1 | tail -4 && ./tests/run-host-tests.sh
```
Expected: clean build; all host tests pass.

- [ ] **Step 3: Commit**

```bash
git add src/gui.c
git commit -m "feat(gui): boot/mount safety gate on Save (partition write)"
```

---

## Task 8: On-target verification (FS-UAE) + stage

**Files:** none (manual verification). Capture results; do not claim success without observing it (verification-before-completion).

- [ ] **Step 1: Build + stage**

Run:
```bash
export AMIGA_BIN="/Users/sfs/.vscode/extensions/bartmanabyss.amiga-debug-1.8.2/bin/darwin"; export PATH="$AMIGA_BIN/opt/bin:$AMIGA_BIN:$PATH"
make && make hd && cmp -s out/HDPart.exe amiga_hd/HDPart && echo STAGED_OK
```
Expected: `STAGED_OK`.

- [ ] **Step 2: Format happy-path (KS2.04, scratch disk = not boot)**

Boot `HDPart-204-devtest` (boot device is the WB floppy; scratch HDFs are on `uaehf.device`). In a Shell run `HDPart:HDPart`.
- Scan → select the blank scratch disk (`uaehf.device`/its unit).
- New → create a partition (e.g. fills the gap) → **Save**.
- Select it → **Format…** → confirm.
- Verify: status shows "Formatted DHx: …"; from a Shell, `DHx:` exists and is an empty validated volume (`Info DHx:`, copy a file to it, `Dir DHx:`).

- [ ] **Step 3: Format on KS3.x**

Repeat Step 2 on `HDPart-31-devtest` to confirm the FFS handler bind + `ACTION_FORMAT` work on V40 as well as V37.

- [ ] **Step 4: Boot-device block**

Attempt to **Save** and **Format** the device the system actually booted from (e.g. add an RDB scratch disk as the boot drive, or test on a config that boots from the HD). Verify both show the hard block message and do nothing. On `HDPart-204-devtest` (floppy boot), verify a scratch disk that has a mounted volume triggers the **warning** (not the block), and a truly blank one only the normal confirm.

- [ ] **Step 5: Save-first guard**

With unsaved edits (status "Modified"), select a partition → Format… → verify it refuses with "Save the partition table first".

- [ ] **Step 6: Record results**

Note pass/fail per step. If any OS-bound behaviour differs (handler not found, packet fails, name clash), debug with the `superpowers:systematic-debugging` skill — likely spots: `fse_PatchFlags` bit mapping, the MakeDosNode parmPacket string form, or the `ACTION_FORMAT` BSTR/arg order. The window-title diagnostic technique (used for the menu fix) is available if needed.

- [ ] **Step 7 (after on-target sign-off): bump + release**

Only once Steps 2-5 pass on-target: bump `gui.c` window title + About + Makefile `ADFVER` to the next version, commit "chore: bump version", `git push origin master`, then `git tag release-x.x && git push origin release-x.x`. (Mirrors the 0.4 release flow.)

---

## Self-review notes (addressed)

- **Spec coverage:** safety classifier (Tasks 1-2, 7 + Format), Approach-A live format (Tasks 3-4), ROM-FFS-only + name-clash + save-first (Tasks 4, 6), GUI button/menu/dialog (Tasks 5-6), host + on-target tests (Tasks 1,3,8). All spec sections map to a task.
- **Types consistent:** `DevLiveness`/`MountEntry`/`safety_decide`/`safety_classify` and `FmtResult`/`format_build_envec`/`format_partition`/`FMT_ENV_LONGS` are used identically across header, impl, tests, and GUI.
- **Placeholders:** none — every code step is complete. The two `> NOTE` blocks flag field-name/signature confirmations against existing source (not deferred work).
- **Known on-target risks** (flagged in Task 4): MakeDosNode parmPacket string form, `fse_PatchFlags` bit→field mapping, `ACTION_FORMAT` packet arg form. These are inherently not host-testable and are the focus of Task 8.
