# HDPart — Plan 1: Foundation + RDB Engine — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stand up the OS-application build (Makefile + startup + an Intuition window that opens and closes in FS-UAE), and build a fully unit-tested, pure-C Rigid Disk Block (RDB) engine that parses and serializes Amiga partition tables, cross-verified against `amitools rdbtool`.

**Architecture:** Two independent, independently-testable subsystems. (1) An AmigaOS hunk executable built with the Bartman `m68k-amiga-elf-gcc` toolchain using a minimal custom startup (no libc/crt0), opening OS libraries via the `proto/*` inline headers. (2) A pure-C RDB engine (`rdb.c`) that operates on raw 512-byte big-endian block buffers through a `BlockIO` callback, so it compiles and runs both on the host (for unit tests) and on the 68k. Endianness is handled explicitly with byte accessors, so host tests are valid regardless of host byte order.

**Tech Stack:** C (freestanding for Amiga, hosted for tests), `m68k-amiga-elf-gcc` 15.1.0, `elf2hunk`, GNU make, FS-UAE, host `cc` (clang), `amitools rdbtool`. AmigaOS APIs: exec, dos, intuition, gadtools (gadtools used in later plans).

---

## Conventions & environment

The toolchain is provided by the Bartman VSCode extension and is **not on the default PATH**. Every Amiga build/run command in this plan assumes this prelude (the Makefile already references `${command:amiga.bin-path}`; for shell use we export it):

```bash
export AMIGA_BIN="/Users/sfs/.vscode/extensions/bartmanabyss.amiga-debug-1.8.2/bin/darwin"
export PATH="$AMIGA_BIN/opt/bin:$AMIGA_BIN:$PATH"
```

Host tests use the system compiler (`cc`/clang) and need no Amiga toolchain.

`rdbtool` is at `/opt/homebrew/bin/rdbtool` (already on PATH).

Project root: `/Users/sfs/Devel/party` (git initialized; demo baseline already committed).

---

## File structure (created by this plan)

```
party/
  Makefile                 # MODIFIED: builds out/HDPart as an OS app (no -Ttext=0)
  src/
    startup.c              # NEW: _start entry, lib open/close, CLI/WB detection
    main.c                 # NEW: top-level; opens a window (smoke test), calls into engine later
    rdb.h                  # NEW: RDB engine public API + model types (portable)
    rdb.c                  # NEW: RDB engine implementation (pure C, no OS calls)
    endian.h              # NEW: big-endian byte accessors (header-only, static inline)
  tests/
    test_rdb.c            # NEW: host unit tests (assert-based, no framework)
    run-host-tests.sh     # NEW: compile + run host tests with system cc
    verify-rdbtool.sh     # NEW: emit an image with the engine, validate via rdbtool
```

Note: the original demo's `main.c` at the repo root is replaced by `src/main.c`. The demo `main.c` remains in git history (baseline commit). Delete the root `main.c` in Task 0.1 so the new Makefile's source globbing does not pick up the demo's hardware-banging code.

---

## Milestone 0 — OS-application foundation

Outcome: `make` produces `out/HDPart`, which when run in FS-UAE opens a titled window that closes on the close gadget and exits cleanly to CLI/Workbench.

### Task 0.1: New OS-app Makefile

**Files:**
- Modify: `Makefile` (replace whole file)
- Delete: `main.c` (root — the demo; preserved in git history)

- [ ] **Step 1: Remove the demo source from the build root**

Run:
```bash
cd /Users/sfs/Devel/party && git rm main.c
```
Expected: `rm 'main.c'`. (The demo's `bob.bpl`, `image.bpl`, `support/depacker_*`, `player610*` are left in place but will no longer be compiled, because the new Makefile only globs `src/`.)

- [ ] **Step 2: Write the new Makefile**

Replace `Makefile` with exactly:

```make
# HDPart - AmigaOS application Makefile (Bartman m68k-amiga-elf toolchain)
# Builds a normal AmigaDOS hunk executable (NOT a bare-metal demo).

ifdef OS
	WINDOWS = 1
	SHELL = cmd.exe
endif

CC    = m68k-amiga-elf-gcc
OUT   = out/HDPart

# Only compile our own sources under src/.
c_sources := $(wildcard src/*.c)
c_objects := $(addprefix obj/,$(patsubst src/%.c,%.o,$(c_sources)))

ifdef WINDOWS
	SDKDIR = $(abspath $(dir $(shell where $(CC)))..\m68k-amiga-elf\sys-include)
else
	SDKDIR = $(abspath $(dir $(shell which $(CC)))../m68k-amiga-elf/sys-include)
endif

# OS app: freestanding (no libc available) but a normal relocatable executable.
# No -Ttext=0 (that is for bare-metal). Provide our own _start entry.
CCFLAGS = -g -MP -MMD -m68000 -Os -nostdlib -fomit-frame-pointer \
          -Wall -Wextra -Wno-unused-function \
          -ffunction-sections -fdata-sections -Isrc
LDFLAGS = -nostdlib -Wl,-e,_start,--emit-relocs,--gc-sections,-Map=$(OUT).map

all: $(OUT)

$(OUT): $(OUT).elf
	$(info Elf2Hunk $@)
	@elf2hunk $(OUT).elf $(OUT)

$(OUT).elf: $(c_objects)
	$(info Linking $@)
	@$(CC) $(CCFLAGS) $(LDFLAGS) $(c_objects) -o $@

obj/%.o : src/%.c
	$(info Compiling $<)
	@$(CC) $(CCFLAGS) -c -o $@ $(CURDIR)/$<

clean:
	$(info Cleaning...)
ifdef WINDOWS
	@del /q obj\* out\*
else
	@$(RM) obj/* out/*
endif

-include $(c_objects:.o=.d)
```

- [ ] **Step 3: Ensure output dirs exist and are kept**

Run:
```bash
cd /Users/sfs/Devel/party && mkdir -p src obj out tests && touch obj/.keep out/.keep
```
Expected: no output; directories exist.

- [ ] **Step 4: Commit**

```bash
cd /Users/sfs/Devel/party
git add -A
git commit -m "build: OS-app Makefile for HDPart, drop demo main.c from build"
```

### Task 0.2: Minimal startup (entry, library open/close, CLI/WB detection)

**Files:**
- Create: `src/startup.c`

- [ ] **Step 1: Write `src/startup.c`**

```c
/* HDPart startup: freestanding entry point for an AmigaOS application.
 * No crt0/libc. Sets SysBase, opens dos.library, detects CLI vs Workbench
 * launch, calls hdpart_main(), then cleans up and returns a DOS rc. */
#include <exec/types.h>
#include <exec/execbase.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <workbench/startup.h>
#include <proto/exec.h>
#include <proto/dos.h>

struct ExecBase  *SysBase = 0;
struct DosLibrary *DOSBase = 0;

/* Provided by main.c */
extern int hdpart_main(struct WBStartup *wbmsg);

int _start(void)
{
    struct Process  *proc;
    struct WBStartup *wbmsg = 0;
    int rc = RETURN_FAIL;

    /* exec library base lives at absolute address 4 */
    SysBase = *(struct ExecBase **)4UL;

    DOSBase = (struct DosLibrary *)OpenLibrary("dos.library", 37);
    if (!DOSBase)
        return RETURN_FAIL; /* nothing else we can do */

    proc = (struct Process *)FindTask(0);

    if (proc->pr_CLI == 0) {
        /* Started from Workbench: a WBStartup message is waiting at our
         * process port. Receive it now; reply only at exit. */
        WaitPort(&proc->pr_MsgPort);
        wbmsg = (struct WBStartup *)GetMsg(&proc->pr_MsgPort);
    }

    rc = hdpart_main(wbmsg);

    if (wbmsg) {
        /* Must Forbid() before replying so we are not unloaded mid-reply. */
        Forbid();
        ReplyMsg((struct Message *)wbmsg);
    }

    CloseLibrary((struct Library *)DOSBase);
    return rc;
}
```

- [ ] **Step 2: Sanity-compile just this file (it references hdpart_main, so expect a link error only at link time)**

Run:
```bash
export AMIGA_BIN="/Users/sfs/.vscode/extensions/bartmanabyss.amiga-debug-1.8.2/bin/darwin"
export PATH="$AMIGA_BIN/opt/bin:$AMIGA_BIN:$PATH"
cd /Users/sfs/Devel/party && m68k-amiga-elf-gcc -m68000 -nostdlib -Os -Isrc -c src/startup.c -o /tmp/startup.o && echo COMPILE_OK
```
Expected: `COMPILE_OK` (compiles; `hdpart_main` is an unresolved extern, fine for `-c`).

- [ ] **Step 3: Commit**

```bash
cd /Users/sfs/Devel/party
git add src/startup.c
git commit -m "feat: minimal AmigaOS startup (entry, lib open, CLI/WB detect)"
```

### Task 0.3: Window skeleton (`hdpart_main`)

**Files:**
- Create: `src/main.c`

- [ ] **Step 1: Write `src/main.c`**

```c
/* HDPart main: Milestone-0 smoke test. Opens an Intuition window on the
 * Workbench screen and waits for the close gadget. Later plans replace the
 * body with the real GadTools UI wired to the RDB engine. */
#include <exec/types.h>
#include <intuition/intuition.h>
#include <workbench/startup.h>
#include <proto/exec.h>
#include <proto/intuition.h>

struct IntuitionBase *IntuitionBase = 0;

int hdpart_main(struct WBStartup *wbmsg)
{
    struct Window *win;
    (void)wbmsg;

    IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 37);
    if (!IntuitionBase)
        return 20;

    win = OpenWindowTags(0,
        WA_Left,        80,
        WA_Top,         40,
        WA_Width,       360,
        WA_Height,      120,
        WA_Title,       (ULONG)"HDPart 0.1",
        WA_IDCMP,       IDCMP_CLOSEWINDOW,
        WA_Flags,       WFLG_DRAGBAR | WFLG_DEPTHGADGET | WFLG_CLOSEGADGET |
                        WFLG_ACTIVATE | WFLG_SMART_REFRESH,
        TAG_END);

    if (win) {
        BOOL done = FALSE;
        while (!done) {
            struct IntuiMessage *msg;
            WaitPort(win->UserPort);
            while ((msg = (struct IntuiMessage *)GetMsg(win->UserPort))) {
                ULONG cls = msg->Class;
                ReplyMsg((struct Message *)msg);
                if (cls == IDCMP_CLOSEWINDOW)
                    done = TRUE;
            }
        }
        CloseWindow(win);
    }

    CloseLibrary((struct Library *)IntuitionBase);
    return 0;
}
```

- [ ] **Step 2: Full build**

Run:
```bash
export AMIGA_BIN="/Users/sfs/.vscode/extensions/bartmanabyss.amiga-debug-1.8.2/bin/darwin"
export PATH="$AMIGA_BIN/opt/bin:$AMIGA_BIN:$PATH"
cd /Users/sfs/Devel/party && make clean && make 2>&1 | tail -20 && ls -l out/HDPart
```
Expected: `Compiling src/startup.c`, `Compiling src/main.c`, `Linking out/HDPart.elf`, `Elf2Hunk out/HDPart`, and `out/HDPart` exists (a few KB).

- [ ] **Step 3: Confirm it is a real AmigaDOS hunk executable**

Run:
```bash
xxd out/HDPart | head -1
```
Expected: first longword `00000000 03f3 ...` — `0x000003F3` is `HUNK_HEADER`, confirming a valid Amiga executable.

- [ ] **Step 4: Commit**

```bash
cd /Users/sfs/Devel/party
git add src/main.c
git commit -m "feat: window skeleton smoke test (opens/closes HDPart window)"
```

### Task 0.4: Run the skeleton in FS-UAE (manual verification gate)

**Files:** none (verification only)

- [ ] **Step 1: Launch via the VSCode amiga-debug launch config**

In VSCode, pick the "Amiga 1200" (or A500) launch configuration with `amiga.program` pointing at `out/HDPart`. Press F5. Bartman's bundled FS-UAE boots and runs the program.

Alternative (manual): copy `out/HDPart` into an FS-UAE hard-drive directory mapped to the emulated Workbench and run it from a CLI/Shell.

- [ ] **Step 2: Verify behavior**

Expected: a window titled "HDPart 0.1" appears on the Workbench screen; clicking its close gadget closes it and returns to CLI/Workbench with no crash, no leftover task. Confirm CLI prompt returns (CLI launch) or the program's icon de-highlights (WB launch).

- [ ] **Step 3: Record the result**

If it works, note "M0 smoke test passed in FS-UAE (config: A1200, KS 3.x / 2.04)" in the commit body of the next task. If it fails, STOP and debug startup/linker before proceeding (this is the de-risking gate for the whole project).

---

## Milestone 1 — RDB engine (pure C, host-tested)

Outcome: `tests/run-host-tests.sh` passes; `tests/verify-rdbtool.sh` shows `rdbtool` reading an engine-produced image; `rdb.c` also compiles for m68k.

Design notes (apply to all Task 1.x):
- All on-disk values are **big-endian**; the engine uses explicit accessors (`endian.h`) so host tests are byte-order independent.
- The engine never calls the OS. Block access goes through a caller-supplied callback:
  ```c
  typedef int (*BlockIO)(void *ctx, uint32_t block, uint8_t *buf, int write);
  /* read 512 bytes into buf when write==0; write 512 bytes from buf when write==1.
     return 0 on success, non-zero on I/O error. */
  ```
- Block size is fixed at 512 for phase 1.
- Layout produced by the engine (mirrors rdbtool defaults): RDSK at block 0; PART blocks at blocks 1..N within the reserved area; first usable cylinder `lo_cyl = 2`; `rdb_RDBBlocksLo = 0`, `rdb_RDBBlocksHi = lo_cyl*cyl_blocks - 1`.

### Task 1.1: Big-endian accessors + test scaffold

**Files:**
- Create: `src/endian.h`
- Create: `tests/test_rdb.c`
- Create: `tests/run-host-tests.sh`

- [ ] **Step 1: Write the failing test (BE round-trip)**

`tests/test_rdb.c`:
```c
/* HDPart host unit tests. Assert-based, no framework.
 * Compiled with system cc; includes the engine source directly. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "../src/endian.h"
#include "../src/rdb.h"

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); g_fail++; } } while (0)

static void test_endian(void)
{
    uint8_t b[4];
    be_put32(b, 0x12345678u);
    CHECK(b[0]==0x12 && b[1]==0x34 && b[2]==0x56 && b[3]==0x78);
    CHECK(be_get32(b) == 0x12345678u);
}

int main(void)
{
    test_endian();
    /* further test functions appended in later tasks */
    if (g_fail) { printf("%d CHECK(s) FAILED\n", g_fail); return 1; }
    printf("ALL TESTS PASSED\n");
    return 0;
}
```

`tests/run-host-tests.sh`:
```sh
#!/bin/sh
# Compile and run HDPart host unit tests with the system compiler.
set -e
cd "$(dirname "$0")/.."
cc -std=c99 -Wall -Wextra -g -o /tmp/hdpart_tests tests/test_rdb.c src/rdb.c
/tmp/hdpart_tests
```

- [ ] **Step 2: Run it to verify it fails (no endian.h/rdb.h yet)**

Run:
```bash
cd /Users/sfs/Devel/party && chmod +x tests/run-host-tests.sh && ./tests/run-host-tests.sh
```
Expected: FAIL — compile error, `src/endian.h` / `src/rdb.h` / `src/rdb.c` not found.

- [ ] **Step 3: Write `src/endian.h`**

```c
#ifndef HDPART_ENDIAN_H
#define HDPART_ENDIAN_H
#include <stdint.h>

/* Big-endian (Amiga on-disk) accessors over a raw byte buffer. */
static inline uint32_t be_get32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}
static inline void be_put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
#endif
```

- [ ] **Step 4: Write a minimal `src/rdb.h` and empty `src/rdb.c` so tests link**

`src/rdb.h`:
```c
#ifndef HDPART_RDB_H
#define HDPART_RDB_H
#include <stdint.h>

#define RDB_BLOCK_BYTES 512
#define RDB_MAX_PARTS   32
#define RDB_NAME_LEN    32

typedef int (*BlockIO)(void *ctx, uint32_t block, uint8_t *buf, int write);

#endif
```

`src/rdb.c`:
```c
#include "rdb.h"
#include "endian.h"
/* implementation grows over the following tasks */
```

- [ ] **Step 5: Run tests to verify they pass**

Run:
```bash
cd /Users/sfs/Devel/party && ./tests/run-host-tests.sh
```
Expected: `ALL TESTS PASSED`.

- [ ] **Step 6: Commit**

```bash
cd /Users/sfs/Devel/party
git add src/endian.h src/rdb.h src/rdb.c tests/test_rdb.c tests/run-host-tests.sh
git commit -m "test: host test scaffold + big-endian accessors"
```

### Task 1.2: Block checksum

**Files:**
- Modify: `src/rdb.h`, `src/rdb.c`, `tests/test_rdb.c`

- [ ] **Step 1: Write the failing test**

Append to `tests/test_rdb.c` before `main()` and add a call in `main()`:
```c
static void test_checksum(void)
{
    uint8_t blk[RDB_BLOCK_BYTES];
    memset(blk, 0, sizeof(blk));
    be_put32(blk + 0, 0x5244534Bu); /* 'RDSK' */
    be_put32(blk + 4, 64);          /* summed longs */
    /* checksum field at offset 8 left zero, then computed */
    rdb_set_checksum(blk, 64, 8);
    CHECK(rdb_checksum_ok(blk, 64));
    /* corrupt a byte -> checksum must fail */
    blk[20] ^= 0xFF;
    CHECK(!rdb_checksum_ok(blk, 64));
}
```
Add `test_checksum();` after `test_endian();` in `main()`.

- [ ] **Step 2: Run to verify it fails**

Run: `cd /Users/sfs/Devel/party && ./tests/run-host-tests.sh`
Expected: FAIL — `rdb_set_checksum` / `rdb_checksum_ok` undefined.

- [ ] **Step 3: Implement**

Add to `src/rdb.h` (before `#endif`):
```c
/* Checksum: sum of `summed_longs` big-endian longwords must equal 0.
   chk_off is the byte offset of the checksum field within the block. */
void     rdb_set_checksum(uint8_t *blk, uint32_t summed_longs, uint32_t chk_off);
int      rdb_checksum_ok(const uint8_t *blk, uint32_t summed_longs);
uint32_t rdb_sum_longs(const uint8_t *blk, uint32_t summed_longs);
```

Add to `src/rdb.c`:
```c
uint32_t rdb_sum_longs(const uint8_t *blk, uint32_t summed_longs)
{
    uint32_t sum = 0, i;
    for (i = 0; i < summed_longs; i++)
        sum += be_get32(blk + i * 4);
    return sum;
}

void rdb_set_checksum(uint8_t *blk, uint32_t summed_longs, uint32_t chk_off)
{
    uint32_t sum;
    be_put32(blk + chk_off, 0);
    sum = rdb_sum_longs(blk, summed_longs);
    be_put32(blk + chk_off, (uint32_t)(0u - sum)); /* two's complement */
}

int rdb_checksum_ok(const uint8_t *blk, uint32_t summed_longs)
{
    return rdb_sum_longs(blk, summed_longs) == 0u;
}
```

- [ ] **Step 4: Run to verify pass**

Run: `cd /Users/sfs/Devel/party && ./tests/run-host-tests.sh`
Expected: `ALL TESTS PASSED`.

- [ ] **Step 5: Commit**

```bash
cd /Users/sfs/Devel/party
git add src/rdb.h src/rdb.c tests/test_rdb.c
git commit -m "feat(rdb): block checksum compute/verify + tests"
```

### Task 1.3: Geometry math (MB ↔ cylinders)

**Files:**
- Modify: `src/rdb.h`, `src/rdb.c`, `tests/test_rdb.c`

- [ ] **Step 1: Write the failing test**

Append to `tests/test_rdb.c`, add call in `main()`:
```c
static void test_geometry(void)
{
    /* 16 heads * 63 sectors = 1008 blocks/cyl; 512 b/blk => 504 KB/cyl
       => ~0.4922 MB/cyl. 200 MB -> ceil(200*1024*1024 / (1008*512)) cyls. */
    uint32_t cyl_blocks = 16u * 63u;            /* 1008 */
    uint32_t cyls = rdb_mb_to_cyls(200, cyl_blocks, RDB_BLOCK_BYTES);
    CHECK(cyls == 407);                         /* 200MB rounds up to 407 cyl */
    /* round-trip: cyls back to MB (floor) */
    uint32_t mb = rdb_cyls_to_mb(cyls, cyl_blocks, RDB_BLOCK_BYTES);
    CHECK(mb == 200);                           /* 407 cyl -> 200 MB (floor) */
}
```

Derivation for the assertion: bytes per cyl = 1008*512 = 516096. 200 MB = 209715200 bytes. 209715200 / 516096 = 406.35 → ceil = 407. 407*516096 = 210,051,072 bytes / 1048576 = 200.32 → floor 200.

- [ ] **Step 2: Run to verify it fails**

Run: `cd /Users/sfs/Devel/party && ./tests/run-host-tests.sh`
Expected: FAIL — functions undefined.

- [ ] **Step 3: Implement**

Add to `src/rdb.h`:
```c
/* MB<->cylinder conversions. 1 MB = 1024*1024 bytes.
   mb_to_cyls rounds UP (so the partition is at least the requested size).
   cyls_to_mb rounds DOWN (reported usable size). */
uint32_t rdb_mb_to_cyls(uint32_t mb, uint32_t cyl_blocks, uint32_t block_bytes);
uint32_t rdb_cyls_to_mb(uint32_t cyls, uint32_t cyl_blocks, uint32_t block_bytes);
```

Add to `src/rdb.c`:
```c
uint32_t rdb_mb_to_cyls(uint32_t mb, uint32_t cyl_blocks, uint32_t block_bytes)
{
    /* Use 64-bit to avoid overflow on large sizes. */
    uint64_t bytes      = (uint64_t)mb * 1024u * 1024u;
    uint64_t cyl_bytes  = (uint64_t)cyl_blocks * block_bytes;
    uint64_t cyls       = (bytes + cyl_bytes - 1) / cyl_bytes; /* ceil */
    if (cyls == 0) cyls = 1;
    return (uint32_t)cyls;
}

uint32_t rdb_cyls_to_mb(uint32_t cyls, uint32_t cyl_blocks, uint32_t block_bytes)
{
    uint64_t bytes = (uint64_t)cyls * cyl_blocks * block_bytes;
    return (uint32_t)(bytes / (1024u * 1024u)); /* floor */
}
```

- [ ] **Step 4: Run to verify pass**

Run: `cd /Users/sfs/Devel/party && ./tests/run-host-tests.sh`
Expected: `ALL TESTS PASSED`.

- [ ] **Step 5: Commit**

```bash
cd /Users/sfs/Devel/party
git add src/rdb.h src/rdb.c tests/test_rdb.c
git commit -m "feat(rdb): MB<->cylinder geometry math + tests"
```

### Task 1.4: Model type + init

**Files:**
- Modify: `src/rdb.h`, `src/rdb.c`, `tests/test_rdb.c`

- [ ] **Step 1: Write the failing test**

Append, add call in `main()`:
```c
static void test_init_model(void)
{
    RdbModel m;
    rdb_init_model(&m, /*cyl*/996, /*heads*/16, /*sectors*/63);
    CHECK(m.cylinders == 996);
    CHECK(m.heads == 16);
    CHECK(m.sectors == 63);
    CHECK(m.block_bytes == RDB_BLOCK_BYTES);
    CHECK(m.cyl_blocks == 16u * 63u);          /* 1008 */
    CHECK(m.lo_cyl == 2);                       /* reserve 2 cyl for RDB */
    CHECK(m.hi_cyl == 995);                     /* cylinders-1 */
    CHECK(m.rdb_blocks_lo == 0);
    CHECK(m.rdb_blocks_hi == 2u * 1008u - 1u);  /* 2015 */
    CHECK(m.num_parts == 0);
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `cd /Users/sfs/Devel/party && ./tests/run-host-tests.sh`
Expected: FAIL — `RdbModel` / `rdb_init_model` undefined.

- [ ] **Step 3: Implement**

Add to `src/rdb.h`:
```c
typedef struct {
    char     name[RDB_NAME_LEN]; /* NUL-terminated device name, e.g. "DH0" */
    uint32_t low_cyl;
    uint32_t high_cyl;
    uint32_t dos_type;           /* e.g. 0x444F5303 = DOS\3 (FFS Intl) */
    uint32_t num_buffers;
    int32_t  boot_pri;
    uint8_t  bootable;           /* 0/1 (UI greyed in phase 1) */
} RdbPartition;

typedef struct {
    uint32_t cylinders, heads, sectors;
    uint32_t block_bytes;
    uint32_t cyl_blocks;         /* heads*sectors */
    uint32_t lo_cyl, hi_cyl;     /* partitionable cylinder range */
    uint32_t rdb_blocks_lo, rdb_blocks_hi;
    int          num_parts;
    RdbPartition parts[RDB_MAX_PARTS];
} RdbModel;

#define RDB_RESERVED_CYLS 2u     /* cylinders reserved for RDB metadata */
#define RDB_DOSTYPE_FFS_INTL 0x444F5303u

void rdb_init_model(RdbModel *m, uint32_t cyl, uint32_t heads, uint32_t sectors);
```

Add to `src/rdb.c` (need `<string.h>` for memset — already pulled in by tests; for the engine add the include):
```c
#include <string.h>
...
void rdb_init_model(RdbModel *m, uint32_t cyl, uint32_t heads, uint32_t sectors)
{
    memset(m, 0, sizeof(*m));
    m->cylinders   = cyl;
    m->heads       = heads;
    m->sectors     = sectors;
    m->block_bytes = RDB_BLOCK_BYTES;
    m->cyl_blocks  = heads * sectors;
    m->lo_cyl      = RDB_RESERVED_CYLS;
    m->hi_cyl      = (cyl > 0) ? cyl - 1 : 0;
    m->rdb_blocks_lo = 0;
    m->rdb_blocks_hi = RDB_RESERVED_CYLS * m->cyl_blocks - 1;
    m->num_parts   = 0;
}
```

Note: `src/rdb.c` `#include <string.h>` is valid both on the host and on the Bartman toolchain (its sys-include provides `string.h`; `memset` is provided by `support/gcc8_c_support.c` at link time on Amiga — wired up in Task 1.9).

- [ ] **Step 4: Run to verify pass**

Run: `cd /Users/sfs/Devel/party && ./tests/run-host-tests.sh`
Expected: `ALL TESTS PASSED`.

- [ ] **Step 5: Commit**

```bash
cd /Users/sfs/Devel/party
git add src/rdb.h src/rdb.c tests/test_rdb.c
git commit -m "feat(rdb): model type + init with geometry/reserved area + tests"
```

### Task 1.5: Add / validate partitions

**Files:**
- Modify: `src/rdb.h`, `src/rdb.c`, `tests/test_rdb.c`

- [ ] **Step 1: Write the failing test**

Append, add call in `main()`:
```c
static void test_add_partition(void)
{
    RdbModel m;
    rdb_init_model(&m, 996, 16, 63);

    /* First partition: 200 MB -> 407 cyl, starts at lo_cyl=2 */
    int r = rdb_add_partition(&m, "DH0", 200, RDB_DOSTYPE_FFS_INTL);
    CHECK(r == RDB_OK);
    CHECK(m.num_parts == 1);
    CHECK(m.parts[0].low_cyl == 2);
    CHECK(m.parts[0].high_cyl == 2 + 407 - 1);          /* 408 */
    CHECK(strcmp(m.parts[0].name, "DH0") == 0);
    CHECK(m.parts[0].dos_type == RDB_DOSTYPE_FFS_INTL);

    /* Second partition follows immediately */
    r = rdb_add_partition(&m, "DH1", 200, RDB_DOSTYPE_FFS_INTL);
    CHECK(r == RDB_OK);
    CHECK(m.parts[1].low_cyl == 409);
    CHECK(m.parts[1].high_cyl == 409 + 407 - 1);        /* 815 */

    /* Duplicate name rejected */
    r = rdb_add_partition(&m, "DH0", 10, RDB_DOSTYPE_FFS_INTL);
    CHECK(r == RDB_ERR_DUP_NAME);

    /* Too big to fit (remaining cyls 816..995 = 180 cyl ~ 88 MB) */
    r = rdb_add_partition(&m, "DH2", 500, RDB_DOSTYPE_FFS_INTL);
    CHECK(r == RDB_ERR_NO_SPACE);

    /* validate passes on the good model */
    CHECK(rdb_validate(&m) == RDB_OK);
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `cd /Users/sfs/Devel/party && ./tests/run-host-tests.sh`
Expected: FAIL — `rdb_add_partition` / `rdb_validate` / `RDB_OK` undefined.

- [ ] **Step 3: Implement**

Add to `src/rdb.h`:
```c
enum {
    RDB_OK = 0,
    RDB_ERR_NO_SPACE,
    RDB_ERR_DUP_NAME,
    RDB_ERR_TOO_MANY,
    RDB_ERR_BAD_NAME,
    RDB_ERR_OVERLAP,
    RDB_ERR_IO,
    RDB_ERR_NO_RDB
};

/* Append a partition sized in MB, placed immediately after the last one
   (or at lo_cyl). Returns RDB_OK or an RDB_ERR_*. */
int rdb_add_partition(RdbModel *m, const char *name, uint32_t size_mb,
                      uint32_t dos_type);
/* Remove partition by index, shifting the rest down. */
int rdb_delete_partition(RdbModel *m, int index);
/* Validate all partitions: bounds within [lo_cyl,hi_cyl], no overlaps,
   unique names. Returns RDB_OK or first error found. */
int rdb_validate(const RdbModel *m);
```

Add to `src/rdb.c`:
```c
static uint32_t next_free_cyl(const RdbModel *m)
{
    uint32_t top = m->lo_cyl;
    int i;
    for (i = 0; i < m->num_parts; i++)
        if (m->parts[i].high_cyl + 1 > top)
            top = m->parts[i].high_cyl + 1;
    return top;
}

static int name_taken(const RdbModel *m, const char *name, int skip)
{
    int i;
    for (i = 0; i < m->num_parts; i++) {
        if (i == skip) continue;
        if (strcmp(m->parts[i].name, name) == 0) return 1;
    }
    return 0;
}

static void copy_name(char *dst, const char *src)
{
    int i;
    for (i = 0; i < RDB_NAME_LEN - 1 && src[i]; i++) dst[i] = src[i];
    dst[i] = 0;
}

int rdb_add_partition(RdbModel *m, const char *name, uint32_t size_mb,
                      uint32_t dos_type)
{
    uint32_t start, cyls, end;
    RdbPartition *p;

    if (m->num_parts >= RDB_MAX_PARTS) return RDB_ERR_TOO_MANY;
    if (!name || !name[0])             return RDB_ERR_BAD_NAME;
    if (name_taken(m, name, -1))       return RDB_ERR_DUP_NAME;

    start = next_free_cyl(m);
    cyls  = rdb_mb_to_cyls(size_mb, m->cyl_blocks, m->block_bytes);
    if (cyls == 0) cyls = 1;
    end   = start + cyls - 1;
    if (start > m->hi_cyl || end > m->hi_cyl) return RDB_ERR_NO_SPACE;

    p = &m->parts[m->num_parts++];
    copy_name(p->name, name);
    p->low_cyl     = start;
    p->high_cyl    = end;
    p->dos_type    = dos_type;
    p->num_buffers = 30;
    p->boot_pri    = 0;
    p->bootable    = 0;
    return RDB_OK;
}

int rdb_delete_partition(RdbModel *m, int index)
{
    int i;
    if (index < 0 || index >= m->num_parts) return RDB_ERR_BAD_NAME;
    for (i = index; i < m->num_parts - 1; i++)
        m->parts[i] = m->parts[i + 1];
    m->num_parts--;
    return RDB_OK;
}

int rdb_validate(const RdbModel *m)
{
    int i, j;
    for (i = 0; i < m->num_parts; i++) {
        const RdbPartition *a = &m->parts[i];
        if (a->low_cyl < m->lo_cyl || a->high_cyl > m->hi_cyl)
            return RDB_ERR_NO_SPACE;
        if (a->low_cyl > a->high_cyl)
            return RDB_ERR_OVERLAP;
        if (!a->name[0]) return RDB_ERR_BAD_NAME;
        for (j = i + 1; j < m->num_parts; j++) {
            const RdbPartition *b = &m->parts[j];
            if (strcmp(a->name, b->name) == 0) return RDB_ERR_DUP_NAME;
            if (a->low_cyl <= b->high_cyl && b->low_cyl <= a->high_cyl)
                return RDB_ERR_OVERLAP;
        }
    }
    return RDB_OK;
}
```

- [ ] **Step 4: Run to verify pass**

Run: `cd /Users/sfs/Devel/party && ./tests/run-host-tests.sh`
Expected: `ALL TESTS PASSED`.

- [ ] **Step 5: Commit**

```bash
cd /Users/sfs/Devel/party
git add src/rdb.h src/rdb.c tests/test_rdb.c
git commit -m "feat(rdb): add/delete/validate partitions + tests"
```

### Task 1.6: Serialize (RDSK + PART chain) and parse — round-trip

**Files:**
- Modify: `src/rdb.h`, `src/rdb.c`, `tests/test_rdb.c`

This task implements both serialize and parse together because the strongest test is a round-trip through a RAM-backed `BlockIO`.

- [ ] **Step 1: Write the failing test (RAM disk + round-trip)**

Append, add call in `main()`:
```c
/* Simple RAM-backed BlockIO for tests: 4096 blocks * 512 bytes. */
#define RAMDISK_BLOCKS 4096
static uint8_t g_ram[RAMDISK_BLOCKS][RDB_BLOCK_BYTES];

static int ram_io(void *ctx, uint32_t block, uint8_t *buf, int write)
{
    (void)ctx;
    if (block >= RAMDISK_BLOCKS) return 1;
    if (write) memcpy(g_ram[block], buf, RDB_BLOCK_BYTES);
    else       memcpy(buf, g_ram[block], RDB_BLOCK_BYTES);
    return 0;
}

static void test_serialize_parse(void)
{
    RdbModel m, back;
    int r;
    memset(g_ram, 0, sizeof(g_ram));

    rdb_init_model(&m, 996, 16, 63);
    CHECK(rdb_add_partition(&m, "DH0", 200, RDB_DOSTYPE_FFS_INTL) == RDB_OK);
    CHECK(rdb_add_partition(&m, "DH1", 200, RDB_DOSTYPE_FFS_INTL) == RDB_OK);

    r = rdb_serialize(&m, ram_io, 0);
    CHECK(r == RDB_OK);

    /* RDSK lives at block 0 with a valid checksum */
    CHECK(be_get32(g_ram[0] + 0) == 0x5244534Bu);          /* 'RDSK' */
    CHECK(rdb_checksum_ok(g_ram[0], be_get32(g_ram[0] + 4)));

    /* Parse it back */
    r = rdb_parse(&back, ram_io, 0);
    CHECK(r == RDB_OK);
    CHECK(back.cylinders == 996);
    CHECK(back.heads == 16);
    CHECK(back.sectors == 63);
    CHECK(back.num_parts == 2);
    CHECK(strcmp(back.parts[0].name, "DH0") == 0);
    CHECK(back.parts[0].low_cyl == 2);
    CHECK(back.parts[0].high_cyl == 408);
    CHECK(strcmp(back.parts[1].name, "DH1") == 0);
    CHECK(back.parts[1].low_cyl == 409);
    CHECK(back.parts[1].dos_type == RDB_DOSTYPE_FFS_INTL);
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `cd /Users/sfs/Devel/party && ./tests/run-host-tests.sh`
Expected: FAIL — `rdb_serialize` / `rdb_parse` undefined.

- [ ] **Step 3: Implement serialize + parse**

Add to `src/rdb.h`:
```c
/* Serialize the model to disk via io: writes RDSK at block 0 and a PART
   chain in the reserved area. Returns RDB_OK or RDB_ERR_*. Validates first. */
int rdb_serialize(const RdbModel *m, BlockIO io, void *ctx);

/* Parse an existing RDB from disk via io: scans blocks 0..RDB_LOCATION_LIMIT
   for RDSK, reads geometry, walks the PART chain. Returns RDB_OK,
   RDB_ERR_NO_RDB, or RDB_ERR_IO. */
int rdb_parse(RdbModel *m, BlockIO io, void *ctx);
```

Add to `src/rdb.c` (offsets follow `devices/hardblocks.h`):
```c
/* ---- RDB on-disk constants (byte offsets within a 512-byte block) ---- */
#define ID_RDSK 0x5244534Bu
#define ID_PART 0x50415254u
#define RDB_LOCATION_LIMIT 16u
#define NULLPTR 0xFFFFFFFFu

/* RigidDiskBlock field offsets */
#define RDB_o_ID            0
#define RDB_o_SummedLongs   4
#define RDB_o_ChkSum        8
#define RDB_o_HostID        12
#define RDB_o_BlockBytes    16
#define RDB_o_Flags         20
#define RDB_o_BadBlockList  24
#define RDB_o_PartitionList 28
#define RDB_o_FileSysHdr    32
#define RDB_o_DriveInit     36
#define RDB_o_Cylinders     64
#define RDB_o_Sectors       68
#define RDB_o_Heads         72
#define RDB_o_Interleave    76
#define RDB_o_Park          80
#define RDB_o_WritePreComp  96
#define RDB_o_ReducedWrite  100
#define RDB_o_StepRate      104
#define RDB_o_RDBBlocksLo   128
#define RDB_o_RDBBlocksHi   132
#define RDB_o_LoCylinder    136
#define RDB_o_HiCylinder    140
#define RDB_o_CylBlocks     144
#define RDB_o_AutoParkSecs  148
#define RDB_o_HighRDSKBlock 152
#define RDB_SUMMEDLONGS     64u

/* PartitionBlock field offsets */
#define PART_o_ID           0
#define PART_o_SummedLongs  4
#define PART_o_ChkSum       8
#define PART_o_HostID       12
#define PART_o_Next         16
#define PART_o_Flags        20
#define PART_o_DevFlags     32
#define PART_o_DriveName    36   /* BSTR: length byte then chars */
#define PART_o_Environment  128  /* DosEnvec, 20 longs */
#define PART_SUMMEDLONGS    64u

/* DosEnvec indices (longword index within pb_Environment) */
#define DE_TableSize     0
#define DE_SizeBlock     1
#define DE_SecOrg        2
#define DE_Surfaces      3
#define DE_SectorPerBlk  4
#define DE_BlocksPerTrk  5
#define DE_Reserved      6
#define DE_PreAlloc      7
#define DE_Interleave    8
#define DE_LowCyl        9
#define DE_HighCyl       10
#define DE_NumBuffers    11
#define DE_BufMemType    12
#define DE_MaxTransfer   13
#define DE_Mask          14
#define DE_BootPri       15
#define DE_DosType       16

static void env_put(uint8_t *blk, int idx, uint32_t v)
{
    be_put32(blk + PART_o_Environment + idx * 4, v);
}
static uint32_t env_get(const uint8_t *blk, int idx)
{
    return be_get32(blk + PART_o_Environment + idx * 4);
}

static void write_partition_block(uint8_t *blk, const RdbModel *m,
                                  const RdbPartition *p, uint32_t next)
{
    int i, n;
    memset(blk, 0, RDB_BLOCK_BYTES);
    be_put32(blk + PART_o_ID, ID_PART);
    be_put32(blk + PART_o_SummedLongs, PART_SUMMEDLONGS);
    be_put32(blk + PART_o_HostID, 7);
    be_put32(blk + PART_o_Next, next);
    be_put32(blk + PART_o_Flags, p->bootable ? 1u : 0u); /* PBFF_BOOTABLE */
    be_put32(blk + PART_o_DevFlags, 0);

    /* pb_DriveName as BSTR: length byte then characters */
    for (n = 0; n < RDB_NAME_LEN - 1 && p->name[n]; n++) ;
    blk[PART_o_DriveName] = (uint8_t)n;
    for (i = 0; i < n; i++) blk[PART_o_DriveName + 1 + i] = (uint8_t)p->name[i];

    env_put(blk, DE_TableSize,    16);
    env_put(blk, DE_SizeBlock,    m->block_bytes / 4);   /* longs per block */
    env_put(blk, DE_SecOrg,       0);
    env_put(blk, DE_Surfaces,     m->heads);
    env_put(blk, DE_SectorPerBlk, 1);
    env_put(blk, DE_BlocksPerTrk, m->sectors);
    env_put(blk, DE_Reserved,     2);
    env_put(blk, DE_PreAlloc,     0);
    env_put(blk, DE_Interleave,   0);
    env_put(blk, DE_LowCyl,       p->low_cyl);
    env_put(blk, DE_HighCyl,      p->high_cyl);
    env_put(blk, DE_NumBuffers,   p->num_buffers);
    env_put(blk, DE_BufMemType,   0);
    env_put(blk, DE_MaxTransfer,  0x7FFFFFFFu);
    env_put(blk, DE_Mask,         0x7FFFFFFEu);
    env_put(blk, DE_BootPri,      (uint32_t)p->boot_pri);
    env_put(blk, DE_DosType,      p->dos_type);

    rdb_set_checksum(blk, PART_SUMMEDLONGS, PART_o_ChkSum);
}

int rdb_serialize(const RdbModel *m, BlockIO io, void *ctx)
{
    uint8_t blk[RDB_BLOCK_BYTES];
    uint32_t part_block_first, i;
    int v = rdb_validate(m);
    if (v != RDB_OK) return v;

    /* PART blocks occupy blocks 1..num_parts (block 0 is RDSK). */
    part_block_first = (m->num_parts > 0) ? 1u : NULLPTR;

    /* Write RDSK */
    memset(blk, 0, sizeof(blk));
    be_put32(blk + RDB_o_ID, ID_RDSK);
    be_put32(blk + RDB_o_SummedLongs, RDB_SUMMEDLONGS);
    be_put32(blk + RDB_o_HostID, 7);
    be_put32(blk + RDB_o_BlockBytes, m->block_bytes);
    be_put32(blk + RDB_o_Flags, 0);
    be_put32(blk + RDB_o_BadBlockList, NULLPTR);
    be_put32(blk + RDB_o_PartitionList, part_block_first);
    be_put32(blk + RDB_o_FileSysHdr, NULLPTR);
    be_put32(blk + RDB_o_DriveInit, NULLPTR);
    be_put32(blk + RDB_o_Cylinders, m->cylinders);
    be_put32(blk + RDB_o_Sectors, m->sectors);
    be_put32(blk + RDB_o_Heads, m->heads);
    be_put32(blk + RDB_o_Interleave, 1);
    be_put32(blk + RDB_o_Park, m->cylinders);
    be_put32(blk + RDB_o_WritePreComp, m->cylinders);
    be_put32(blk + RDB_o_ReducedWrite, m->cylinders);
    be_put32(blk + RDB_o_StepRate, 3);
    be_put32(blk + RDB_o_RDBBlocksLo, m->rdb_blocks_lo);
    be_put32(blk + RDB_o_RDBBlocksHi, m->rdb_blocks_hi);
    be_put32(blk + RDB_o_LoCylinder, m->lo_cyl);
    be_put32(blk + RDB_o_HiCylinder, m->hi_cyl);
    be_put32(blk + RDB_o_CylBlocks, m->cyl_blocks);
    be_put32(blk + RDB_o_AutoParkSecs, 0);
    be_put32(blk + RDB_o_HighRDSKBlock, m->num_parts > 0 ? (uint32_t)m->num_parts : 0u);
    rdb_set_checksum(blk, RDB_SUMMEDLONGS, RDB_o_ChkSum);
    if (io(ctx, 0, blk, 1)) return RDB_ERR_IO;

    /* Write PART chain at blocks 1..num_parts */
    for (i = 0; i < (uint32_t)m->num_parts; i++) {
        uint32_t next = (i + 1 < (uint32_t)m->num_parts) ? (i + 2) : NULLPTR;
        write_partition_block(blk, m, &m->parts[i], next);
        if (io(ctx, 1 + i, blk, 1)) return RDB_ERR_IO;
    }
    return RDB_OK;
}

int rdb_parse(RdbModel *m, BlockIO io, void *ctx)
{
    uint8_t blk[RDB_BLOCK_BYTES];
    uint32_t b, part_ptr;
    int found = 0;

    memset(m, 0, sizeof(*m));

    for (b = 0; b < RDB_LOCATION_LIMIT; b++) {
        if (io(ctx, b, blk, 0)) return RDB_ERR_IO;
        if (be_get32(blk + RDB_o_ID) == ID_RDSK &&
            rdb_checksum_ok(blk, be_get32(blk + RDB_o_SummedLongs))) {
            found = 1;
            break;
        }
    }
    if (!found) return RDB_ERR_NO_RDB;

    m->block_bytes   = be_get32(blk + RDB_o_BlockBytes);
    if (m->block_bytes == 0) m->block_bytes = RDB_BLOCK_BYTES;
    m->cylinders     = be_get32(blk + RDB_o_Cylinders);
    m->sectors       = be_get32(blk + RDB_o_Sectors);
    m->heads         = be_get32(blk + RDB_o_Heads);
    m->cyl_blocks    = be_get32(blk + RDB_o_CylBlocks);
    if (m->cyl_blocks == 0) m->cyl_blocks = m->heads * m->sectors;
    m->lo_cyl        = be_get32(blk + RDB_o_LoCylinder);
    m->hi_cyl        = be_get32(blk + RDB_o_HiCylinder);
    m->rdb_blocks_lo = be_get32(blk + RDB_o_RDBBlocksLo);
    m->rdb_blocks_hi = be_get32(blk + RDB_o_RDBBlocksHi);
    part_ptr         = be_get32(blk + RDB_o_PartitionList);

    while (part_ptr != NULLPTR && part_ptr != 0 && m->num_parts < RDB_MAX_PARTS) {
        RdbPartition *p;
        int len, i;
        if (io(ctx, part_ptr, blk, 0)) return RDB_ERR_IO;
        if (be_get32(blk + PART_o_ID) != ID_PART) break;
        if (!rdb_checksum_ok(blk, be_get32(blk + PART_o_SummedLongs))) break;

        p = &m->parts[m->num_parts++];
        len = blk[PART_o_DriveName];
        if (len > RDB_NAME_LEN - 1) len = RDB_NAME_LEN - 1;
        for (i = 0; i < len; i++) p->name[i] = (char)blk[PART_o_DriveName + 1 + i];
        p->name[len] = 0;

        p->low_cyl     = env_get(blk, DE_LowCyl);
        p->high_cyl    = env_get(blk, DE_HighCyl);
        p->num_buffers = env_get(blk, DE_NumBuffers);
        p->boot_pri    = (int32_t)env_get(blk, DE_BootPri);
        p->dos_type    = env_get(blk, DE_DosType);
        p->bootable    = (be_get32(blk + PART_o_Flags) & 1u) ? 1 : 0;

        part_ptr = be_get32(blk + PART_o_Next);
    }
    return RDB_OK;
}
```

- [ ] **Step 4: Run to verify pass**

Run: `cd /Users/sfs/Devel/party && ./tests/run-host-tests.sh`
Expected: `ALL TESTS PASSED`.

- [ ] **Step 5: Commit**

```bash
cd /Users/sfs/Devel/party
git add src/rdb.h src/rdb.c tests/test_rdb.c
git commit -m "feat(rdb): serialize RDSK+PART and parse, with round-trip tests"
```

### Task 1.7: Cross-verify against amitools `rdbtool`

**Files:**
- Create: `tests/verify-rdbtool.sh`
- Modify: `tests/test_rdb.c` (add an image-emitting mode)

- [ ] **Step 1: Add a file-emitting mode to the test program**

In `tests/test_rdb.c`, replace `int main(void)` with a version that, when called with an argument, writes a disk image instead of running tests:
```c
static int file_io_write_image(const char *path, uint32_t total_blocks)
{
    /* Build a model and serialize into a flat file of total_blocks*512. */
    extern int rdb_serialize(const RdbModel*, BlockIO, void*);
    FILE *f = fopen(path, "wb+");
    if (!f) { perror("fopen"); return 1; }
    /* zero-fill */
    {
        static uint8_t z[RDB_BLOCK_BYTES];
        uint32_t i;
        for (i = 0; i < total_blocks; i++) fwrite(z, 1, RDB_BLOCK_BYTES, f);
    }
    {
        RdbModel m;
        rdb_init_model(&m, 996, 16, 63);
        rdb_add_partition(&m, "DH0", 200, RDB_DOSTYPE_FFS_INTL);
        rdb_add_partition(&m, "DH1", 200, RDB_DOSTYPE_FFS_INTL);
        /* file-backed BlockIO */
        struct { FILE *f; } c = { f };
        int r = rdb_serialize(&m, /*io*/0, &c); /* replaced below */
        (void)r;
    }
    fclose(f);
    return 0;
}
```

Note: the cleanest approach is a file-backed `BlockIO`. Use this exact helper set instead of the sketch above — add near the top of `test_rdb.c`:
```c
#include <stdio.h>
typedef struct { FILE *f; } FileCtx;
static int file_io(void *ctx, uint32_t block, uint8_t *buf, int write)
{
    FileCtx *c = (FileCtx *)ctx;
    if (fseek(c->f, (long)block * RDB_BLOCK_BYTES, SEEK_SET)) return 1;
    if (write) { if (fwrite(buf, 1, RDB_BLOCK_BYTES, c->f) != RDB_BLOCK_BYTES) return 1; }
    else       { if (fread (buf, 1, RDB_BLOCK_BYTES, c->f) != RDB_BLOCK_BYTES) return 1; }
    return 0;
}
```

And set `main()` to:
```c
int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "--emit") == 0) {
        /* Small, self-consistent geometry so the image is only a few MB and
           its size matches the geometry declared in the RDB (keeps rdbtool
           happy): 64 cyl * 4 heads * 32 sec = 8192 blocks = 4 MB. */
        FILE *f = fopen(argv[2], "wb+");
        static uint8_t z[RDB_BLOCK_BYTES];
        uint32_t i, total = 64u * 4u * 32u;   /* cylinders * cyl_blocks */
        FileCtx c;
        RdbModel m;
        if (!f) { perror("fopen"); return 2; }
        for (i = 0; i < total; i++) fwrite(z, 1, RDB_BLOCK_BYTES, f);
        c.f = f;
        rdb_init_model(&m, 64, 4, 32);
        rdb_add_partition(&m, "DH0", 1, RDB_DOSTYPE_FFS_INTL);
        rdb_add_partition(&m, "DH1", 1, RDB_DOSTYPE_FFS_INTL);
        if (rdb_serialize(&m, file_io, &c) != RDB_OK) { fclose(f); return 3; }
        fclose(f);
        printf("wrote %s (%u blocks)\n", argv[2], total);
        return 0;
    }

    test_endian();
    test_checksum();
    test_geometry();
    test_init_model();
    test_add_partition();
    test_serialize_parse();
    if (g_fail) { printf("%d CHECK(s) FAILED\n", g_fail); return 1; }
    printf("ALL TESTS PASSED\n");
    return 0;
}
```
Remove the earlier `file_io_write_image` sketch entirely; it was illustrative. Keep only `file_io`, `FileCtx`, and the new `main`.

- [ ] **Step 2: Write `tests/verify-rdbtool.sh`**

```sh
#!/bin/sh
# Emit an RDB image with the HDPart engine, then validate it with amitools rdbtool.
set -e
cd "$(dirname "$0")/.."
cc -std=c99 -Wall -Wextra -g -o /tmp/hdpart_tests tests/test_rdb.c src/rdb.c
/tmp/hdpart_tests --emit /tmp/hdpart_test.img
echo "=== rdbtool free ==="
rdbtool /tmp/hdpart_test.img free
echo "=== rdbtool info ==="
rdbtool /tmp/hdpart_test.img info
```

- [ ] **Step 3: Run host tests then the cross-check**

Run:
```bash
cd /Users/sfs/Devel/party && ./tests/run-host-tests.sh && chmod +x tests/verify-rdbtool.sh && ./tests/verify-rdbtool.sh
```
Expected: `ALL TESTS PASSED`, then `rdbtool` prints physical disk / RDB info listing two partitions **DH0** and **DH1**. With the emit geometry (64×4×32, 1 MB each → 16 cyl each), DH0 spans cyl 2..17 and DH1 18..33, DOS type `DOS\3`. `rdbtool` must not report a checksum error. (Exact column formatting varies by rdbtool version; what matters is two valid partitions, correct names/dostype, and no checksum/parse error.)

- [ ] **Step 4: Commit**

```bash
cd /Users/sfs/Devel/party
git add tests/test_rdb.c tests/verify-rdbtool.sh
git commit -m "test(rdb): cross-verify engine output with amitools rdbtool"
```

### Task 1.8: Compile the engine for the Amiga target

Ensures `rdb.c` builds under the freestanding m68k toolchain (it must, since later plans link it into `out/HDPart`), and that `memset`/`memcpy` resolve.

**Files:**
- Modify: `Makefile` (add the demo's C-support object so freestanding libc helpers resolve)
- Create: `src/rdb_amiga_compile_check` is NOT created — we compile via make.

- [ ] **Step 1: Add gcc8 C-support to the link so memset/memcpy/strlen resolve**

In `Makefile`, change the sources/objects section to also compile `support/gcc8_c_support.c`:
```make
c_sources := $(wildcard src/*.c) support/gcc8_c_support.c
c_objects := $(addprefix obj/,$(patsubst %.c,%.o,$(notdir $(c_sources))))
```
And add a pattern rule for the support dir (place after the existing `obj/%.o : src/%.c` rule):
```make
obj/%.o : support/%.c
	$(info Compiling $<)
	@$(CC) $(CCFLAGS) -c -o $@ $(CURDIR)/$<
```

- [ ] **Step 2: Add a temporary engine reference so the linker keeps rdb.o (and surfaces undefined symbols)**

In `src/main.c`, near the top of `hdpart_main`, add a compile/link smoke reference (removed in Plan 3):
```c
#include "rdb.h"
...
int hdpart_main(struct WBStartup *wbmsg)
{
    RdbModel probe;
    rdb_init_model(&probe, 100, 4, 17);   /* link-smoke: pulls in rdb.o */
    (void)probe;
    ...
```

- [ ] **Step 3: Build for Amiga**

Run:
```bash
export AMIGA_BIN="/Users/sfs/.vscode/extensions/bartmanabyss.amiga-debug-1.8.2/bin/darwin"
export PATH="$AMIGA_BIN/opt/bin:$AMIGA_BIN:$PATH"
cd /Users/sfs/Devel/party && make clean && make 2>&1 | tail -25 && ls -l out/HDPart
```
Expected: compiles `src/rdb.c`, `src/startup.c`, `src/main.c`, `support/gcc8_c_support.c`; links with no undefined-symbol errors; `out/HDPart` produced.

- [ ] **Step 4: Re-run host tests (guard against regressions from any shared edits)**

Run: `cd /Users/sfs/Devel/party && ./tests/run-host-tests.sh`
Expected: `ALL TESTS PASSED`.

- [ ] **Step 5: Commit**

```bash
cd /Users/sfs/Devel/party
git add Makefile src/main.c
git commit -m "build: compile RDB engine for m68k target; link gcc8 c-support"
```

---

## Definition of done (Plan 1)

- [ ] `out/HDPart` builds cleanly and is a valid hunk executable (`0x03F3` header).
- [ ] The window skeleton opens and closes correctly in FS-UAE (M0 gate).
- [ ] `tests/run-host-tests.sh` prints `ALL TESTS PASSED`.
- [ ] `tests/verify-rdbtool.sh` shows `rdbtool` reading the engine's image with two correct partitions and no checksum errors.
- [ ] `src/rdb.c` compiles for both host and m68k from the same source.

## Self-review notes (author)

- **Spec coverage:** This plan covers spec §2 (build/startup), §4 `rdb.c` module, §5 (on-disk structures, DosEnvec defaults), §7 (host tests + rdbtool cross-check + FS-UAE smoke). Spec §4 `device.c`/`model.c`/`gui.c` and §3 (full GUI) and §6 (write/verify/confirm flow) are intentionally deferred to Plan 2 (device I/O + discovery) and Plan 3 (model + GUI).
- **Type consistency:** `BlockIO` signature is identical in `rdb.h` and all call sites/tests. `RdbModel`/`RdbPartition` field names are used consistently across init/add/validate/serialize/parse and tests. Error enum `RDB_OK`/`RDB_ERR_*` consistent.
- **No placeholders:** every code step contains complete code; the one illustrative sketch in Task 1.7 Step 1 is explicitly replaced by the final `file_io`/`main` shown in the same step.
```
