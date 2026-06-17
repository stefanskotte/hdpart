# HDPart Embed Filesystem (Feature B) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Embed non-ROM filesystems (PFS3/SFS) into a disk's RDB as `FSHD`+`LSEG` blocks — loaded from a file or copied from another disk's RDB — exposed in the Edit FS-cycle and usable by Format even when the handler is not installed.

**Architecture:** Four layers, bottom-up: (1) the RDB engine learns to model, parse and serialize `FSHD`/`LSEG` chains with a linear block allocator; (2) a new `fsload` module acquires a filesystem from a file or another disk; (3) `format.c` gains a Strategy-3 binding that reconstructs a live seglist from embedded `LSEG` via `InternalLoadSeg`; (4) the GUI gets a dedicated Filesystems dialog plus Edit FS-cycle wiring. All embedding edits the in-memory `RdbModel` and is written on Save.

**Tech Stack:** C99 (host tests via system `cc`); Bartman m68k-amiga-elf GCC 15.1.0 for the target; GadTools/Intuition/dos.library/expansion.library; amitools `rdbtool` for cross-check; FS-UAE for on-target verification.

**Spec:** `docs/superpowers/specs/2026-06-17-hdpart-embed-filesystem-design.md`

## Global Constraints

- **Target baseline:** AmigaOS Kickstart 2.04 (exec V37). DirCache/V39 features stay version-gated. `InternalLoadSeg` ABI must be confirmed against V37 dos.library (Task 13).
- **Freestanding C, 32-bit math only:** no libc/libgcc on target; never use 64-bit int math (pulls in absent `__udivdi3`); `src/support.c` provides 32-bit soft-division so `/` and `%` work.
- **Host-testability:** `src/rdb.c` is compiled by host `cc` in `tests/run-host-tests.sh`. All new `rdb.c` code must compile under plain C99 with no Amiga headers. Amiga-only allocation is gated behind `#ifdef HDPART_AMIGA` (the Makefile passes `-DHDPART_AMIGA`); host builds use `malloc/free`.
- **Heap discipline:** big buffers stay off the ~4KB Shell stack — `static` or heap. `seg_data` is heap-owned and must be freed on model teardown and on dialog Remove.
- **Caps:** `RDB_MAX_FS = 8`. Max FS file size 512K (`FSL_ETOOBIG`).
- **ASL on V37:** no `ASLFR_DoPatterns` filter on requesters whose Ok must work; always pass `ASLFR_Screen`.
- **Block IDs:** `FSHD = 0x46534844`, `LSEG = 0x4C534547`. FSHD `SummedLongs=64`, LSEG `SummedLongs=128`.
- **Build/stage/test:** after `make`, ALWAYS `make hd` before any FS-UAE test (stale-binary trap). Toolchain env:
  `export AMIGA_BIN="/Users/sfs/.vscode/extensions/bartmanabyss.amiga-debug-1.8.2/bin/darwin"; export PATH="$AMIGA_BIN/opt/bin:$AMIGA_BIN:$PATH"`.
- **Commits:** work goes straight to `master`, frequent focused commits. Stage only this task's files (subagent `git add <file>` can sweep up unrelated pending edits). End commit messages with `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`.

## File Structure

- **Modify** `src/rdb.h` — `RdbFileSys` struct, `RDB_MAX_FS`, `fs[]`/`num_fs` on `RdbModel`, `RDB_ERR_NO_RDB_SPACE`, `rdb_model_free`, FSHD/LSEG block-count helpers, source enum.
- **Modify** `src/rdb.c` — FSHD/LSEG parse, serialize with linear allocator, capacity guard, alloc shim, teardown.
- **Create** `src/fsload.h` / `src/fsload.c` — `fsload_from_file`, `fsload_from_disk`, error text. (Amiga-only; not in host tests except a small pure helper.)
- **Modify** `src/format.c` / `src/format.h` — Strategy-3 embedded-FS binding; thread the model into `format_partition`.
- **Modify** `src/gui.c` — Filesystems dialog, menu/button entry, Edit FS-cycle wiring, model teardown on disk change.
- **Create** `tests/test_fshd.c` — host tests for FSHD/LSEG round-trip, allocator, capacity guard, preservation.
- **Modify** `tests/run-host-tests.sh` — wire `test_fshd`.
- **Modify** `tests/verify-rdbtool.sh` — cross-check embedded FS via `rdbtool`.

---

## PHASE 1 — RDB ENGINE: FSHD/LSEG read + write

### Task 1: Model types and constants for embedded filesystems

**Files:**
- Modify: `src/rdb.h`

**Interfaces:**
- Produces: `RdbFileSys` struct; `RdbModel.fs[RDB_MAX_FS]`, `RdbModel.num_fs`; `RDB_MAX_FS`; `RDB_ERR_NO_RDB_SPACE`; `RDB_FS_EMBEDDED/_FILE/_COPIED`; `rdb_model_free(RdbModel*)`.

- [ ] **Step 1: Add the struct, fields, constants, error code, source enum and teardown prototype**

In `src/rdb.h`, after the `RdbPartition` struct (line 33) add:

```c
#define RDB_MAX_FS 8

/* Source of an embedded filesystem (display only). */
enum { RDB_FS_EMBEDDED = 0, RDB_FS_FILE = 1, RDB_FS_COPIED = 2 };

typedef struct {
    uint32_t dos_type;        /* fhb_DosType, e.g. 0x50465303 = PFS\3 */
    uint32_t version;         /* fhb_Version (ver<<16 | patch) */
    uint32_t patch_flags;     /* fhb_PatchFlags: which dn_* fields are valid */
    uint32_t dn_type, dn_task, dn_lock, dn_handler;
    uint32_t dn_stack, dn_pri, dn_startup, dn_globalvec;
    uint32_t seg_len;         /* bytes of the hunk image in seg_data */
    uint8_t *seg_data;        /* heap-owned hunk image (rdb_model_free releases) */
    char     name[RDB_NAME_LEN]; /* display name, e.g. "PFSFileSystem" */
    uint8_t  source;          /* RDB_FS_* */
} RdbFileSys;
```

In the `RdbModel` struct (lines 35-43), add before the closing brace:

```c
    RdbFileSys   fs[RDB_MAX_FS];
    int          num_fs;
```

In the error enum (lines 55-65), add a new member:

```c
    RDB_ERR_RANGE    = -8,
    RDB_ERR_NO_RDB_SPACE = -9
```
(add the comma after `RDB_ERR_RANGE`.)

After the `rdb_parse` prototype (line 146) add:

```c
/* Release all heap owned by the model (each fs[].seg_data) and zero num_fs.
   Safe to call repeatedly and on a zero-initialized model. */
void rdb_model_free(RdbModel *m);

/* Number of 512-byte LSEG blocks needed to store seg_len bytes
   (492 payload bytes per LSEG block; 0 bytes -> 0 blocks). */
uint32_t rdb_lseg_block_count(uint32_t seg_len);
```

- [ ] **Step 2: Verify it compiles (header only, via existing tests)**

Run: `./tests/run-host-tests.sh`
Expected: PASS (header change is additive; existing tests still build and pass).

- [ ] **Step 3: Commit**

```bash
git add src/rdb.h
git commit -m "feat(rdb): model types + constants for embedded filesystems

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 2: LSEG block-count helper + model teardown

**Files:**
- Modify: `src/rdb.c`
- Test: `tests/test_fshd.c` (create)
- Modify: `tests/run-host-tests.sh`

**Interfaces:**
- Consumes: `rdb_lseg_block_count`, `rdb_model_free` prototypes (Task 1).
- Produces: implementations; the `LSEG_PAYLOAD` constant (492); the alloc shim `rdb_alloc`/`rdb_free`.

- [ ] **Step 1: Write the failing test**

Create `tests/test_fshd.c`:

```c
/* Host unit tests for FSHD/LSEG embedded-filesystem support in rdb.c. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/rdb.h"

static int tests_run, tests_failed;
#define CHECK(c) do { tests_run++; if(!(c)){ tests_failed++; \
    printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c);} } while(0)

static void test_lseg_block_count(void)
{
    CHECK(rdb_lseg_block_count(0) == 0);
    CHECK(rdb_lseg_block_count(1) == 1);
    CHECK(rdb_lseg_block_count(492) == 1);
    CHECK(rdb_lseg_block_count(493) == 2);
    CHECK(rdb_lseg_block_count(984) == 2);
    CHECK(rdb_lseg_block_count(985) == 3);
}

static void test_model_free_safe(void)
{
    RdbModel m;
    memset(&m, 0, sizeof m);
    rdb_model_free(&m);            /* zero model: must not crash */
    CHECK(m.num_fs == 0);
}

int main(void)
{
    test_lseg_block_count();
    test_model_free_safe();
    printf("test_fshd: %d run, %d failed\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
```

- [ ] **Step 2: Wire the test into the runner and run it to see it fail**

In `tests/run-host-tests.sh`, after the `test_rdb.c` line (line 5) add:

```sh
cc -std=c99 -Wall -Wextra -g -o /tmp/hdpart_fshd tests/test_fshd.c src/rdb.c
/tmp/hdpart_fshd
```

Run: `./tests/run-host-tests.sh`
Expected: FAIL — link error `undefined reference to rdb_lseg_block_count` / `rdb_model_free`.

- [ ] **Step 3: Implement the helper, the alloc shim and teardown**

In `src/rdb.c`, near the top after the existing includes add the shim and constant:

```c
#define LSEG_PAYLOAD 492u   /* raw hunk bytes carried per LSEG block */

#ifdef HDPART_AMIGA
#include <exec/memory.h>
#include <proto/exec.h>
static void *rdb_alloc(uint32_t n) { return AllocMem(n, MEMF_PUBLIC | MEMF_CLEAR); }
/* Amiga FreeMem needs the size; we store it in a header longword. */
static void *rdb_alloc_sized(uint32_t n) {
    uint32_t *p = (uint32_t *)AllocMem(n + 4, MEMF_PUBLIC | MEMF_CLEAR);
    if (!p) return 0; p[0] = n + 4; return p + 1;
}
static void rdb_free_sized(void *p) {
    if (!p) return; uint32_t *h = (uint32_t *)p - 1; FreeMem(h, h[0]);
}
#else
#include <stdlib.h>
static void *rdb_alloc_sized(uint32_t n) { return calloc(1, n ? n : 1); }
static void rdb_free_sized(void *p) { free(p); }
#endif
```

Then add the two public functions (place them after `rdb_init_model`):

```c
uint32_t rdb_lseg_block_count(uint32_t seg_len)
{
    return (seg_len + LSEG_PAYLOAD - 1u) / LSEG_PAYLOAD;
}

void rdb_model_free(RdbModel *m)
{
    int i;
    if (!m) return;
    for (i = 0; i < m->num_fs; i++) {
        if (m->fs[i].seg_data) { rdb_free_sized(m->fs[i].seg_data); m->fs[i].seg_data = 0; }
    }
    m->num_fs = 0;
}
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `./tests/run-host-tests.sh`
Expected: PASS — `test_fshd: 8 run, 0 failed` plus all existing suites pass.

- [ ] **Step 5: Commit**

```bash
git add src/rdb.c tests/test_fshd.c tests/run-host-tests.sh
git commit -m "feat(rdb): LSEG block-count helper, alloc shim, model teardown

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 3: Serialize FSHD/LSEG with linear allocator + capacity guard

**Files:**
- Modify: `src/rdb.c` (the `rdb_serialize` function and new static writers)
- Test: `tests/test_fshd.c`

**Interfaces:**
- Consumes: `rdb_lseg_block_count`, `LSEG_PAYLOAD`, existing `rdb_set_checksum`, `be_put32`, the `BlockIO` callback, `rdb_validate`.
- Produces: FSHD/LSEG on-disk blocks; `RDB_o_FileSysHdr` wired to the first FSHD; `HighRDSKBlock` set to last used block; `RDB_ERR_NO_RDB_SPACE` when over capacity.

- [ ] **Step 1: Write the failing test (round-trip will be completed in Task 4; here assert write succeeds and capacity guard fires)**

Add to `tests/test_fshd.c` a RAM-disk BlockIO and tests. Put this above `main` and call the new tests from `main`:

```c
/* Simple RAM block device for tests: 4096 blocks of 512 bytes. */
#define RAM_BLOCKS 4096
typedef struct { uint8_t b[RAM_BLOCKS][512]; } RamDisk;
static int ram_io(void *ctx, uint32_t blk, uint8_t *buf, int write)
{
    RamDisk *d = (RamDisk *)ctx;
    if (blk >= RAM_BLOCKS) return 1;
    if (write) memcpy(d->b[blk], buf, 512);
    else       memcpy(buf, d->b[blk], 512);
    return 0;
}

/* Build a tiny fake hunk image of n bytes with the HUNK_HEADER magic. */
static uint8_t *fake_seg(uint32_t n)
{
    uint8_t *p = (uint8_t *)calloc(1, n < 4 ? 4 : n);
    p[0]=0x00; p[1]=0x00; p[2]=0x03; p[3]=0xF3;   /* HUNK_HEADER */
    { uint32_t i; for (i = 4; i < n; i++) p[i] = (uint8_t)(i * 7u + 1u); }
    return p;
}

static void test_serialize_with_fs(void)
{
    RamDisk *d = (RamDisk *)calloc(1, sizeof *d);
    RdbModel m; memset(&m, 0, sizeof m);
    rdb_init_model(&m, 100, 16, 63);
    CHECK(rdb_add_partition(&m, "DH0", 20, RDB_DOSTYPE_FFS_INTL) == RDB_OK);

    m.num_fs = 1;
    m.fs[0].dos_type = 0x50465303u;       /* PFS\3 */
    m.fs[0].version  = (53u << 16) | 3u;
    m.fs[0].seg_len  = 1000;              /* -> 3 LSEG blocks */
    m.fs[0].seg_data = fake_seg(1000);
    strcpy(m.fs[0].name, "PFSFileSystem");

    CHECK(rdb_serialize(&m, ram_io, d) == RDB_OK);

    /* RDSK FileSysHdr (offset 32) must point at a block whose ID is 'FSHD'. */
    {
        uint8_t blk[512]; uint32_t fshd;
        ram_io(d, 0, blk, 0);
        fshd = (blk[32]<<24)|(blk[33]<<16)|(blk[34]<<8)|blk[35];
        CHECK(fshd != 0xFFFFFFFFu);
        ram_io(d, fshd, blk, 0);
        CHECK(blk[0]=='F' && blk[1]=='S' && blk[2]=='H' && blk[3]=='D');
    }
    free(m.fs[0].seg_data); free(d);
}

static void test_capacity_guard(void)
{
    RamDisk *d = (RamDisk *)calloc(1, sizeof *d);
    RdbModel m; memset(&m, 0, sizeof m);
    /* 2 reserved cyls * (4 heads * 4 sectors) = 32 reserved blocks: tiny. */
    rdb_init_model(&m, 50, 4, 4);
    m.num_fs = 1;
    m.fs[0].dos_type = 0x50465303u;
    m.fs[0].seg_len  = 200000;            /* needs ~407 LSEG blocks, won't fit */
    m.fs[0].seg_data = fake_seg(200000);
    CHECK(rdb_serialize(&m, ram_io, d) == RDB_ERR_NO_RDB_SPACE);
    free(m.fs[0].seg_data); free(d);
}
```

Add to `main`: `test_serialize_with_fs(); test_capacity_guard();`

- [ ] **Step 2: Run to verify it fails**

Run: `./tests/run-host-tests.sh`
Expected: FAIL — current `rdb_serialize` writes `RDB_o_FileSysHdr = NULLPTR`, so the FSHD-pointer assertion fails (and the capacity guard returns `RDB_OK`).

- [ ] **Step 3: Implement the writers + allocator in `rdb_serialize`**

In `src/rdb.c`, add the FSHD/LSEG block IDs near the existing `ID_RDSK`/`ID_PART` defines:

```c
#define ID_FSHD 0x46534844u
#define ID_LSEG 0x4C534547u
#define FSHD_SUMMEDLONGS 64u
#define LSEG_SUMMEDLONGS 128u
```

Add static writers (above `rdb_serialize`):

```c
/* FSHD field offsets within the 512-byte block. */
#define FSHD_o_Next       16
#define FSHD_o_Flags      20
#define FSHD_o_DosType    32
#define FSHD_o_Version    36
#define FSHD_o_PatchFlags 40
#define FSHD_o_Type       44   /* dn_Type */
#define FSHD_o_Task       48
#define FSHD_o_Lock       52
#define FSHD_o_Handler    56
#define FSHD_o_StackSize  60
#define FSHD_o_Priority   64
#define FSHD_o_Startup    68
#define FSHD_o_SegList    72   /* first LSEG block# */
#define FSHD_o_GlobalVec  76
#define LSEG_o_Next       16
#define LSEG_o_LoadData   20

static void write_fshd_block(uint8_t *blk, const RdbFileSys *fs,
                             uint32_t next_blk, uint32_t first_lseg)
{
    memset(blk, 0, 512);
    be_put32(blk + 0,  ID_FSHD);
    be_put32(blk + 4,  FSHD_SUMMEDLONGS);
    be_put32(blk + 12, 7u);                 /* HostID */
    be_put32(blk + FSHD_o_Next,      next_blk);
    be_put32(blk + FSHD_o_Flags,     0u);
    be_put32(blk + FSHD_o_DosType,   fs->dos_type);
    be_put32(blk + FSHD_o_Version,   fs->version);
    be_put32(blk + FSHD_o_PatchFlags,fs->patch_flags);
    be_put32(blk + FSHD_o_Type,      fs->dn_type);
    be_put32(blk + FSHD_o_Task,      fs->dn_task);
    be_put32(blk + FSHD_o_Lock,      fs->dn_lock);
    be_put32(blk + FSHD_o_Handler,   fs->dn_handler);
    be_put32(blk + FSHD_o_StackSize, fs->dn_stack);
    be_put32(blk + FSHD_o_Priority,  fs->dn_pri);
    be_put32(blk + FSHD_o_Startup,   fs->dn_startup);
    be_put32(blk + FSHD_o_SegList,   first_lseg);
    be_put32(blk + FSHD_o_GlobalVec, fs->dn_globalvec);
    rdb_set_checksum(blk, FSHD_SUMMEDLONGS, 8);
}

static void write_lseg_block(uint8_t *blk, const uint8_t *data, uint32_t len,
                             uint32_t next_blk)
{
    memset(blk, 0, 512);
    be_put32(blk + 0,  ID_LSEG);
    be_put32(blk + 4,  LSEG_SUMMEDLONGS);
    be_put32(blk + 12, 7u);
    be_put32(blk + LSEG_o_Next, next_blk);
    if (len > LSEG_PAYLOAD) len = LSEG_PAYLOAD;
    memcpy(blk + LSEG_o_LoadData, data, len);
    rdb_set_checksum(blk, LSEG_SUMMEDLONGS, 8);
}
```

Now in `rdb_serialize`, replace the fixed layout. After the existing PART-block writing loop (which uses blocks `1..num_parts`), insert FSHD/LSEG writing. The allocator's next free block starts at `1 + num_parts`. Replace the final `HighRDSKBlock` handling and the hardcoded `RDB_o_FileSysHdr` NULL:

```c
    /* Capacity check + linear allocation for FSHD/LSEG, after PART blocks. */
    {
        uint32_t next = 1u + (uint32_t)m->num_parts;       /* first free block */
        uint32_t reserved_hi = m->rdb_blocks_hi;           /* inclusive */
        uint32_t total = next, i;
        for (i = 0; i < (uint32_t)m->num_fs; i++)
            total += 1u + rdb_lseg_block_count(m->fs[i].seg_len);  /* FSHD + LSEGs */
        if (m->num_fs > 0 && total - 1u > reserved_hi)
            return RDB_ERR_NO_RDB_SPACE;
    }
```

Place that check *before* writing block 0 so nothing is written on failure. When writing the RDSK block, set the FileSysHdr pointer to the first FSHD (or `NULLPTR` if `num_fs==0`):

```c
    be_put32(blk + RDB_o_FileSysHdr,
             m->num_fs > 0 ? (1u + (uint32_t)m->num_parts) : NULLPTR);
```

After the PART loop, write the FSHD/LSEG chains:

```c
    {
        uint32_t cur = 1u + (uint32_t)m->num_parts;   /* current FSHD block */
        uint32_t i;
        for (i = 0; i < (uint32_t)m->num_fs; i++) {
            const RdbFileSys *fs = &m->fs[i];
            uint32_t nseg = rdb_lseg_block_count(fs->seg_len);
            uint32_t first_lseg = nseg ? cur + 1u : NULLPTR;
            uint32_t fshd_blk = cur;
            uint32_t next_fshd;
            uint32_t s, off = 0;
            cur += 1u + nseg;                          /* advance past this FSHD+LSEGs */
            next_fshd = (i + 1u < (uint32_t)m->num_fs) ? cur : NULLPTR;
            write_fshd_block(blk, fs, next_fshd, first_lseg);
            if (io(ctx, fshd_blk, blk, 1)) return RDB_ERR_IO;
            for (s = 0; s < nseg; s++) {
                uint32_t lseg_blk = fshd_blk + 1u + s;
                uint32_t next_lseg = (s + 1u < nseg) ? lseg_blk + 1u : NULLPTR;
                uint32_t chunk = fs->seg_len - off;
                if (chunk > LSEG_PAYLOAD) chunk = LSEG_PAYLOAD;
                write_lseg_block(blk, fs->seg_data + off, chunk, next_lseg);
                if (io(ctx, lseg_blk, blk, 1)) return RDB_ERR_IO;
                off += chunk;
            }
        }
        /* update HighRDSKBlock: rewrite block 0's field if needed, or compute it
           before writing block 0. Simplest: compute last-used before block 0. */
    }
```

To keep HighRDSKBlock correct, compute the last used block *before* writing block 0 and store it in a local `high_rdsk` used where `RDB_o_HighRDSKBlock` is set:

```c
    uint32_t high_rdsk = (uint32_t)m->num_parts;   /* default (parts only) */
    if (m->num_fs > 0) {
        uint32_t t = 1u + (uint32_t)m->num_parts, i;
        for (i = 0; i < (uint32_t)m->num_fs; i++)
            t += 1u + rdb_lseg_block_count(m->fs[i].seg_len);
        high_rdsk = t - 1u;
    }
    /* ... be_put32(blk + RDB_o_HighRDSKBlock, high_rdsk); */
```

- [ ] **Step 4: Run to verify pass**

Run: `./tests/run-host-tests.sh`
Expected: PASS — `test_serialize_with_fs` and `test_capacity_guard` green; all existing suites green.

- [ ] **Step 5: Commit**

```bash
git add src/rdb.c tests/test_fshd.c
git commit -m "feat(rdb): serialize FSHD/LSEG with linear allocator + capacity guard

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 4: Parse FSHD/LSEG and round-trip

**Files:**
- Modify: `src/rdb.c` (the `rdb_parse` function + static readers)
- Test: `tests/test_fshd.c`

**Interfaces:**
- Consumes: the serialized FSHD/LSEG from Task 3; `be_get32`, `rdb_checksum_ok`, `rdb_alloc_sized`, `LSEG_PAYLOAD`.
- Produces: populated `m->fs[]`/`m->num_fs` after `rdb_parse`; a full serialize→parse round-trip.

- [ ] **Step 1: Write the failing round-trip test**

Add to `tests/test_fshd.c`:

```c
static void test_roundtrip(void)
{
    RamDisk *d = (RamDisk *)calloc(1, sizeof *d);
    RdbModel m; memset(&m, 0, sizeof m);
    rdb_init_model(&m, 100, 16, 63);
    CHECK(rdb_add_partition(&m, "DH0", 20, RDB_DOSTYPE_FFS_INTL) == RDB_OK);
    m.num_fs = 1;
    m.fs[0].dos_type = 0x50465303u;
    m.fs[0].version  = (53u << 16) | 3u;
    m.fs[0].seg_len  = 1000;
    m.fs[0].seg_data = fake_seg(1000);
    strcpy(m.fs[0].name, "PFSFileSystem");
    CHECK(rdb_serialize(&m, ram_io, d) == RDB_OK);

    RdbModel r; memset(&r, 0, sizeof r);
    CHECK(rdb_parse(&r, ram_io, d) == RDB_OK);
    CHECK(r.num_fs == 1);
    CHECK(r.fs[0].dos_type == 0x50465303u);
    CHECK(r.fs[0].version  == ((53u<<16)|3u));
    CHECK(r.fs[0].seg_len  == 1000);
    CHECK(r.fs[0].seg_data != 0);
    {
        uint8_t *orig = fake_seg(1000);
        CHECK(memcmp(r.fs[0].seg_data, orig, 1000) == 0);  /* bytes preserved */
        free(orig);
    }
    rdb_model_free(&r);
    free(m.fs[0].seg_data); free(d);
}
```

Add `test_roundtrip();` to `main`.

- [ ] **Step 2: Run to verify it fails**

Run: `./tests/run-host-tests.sh`
Expected: FAIL — `r.num_fs == 1` fails (parse doesn't read FSHD yet; it stays 0).

- [ ] **Step 3: Implement the FSHD/LSEG readers in `rdb_parse`**

In `src/rdb.c`, add a static reader (above `rdb_parse`):

```c
/* Read the FSHD chain starting at block `fshd_blk` into m->fs[]. Returns RDB_OK
   or RDB_ERR_IO. Silently stops at RDB_MAX_FS or a bad/zero pointer. */
static int read_fshd_chain(RdbModel *m, BlockIO io, void *ctx, uint32_t fshd_blk)
{
    uint8_t blk[512];
    int guard = 0;
    while (fshd_blk != NULLPTR && fshd_blk != 0 && m->num_fs < RDB_MAX_FS
           && ++guard < 64) {
        RdbFileSys *fs = &m->fs[m->num_fs];
        uint32_t seg_blk, seg_len = 0, cap, off = 0;
        int sguard = 0;
        if (io(ctx, fshd_blk, blk, 0)) return RDB_ERR_IO;
        if (be_get32(blk + 0) != ID_FSHD ||
            !rdb_checksum_ok(blk, FSHD_SUMMEDLONGS)) break;
        memset(fs, 0, sizeof *fs);
        fs->dos_type     = be_get32(blk + FSHD_o_DosType);
        fs->version      = be_get32(blk + FSHD_o_Version);
        fs->patch_flags  = be_get32(blk + FSHD_o_PatchFlags);
        fs->dn_type      = be_get32(blk + FSHD_o_Type);
        fs->dn_task      = be_get32(blk + FSHD_o_Task);
        fs->dn_lock      = be_get32(blk + FSHD_o_Lock);
        fs->dn_handler   = be_get32(blk + FSHD_o_Handler);
        fs->dn_stack     = be_get32(blk + FSHD_o_StackSize);
        fs->dn_pri       = be_get32(blk + FSHD_o_Priority);
        fs->dn_startup   = be_get32(blk + FSHD_o_Startup);
        fs->dn_globalvec = be_get32(blk + FSHD_o_GlobalVec);
        fs->source       = RDB_FS_EMBEDDED;
        dostype_name(fs->name, fs->dos_type);   /* readable default name */

        /* First pass: count LSEG payload bytes. */
        seg_blk = be_get32(blk + FSHD_o_SegList);
        { uint32_t b = seg_blk; int g = 0;
          while (b != NULLPTR && b != 0 && ++g < 4096) {
            uint8_t lb[512];
            if (io(ctx, b, lb, 0)) return RDB_ERR_IO;
            if (be_get32(lb + 0) != ID_LSEG) break;
            seg_len += LSEG_PAYLOAD;
            b = be_get32(lb + LSEG_o_Next);
          } }
        cap = seg_len;
        fs->seg_data = (uint8_t *)rdb_alloc_sized(cap ? cap : 1);
        if (!fs->seg_data) return RDB_ERR_IO;
        /* Second pass: copy payloads. */
        { uint32_t b = seg_blk;
          while (b != NULLPTR && b != 0 && ++sguard < 4096 && off < cap) {
            uint8_t lb[512];
            if (io(ctx, b, lb, 0)) return RDB_ERR_IO;
            if (be_get32(lb + 0) != ID_LSEG) break;
            memcpy(fs->seg_data + off, lb + LSEG_o_LoadData, LSEG_PAYLOAD);
            off += LSEG_PAYLOAD;
            b = be_get32(lb + LSEG_o_Next);
          } }
        fs->seg_len = off;
        m->num_fs++;
        fshd_blk = be_get32(blk + FSHD_o_Next);
    }
    return RDB_OK;
}
```

Add a small name helper if one is not already present in `rdb.c` (mirror `gui.c`'s `dostype_label`, but keep it local to rdb.c and pure):

```c
/* Printable 3 chars + version byte, e.g. "PFS\3"; hex fallback. out >= 16. */
static void dostype_name(char *out, uint32_t t)
{
    unsigned char c0=(t>>24)&0xff, c1=(t>>16)&0xff, c2=(t>>8)&0xff, v=t&0xff;
    if (c0>=32&&c0<127&&c1>=32&&c1<127&&c2>=32&&c2<127) {
        out[0]=c0; out[1]=c1; out[2]=c2; out[3]='\\';
        out[4]=(char)('0'+(v<=9?v:0)); out[5]=0;
    } else {
        static const char *h="0123456789ABCDEF"; int i;
        out[0]='0'; out[1]='x';
        for (i=0;i<8;i++) out[2+i]=h[(t>>((7-i)*4))&0xf];
        out[10]=0;
    }
}
```
(Define `dostype_name` above `read_fshd_chain`.)

In `rdb_parse`, after the PART-chain walk completes and before `return RDB_OK`, add:

```c
    {
        uint32_t fshd = be_get32(blk_rdsk + RDB_o_FileSysHdr);
        /* blk_rdsk is the saved RDSK block buffer; if rdb_parse overwrote `blk`
           during the PART walk, re-read block 0 into a local buffer first. */
        int rc = read_fshd_chain(m, io, ctx, fshd);
        if (rc != RDB_OK) return rc;
    }
```

NOTE: in the current `rdb_parse`, the RDSK fields are read into `blk` then the PART walk reuses `blk`. Capture the FileSysHdr pointer into a local `uint32_t fshd_ptr` at the same place geometry is read (right after the RDSK is validated), then call `read_fshd_chain(m, io, ctx, fshd_ptr)` at the end. Do not rely on `blk` still holding the RDSK.

- [ ] **Step 4: Run to verify pass**

Run: `./tests/run-host-tests.sh`
Expected: PASS — `test_roundtrip` green; existing suites green.

- [ ] **Step 5: Commit**

```bash
git add src/rdb.c tests/test_fshd.c
git commit -m "feat(rdb): parse FSHD/LSEG chain; full serialize/parse round-trip

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 5: Preservation regression + rdbtool cross-check

**Files:**
- Test: `tests/test_fshd.c`
- Modify: `tests/verify-rdbtool.sh`

**Interfaces:**
- Consumes: serialize+parse from Tasks 3-4.

- [ ] **Step 1: Write the preservation test (the silent-drop bug)**

Add to `tests/test_fshd.c`:

```c
static void test_preserve_on_resave(void)
{
    /* A disk with an embedded FS, parsed and re-saved, keeps the FS. */
    RamDisk *d = (RamDisk *)calloc(1, sizeof *d);
    RdbModel m; memset(&m, 0, sizeof m);
    rdb_init_model(&m, 100, 16, 63);
    CHECK(rdb_add_partition(&m, "DH0", 20, RDB_DOSTYPE_FFS_INTL) == RDB_OK);
    m.num_fs = 1; m.fs[0].dos_type = 0x50465303u; m.fs[0].seg_len = 600;
    m.fs[0].seg_data = fake_seg(600);
    CHECK(rdb_serialize(&m, ram_io, d) == RDB_OK);

    RdbModel r; memset(&r, 0, sizeof r);
    CHECK(rdb_parse(&r, ram_io, d) == RDB_OK);
    CHECK(r.num_fs == 1);
    /* re-save the parsed model to a second disk, parse again */
    RamDisk *d2 = (RamDisk *)calloc(1, sizeof *d2);
    CHECK(rdb_serialize(&r, ram_io, d2) == RDB_OK);
    RdbModel r2; memset(&r2, 0, sizeof r2);
    CHECK(rdb_parse(&r2, ram_io, d2) == RDB_OK);
    CHECK(r2.num_fs == 1);
    CHECK(r2.fs[0].seg_len == 600);
    rdb_model_free(&r); rdb_model_free(&r2);
    free(m.fs[0].seg_data); free(d); free(d2);
}
```

Add `test_preserve_on_resave();` to `main`.

- [ ] **Step 2: Run to verify pass**

Run: `./tests/run-host-tests.sh`
Expected: PASS.

- [ ] **Step 3: Extend the rdbtool cross-check**

Read `tests/verify-rdbtool.sh` first to match its style. Append a section that: writes a small RDB with an embedded FS using a tiny host helper (or reuse the test's RAM disk dumped to a file), then runs `rdbtool <img> fsget` / `rdbtool <img> list` and greps for the embedded FS DosType. If `rdbtool`'s exact subcommand differs, use `rdbtool <img> list filesystems`. The check passes if rdbtool reports a filesystem with the expected DosType and size.

Run: `./tests/verify-rdbtool.sh`
Expected: PASS (rdbtool sees the embedded FS).

- [ ] **Step 4: Commit**

```bash
git add tests/test_fshd.c tests/verify-rdbtool.sh
git commit -m "test(rdb): FSHD preservation regression + rdbtool cross-check

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## PHASE 2 — FS ACQUISITION (`fsload`)

### Task 6: `fsload` module skeleton + error text + pure validation helper

**Files:**
- Create: `src/fsload.h`, `src/fsload.c`
- Test: extend `tests/test_fshd.c` (pure helper only) OR a new `tests/test_fsload.c` for the pure parts.

**Interfaces:**
- Produces: `enum { FSL_OK, FSL_EOPEN, FSL_EREAD, FSL_ETOOBIG, FSL_ENOTLOADFILE, FSL_ENOMEM, FSL_EFULL, FSL_ENOFS }`; `const char *fsl_err_text(int)`; `int fsload_is_hunk_file(const uint8_t *buf, uint32_t len)`.

- [ ] **Step 1: Create the header**

Create `src/fsload.h`:

```c
#ifndef HDPART_FSLOAD_H
#define HDPART_FSLOAD_H
#include <stdint.h>
#include "rdb.h"

enum {
    FSL_OK = 0,
    FSL_EOPEN = -1,        /* could not open the file */
    FSL_EREAD = -2,        /* read error */
    FSL_ETOOBIG = -3,      /* exceeds FSLOAD_MAX_BYTES */
    FSL_ENOTLOADFILE = -4, /* not an AmigaDOS hunk load file */
    FSL_ENOMEM = -5,
    FSL_EFULL = -6,        /* model already has RDB_MAX_FS filesystems */
    FSL_ENOFS = -7         /* source disk has no embedded filesystem */
};

#define FSLOAD_MAX_BYTES (512u * 1024u)
#define HUNK_HEADER 0x000003F3u

const char *fsl_err_text(int rc);

/* Pure: 1 if buf starts with HUNK_HEADER and len is plausible, else 0. */
int fsload_is_hunk_file(const uint8_t *buf, uint32_t len);

#ifdef HDPART_AMIGA
/* Load a filesystem handler from a file into out (out->seg_data heap-owned).
   dos_type is left as a placeholder (set later in the GUI). */
int fsload_from_file(const char *path, RdbFileSys *out);

/* Copy an embedded FS from another disk's RDB. Opens driver/unit transiently,
   parses, copies fs index `which` into out. */
int fsload_from_disk(const char *driver, uint32_t unit, int which, RdbFileSys *out);
#endif

#endif
```

- [ ] **Step 2: Write the failing pure-helper test**

Add to `tests/test_fshd.c` (it already links rdb.c; add fsload.c to its compile line in Step 4):

```c
#include "../src/fsload.h"
static void test_is_hunk_file(void)
{
    uint8_t good[8] = {0x00,0x00,0x03,0xF3, 0,0,0,0};
    uint8_t bad[8]  = {0xDE,0xAD,0xBE,0xEF, 0,0,0,0};
    CHECK(fsload_is_hunk_file(good, 8) == 1);
    CHECK(fsload_is_hunk_file(bad, 8) == 0);
    CHECK(fsload_is_hunk_file(good, 3) == 0);   /* too short */
    CHECK(fsl_err_text(FSL_ENOTLOADFILE) != 0);
}
```

Add `test_is_hunk_file();` to `main`.

- [ ] **Step 3: Run to see it fail**

Update the `test_fshd` compile line in `tests/run-host-tests.sh` to include `src/fsload.c`:
```sh
cc -std=c99 -Wall -Wextra -g -o /tmp/hdpart_fshd tests/test_fshd.c src/rdb.c src/fsload.c
```
Run: `./tests/run-host-tests.sh`
Expected: FAIL — `fsload.c` does not exist / undefined `fsload_is_hunk_file`.

- [ ] **Step 4: Implement the pure parts of `fsload.c`**

Create `src/fsload.c` with the host-safe (non-Amiga) parts first:

```c
#include "fsload.h"
#include <string.h>

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
}

int fsload_is_hunk_file(const uint8_t *buf, uint32_t len)
{
    if (!buf || len < 4) return 0;
    return be32(buf) == HUNK_HEADER ? 1 : 0;
}

const char *fsl_err_text(int rc)
{
    switch (rc) {
    case FSL_OK:           return "OK";
    case FSL_EOPEN:        return "Could not open the file.";
    case FSL_EREAD:        return "Error reading the file.";
    case FSL_ETOOBIG:      return "File is too large for an RDB filesystem.";
    case FSL_ENOTLOADFILE: return "Not an AmigaDOS filesystem (no HUNK header).";
    case FSL_ENOMEM:       return "Out of memory.";
    case FSL_EFULL:        return "Filesystem list is full.";
    case FSL_ENOFS:        return "That disk has no embedded filesystem.";
    default:               return "Unknown error.";
    }
}
```

(Amiga-only `fsload_from_file`/`fsload_from_disk` come in Tasks 7-8, guarded by `#ifdef HDPART_AMIGA`.)

- [ ] **Step 5: Run to verify pass**

Run: `./tests/run-host-tests.sh`
Expected: PASS — `test_is_hunk_file` green.

- [ ] **Step 6: Commit**

```bash
git add src/fsload.h src/fsload.c tests/test_fshd.c tests/run-host-tests.sh
git commit -m "feat(fsload): module skeleton, error text, hunk-file validation

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 7: `fsload_from_file` (Amiga)

**Files:**
- Modify: `src/fsload.c`

**Interfaces:**
- Consumes: `fsload_is_hunk_file`, `RdbFileSys`, dos.library `Open/Read/Close`, exec `AllocMem/FreeMem`.
- Produces: `int fsload_from_file(const char *path, RdbFileSys *out)`.

- [ ] **Step 1: Implement (Amiga-guarded)**

Add to `src/fsload.c` inside `#ifdef HDPART_AMIGA`:

```c
#ifdef HDPART_AMIGA
#include <proto/dos.h>
#include <proto/exec.h>
#include <exec/memory.h>
#include <dos/dos.h>

/* seg_data uses the same sized-alloc convention as rdb.c (4-byte size header).
   Keep these helpers identical to rdb.c's. */
static void *fsl_alloc(uint32_t n) {
    uint32_t *p = (uint32_t *)AllocMem(n + 4, MEMF_PUBLIC | MEMF_CLEAR);
    if (!p) return 0; p[0] = n + 4; return p + 1;
}

static void copy_name(char *dst, int sz, const char *src) {
    int i = 0; if (sz <= 0) return;
    for (; src && src[i] && i < sz - 1; i++) dst[i] = src[i];
    dst[i] = 0;
}

int fsload_from_file(const char *path, RdbFileSys *out)
{
    BPTR fh; LONG flen, got; uint8_t *buf; const char *fp;
    memset(out, 0, sizeof *out);

    fh = Open((CONST_STRPTR)path, MODE_OLDFILE);
    if (!fh) return FSL_EOPEN;
    Seek(fh, 0, OFFSET_END); flen = Seek(fh, 0, OFFSET_BEGINNING);
    /* Seek returns previous position; the second Seek's return is the end pos. */
    if (flen <= 0) { /* fall back: read incrementally below */ }
    /* Determine length via Seek to end then beginning. */
    Seek(fh, 0, OFFSET_END);
    flen = Seek(fh, 0, OFFSET_BEGINNING); /* now flen = file size */
    if (flen <= 0) { Close(fh); return FSL_EREAD; }
    if ((uint32_t)flen > FSLOAD_MAX_BYTES) { Close(fh); return FSL_ETOOBIG; }

    buf = (uint8_t *)fsl_alloc((uint32_t)flen);
    if (!buf) { Close(fh); return FSL_ENOMEM; }
    got = Read(fh, buf, flen);
    Close(fh);
    if (got != flen) { FreeMem((uint32_t*)buf - 1, ((uint32_t*)buf)[-1]); return FSL_EREAD; }

    if (!fsload_is_hunk_file(buf, (uint32_t)flen)) {
        FreeMem((uint32_t*)buf - 1, ((uint32_t*)buf)[-1]); return FSL_ENOTLOADFILE;
    }

    out->seg_data = buf;
    out->seg_len  = (uint32_t)flen;
    out->dos_type = 0x50465303u;             /* placeholder PFS\3; user sets it */
    out->version  = 0;
    out->patch_flags = 0x180u;               /* StackSize+Priority+GlobalVec bits */
    out->dn_stack = 4096; out->dn_pri = 10; out->dn_globalvec = (uint32_t)-1;
    out->source = RDB_FS_FILE;
    /* name = file part of path */
    fp = path; { const char *p = path; for (; *p; p++) if (*p=='/'||*p==':') fp=p+1; }
    copy_name(out->name, RDB_NAME_LEN, fp);
    return FSL_OK;
}
#endif
```

NOTE: confirm the exact `fhb_PatchFlags` bit values against `dos/filehandler.h` (`FSF_*`). The `0x180` is `FSB_STACKSIZE|FSB_PRIORITY|FSB_GLOBALVEC` placeholder — fix to the real macro OR-combination during implementation. Use the SDK header, do not guess.

- [ ] **Step 2: Build for target to verify it compiles**

```bash
export AMIGA_BIN="/Users/sfs/.vscode/extensions/bartmanabyss.amiga-debug-1.8.2/bin/darwin"
export PATH="$AMIGA_BIN/opt/bin:$AMIGA_BIN:$PATH"
make
```
Expected: compiles clean (fsload.c builds; no link errors — `fsload_from_file` is referenced once gui.c is wired, so until then it may be `-Wunused`; that is fine with `-Wno-unused-function`).

- [ ] **Step 3: Run host tests (no regression)**

Run: `./tests/run-host-tests.sh`
Expected: PASS (the `#ifdef HDPART_AMIGA` body is excluded from host builds).

- [ ] **Step 4: Commit**

```bash
git add src/fsload.c
git commit -m "feat(fsload): load filesystem handler from a file (Amiga)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 8: `fsload_from_disk` (Amiga, copy from another disk's RDB)

**Files:**
- Modify: `src/fsload.c`
- Reference: `src/device.c`, `src/discover.c` (transient open + block IO), `src/rdb.c` (`rdb_parse`).

**Interfaces:**
- Consumes: `device.c` open/close + a `BlockIO` that reads the chosen device; `rdb_parse`; `rdb_model_free`.
- Produces: `int fsload_from_disk(const char *driver, uint32_t unit, int which, RdbFileSys *out)`.

- [ ] **Step 1: Implement**

Inspect `src/device.c` / `src/discover.c` for the existing transient-open + `dev_block_io` pattern used by `discover_probe_driver`. Reuse it. Add to `src/fsload.c` inside `#ifdef HDPART_AMIGA`:

```c
int fsload_from_disk(const char *driver, uint32_t unit, int which, RdbFileSys *out)
{
    RdbModel scratch; int rc; RdbFileSys *src;
    /* dev_open / dev_block_io / dev_close are the device.c primitives used by
       discover.c — open the named driver+unit, parse its RDB into scratch. */
    DevHandle h;                                  /* type per device.h */
    memset(&scratch, 0, sizeof scratch);
    if (dev_open(&h, driver, unit) != 0) return FSL_EOPEN;
    rc = rdb_parse(&scratch, dev_block_io, &h);
    dev_close(&h);
    if (rc != RDB_OK) { rdb_model_free(&scratch); return FSL_ENOFS; }
    if (scratch.num_fs <= 0 || which < 0 || which >= scratch.num_fs) {
        rdb_model_free(&scratch); return FSL_ENOFS;
    }
    src = &scratch.fs[which];
    /* deep-copy into out, transferring ownership of a fresh seg_data buffer */
    memset(out, 0, sizeof *out);
    *out = *src;
    out->seg_data = (uint8_t *)fsl_alloc(src->seg_len ? src->seg_len : 1);
    if (!out->seg_data) { rdb_model_free(&scratch); return FSL_ENOMEM; }
    memcpy(out->seg_data, src->seg_data, src->seg_len);
    out->source = RDB_FS_COPIED;
    rdb_model_free(&scratch);                     /* frees scratch's seg_data copies */
    return FSL_OK;
}
```

NOTE: match the real `device.c` API names (`dev_open`/`dev_block_io`/`dev_close` and the handle type) — read `src/device.h` and copy the exact signatures used by `discover.c`. `discover.c` already does open→`rdb_parse`→close; mirror it exactly.

- [ ] **Step 2: Build for target**

Run: `make`
Expected: compiles clean.

- [ ] **Step 3: Host tests (no regression)**

Run: `./tests/run-host-tests.sh`
Expected: PASS.

- [ ] **Step 4: Commit**

```bash
git add src/fsload.c
git commit -m "feat(fsload): copy embedded filesystem from another disk's RDB

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## PHASE 3 — FORMAT: Strategy-3 embedded-FS binding

### Task 9: Thread the model into format + add embedded-FS binding

**Files:**
- Modify: `src/format.c`, `src/format.h`

**Interfaces:**
- Consumes: `RdbModel.fs[]`; dos.library `InternalLoadSeg`; existing `bind_handler` Strategies 1-2.
- Produces: `bind_handler` Strategy 3; `format_partition` signature already takes the model (`src/format.c` `format_partition(driver, unit, model, part_index, volname)` per the v0.5 spec) — confirm and use `model->fs[]`.

- [ ] **Step 1: Confirm the InternalLoadSeg ABI**

Read `m68k-amiga-elf/sys-include/proto/dos.h` and `inline/dos.h` (or `clib/dos_protos.h`) for `InternalLoadSeg`. Its signature is:
`BPTR InternalLoadSeg(BPTR fh, BPTR table, const LONG *funcarray, LONG *stack)`.
We will NOT use a file handle; instead use the `funcarray` read-hook form where `funcarray[0]` is a read function reading from our in-memory buffer. Confirm the V37 funcarray convention (read/alloc/free hooks) from the autodocs before writing the hook. Document the confirmed convention as a comment.

- [ ] **Step 2: Implement Strategy 3 in `bind_handler`**

In `src/format.c`, after Strategy 2 (FileSystem.resource) and before the final `return 0;`, add Strategy 3. `bind_handler` must receive the model (add a `const RdbModel *model` parameter; update its one caller in `format_partition`):

```c
    /* Strategy 3: reconstruct a seglist from an embedded FSHD/LSEG in this RDB. */
    if (model) {
        int i;
        for (i = 0; i < model->num_fs; i++) {
            const RdbFileSys *fs = &model->fs[i];
            if ((fs->dos_type & 0xFFFFFF00u) != fam) continue;
            if (!fs->seg_data || fs->seg_len == 0) continue;
            BPTR seg = embedded_loadseg(fs->seg_data, fs->seg_len);
            if (!seg) continue;
            dn->dn_SegList   = seg;
            dn->dn_GlobalVec = (BPTR)(fs->dn_globalvec ? fs->dn_globalvec : (uint32_t)-1);
            dn->dn_StackSize = fs->dn_stack ? fs->dn_stack : 4096;
            dn->dn_Priority  = fs->dn_pri   ? fs->dn_pri   : 10;
            return 1;
        }
    }
```

Add the `embedded_loadseg` helper (above `bind_handler`): a wrapper around `InternalLoadSeg` with an in-memory read hook over `(seg_data, seg_len)`. Implement the read-hook funcarray per the confirmed ABI from Step 1. The hook tracks an offset into `seg_data` and copies the requested byte count out. On success returns the BPTR seglist; never `UnLoadSeg` it (owned by the mount).

- [ ] **Step 3: Build for target**

Run: `make`
Expected: compiles clean.

- [ ] **Step 4: Host tests (format host tests still compile rdb.c + format.c; the new code is Amiga-guarded or model-driven)**

Ensure the Strategy-3 block and `embedded_loadseg` are inside `#ifdef HDPART_AMIGA` (InternalLoadSeg is OS-only). The `bind_handler` signature change is shared; update the host test `tests/test_format.c` call sites to pass `NULL` (or the model) so they still compile.

Run: `./tests/run-host-tests.sh`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/format.c src/format.h tests/test_format.c
git commit -m "feat(format): Strategy-3 embedded-FS binding via InternalLoadSeg

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## PHASE 4 — GUI

### Task 10: Free embedded FS on disk change + Edit FS-cycle reads the pool

**Files:**
- Modify: `src/gui.c`

**Interfaces:**
- Consumes: `g_model` (`RdbModel`), `rdb_model_free`, `model.fs[]`; existing `gui_edit_dialog` FS-cycle (`fsLabels`, `fsActive`, `fsKeepIdx`, DosType computation).
- Produces: FS-cycle that lists `FFS, OFS, <pool entries>, [keep]`; `g_model.fs[]` freed wherever the model is replaced.

- [ ] **Step 1: Free the model's FS pool wherever the model is replaced**

Find every place `g_model` is overwritten by a fresh parse/scan/refresh (e.g. `gui_refresh_current`, the Scan path, startup). Before re-parsing into `g_model`, call `rdb_model_free(&g_model)`. Also free on quit/cleanup. (Grep for `rdb_parse(&g_model` and `rdb_init_model(&g_model` and guard each.)

- [ ] **Step 2: Extend the Edit FS-cycle to include pool entries**

In `gui_edit_dialog`, where `fsLabels[]` is built (around the `FFS`/`OFS`/`keep` assembly), insert pool entries after `OFS` and before `keep`:

```c
    { int n = 0;
      fsLabels[n++] = "FFS";
      fsLabels[n++] = "OFS";
      /* user-loaded / embedded filesystems from the model pool */
      fsPoolBase = n;
      { int k; for (k = 0; k < g_model.num_fs && n < (int)(sizeof fsLabels/sizeof fsLabels[0]) - 2; k++) {
          fsLabels[n] = g_model.fs[k].name;   /* names persist in g_model */
          fsPoolIdx[n] = k;                    /* map label index -> fs[] index */
          n++;
      } }
      if (!fsRom) { dostype_label(fsKeep, pt->dos_type); fsKeepIdx = n; fsLabels[n++] = fsKeep; }
      else fsKeepIdx = -1;
      fsLabels[n] = 0; }
```

Add module/dialog locals `int fsPoolBase; int fsPoolIdx[16];`. When the FS cycle changes to a pool index (`fsIdx >= fsPoolBase && fsIdx != fsKeepIdx`), set the partition's computed dos_type to `g_model.fs[fsPoolIdx[fsIdx]].dos_type` and disable the Intl/Cache checkboxes (those apply only to `DOS\x`). In the Ok DosType computation, branch:

```c
    if (fsKeepIdx >= 0 && fsIdx == fsKeepIdx) {
        fdt = pt->dos_type;
    } else if (fsIdx >= fsPoolBase && (fsKeepIdx < 0 || fsIdx < fsKeepIdx)) {
        fdt = g_model.fs[fsPoolIdx[fsIdx]].dos_type;   /* embedded FS */
    } else {
        fdt = 0x444F5300u;
        if (fsIdx == 0) fdt |= 1u;
        if (gIntl->Flags & GFLG_SELECTED) fdt |= 2u;
        if (v39 && (gCache->Flags & GFLG_SELECTED)) fdt |= 4u;
    }
```

Set the initial `fsActive` to a matching pool entry if the partition's current `dos_type` equals one of the pool entries' types (so re-opening Edit shows the right selection).

- [ ] **Step 3: Build + stage + smoke test**

```bash
make && make hd
```
Expected: compiles; `make hd` stages. (Functional check happens in Task 12 on-target, after the dialog exists, but verify the Edit dialog still opens and the cycle shows FFS/OFS when the pool is empty.)

- [ ] **Step 4: Host tests (no regression)**

Run: `./tests/run-host-tests.sh`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/gui.c
git commit -m "feat(gui): Edit FS-cycle lists embedded filesystems; free pool on disk change

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 11: Filesystems dialog (list + Add from file + DosType + Remove + Done)

**Files:**
- Modify: `src/gui.c`

**Interfaces:**
- Consumes: `dlg_center/open/refresh/close`, GadTools listview/cycle/string/button creation patterns (mirror `gui_edit_dialog` and `gui_load_driver`), `fsload_from_file`, `fsl_err_text`, `rdb_model_free`/per-entry free, ASL requester (per `gui_load_driver`).
- Produces: `static void gui_filesystems(void)`; `Filesystems…` button + Disk-menu item (rA-Y); ghosting via `gui_update_buttons`/`gui_menu_enable`.

- [ ] **Step 1: Build the dialog**

Add `static void gui_filesystems(void)` modeled on `gui_edit_dialog`'s scaffolding:
- A listview gadget over a `char rows[RDB_MAX_FS][48]` built from `g_model.fs[]` (`> DosType  Name  Source  Size`), using the same `>`-marker selection approach as the partition list (`GTLV_Selected` is V39+, so track the selected index in a local and rebuild labels).
- A `DosType` string gadget (hex) + a preset CYCLE (`PFS\3`,`PDS\3`,`SFS\0`,`SFS\2`,`Custom`) that writes the selected row's `dos_type`.
- Buttons: `Add from file..`, `Copy from disk..` (Task 11 wires file; Copy is wired in this task's Step 2 to call into the Task-8 function via a small driver/unit sub-pick — if the sub-pick is large, defer Copy to its own commit), `Remove`, `Done`.
- `Add from file..` → replicate `gui_load_driver`'s ASL block (initial drawer `"L:"`, NO pattern filter, `ASLFR_Screen`), then:

```c
    rc = fsload_from_file(path, &g_model.fs[g_model.num_fs]);
    if (rc != FSL_OK) { gui_msg("Filesystems", fsl_err_text(rc)); }
    else { g_model.num_fs++; g_dirty = 1; /* rebuild listview labels */ }
```
  Guard `g_model.num_fs >= RDB_MAX_FS` → show `fsl_err_text(FSL_EFULL)` instead of loading.

- `Remove` → free the selected entry's `seg_data` (use the same sized-free as rdb.c — expose a small `rdb_free_fs(RdbFileSys*)` from rdb.c if needed), shift the array down, `g_dirty=1`, rebuild labels. If a partition uses that dos_type, `gui_request` confirm first.
- `Done` → close. Changes already in `g_model`, written on Save.

- [ ] **Step 2: Wire the entry points**

- Add a `Filesystems…` button in the Disk panel toolbar (mirror the Init/Load button creation). 
- Add a Disk-menu item `Filesystems` with `nm_CommKey="Y"` in `NewMenu[]` and an `IT_*`/`ID_*` enum; route MENUPICK + the button to `gui_filesystems()`.
- In `gui_update_buttons`, ghost the button and `gui_menu_enable` the menu item together, enabled only when `g_have_model`.

- [ ] **Step 3: Build + stage**

```bash
make && make hd
cmp -s out/HDPart.exe amiga_hd/HDPart && echo STAGED_OK
```
Expected: compiles; `STAGED_OK`.

- [ ] **Step 4: Host tests (no regression)**

Run: `./tests/run-host-tests.sh`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/gui.c src/rdb.c src/rdb.h
git commit -m "feat(gui): Filesystems dialog (list/add-from-file/dostype/remove)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## PHASE 5 — ON-TARGET VERIFICATION

### Task 12: On-target verification in FS-UAE

**Files:** none (manual verification); may add notes to README.

**Verification (run the devtest config — KS2.04/WB2.04 floppy + scratch hardfiles; or the KS3.x config for the live PFS handler):**

- [ ] **Step 1: Stage and launch**

```bash
make && make hd
make install-fsuae   # if config changes are needed
```
Launch the `HDPart-204-devtest` (or `HDPart` KS3.x) FS-UAE config; in a Shell run `HDPart:HDPart`.

- [ ] **Step 2: Add-from-file round-trip**

In HDPart: Scan a scratch disk → open Filesystems → Add from file → pick `L:FastFileSystem` (or a PFS3 handler if present) → set DosType (e.g. `PFS\3`) → Done → Save → Refresh (re-read from disk). Expected: the FS still appears in the Filesystems list (real-RDB parse round-trip). Cross-check on host: `rdbtool <scratch.hdf> list` shows the embedded FS.

- [ ] **Step 3: Format with embedded FS (the feature-3 gate)**

Create/Edit a partition, set its FS-cycle to the embedded `PFS\3`, Save, then Format it. Expected: it formats and mounts as a working volume of that type. On a system where the handler was NOT already live, this exercises Strategy 3 (`InternalLoadSeg` from the embedded LSEG). If auto-mount makes this ambiguous, use a scratch disk whose FS is genuinely absent from the running system.

- [ ] **Step 4: Copy-from-disk + preservation regression**

Copy an embedded FS from a second scratch disk into the target. Then Save a disk that already had an embedded FS and confirm it is preserved (not nulled) after Refresh.

- [ ] **Step 5: Commit any doc/screenshot updates**

```bash
git add README.md docs/screenshot.png 2>/dev/null || true
git commit -m "docs: embed-filesystem feature notes + screenshot" || true
```

---

## Self-Review

**Spec coverage:**
- §1 user capabilities → Tasks 10 (FS-cycle), 1-5/11 (embed), 9/12 (format) ✓
- §3 hunk-bytes-not-LoadSeg → Task 7 (read raw), Task 9 (InternalLoadSeg reconstruct) ✓
- §5 engine (model/parse/serialize/allocator/guard) → Tasks 1-5 ✓
- §6 acquisition (file + copy + DosType editing + errors) → Tasks 6-8, 11 ✓
- §7 Strategy 3 → Task 9 ✓
- §8 GUI (dialog, entry points, Edit-cycle) → Tasks 10-11 ✓
- §9 testing (host, rdbtool, on-target) → Tasks 2-5, 12 ✓
- §10 phasing → Phases 1-5 ✓
- §11 risks (InternalLoadSeg ABI, cold-test, DosType) → Tasks 9 Step 1, 12 Step 3, 11 ✓

**Placeholder scan:** the few "confirm against SDK header" notes (PatchFlags bits in Task 7, InternalLoadSeg funcarray ABI in Task 9, device.c API names in Task 8) are deliberate — they require reading the on-disk SDK/headers that aren't in this repo and must not be guessed. Each names the exact header to read.

**Type consistency:** `RdbFileSys`, `rdb_model_free`, `rdb_lseg_block_count`, `LSEG_PAYLOAD(492)`, `FSL_*`, `fsload_from_file/_from_disk`, `fsload_is_hunk_file`, `fsl_err_text`, `RDB_ERR_NO_RDB_SPACE`, `RDB_FS_EMBEDDED/_FILE/_COPIED` are used consistently across tasks. The sized-alloc convention (4-byte size header) is shared by `rdb.c` and `fsload.c` so `rdb_model_free` can release `seg_data` regardless of which module allocated it.
