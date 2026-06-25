# HDPart Live-Mount & Format Rework (0.9) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make a freshly-created partition mount as a real, named, session-persistent DOS device after Format (no reboot), fix empty-disk button enablement, and prompt for a volume name.

**Architecture:** Three changes. (#2) `gui_select_unit` initializes an empty in-memory table when a disk has geometry but no RDB. (#3) A reusable string dialog provides a volume-name prompt wired into `gui_format`. (#4) A new `fsres` module (pure field-resolution + OS `filesystem.resource`/device-list helpers) lets `format_partition` mount a fresh partition under its real name, register the handler in `filesystem.resource`, `AddDosNode(ADNF_STARTPROC)` persistently, then `GetDeviceProc` + `ACTION_FORMAT` — mirroring `scsi.device`/a4091-mounter.

**Tech Stack:** C99 freestanding m68k (Bartman amiga-debug toolchain); AmigaOS 2.04+/V37 dos/expansion/filesystem.resource APIs; host unit tests via system `cc`.

## Global Constraints

- Freestanding build: no libc/libgcc; **no 64-bit integer math** (pulls in absent `__udivdi3`); keep math 32-bit. Source: [hdpart-project memory].
- All OS calls live under `#ifdef HDPART_AMIGA`; pure logic compiles with the host `cc` and is unit-tested via `./tests/run-host-tests.sh`.
- Amiga build command (toolchain not on default PATH):
  ```
  export AMIGA_BIN="/Users/sfs/.vscode/extensions/bartmanabyss.amiga-debug-1.8.2/bin/darwin"
  export PATH="$AMIGA_BIN/opt/bin:$AMIGA_BIN:$PATH"
  make
  ```
- After `make`, run `make hd` before any FS-UAE test (else stale binary).
- PatchFlags bit map (devices/hardblocks.h `fhb_PatchFlags`): Type 0x01, Task 0x02, Lock 0x04, Handler 0x08, StackSize 0x10, Priority 0x20, Startup 0x40, SegList 0x80, GlobalVec 0x100.
- Handler defaults when not patched: StackSize ≥ 4096, Priority 10, GlobalVec 0xFFFFFFFF (-1, non-BCPL).
- Work commits directly to `master`. Version bump touches exactly 3 spots: `src/gui.c` window title, `src/gui.c` About line, `Makefile` `ADFVER`.
- GUI/OS tasks cannot be host-tested; their gate is a clean `make` plus the on-target acceptance checklist in Task 7 (the A3000 tester verifies).

---

### Task 1: Empty-disk model init (#2)

**Files:**
- Modify: `src/gui.c` — `gui_select_unit` (around line 1980, the `rdb_parse` branch)

**Interfaces:**
- Consumes: `rdb_init_model(RdbModel*, uint32_t cyl, uint32_t heads, uint32_t sectors)` (rdb.h), globals `g_geo` (`DeviceInfo`), `g_model`, `g_have_model`.
- Produces: nothing for later tasks.

- [ ] **Step 1: Modify the `rdb_parse` branch in `gui_select_unit`**

Find this block in `src/gui.c`:
```c
        dev_geometry(h, &g_geo);
        rdb_model_free(&g_model);
        if (rdb_parse(&g_model, dev_block_io, h) == RDB_OK) {
            g_have_model = 1;
            fs_pool_merge_from_model(&g_model);
        }
        dev_close(h);
```
Replace with:
```c
        dev_geometry(h, &g_geo);
        rdb_model_free(&g_model);
        if (rdb_parse(&g_model, dev_block_io, h) == RDB_OK) {
            g_have_model = 1;
            fs_pool_merge_from_model(&g_model);
        } else if (g_geo.has_media && g_geo.cylinders > 0) {
            /* Blank disk (valid geometry, no RDB): start an empty in-memory table
               so New / Filesystems work immediately without a failed Split first.
               g_dirty stays 0 — nothing is written until the user Saves. */
            rdb_init_model(&g_model, g_geo.cylinders, g_geo.heads, g_geo.sectors);
            g_have_model = 1;
        }
        dev_close(h);
```

- [ ] **Step 2: Build for Amiga**

Run (with the toolchain env from Global Constraints):
```
make
```
Expected: `Elf2Hunk out/HDPart.exe` and `entry-order OK: _start is first in .text`, no errors.

- [ ] **Step 3: Stage for FS-UAE**

Run: `make hd`
Expected: `Staged -> amiga_hd/HDPart ...`

- [ ] **Step 4: Commit**

```bash
git add src/gui.c
git commit -m "fix(gui): enable New/Filesystems on a blank disk (no failed Split needed)"
```

---

### Task 2: Volume-name prompt (#3)

**Files:**
- Modify: `src/gui.c` — generalize `gui_path_dialog` into `gui_string_dialog(title, prompt, init, out, outsz)`; add `gui_volname_dialog`; call it from `gui_format`.

**Interfaces:**
- Consumes: existing dialog helpers `dlg_center`, `dlg_open`, `dlg_close`, `gui_draw_text`, `BUTTONIDCMP`, `STRINGIDCMP`, `g_font`, `g_vi`.
- Produces: `int gui_volname_dialog(const char *deflt, char *out, int outsz)` → 1 on OK (out filled, non-empty), 0 on Cancel.

- [ ] **Step 1: Generalize `gui_path_dialog` to take a prompt string**

In `src/gui.c`, change the signature and the two hard-coded prompt strings.

Signature (line ~553):
```c
static int gui_string_dialog(const char *title, const char *prompt,
                             const char *init, char *out, int outsz)
```
Replace the two occurrences of the literal `"Enter the full file path:"` (the initial `gui_draw_text` at ~line 594 and the `IDCMP_REFRESHWINDOW` redraw at ~line 606) with `prompt`.

- [ ] **Step 2: Add a thin `gui_path_dialog` wrapper + the volume-name dialog**

Immediately after `gui_string_dialog`, add:
```c
static int gui_path_dialog(const char *title, const char *init,
                           char *out, int outsz)
{
    return gui_string_dialog(title, "Enter the full file path:", init, out, outsz);
}

/* Prompt for a volume name (label written by ACTION_FORMAT). Pre-fills with the
   partition's RDB name. Returns 1 with the name in out[], 0 on Cancel. */
static int gui_volname_dialog(const char *deflt, char *out, int outsz)
{
    return gui_string_dialog("Volume name", "Enter a volume name:",
                             deflt, out, outsz);
}
```
(The existing forward declaration / callers of `gui_path_dialog` keep working unchanged.)

- [ ] **Step 3: Call the prompt from `gui_format` before formatting**

In `gui_format` (`src/gui.c`, around line 1361), locate where it currently calls `format_partition(...)`. Immediately before that call, add the prompt and thread the result into the `volname` argument. Replace the existing `format_partition` call site:
```c
    {
        static char volname[32];
        const char *deflt = g_model.parts[index].name;
        if (!gui_volname_dialog(deflt, volname, sizeof(volname)))
            return;                       /* user cancelled the format */
        rc = format_partition(g_cur_driver, g_cur_unit, &g_model, index, volname);
    }
```
(Match the surrounding code: use the same result variable name `rc`/`fr` that the function already uses, and keep the existing result-handling block that follows.)

- [ ] **Step 4: Build for Amiga**

Run: `make`
Expected: clean build, `entry-order OK`.

- [ ] **Step 5: Commit**

```bash
git add src/gui.c
git commit -m "feat(gui): prompt for a volume name on Format (default = partition name)"
```

---

### Task 3: Pure fsres helpers + host tests (#4 foundation)

**Files:**
- Create: `src/fsres.h`, `src/fsres.c`
- Create: `tests/test_fsres.c`
- Modify: `tests/run-host-tests.sh`

**Interfaces:**
- Consumes: `RdbModel`, `RdbFileSys` (rdb.h).
- Produces:
  - `const RdbFileSys *fsres_find_embedded(const RdbModel *m, uint32_t dos_type);` — matching FS-family entry with non-empty `seg_data`, or NULL.
  - `void fsres_resolve_fields(const RdbFileSys *fs, FsHandlerFields *out);`
  - `int fsres_bstr_eq(const uint8_t *bstr, const char *cstr);`
  - `FsHandlerFields` struct + `FSPF_*` macros (fsres.h).

- [ ] **Step 1: Write the failing tests**

Create `tests/test_fsres.c`:
```c
/* Host unit tests for the pure fsres helpers. */
#include <stdio.h>
#include <string.h>
#include "../src/fsres.h"
#include "../src/rdb.h"

static int fails;
#define CHECK(c) do { if(!(c)){ printf("FAIL %s:%d %s\n",__FILE__,__LINE__,#c); fails++; } } while(0)

static void test_find_embedded(void)
{
    RdbModel m; memset(&m, 0, sizeof m);
    static uint8_t seg[8] = {0,0,3,0xF3, 1,2,3,4};
    m.num_fs = 1;
    m.fs[0].dos_type = 0x50465303u;   /* PFS\3 */
    m.fs[0].seg_data = seg; m.fs[0].seg_len = 8;
    /* family match: query DosType differs in low byte but same family */
    CHECK(fsres_find_embedded(&m, 0x50465300u) == &m.fs[0]);
    CHECK(fsres_find_embedded(&m, 0x50465303u) == &m.fs[0]);
    /* different family -> NULL */
    CHECK(fsres_find_embedded(&m, 0x53465302u) == 0);
    /* empty seg_data -> NULL */
    m.fs[0].seg_data = 0;
    CHECK(fsres_find_embedded(&m, 0x50465303u) == 0);
}

static void test_resolve_fields_patched(void)
{
    RdbFileSys fs; memset(&fs, 0, sizeof fs);
    fs.dos_type = 0x50465303u; fs.version = (19u<<16)|2u;
    fs.patch_flags = FSPF_STACKSIZE | FSPF_PRIORITY | FSPF_GLOBALVEC; /* 0x130 */
    fs.dn_stack = 8192; fs.dn_pri = 5; fs.dn_globalvec = 0xFFFFFFFFu;
    FsHandlerFields f; memset(&f, 0, sizeof f);
    fsres_resolve_fields(&fs, &f);
    CHECK(f.dos_type == 0x50465303u);
    CHECK(f.patch_flags == 0x130u);
    CHECK(f.dn_stack == 8192);       /* patched value honoured */
    CHECK(f.dn_pri == 5);
    CHECK(f.dn_globalvec == 0xFFFFFFFFu);
    CHECK(f.dn_type == 0 && f.dn_handler == 0); /* not patched -> 0 */
}

static void test_resolve_fields_defaults(void)
{
    RdbFileSys fs; memset(&fs, 0, sizeof fs);
    fs.dos_type = 0x444F5303u;        /* DOS\3 */
    fs.patch_flags = 0;               /* nothing patched (e.g. OS3.2 FSHD) */
    fs.dn_stack = 0; fs.dn_pri = 0; fs.dn_globalvec = 0;
    FsHandlerFields f; memset(&f, 0, sizeof f);
    fsres_resolve_fields(&fs, &f);
    CHECK(f.dn_stack == 4096);        /* default */
    CHECK(f.dn_pri == 10);            /* default */
    CHECK(f.dn_globalvec == 0xFFFFFFFFu); /* default -1 */
}

static void test_bstr_eq(void)
{
    uint8_t a[5] = {3,'D','H','5',0};
    CHECK(fsres_bstr_eq(a, "DH5") == 1);
    CHECK(fsres_bstr_eq(a, "DH6") == 0);
    CHECK(fsres_bstr_eq(a, "DH")  == 0);   /* length mismatch */
    CHECK(fsres_bstr_eq(a, "DH50")== 0);
}

int main(void)
{
    test_find_embedded();
    test_resolve_fields_patched();
    test_resolve_fields_defaults();
    test_bstr_eq();
    printf("test_fsres: %s\n", fails ? "FAILURES" : "ALL TESTS PASSED");
    return fails ? 1 : 0;
}
```

- [ ] **Step 2: Create the header**

Create `src/fsres.h`:
```c
#ifndef HDPART_FSRES_H
#define HDPART_FSRES_H
#include <stdint.h>
#include "rdb.h"

/* fhb_PatchFlags bits (devices/hardblocks.h). */
#define FSPF_TYPE      0x0001u
#define FSPF_TASK      0x0002u
#define FSPF_LOCK      0x0004u
#define FSPF_HANDLER   0x0008u
#define FSPF_STACKSIZE 0x0010u
#define FSPF_PRIORITY  0x0020u
#define FSPF_STARTUP   0x0040u
#define FSPF_SEGLIST   0x0080u
#define FSPF_GLOBALVEC 0x0100u

/* Resolved DeviceNode / FileSysEntry handler params from an embedded RdbFileSys,
   PatchFlags-gated with safe defaults. (Seglist itself is loaded by OS code.) */
typedef struct {
    uint32_t dos_type, version, patch_flags;
    uint32_t dn_type, dn_task, dn_lock, dn_handler, dn_startup;
    int32_t  dn_stack, dn_pri;
    uint32_t dn_globalvec;
} FsHandlerFields;

/* Pure. First model->fs[] entry whose DosType family == dos_type's family and
   that carries a non-empty seg_data, or NULL. */
const RdbFileSys *fsres_find_embedded(const RdbModel *m, uint32_t dos_type);

/* Pure. Resolve handler fields from fs, applying PatchFlags and defaults
   (stack>=4096, pri 10, globalvec 0xFFFFFFFF). */
void fsres_resolve_fields(const RdbFileSys *fs, FsHandlerFields *out);

/* Pure. Does an AmigaDOS BSTR (length byte + chars) equal C string cstr? */
int fsres_bstr_eq(const uint8_t *bstr, const char *cstr);

#ifdef HDPART_AMIGA
#include <exec/types.h>
/* OS. fse_SegList (BPTR) of a filesystem.resource entry matching dos_type's
   family, or 0 if none. */
BPTR fsres_find_seglist(uint32_t dos_type);
/* OS. Allocate + register a FileSysEntry for these fields with an already-loaded
   seglist into filesystem.resource. Returns 1 on success (entry added), else 0
   (entry freed). */
int fsres_register(const FsHandlerFields *f, BPTR seglist);
/* OS. Is a DOS device node named `name` (C string) currently in the device list? */
int fsres_dosnode_exists(const char *name);
#endif

#endif /* HDPART_FSRES_H */
```

- [ ] **Step 3: Implement the pure helpers**

Create `src/fsres.c`:
```c
#include "fsres.h"
#include <string.h>

const RdbFileSys *fsres_find_embedded(const RdbModel *m, uint32_t dos_type)
{
    uint32_t fam = dos_type & 0xFFFFFF00u;
    int i;
    if (!m) return 0;
    for (i = 0; i < m->num_fs; i++) {
        const RdbFileSys *fs = &m->fs[i];
        if ((fs->dos_type & 0xFFFFFF00u) != fam) continue;
        if (!fs->seg_data || fs->seg_len == 0) continue;
        return fs;
    }
    return 0;
}

void fsres_resolve_fields(const RdbFileSys *fs, FsHandlerFields *out)
{
    uint32_t pf = fs->patch_flags;
    memset(out, 0, sizeof *out);
    out->dos_type    = fs->dos_type;
    out->version     = fs->version;
    out->patch_flags = pf;
    out->dn_type     = (pf & FSPF_TYPE)    ? fs->dn_type    : 0u;
    out->dn_task     = (pf & FSPF_TASK)    ? fs->dn_task    : 0u;
    out->dn_lock     = (pf & FSPF_LOCK)    ? fs->dn_lock    : 0u;
    out->dn_handler  = (pf & FSPF_HANDLER) ? fs->dn_handler : 0u;
    out->dn_startup  = (pf & FSPF_STARTUP) ? fs->dn_startup : 0u;
    out->dn_stack    = ((pf & FSPF_STACKSIZE) && fs->dn_stack) ? (int32_t)fs->dn_stack : 4096;
    out->dn_pri      = (pf & FSPF_PRIORITY) ? (int32_t)fs->dn_pri : 10;
    out->dn_globalvec= (pf & FSPF_GLOBALVEC) ? fs->dn_globalvec : 0xFFFFFFFFu;
}

int fsres_bstr_eq(const uint8_t *bstr, const char *cstr)
{
    int n = bstr[0], i;
    for (i = 0; i < n; i++) {
        if (cstr[i] == 0 || (uint8_t)cstr[i] != bstr[1 + i]) return 0;
    }
    return cstr[n] == 0;   /* both ended at the same length */
}
```

- [ ] **Step 4: Wire the new test into the host runner**

In `tests/run-host-tests.sh`, after the `test_format` line, add:
```sh
cc -std=c99 -Wall -Wextra -g -o /tmp/hdpart_fsres tests/test_fsres.c src/fsres.c
/tmp/hdpart_fsres
```

- [ ] **Step 5: Run host tests — verify pass**

Run: `./tests/run-host-tests.sh`
Expected: `test_fsres: ALL TESTS PASSED` and all other suites still pass.

- [ ] **Step 6: Commit**

```bash
git add src/fsres.h src/fsres.c tests/test_fsres.c tests/run-host-tests.sh
git commit -m "feat(fsres): pure FileSysEntry field resolution + BSTR/embedded-FS helpers"
```

---

### Task 4: filesystem.resource + device-list OS helpers (#4)

**Files:**
- Modify: `src/fsres.c` (add an `#ifdef HDPART_AMIGA` section)
- Modify: `Makefile` is unaffected (it globs `src/*.c`); `tests/run-host-tests.sh` unaffected (pure section already compiles fsres.c without HDPART_AMIGA).

**Interfaces:**
- Consumes: `FsHandlerFields`, `fsres_bstr_eq` (Task 3); AmigaOS `OpenResource`, `Forbid/Permit`, `AddHead`, `AllocMem`, `FreeMem`, `LockDosList`/`NextDosEntry`/`UnLockDosList`.
- Produces: `BPTR fsres_find_seglist(uint32_t)`, `int fsres_register(const FsHandlerFields*, BPTR)`, `int fsres_dosnode_exists(const char*)` (declared in fsres.h).

- [ ] **Step 1: Add the OS section to `src/fsres.c`**

Append to `src/fsres.c`:
```c
#ifdef HDPART_AMIGA
#include <proto/exec.h>
#include <proto/dos.h>
#include <exec/memory.h>
#include <exec/lists.h>
#include <dos/dosextens.h>
#include <dos/filehandler.h>

#ifndef FSRNAME
#define FSRNAME "FileSystem.resource"
#endif

BPTR fsres_find_seglist(uint32_t dos_type)
{
    struct FileSysResource *fsr = (struct FileSysResource *)OpenResource((STRPTR)FSRNAME);
    uint32_t fam = dos_type & 0xFFFFFF00u;
    struct FileSysEntry *fse;
    BPTR seg = 0;
    if (!fsr) return 0;
    Forbid();
    for (fse = (struct FileSysEntry *)fsr->fsr_FileSysEntries.lh_Head;
         fse->fse_Node.ln_Succ;
         fse = (struct FileSysEntry *)fse->fse_Node.ln_Succ) {
        if (fse->fse_DosType != dos_type &&
            ((ULONG)fse->fse_DosType & 0xFFFFFF00u) != fam) continue;
        if (!fse->fse_SegList) continue;
        seg = fse->fse_SegList; break;
    }
    Permit();
    return seg;
}

int fsres_register(const FsHandlerFields *f, BPTR seglist)
{
    struct FileSysResource *fsr;
    struct FileSysEntry *fse;
    if (!seglist) return 0;
    fsr = (struct FileSysResource *)OpenResource((STRPTR)FSRNAME);
    if (!fsr) return 0;
    fse = (struct FileSysEntry *)AllocMem(sizeof(struct FileSysEntry),
                                          MEMF_PUBLIC | MEMF_CLEAR);
    if (!fse) return 0;
    fse->fse_DosType    = f->dos_type;
    fse->fse_Version    = f->version;
    fse->fse_PatchFlags = FSPF_SEGLIST | FSPF_GLOBALVEC |
                          FSPF_STACKSIZE | FSPF_PRIORITY;
    fse->fse_SegList    = seglist;
    fse->fse_GlobalVec  = (BPTR)(ULONG)f->dn_globalvec;
    fse->fse_StackSize  = f->dn_stack;
    fse->fse_Priority   = f->dn_pri;
    fse->fse_Node.ln_Name = (UBYTE *)"hdpart";
    Forbid();
    AddHead(&fsr->fsr_FileSysEntries, &fse->fse_Node);
    Permit();
    return 1;
}

int fsres_dosnode_exists(const char *name)
{
    struct DosList *e;
    int found = 0, guard = 0;
    e = LockDosList(LDF_DEVICES | LDF_READ);
    while ((e = NextDosEntry(e, LDF_DEVICES | LDF_READ)) != 0 && ++guard < 512) {
        struct DeviceNode *dn = (struct DeviceNode *)e;
        UBYTE *bn;
        if (!dn->dn_Name) continue;
        bn = (UBYTE *)BADDR(dn->dn_Name);
        if (fsres_bstr_eq(bn, name)) { found = 1; break; }
    }
    UnLockDosList(LDF_DEVICES | LDF_READ);
    return found;
}
#endif /* HDPART_AMIGA */
```

- [ ] **Step 2: Build for Amiga**

Run: `make`
Expected: `Compiling src/fsres.c`, clean link, `entry-order OK`.

- [ ] **Step 3: Re-run host tests (pure section must still build/pass)**

Run: `./tests/run-host-tests.sh`
Expected: `test_fsres: ALL TESTS PASSED` (the `#ifdef HDPART_AMIGA` block is excluded host-side).

- [ ] **Step 4: Commit**

```bash
git add src/fsres.c
git commit -m "feat(fsres): filesystem.resource find/register + DOS device-name lookup"
```

---

### Task 5: Persistent fresh-mount path in format_partition (#4)

**Files:**
- Modify: `src/format.c` — add the persistent real-name branch before the existing ephemeral `HDP0` path; `#include "fsres.h"`.

**Interfaces:**
- Consumes: `fsres_find_seglist`, `fsres_find_embedded`, `fsres_resolve_fields`, `fsres_register`, `fsres_dosnode_exists` (Task 3/4); existing `format.c` statics `embedded_loadseg`, `cstr_to_bstr`, `format_build_envec`, `FMT_ENV_LONGS`, `bind_handler`, `volbstr`; AmigaOS `MakeDosNode`, `AddDosNode`, `GetDeviceProc`, `FreeDeviceProc`, `DoPkt`, `ADNF_STARTPROC`.
- Produces: unchanged `format_partition` signature; new internal behavior.

- [ ] **Step 1: Include fsres.h**

At the top of `src/format.c` (with the other includes, inside the existing include group), add:
```c
#include "fsres.h"
```

- [ ] **Step 2: Insert the persistent fresh-mount branch**

In `format_partition`, the current flow after the "already mounted, family matches" block (line ~401) falls straight into the ephemeral `HDP0` path (`pick_free_devname`). Insert this branch **immediately before** the `/* Copy driver into a stable static buffer ... */` comment (line ~403), so it runs only when no DOS node already owns the partition's real name:

```c
    /* Approach A — truly-fresh partition (no DOS node with this name yet):
       mount persistently under the REAL partition name so a usable, named DH#:
       device survives the session (no reboot), replicating scsi.device:
       filesystem.resource lookup/register -> MakeDosNode(real name) ->
       AddDosNode(STARTPROC, persist) -> GetDeviceProc -> ACTION_FORMAT. */
    if (!fsres_dosnode_exists(p->name)) {
        struct Library *ExpBase;
        struct DeviceNode *dnp;
        static ULONG ppp[4 + FMT_ENV_LONGS];
        static char  rdriver[40];
        static char  rcolon[10];
        BPTR seg;
        FsHandlerFields hf;
        const RdbFileSys *efs;
        int z;

        /* Resolve a seglist: an already-loaded handler in filesystem.resource,
           else our embedded FSHD/LSEG (which we then register). */
        memset(&hf, 0, sizeof hf);
        seg = fsres_find_seglist((uint32_t)p->dos_type);
        if (!seg) {
            efs = fsres_find_embedded(m, (uint32_t)p->dos_type);
            if (efs) {
                fsres_resolve_fields(efs, &hf);
                seg = embedded_loadseg(efs->seg_data, efs->seg_len);
                if (seg) fsres_register(&hf, seg);   /* best-effort */
            }
        } else {
            /* Use sane defaults for node params when reusing a resource seglist. */
            hf.dn_stack = 4096; hf.dn_pri = 10; hf.dn_globalvec = 0xFFFFFFFFu;
        }

        ExpBase = OpenLibrary((STRPTR)"expansion.library", 37);
        if (!ExpBase) return FMT_ERR_MAKENODE;

        for (z = 0; z < 39 && driver[z]; z++) rdriver[z] = driver[z];
        rdriver[z] = 0;
        ppp[0] = (ULONG)p->name;            /* REAL partition name, e.g. "DH5" */
        ppp[1] = (ULONG)rdriver;
        ppp[2] = (ULONG)unit;
        ppp[3] = 0;
        for (z = 0; z < FMT_ENV_LONGS; z++) ppp[4 + z] = env[z];

        dnp = (struct DeviceNode *)MakeDosNode((APTR)ppp);
        if (!dnp) { CloseLibrary(ExpBase); return FMT_ERR_MAKENODE; }

        if (seg) {
            dnp->dn_SegList   = seg;
            dnp->dn_GlobalVec = (BPTR)(ULONG)hf.dn_globalvec;
            dnp->dn_StackSize = hf.dn_stack ? hf.dn_stack : 4096;
            dnp->dn_Priority  = hf.dn_pri;
        } else if (!bind_handler(dnp, (ULONG)p->dos_type, m)) {
            /* No resource entry, no embedded image, no live-mount clone. */
            CloseLibrary(ExpBase);
            return FMT_ERR_NO_HANDLER;
        }

        if (!AddDosNode(0, ADNF_STARTPROC, dnp)) {
            CloseLibrary(ExpBase);
            return FMT_ERR_ADDNODE;
        }
        CloseLibrary(ExpBase);              /* node now owned by DOS, persists */

        /* Build "<name>:" and format via the live device proc. Use DeviceProc
           (already used in this file; returns/starts the handler port for the
           just-added node) — do NOT mix with GetDeviceProc. NO RemDosEntry: the
           device stays mounted for the session. */
        { int c; for (c = 0; c < 7 && p->name[c]; c++) rcolon[c] = p->name[c];
          rcolon[c++] = ':'; rcolon[c] = 0; }
        {
            struct MsgPort *port = DeviceProc((STRPTR)rcolon);
            BPTR vb; LONG ok;
            if (!port) return FMT_ERR_FORMAT;
            vb = cstr_to_bstr(volname && volname[0] ? volname : p->name, volbstr);
            ok = DoPkt(port, ACTION_FORMAT, (LONG)vb, (LONG)p->dos_type, 0L, 0L, 0L);
            if (!ok) return FMT_ERR_FORMAT;
        }
        return FMT_OK;
    }
```

- [ ] **Step 3: Build for Amiga**

Run: `make`
Expected: clean build, `entry-order OK`. If `volbstr`/`env`/`p` scoping errors appear, ensure the new block is inside `format_partition` after `env` and `p` are initialized (it is, per the insertion point).

- [ ] **Step 4: Stage for FS-UAE**

Run: `make hd`
Expected: staged binary.

- [ ] **Step 5: Commit**

```bash
git add src/format.c
git commit -m "feat(format): mount fresh partition persistently under its real name (filesystem.resource + AddDosNode)"
```

---

### Task 6: Host-test the embedded-FS lifetime guard (regression)

**Files:**
- Modify: `tests/test_fsres.c` — add a test that `fsres_find_embedded` ignores a same-family entry whose seg is empty but picks a later valid one.

**Interfaces:**
- Consumes: `fsres_find_embedded` (Task 3).
- Produces: nothing.

- [ ] **Step 1: Add the regression test**

In `tests/test_fsres.c`, add before `main` and call it from `main`:
```c
static void test_find_embedded_skips_empty(void)
{
    RdbModel m; memset(&m, 0, sizeof m);
    static uint8_t seg[4] = {0,0,3,0xF3};
    m.num_fs = 2;
    m.fs[0].dos_type = 0x50465303u; m.fs[0].seg_data = 0;  m.fs[0].seg_len = 0;
    m.fs[1].dos_type = 0x50465303u; m.fs[1].seg_data = seg; m.fs[1].seg_len = 4;
    CHECK(fsres_find_embedded(&m, 0x50465303u) == &m.fs[1]);  /* skip empty fs[0] */
}
```
And add `test_find_embedded_skips_empty();` to `main` before the print.

- [ ] **Step 2: Run host tests**

Run: `./tests/run-host-tests.sh`
Expected: `test_fsres: ALL TESTS PASSED`.

- [ ] **Step 3: Commit**

```bash
git add tests/test_fsres.c
git commit -m "test(fsres): find_embedded skips empty same-family entries"
```

---

### Task 7: Version bump to 0.9 + ADF, on-target acceptance

**Files:**
- Modify: `src/gui.c` (window title `"HDPart 0.8"` → `"HDPart 0.9"`, About line), `Makefile` (`ADFVER = 0.8` → `0.9`, and the comment `"HDPart 0.8"` → `"HDPart 0.9"`).

**Interfaces:** none.

- [ ] **Step 1: Bump the three version spots**

```bash
cd /Users/sfs/Devel/party
perl -0pi -e 's/WA_Title, \(ULONG\)"HDPart 0\.8"/WA_Title, (ULONG)"HDPart 0.9"/' src/gui.c
perl -0pi -e 's/"HDPart 0\.8\\n"/"HDPart 0.9\\n"/' src/gui.c
perl -0pi -e 's/\("HDPart 0\.8"\)/("HDPart 0.9")/; s/^ADFVER = 0\.8/ADFVER = 0.9/m' Makefile
```

- [ ] **Step 2: Verify no 0.8 strings remain in those spots**

Run: `grep -n 'HDPart 0\.8\|ADFVER = 0\.8' src/gui.c Makefile`
Expected: no output.

- [ ] **Step 3: Build executable, host tests, and ADF**

Run:
```
make && ./tests/run-host-tests.sh && make adf
```
Expected: `entry-order OK`, `ALL TESTS PASSED`, `Built -> out/HDPart-0.9.adf  (+ bundled Libs/asl.library)`.

- [ ] **Step 4: Commit**

```bash
git add src/gui.c Makefile
git commit -m "chore: bump version to 0.9 (live-mount & format rework)"
```

- [ ] **Step 5: On-target acceptance checklist (A3000 tester)**

Hand the tester `out/HDPart-0.9.adf` and confirm:
1. Boot Workbench floppy; run HDPart from the second drive against a **wiped** disk.
2. On the blank disk, **New** and **Filesystems** are available immediately (no Split-then-fail dance).
3. Create a partition, add pfs3aio via Filesystems, assign it, Save.
4. Format → **prompted for a volume name** (pre-filled with the partition name); accept.
5. `Info` shows the volume **with the chosen name**; copy a file to it **without rebooting**.
6. Reboot **with no other PFS drive present**: the partition is **mounted and named**; if it's the boot partition, the machine **boots without crashing**.

---

## Self-Review

**Spec coverage:**
- §1 empty-disk init → Task 1. ✓
- §2 volume-name prompt → Task 2. ✓
- §3 fresh mount (filesystem.resource find/register, real-name node, persist, ACTION_FORMAT) → Tasks 3 (pure), 4 (OS resource/list), 5 (format path). ✓
- §4 module boundaries (pure vs OS) → fsres.h split + Tasks 3/4. ✓
- §5 error handling (cancel, NO_HANDLER, MAKENODE/ADDNODE, free-on-fail) → Tasks 2/5 + fsres_register. ✓
- §6 testing (host pure + on-target checklist) → Tasks 3/6 + Task 7 step 5. ✓
- §8 version 0.9 → Task 7. ✓

**Placeholder scan:** no TBD/TODO; all code blocks concrete. The one prose "Note for the implementer" in Task 5 is immediately resolved by Step 3 (explicit `DeviceProc` line). ✓

**Type consistency:** `FsHandlerFields`, `fsres_find_embedded`, `fsres_resolve_fields`, `fsres_bstr_eq`, `fsres_find_seglist`, `fsres_register`, `fsres_dosnode_exists` names/signatures identical across fsres.h (Task 3), fsres.c (Tasks 3/4), and format.c (Task 5). `FMT_ENV_LONGS`, `embedded_loadseg`, `cstr_to_bstr`, `volbstr`, `bind_handler` reused from existing format.c. ✓
