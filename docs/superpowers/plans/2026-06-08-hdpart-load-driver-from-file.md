# Load a Device Driver From File — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let the user pick a `.device` file via an ASL file requester; HDPart loads it into the running system (`LoadSeg` + `InitResident`), probes that one driver's units, merges them into the live disk list, and preselects the first unit with media.

**Architecture:** A new pure helper `drv_find_romtag` (host-tested) locates the device romtag inside a loaded segment; the OS-bound `driver_load_file` drives `LoadSeg`/`InitResident`. Discovery gains an extra-driver registry (so loaded drivers persist across Rescan) and a targeted single-driver probe. The GUI adds a `Driver…` button wired to an ASL requester.

**Tech Stack:** C (freestanding, m68000, Bartman amiga-debug GCC), AmigaOS V37 exec/dos/asl/gadtools. Host unit tests via system `cc`.

**Build/test reminders (from project memory):**
```
export AMIGA_BIN="/Users/sfs/.vscode/extensions/bartmanabyss.amiga-debug-1.8.2/bin/darwin"
export PATH="$AMIGA_BIN/opt/bin:$AMIGA_BIN:$PATH"
make                       # -> out/HDPart.exe (+ .elf)
./tests/run-host-tests.sh  # host unit tests (system cc)
```
- `Makefile` compiles `src/*.c` via `wildcard`, so new `src/driver.c` is picked up automatically. `HDPART_AMIGA` is defined for the target build only; host tests compile with it undefined (only code outside `#ifdef HDPART_AMIGA` builds on host).
- Keep math 32-bit; no 64-bit ints. Keep large buffers `static`, not on the stack.

## File Structure

- **Create `src/driver.h`** — public API: `drv_find_romtag` (pure), `driver_load_file` (OS-bound), `DRVL_*` codes.
- **Create `src/driver.c`** — pure `drv_find_romtag` outside `#ifdef HDPART_AMIGA`; `driver_load_file` inside it. Mirrors the `discover.c` split pattern.
- **Create `tests/test_driver.c`** — host tests for `drv_find_romtag`.
- **Modify `src/endian.h`** — add `be_get16`/`be_put16`.
- **Modify `src/discover.h` / `src/discover.c`** — `disc_add_extra_driver`, `disc_extra_count` (registry, host-testable) + OS-bound `discover_probe_driver`.
- **Modify `tests/test_discover.c`** — tests for the extra-driver registry.
- **Modify `tests/run-host-tests.sh`** — build & run `test_driver.c`.
- **Modify `src/gui.c`** — open `asl.library`, `Driver…` button, `gui_load_driver`, factor `gui_rebuild_devlabels`, wire event loop + teardown.

---

## Task 1: `be16` helpers + `drv_find_romtag` pure helper

**Files:**
- Modify: `src/endian.h`
- Create: `src/driver.h`, `src/driver.c`
- Create: `tests/test_driver.c`
- Modify: `tests/run-host-tests.sh`

- [ ] **Step 1: Add 16-bit big-endian accessors to `src/endian.h`**

Insert before the final `#endif`:

```c
static inline uint16_t be_get16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}
static inline void be_put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v;
}
```

- [ ] **Step 2: Create `src/driver.h`**

```c
#ifndef HDPART_DRIVER_H
#define HDPART_DRIVER_H

#include <stdint.h>

#define DRV_NAME_LEN 40

/* Result codes for driver_load_file (negative = failure). */
enum {
    DRVL_OK         =  0,
    DRVL_ELOAD      = -1,  /* LoadSeg failed (missing / not a load file) */
    DRVL_ENOROMTAG  = -2,  /* no Resident romtag found */
    DRVL_ENOTDEVICE = -3,  /* a romtag exists but none is NT_DEVICE */
    DRVL_EINIT      = -4   /* InitResident ran but device not in list */
};

/* Exec node type for a device romtag (exec/nodes.h NT_DEVICE). Defined here so
   the pure helper and its host tests need no AmigaOS headers. */
#define DRV_NT_DEVICE 3

/* ---- pure helper (no OS calls; unit-tested on host) ----
   Scan [base, base+size) from byte offset start_off for the next valid Resident
   romtag: a big-endian 16-bit rt_MatchWord == 0x4AFC at an even offset whose
   big-endian 32-bit rt_MatchTag (at off+2) equals the low 32 bits of the
   romtag's own address (self-pointer). On a match, set *off_out to the offset,
   *type_out to rt_Type (the byte at off+12), and return 1. Return 0 if none.
   Stepping is 2 bytes (romtags are word-aligned). */
int drv_find_romtag(const unsigned char *base, uint32_t size,
                    uint32_t start_off, uint32_t *off_out, int *type_out);

/* ---- OS-bound entry point (implemented under HDPART_AMIGA in driver.c) ----
   Load a .device file so OpenDevice() can reach it. On success copies the exec
   device name (romtag rt_Name) into name_out (<= name_sz) and returns DRVL_OK.
   If a device of that name is already resident, returns DRVL_OK without
   re-loading. Otherwise returns one of DRVL_E*. */
int driver_load_file(const char *path, char *name_out, int name_sz);

#endif /* HDPART_DRIVER_H */
```

- [ ] **Step 3: Create `src/driver.c` with the pure helper only (OS part added in Task 2)**

```c
/* HDPart driver loading. Pure romtag-finder (host-testable) plus the OS-bound
   loader (guarded by HDPART_AMIGA, added in Task 2). Mirrors discover.c. */
#ifdef HDPART_AMIGA
#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/resident.h>
#include <exec/nodes.h>
#include <proto/exec.h>
#include <proto/dos.h>
#endif
#include "driver.h"
#include "endian.h"

/* struct Resident field offsets (exec/resident.h), as raw bytes: */
#define RT_MATCHWORD 0    /* UWORD */
#define RT_MATCHTAG  2    /* APTR  */
#define RT_TYPE      12   /* UBYTE */
#define RT_SIZE      26   /* sizeof(struct Resident) */
#define RTC_MATCHWORD 0x4AFCu

int drv_find_romtag(const unsigned char *base, uint32_t size,
                    uint32_t start_off, uint32_t *off_out, int *type_out)
{
    uint32_t off;
    if (size < RT_SIZE) return 0;
    /* romtags are word-aligned; start on an even offset */
    off = (start_off + 1u) & ~1u;
    for (; off + RT_SIZE <= size; off += 2) {
        uint32_t self;
        if (be_get16(base + off + RT_MATCHWORD) != RTC_MATCHWORD) continue;
        self = (uint32_t)(uintptr_t)(base + off);
        if (be_get32(base + off + RT_MATCHTAG) != self) continue;
        if (off_out)  *off_out  = off;
        if (type_out) *type_out = (int)base[off + RT_TYPE];
        return 1;
    }
    return 0;
}
```

- [ ] **Step 4: Create `tests/test_driver.c`**

```c
/* Host unit tests for driver.c pure helper drv_find_romtag. The OS-bound
   driver_load_file() is NOT exercised here (it needs AmigaOS). */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "../src/driver.h"
#include "../src/endian.h"

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); g_fail++; } } while (0)

/* Lay a valid Resident romtag into buf at offset off, of the given rt_Type.
   The self-pointer (rt_MatchTag) is the low 32 bits of the tag's own address. */
static void put_romtag(unsigned char *buf, uint32_t off, int type)
{
    memset(buf + off, 0, 26);
    be_put16(buf + off + 0, 0x4AFC);                              /* rt_MatchWord */
    be_put32(buf + off + 2, (uint32_t)(uintptr_t)(buf + off));    /* rt_MatchTag (self) */
    buf[off + 12] = (unsigned char)type;                         /* rt_Type */
}

static void test_finds_valid_romtag(void)
{
    unsigned char buf[128];
    uint32_t off = 99; int type = -1;
    memset(buf, 0, sizeof(buf));
    put_romtag(buf, 8, DRV_NT_DEVICE);   /* place at an even offset */
    CHECK(drv_find_romtag(buf, sizeof(buf), 0, &off, &type) == 1);
    CHECK(off == 8);
    CHECK(type == DRV_NT_DEVICE);
}

static void test_rejects_bare_matchword(void)
{
    /* 0x4AFC present but rt_MatchTag is NOT the self-pointer -> not a romtag. */
    unsigned char buf[128];
    uint32_t off = 99; int type = -1;
    memset(buf, 0, sizeof(buf));
    be_put16(buf + 8, 0x4AFC);
    be_put32(buf + 10, 0x12345678u);     /* bogus, not self */
    CHECK(drv_find_romtag(buf, sizeof(buf), 0, &off, &type) == 0);
}

static void test_none_when_absent(void)
{
    unsigned char buf[128];
    uint32_t off = 99; int type = -1;
    memset(buf, 0, sizeof(buf));
    CHECK(drv_find_romtag(buf, sizeof(buf), 0, &off, &type) == 0);
}

static void test_reports_non_device_type(void)
{
    /* A valid romtag that is NOT a device (e.g. NT_LIBRARY=9): found, type 9. */
    unsigned char buf[128];
    uint32_t off = 99; int type = -1;
    memset(buf, 0, sizeof(buf));
    put_romtag(buf, 16, 9);
    CHECK(drv_find_romtag(buf, sizeof(buf), 0, &off, &type) == 1);
    CHECK(off == 16);
    CHECK(type == 9);
}

static void test_respects_bounds(void)
{
    /* A romtag whose 26 bytes would straddle the end must not be reported. */
    unsigned char buf[40];
    uint32_t off = 99; int type = -1;
    memset(buf, 0, sizeof(buf));
    put_romtag(buf, 20, DRV_NT_DEVICE);  /* 20+26 = 46 > 40: out of bounds */
    CHECK(drv_find_romtag(buf, sizeof(buf), 0, &off, &type) == 0);
}

static void test_finds_second_when_start_advanced(void)
{
    /* Two romtags; starting past the first returns the second. */
    unsigned char buf[128];
    uint32_t off = 99; int type = -1;
    memset(buf, 0, sizeof(buf));
    put_romtag(buf, 8,  9);              /* first: a library */
    put_romtag(buf, 48, DRV_NT_DEVICE);  /* second: the device */
    CHECK(drv_find_romtag(buf, sizeof(buf), 0,  &off, &type) == 1);
    CHECK(off == 8 && type == 9);
    CHECK(drv_find_romtag(buf, sizeof(buf), 10, &off, &type) == 1);
    CHECK(off == 48 && type == DRV_NT_DEVICE);
}

int main(void)
{
    test_finds_valid_romtag();
    test_rejects_bare_matchword();
    test_none_when_absent();
    test_reports_non_device_type();
    test_respects_bounds();
    test_finds_second_when_start_advanced();
    if (g_fail) { printf("%d driver test(s) FAILED\n", g_fail); return 1; }
    printf("all driver tests passed\n");
    return 0;
}
```

- [ ] **Step 5: Wire the new test into `tests/run-host-tests.sh`**

Append after the existing `test_discover` lines (before EOF):

```sh
cc -std=c99 -Wall -Wextra -g -o /tmp/hdpart_driver tests/test_driver.c src/driver.c
/tmp/hdpart_driver
```

- [ ] **Step 6: Run host tests, expect the new suite to pass**

Run: `./tests/run-host-tests.sh`
Expected: existing suites still pass, plus `all driver tests passed`.

- [ ] **Step 7: Commit**

```bash
git add src/endian.h src/driver.h src/driver.c tests/test_driver.c tests/run-host-tests.sh
git commit -m "feat(driver): host-tested drv_find_romtag romtag finder + be16 helpers

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 2: `driver_load_file` (OS-bound loader)

**Files:**
- Modify: `src/driver.c` (add the `#ifdef HDPART_AMIGA` section)

This is OS-bound (LoadSeg/InitResident) so it cannot run in host tests; verification is a clean target build plus the on-target test in Task 4.

- [ ] **Step 1: Append the OS-bound loader to `src/driver.c`** (after the pure helper, at end of file)

```c
#ifdef HDPART_AMIGA

/* Copy a C string into out (<= outsz), NUL-terminated. */
static void copy_name(char *out, int outsz, const char *src)
{
    int n = 0;
    if (outsz <= 0) return;
    while (n < outsz - 1 && src && src[n]) { out[n] = src[n]; n++; }
    out[n] = 0;
}

/* True if a device of this name is already in exec's device list. */
static int device_resident(const char *name)
{
    struct Node *n;
    int found = 0;
    Forbid();
    n = FindName(&SysBase->DeviceList, (CONST_STRPTR)name);
    found = (n != 0);
    Permit();
    return found;
}

int driver_load_file(const char *path, char *name_out, int name_sz)
{
    BPTR seglist, s;
    const struct Resident *dev = 0;   /* first NT_DEVICE romtag found */
    int any_romtag = 0;
    char name[DRV_NAME_LEN];

    seglist = LoadSeg((CONST_STRPTR)path);
    if (!seglist) return DRVL_ELOAD;

    /* Walk each segment's data for romtags. Segment layout (LoadSeg):
       [size longword][next BPTR][data...]; BADDR(s) points at the next BPTR,
       size is the longword before it, data starts 4 bytes after BADDR(s). */
    for (s = seglist; s; s = *(BPTR *)BADDR(s)) {
        ULONG *hdr = (ULONG *)BADDR(s);
        ULONG total = hdr[-1];
        const unsigned char *data = (const unsigned char *)(hdr + 1);
        uint32_t dlen = (total >= 8) ? (uint32_t)(total - 8) : 0;
        uint32_t off = 0;
        int type = 0;
        while (drv_find_romtag(data, dlen, off, &off, &type)) {
            any_romtag = 1;
            if (type == DRV_NT_DEVICE) {
                dev = (const struct Resident *)(data + off);
                break;
            }
            off += 2;   /* keep scanning this segment for an NT_DEVICE romtag */
        }
        if (dev) break;
    }

    if (!dev) {
        UnLoadSeg(seglist);
        return any_romtag ? DRVL_ENOTDEVICE : DRVL_ENOROMTAG;
    }

    copy_name(name, sizeof(name), (const char *)dev->rt_Name);

    /* Already loaded (e.g. previously loaded by us, or by the controller ROM)?
       Don't re-init; just hand back the name and drop our redundant copy. */
    if (device_resident(name)) {
        UnLoadSeg(seglist);
        copy_name(name_out, name_sz, name);
        return DRVL_OK;
    }

    /* For an RTF_AUTOINIT device this MakeLibrary+AddDevice's it into exec's
       device list. We must NOT UnLoadSeg afterwards: the live device owns it. */
    InitResident((struct Resident *)dev, seglist);

    if (!device_resident(name))
        return DRVL_EINIT;     /* segment stays loaded; nothing safe to undo */

    copy_name(name_out, name_sz, name);
    return DRVL_OK;
}

#endif /* HDPART_AMIGA */
```

- [ ] **Step 2: Build for the target, expect a clean compile/link**

Run:
```bash
export AMIGA_BIN="/Users/sfs/.vscode/extensions/bartmanabyss.amiga-debug-1.8.2/bin/darwin"
export PATH="$AMIGA_BIN/opt/bin:$AMIGA_BIN:$PATH"
make
```
Expected: builds `out/HDPart.exe` with no warnings/errors (driver.c compiles into the binary).

- [ ] **Step 3: Re-run host tests (ensure the OS section didn't break the host build)**

Run: `./tests/run-host-tests.sh`
Expected: all suites pass (the `#ifdef HDPART_AMIGA` section is excluded on host).

- [ ] **Step 4: Commit**

```bash
git add src/driver.c
git commit -m "feat(driver): driver_load_file — LoadSeg + InitResident a .device file

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 3: Discovery — extra-driver registry + targeted probe

**Files:**
- Modify: `src/discover.h`, `src/discover.c`
- Modify: `tests/test_discover.c`

- [ ] **Step 1: Declare the new API in `src/discover.h`** (before the final `#endif`)

```c
/* Register a user-loaded driver name so the curated probe (scan_probe) also
   probes it on every subsequent discover_disks(). Deduped; ignored past
   capacity. Plain C (no OS calls) so it is host-testable. */
void disc_add_extra_driver(const char *name);

/* Number of registered extra drivers (for tests / introspection). */
int disc_extra_count(void);

/* Targeted probe of ONE driver: open units 0..PROBE_UNITS-1, add the ones that
   open to out[] (starting at *count, capacity max), classify just those new
   entries (geometry + RDB). *count is updated in place. Returns the number of
   entries added. OS-bound. */
int discover_probe_driver(DiscDisk out[], int *count, int max, const char *driver);
```

- [ ] **Step 2: Add the registry to `src/discover.c` (outside the `#ifdef HDPART_AMIGA`, so it host-compiles)**

Place this just after the pure helpers (e.g. after `disc_is_partitionable`, before the `#ifdef HDPART_AMIGA` line):

```c
/* ---- user-loaded driver registry (plain C; shared by host + target) ---- */
#define DISC_EXTRA_MAX 8
static char g_extra[DISC_EXTRA_MAX][DISC_DRIVER_LEN];
static int  g_extra_n = 0;

void disc_add_extra_driver(const char *name)
{
    int i, n;
    if (!name || !name[0]) return;
    for (i = 0; i < g_extra_n; i++) {            /* dedup (exact match) */
        for (n = 0; name[n] && g_extra[i][n] == name[n]; n++) ;
        if (name[n] == 0 && g_extra[i][n] == 0) return;
    }
    if (g_extra_n >= DISC_EXTRA_MAX) return;      /* silently ignore overflow */
    for (n = 0; n < DISC_DRIVER_LEN - 1 && name[n]; n++)
        g_extra[g_extra_n][n] = name[n];
    g_extra[g_extra_n][n] = 0;
    g_extra_n++;
}

int disc_extra_count(void) { return g_extra_n; }
```

- [ ] **Step 3: Make `scan_probe` also probe the registry (inside `#ifdef HDPART_AMIGA`)**

In `scan_probe`, after the existing `kProbeDrivers` loop, add a second loop over the registry. Replace the body of `scan_probe` with:

```c
static void scan_probe(DiscDisk out[], int *count, int max)
{
    int di;
    for (di = 0; kProbeDrivers[di]; di++) {
        ULONG u;
        for (u = 0; u < PROBE_UNITS; u++) {
            DeviceHandle *h = dev_open(kProbeDrivers[di], u);
            if (!h) continue;
            dev_close(h);
            add_unique(out, count, max, kProbeDrivers[di], u);
        }
    }
    for (di = 0; di < g_extra_n; di++) {
        ULONG u;
        for (u = 0; u < PROBE_UNITS; u++) {
            DeviceHandle *h = dev_open(g_extra[di], u);
            if (!h) continue;
            dev_close(h);
            add_unique(out, count, max, g_extra[di], u);
        }
    }
}
```

- [ ] **Step 4: Add `discover_probe_driver` (inside `#ifdef HDPART_AMIGA`, e.g. after `discover_disks`)**

```c
int discover_probe_driver(DiscDisk out[], int *count, int max, const char *driver)
{
    int before = *count;
    ULONG u;
    int i;
    for (u = 0; u < PROBE_UNITS; u++) {
        DeviceHandle *h = dev_open(driver, u);
        if (!h) continue;          /* unit not present */
        dev_close(h);
        add_unique(out, count, max, driver, u);
    }
    /* Classify only the entries we just added. */
    for (i = before; i < *count; i++)
        probe_one(&out[i]);
    return *count - before;
}
```

- [ ] **Step 5: Add registry tests to `tests/test_discover.c`**

Add this function and call it from `main` (next to the other `test_*()` calls):

```c
static void test_extra_driver_registry(void)
{
    /* Starts empty; dedups; respects the cap of 8. */
    CHECK(disc_extra_count() == 0);
    disc_add_extra_driver("lide.device");
    CHECK(disc_extra_count() == 1);
    disc_add_extra_driver("lide.device");          /* duplicate */
    CHECK(disc_extra_count() == 1);
    disc_add_extra_driver("");                      /* ignored */
    CHECK(disc_extra_count() == 1);
    disc_add_extra_driver("a.device");
    disc_add_extra_driver("b.device");
    disc_add_extra_driver("c.device");
    disc_add_extra_driver("d.device");
    disc_add_extra_driver("e.device");
    disc_add_extra_driver("f.device");
    disc_add_extra_driver("g.device");             /* 8th distinct -> at cap */
    CHECK(disc_extra_count() == 8);
    disc_add_extra_driver("h.device");             /* over cap -> ignored */
    CHECK(disc_extra_count() == 8);
}
```

Find `main()` in `tests/test_discover.c` and add `test_extra_driver_registry();` alongside the other test calls.

- [ ] **Step 6: Run host tests**

Run: `./tests/run-host-tests.sh`
Expected: all suites pass including the new registry assertions.

- [ ] **Step 7: Build for target (ensure the OS-side probe loop compiles)**

Run: `make`
Expected: clean build.

- [ ] **Step 8: Commit**

```bash
git add src/discover.h src/discover.c tests/test_discover.c
git commit -m "feat(discover): extra-driver registry + discover_probe_driver targeted probe

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 4: GUI — `Driver…` button + ASL requester wiring

**Files:**
- Modify: `src/gui.c`

GUI/ASL/LoadSeg are all OS-bound; verification is a clean target build plus an on-target FS-UAE test (final steps).

- [ ] **Step 1: Add includes, the AslBase global, and a DOSBase extern**

In `src/gui.c`, after the existing `#include <proto/graphics.h>` line add:

```c
#include <libraries/asl.h>
#include <proto/asl.h>
#include <proto/dos.h>
```

After the existing `struct GfxBase *GfxBase = 0;` line add:

```c
struct Library *AslBase = 0;
extern struct DosLibrary *DOSBase;   /* opened in startup.c (for AddPart) */
```

- [ ] **Step 2: Add `GID_DRIVER` to the gadget-ID enum and a forward decl**

Change the enum to include `GID_DRIVER`:

```c
enum { GID_DEVICE = 1, GID_RESCAN, GID_DRIVER, GID_PARTS, GID_NEW, GID_DELETE,
       GID_EDIT, GID_INIT, GID_SAVE, GID_STATUS };
```

Near the other forward decls (`void gui_rescan(void);` / `void gui_select_device(int idx);`) add:

```c
static void gui_load_driver(void);
```

- [ ] **Step 3: Re-space the cycle/rescan row and add the `Driver…` button**

In `build_gadgets()`, the device cycle currently spans to ~370 and Rescan sits at LeftEdge 380 width 70. Narrow the cycle and add a third control. Replace the cycle width line and the Rescan block:

Change the cycle gadget line from `ng_Width = 300` to `260`:

```c
    ng.ng_LeftEdge = 70 + g_leftb;  ng.ng_TopEdge = 6 + g_topb;  ng.ng_Width = 260; ng.ng_Height = 14;
    ng.ng_GadgetText = (UBYTE *)"Disk:"; ng.ng_GadgetID = GID_DEVICE;
```

Replace the Rescan button block with Driver + Rescan:

```c
    /* Driver… button (load a .device from file via ASL) */
    ng.ng_LeftEdge = 336 + g_leftb; ng.ng_TopEdge = 6 + g_topb; ng.ng_Width = 60; ng.ng_Height = 14;
    ng.ng_GadgetText = (UBYTE *)"Driver…"; ng.ng_GadgetID = GID_DRIVER;
    g = CreateGadget(BUTTON_KIND, g, &ng, GA_Disabled, (ULONG)(AslBase == 0), TAG_END);
    g_gad[GID_DRIVER] = g;

    /* Rescan button */
    ng.ng_LeftEdge = 400 + g_leftb; ng.ng_TopEdge = 6 + g_topb; ng.ng_Width = 56; ng.ng_Height = 14;
    ng.ng_GadgetText = (UBYTE *)"Rescan"; ng.ng_GadgetID = GID_RESCAN;
    g = CreateGadget(BUTTON_KIND, g, &ng, TAG_END);
    g_gad[GID_RESCAN] = g;
```

(`AslBase` is opened in Step 6 before `build_gadgets` runs, so the disabled flag is correct.)

- [ ] **Step 4: Factor the cycle-label rebuild out of `gui_rescan` into a shared helper**

Add this new function immediately before `gui_rescan`. It is the label-building loop currently inline in `gui_rescan`, lifted verbatim, returning the number of cycle entries built:

```c
/* Rebuild g_devlabels/g_devmap/g_devtext from the current g_disks[] (the
   partitionable ones). Returns the number of cycle entries. Shared by the full
   rescan and the load-driver path. */
static int gui_rebuild_devlabels(void)
{
    int i, n = 0;
    for (i = 0; i < g_ndisks && n < DISC_MAX; i++) {
        DiscDisk *d = &g_disks[i];
        char *t = g_devtext[n];
        int p = 0, k;
        if (!d->partitionable) continue;
        g_devmap[n] = i;
        for (k = 0; d->driver[k] && p < 30; k++) t[p++] = d->driver[k];
        t[p++] = ' '; t[p++] = 'u';
        { ULONG u = d->unit; char tmp[12]; int ti = 0;
          if (u == 0) tmp[ti++] = '0';
          while (u) { tmp[ti++] = (char)('0' + (u % 10)); u /= 10; }
          while (ti > 0 && p < 44) t[p++] = tmp[--ti]; }
        t[p++] = ' ';
        { ULONG s = d->size_mb; char tmp[12]; int ti = 0;
          if (s == 0) tmp[ti++] = '0';
          while (s) { tmp[ti++] = (char)('0' + (s % 10)); s /= 10; }
          while (ti > 0 && p < 46) t[p++] = tmp[--ti];
          t[p++] = 'M'; }
        t[p] = 0;
        g_devlabels[n] = t;
        n++;
    }
    if (n == 0) { g_devlabels[0] = "(no disks found)"; g_devlabels[1] = 0; }
    else g_devlabels[n] = 0;
    return n;
}
```

- [ ] **Step 5: Replace the inline loop in `gui_rescan` with a call to the helper**

In `gui_rescan`, replace everything from `int i, n = 0;` through the `if (n == 0) {...} else g_devlabels[n] = 0;` block (the label-building loop) so the function body reads:

```c
void gui_rescan(void)
{
    int n;
    /* Discovery probes many devices/units and can take a moment; show progress
       so the window doesn't look hung while the dropdown is empty. */
    if (g_win && g_gad[GID_STATUS])
        GT_SetGadgetAttrs(g_gad[GID_STATUS], g_win, 0,
                          GTTX_Text, (ULONG)"Scanning for devices...", TAG_END);
    g_ndisks = discover_disks(g_disks, DISC_MAX);

    n = gui_rebuild_devlabels();

    if (g_win && g_gad[GID_DEVICE])
        GT_SetGadgetAttrs(g_gad[GID_DEVICE], g_win, 0,
                          GTCY_Labels, (ULONG)g_devlabels,
                          GTCY_Active, 0, TAG_END);

    gui_select_device(n > 0 ? 0 : -1);
}
```

- [ ] **Step 6: Open `asl.library` in `gui_run` (non-fatal)**

In `gui_run`, after the `GfxBase = ...` open and its `if (!GadToolsBase || !GfxBase)` guard, add:

```c
    AslBase = OpenLibrary("asl.library", 37);   /* optional: Driver… disabled if absent */
```

- [ ] **Step 7: Close `asl.library` in teardown**

At the `cleanup_libs:` label, before `CloseLibrary(GadToolsBase);` add:

```c
    if (AslBase) { CloseLibrary(AslBase); AslBase = 0; }
```

- [ ] **Step 8: Handle the new button in the event loop**

In the `IDCMP_GADGETUP` switch, add a branch (e.g. after the `GID_RESCAN` line):

```c
                    else if (gad->GadgetID == GID_DRIVER) gui_load_driver();
```

- [ ] **Step 9: Implement `gui_load_driver`**

Add this function (e.g. immediately after `gui_rescan`):

```c
/* Map a DRVL_* failure code to a user-facing message. */
static const char *drv_err_text(int code)
{
    switch (code) {
        case DRVL_ELOAD:      return "Could not load that file as a driver.";
        case DRVL_ENOROMTAG:  return "That file has no driver (no Resident tag).";
        case DRVL_ENOTDEVICE: return "That file is not a .device driver.";
        case DRVL_EINIT:      return "Driver loaded but failed to initialise.";
        default:              return "Could not load that driver.";
    }
}

/* Ask for a .device file, load it, probe it, and preselect its first unit with
   media. Static buffers keep big arrays off the stack (project convention). */
static void gui_load_driver(void)
{
    static struct FileRequester *fr;
    static char path[256];
    static char name[DRV_NAME_LEN];
    int rc, i, n, sel = -1;

    if (!AslBase) return;   /* button is disabled, but guard anyway */

    fr = (struct FileRequester *)AllocAslRequestTags(ASL_FileRequest,
            ASLFR_TitleText,     (ULONG)"Select a device driver",
            ASLFR_InitialDrawer, (ULONG)"DEVS:",
            ASLFR_DoPatterns,    TRUE,
            ASLFR_InitialPattern,(ULONG)"#?.device",
            TAG_END);
    if (!fr) { gui_msg("Driver", "Could not open the file requester."); return; }

    if (!AslRequest(fr, 0)) { FreeAslRequest(fr); return; }   /* cancelled */

    /* Join drawer + file into path[] (AddPart inserts any needed separator). */
    { int n = 0; const char *d = (const char *)fr->fr_Drawer;
      while (d && d[n] && n < (int)sizeof(path) - 1) { path[n] = d[n]; n++; }
      path[n] = 0; }
    AddPart((STRPTR)path, (CONST_STRPTR)fr->fr_File, sizeof(path));
    FreeAslRequest(fr);

    rc = driver_load_file(path, name, sizeof(name));
    if (rc != DRVL_OK) { gui_msg("Driver", drv_err_text(rc)); return; }

    /* Remember it for later full rescans, then probe just this driver now. */
    disc_add_extra_driver(name);
    discover_probe_driver(g_disks, &g_ndisks, DISC_MAX, name);

    n = gui_rebuild_devlabels();

    /* Preselect the first partitionable entry belonging to the loaded driver.
       Bound by n (when n==0 the label array holds only a sentinel and g_devmap
       is stale, so never index it). */
    for (i = 0; i < n; i++) {
        DiscDisk *d = &g_disks[g_devmap[i]];
        int k; for (k = 0; d->driver[k] && name[k] && d->driver[k] == name[k]; k++) ;
        if (d->driver[k] == 0 && name[k] == 0 && d->partitionable) { sel = i; break; }
    }

    if (g_win && g_gad[GID_DEVICE])
        GT_SetGadgetAttrs(g_gad[GID_DEVICE], g_win, 0,
                          GTCY_Labels, (ULONG)g_devlabels,
                          GTCY_Active, (ULONG)(sel >= 0 ? sel : 0), TAG_END);

    if (sel >= 0) {
        gui_select_device(sel);
    } else {
        gui_select_device(-1);
        if (g_win && g_gad[GID_STATUS])
            GT_SetGadgetAttrs(g_gad[GID_STATUS], g_win, 0,
                              GTTX_Text, (ULONG)"Driver loaded; no media on units 0-7",
                              TAG_END);
    }
}
```

- [ ] **Step 10: Build for target**

Run: `make`
Expected: clean build of `out/HDPart.exe`. (If the compiler rejects the `…` ellipsis char in the button label, replace `"Driver…"` with `"Driver"`.)

- [ ] **Step 11: Run host tests (no regressions)**

Run: `./tests/run-host-tests.sh`
Expected: all suites pass.

- [ ] **Step 12: On-target manual test in FS-UAE (KS2.04)**

Run:
```bash
make hd            # stage binary into amiga_hd/ + amiga_boot/
make install-fsuae # (first time) install FS-UAE configs
```
Then launch the `HDPart-204` config, and from a WB Shell:
1. Copy a real driver into DEVS: that is NOT already loaded (e.g. from a controller disk, or `copy uaehf.device DEVS:` if it exists on disk).
2. Run `HDPart:HDPart`.
3. Click **Driver…** — confirm the requester opens at `DEVS:` showing `#?.device` files.
4. Pick the driver. Confirm: it loads, the `Disk:` dropdown gains its unit(s), the first unit with media is auto-selected, and its partition list / disk-map bar render.
5. Click **Driver…** again and pick the same driver — confirm it selects without error (already-resident path).
6. Click **Rescan** — confirm the loaded driver's units still appear (registry persistence).
7. Pick a non-driver file (e.g. a text file) — confirm a clear error message, no crash.

Expected: all of the above behave as described; no Guru/illegal-instruction.

- [ ] **Step 13: Commit**

```bash
git add src/gui.c
git commit -m "feat(gui): Driver… button — ASL-pick a .device, load it, probe + preselect

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Self-Review notes

- **Spec coverage:** `driver.c`/`driver.h` + `drv_find_romtag`/`driver_load_file` (Tasks 1–2) ✓; already-loaded guard via `device_resident` ✓; lifetime "stays resident, no RemDevice" ✓ (Task 2 comment); registry + targeted probe (Task 3) ✓; ASL requester defaulting to `DEVS:`, `#?.device`, `Driver…` button disabled without asl.library, factored `gui_rebuild_devlabels`, preselect first unit with media, "no media" status (Task 4) ✓; `DRVL_*`→message table ✓ (`drv_err_text`); host tests for romtag finder incl. bare-matchword / bounds / multi-romtag ✓.
- **Type consistency:** `drv_find_romtag(base,size,start_off,off_out,type_out)`, `driver_load_file(path,name_out,name_sz)`, `disc_add_extra_driver(name)`, `disc_extra_count()`, `discover_probe_driver(out,*count,max,driver)`, `gui_rebuild_devlabels(void)`, `gui_load_driver(void)` — used consistently across tasks. `DRV_NAME_LEN`(40) matches `g_cur_driver[40]` / `DISC_DRIVER_LEN`(40).
- **Known nuance:** the `…` glyph in the button label may not encode under the toolchain's default charset; Step 10 calls out the `"Driver"` fallback.
```
