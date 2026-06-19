/* HDPart partition formatter. Pure env builder (host-testable) + OS-bound
   live-mount + ACTION_FORMAT guarded by HDPART_AMIGA. Mirrors discover.c. */
#ifdef HDPART_AMIGA
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/execbase.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/filehandler.h>
#include <libraries/expansion.h>
#include <resources/filesysres.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/expansion.h>
#include "safety.h"
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

#ifdef HDPART_AMIGA

/* ACTION_FORMAT is defined in dos/dosextens.h as 1020 (confirmed V36+).
   The guard below is a safety net in case an older SDK omits it. */
#ifndef ACTION_FORMAT
#define ACTION_FORMAT 1020L
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

/* Pick a DOS device name not currently in use (HDP0..HDP999).
   out must be >= 10 bytes; outcolon must be >= 12 bytes.
   Returns 1 on success, 0 if all 1000 names are taken. */
static int pick_free_devname(char *out, char *outcolon)
{
    int n;
    for (n = 0; n < 1000; n++) {
        char nm[10];
        int  p = 0, v = n, k;
        nm[p++] = 'H'; nm[p++] = 'D'; nm[p++] = 'P';
        /* Append decimal digits of n (0..999) without libc. */
        if (v >= 100) { nm[p++] = (char)('0' + v / 100); v %= 100; }
        if (v >= 10)  { nm[p++] = (char)('0' + v / 10);  v %= 10; }
        nm[p++] = (char)('0' + v);
        nm[p]   = 0;
        {
            struct DosList *dl = LockDosList(LDF_DEVICES | LDF_READ);
            struct DosList *f  = FindDosEntry(dl, (STRPTR)nm, LDF_DEVICES);
            UnLockDosList(LDF_DEVICES | LDF_READ);
            if (!f) {
                for (k = 0; nm[k]; k++) { out[k] = nm[k]; outcolon[k] = nm[k]; }
                out[k] = 0; outcolon[k] = ':'; outcolon[k + 1] = 0;
                return 1;
            }
        }
    }
    return 0;
}

/* Strategy 3: reconstruct a seglist from an embedded FSHD/LSEG hunk image.
   Called only on Kickstart V36+ (dos.library V36 added InternalLoadSeg).

   InternalLoadSeg ABI confirmed from:
     m68k-amiga-elf/sys-include/inline/dos.h lines 552-554
     m68k-amiga-elf/sys-include/clib/dos_protos.h "Image Management" section
   Signature:
     BPTR InternalLoadSeg(BPTR fh, BPTR table, const LONG *funcarray, LONG *stack)
     registers: fh->d0, table->a0, funcarray->a1, stack->a2
   funcarray convention (Amiga NDK ROM Kernel autodoc, InternalLoadSeg):
     funcarray[0] = read(fh, buf, len): called d1=fh(our cursor*), d2=buf, d3=len;
                    returns d0=actual bytes copied (LONG).
     funcarray[1] = alloc(size, memreqs): called d0=size, d1=memreqs;
                    returns d0=ptr (NULL on failure).
     funcarray[2] = free(mem, size): called a1=mem, d0=size; no return value.
   Under m68k-amiga-elf-gcc the hooks must be declared with explicit asm register
   constraints — the OS calls them with bare JSR through the funcarray pointer,
   not via a C ABI frame.

   InternalUnLoadSeg ABI:
     BOOL InternalUnLoadSeg(BPTR seglist, VOID (*freefunc)())
     registers: seglist->d1, freefunc->a1
*/

/* In-memory stream cursor for embedded_loadseg. */
struct MemCursor {
    const uint8_t *data;
    uint32_t       off;
    uint32_t       len;
};

/* read hook: InternalLoadSeg calls this with d1=fh(our cursor*), d2=buf, d3=count.
   We capture the args via explicit register variables at function entry.
   Returns d0=actual bytes copied (LONG). */
static LONG __attribute__((used)) mem_read(void)
{
    register struct MemCursor *cur __asm("d1");
    register uint8_t          *buf __asm("d2");
    register LONG              cnt __asm("d3");
    uint32_t avail, i;
    /* Force the compiler to read these before any stores (barrier). */
    struct MemCursor *c = cur;
    uint8_t          *b = buf;
    LONG              n = cnt;
    if (!c || n <= 0) return 0;
    avail = c->len - c->off;
    if ((uint32_t)n > avail) n = (LONG)avail;
    for (i = 0; i < (uint32_t)n; i++) b[i] = c->data[c->off + i];
    c->off += (uint32_t)n;
    return n;
}

/* alloc hook: called with d0=size, d1=memreqs; returns d0=ptr. */
static void * __attribute__((used)) mem_alloc(void)
{
    register ULONG size __asm("d0");
    register ULONG reqs __asm("d1");
    ULONG s = size, r = reqs;
    return AllocMem(s, r | MEMF_PUBLIC | MEMF_CLEAR);
}

/* free hook: called with a1=mem, d0=size; no return. */
static void __attribute__((used)) mem_free(void)
{
    register void  *mem  __asm("a1");
    register ULONG  size __asm("d0");
    void  *m = mem;
    ULONG  s = size;
    FreeMem(m, s);
}

/* free hook for InternalUnLoadSeg (same convention as mem_free). */
static void __attribute__((used)) mem_free_for_unload(void)
{
    register void  *mem  __asm("a1");
    register ULONG  size __asm("d0");
    void  *m = mem;
    ULONG  s = size;
    FreeMem(m, s);
}

/* funcarray passed to InternalLoadSeg: pointers to the three hooks above. */
static const LONG g_iloadseg_funcs[3] = {
    (LONG)mem_read,
    (LONG)mem_alloc,
    (LONG)mem_free
};

/* Reconstruct a runnable seglist from a raw HUNK image in memory using
   InternalLoadSeg with in-memory read/alloc/free hooks.
   Returns a BPTR seglist (non-zero) on success, or 0 on failure/unsupported.
   On success the seglist is owned by the caller; free with InternalUnLoadSeg if
   the mount never takes ownership. */
static BPTR embedded_loadseg(const uint8_t *data, uint32_t len)
{
    struct MemCursor cur;
    LONG             stack_dummy = 0;

    /* InternalLoadSeg was added in dos.library V36. Our floor is V37, so this
       guard always passes in practice — but we assert rather than assume.
       SysBase is a global declared in startup.c (struct ExecBase *). */
    if (SysBase->LibNode.lib_Version < 36) return 0;

    cur.data = data;
    cur.off  = 0;
    cur.len  = len;

    /* Pass the cursor as the "fh" BPTR (d0); InternalLoadSeg hands it to
       our read hook verbatim, which treats it as a struct MemCursor *. */
    return InternalLoadSeg((BPTR)&cur, 0, g_iloadseg_funcs, &stack_dummy);
}

/* Bind a filesystem handler (SegList + launch params) for dos_type into dn.
   This only finds the CODE to run; the formatted volume's type is ALWAYS the
   selected dos_type (carried in the env + ACTION_FORMAT). Matching is by FS
   FAMILY (top 3 bytes, e.g. "DOS"/"PFS"/"SFS"), so FFS-Intl uses the FFS handler
   and a future PFS\x uses a PFS handler — never cross-family, never altering the
   chosen type.

   Strategy 1 (preferred): clone the handler fields from a LIVE mounted partition
   of the same family. Every field is OS-validated and proven to launch, and it
   covers custom filesystems (e.g. a PFS handler loaded from a booted disk's RDB)
   as well as ROM FFS. Reading another node's seglist pointer is harmless even if
   that node is the boot disk.
   Strategy 2 (fallback): the FileSystem.resource entry's fse_SegList field
   directly — AmigaOS 3.2 leaves fse_PatchFlags == 0 but still populates
   fse_SegList (the older "patch by PatchFlags bits" approach binds nothing here).
   Strategy 3 (last resort): reconstruct a seglist from an embedded FSHD/LSEG
   hunk image in this RDB model via InternalLoadSeg. Enables Format for a custom
   FS (e.g. PFS3) that is embedded in the RDB but was never loaded on the running
   Amiga. Requires dos.library V36+ (Kickstart 2.0+), which our V37 floor ensures.

   Returns 1 if a SegList was bound, else 0 (caller -> FMT_ERR_NO_HANDLER). */
static int bind_handler(struct DeviceNode *dn, ULONG dos_type,
                        const RdbModel *model)
{
    ULONG fam = dos_type & 0xFFFFFF00u;

    /* Strategy 1: clone from a live mount of the same FS family. */
    {
        struct DosList *e = LockDosList(LDF_DEVICES | LDF_READ);
        int   guard = 0, found = 0;
        ULONG type = 0, seg = 0, gv = 0;
        LONG  stack = 0, pri = 0;
        while ((e = NextDosEntry(e, LDF_DEVICES | LDF_READ)) != 0 && ++guard < 256) {
            struct FileSysStartupMsg *fssm;
            struct DosEnvec *env;
            struct DeviceNode *en;
            BPTR startup = e->dol_misc.dol_handler.dol_Startup;
            if (!startup || TypeOfMem((void *)BADDR(startup)) == 0) continue;
            fssm = (struct FileSysStartupMsg *)BADDR(startup);
            if (!fssm->fssm_Environ || TypeOfMem((void *)BADDR(fssm->fssm_Environ)) == 0) continue;
            env = (struct DosEnvec *)BADDR(fssm->fssm_Environ);
            if (env->de_TableSize < DE_DOSTYPE) continue;
            if ((env->de_DosType & 0xFFFFFF00u) != fam) continue;
            en = (struct DeviceNode *)e;        /* DLT_DEVICE DosList aliases DeviceNode */
            if (!en->dn_SegList) continue;
            type = en->dn_Type; seg = (ULONG)en->dn_SegList; gv = (ULONG)en->dn_GlobalVec;
            stack = en->dn_StackSize; pri = en->dn_Priority; found = 1; break;
        }
        UnLockDosList(LDF_DEVICES | LDF_READ);
        if (found) {
            dn->dn_Type      = type;
            dn->dn_SegList   = (BPTR)seg;
            dn->dn_GlobalVec = (BPTR)gv;
            if (stack > 0) dn->dn_StackSize = stack;
            if (pri   > 0) dn->dn_Priority  = pri;
            if (dn->dn_StackSize == 0) dn->dn_StackSize = 4096;
            if (dn->dn_Priority  == 0) dn->dn_Priority  = 10;
            return 1;
        }
    }

    /* Strategy 2: FileSystem.resource fse_SegList (PatchFlags is 0 on 3.2). */
    {
        struct FileSysResource *fsr = (struct FileSysResource *)OpenResource((STRPTR)FSRNAME);
        struct FileSysEntry *fse;
        if (fsr) {
        Forbid();
        for (fse = (struct FileSysEntry *)fsr->fsr_FileSysEntries.lh_Head;
             fse->fse_Node.ln_Succ;
             fse = (struct FileSysEntry *)fse->fse_Node.ln_Succ) {
            if (fse->fse_DosType != dos_type && (fse->fse_DosType & 0xFFFFFF00u) != fam) continue;
            if (!fse->fse_SegList) continue;
            dn->dn_SegList   = fse->fse_SegList;
            dn->dn_GlobalVec = (BPTR)-1;        /* FFS/PFS handlers are not BCPL */
            if (dn->dn_StackSize == 0) dn->dn_StackSize = 4096;
            if (dn->dn_Priority  == 0) dn->dn_Priority  = 10;
            Permit();
            return 1;
        }
        Permit();
        } /* if (fsr) */
    }

    /* Strategy 3: reconstruct a seglist from an embedded FSHD/LSEG hunk image
       in this RDB model. Used when the FS was never loaded on the running Amiga
       (no live mount of that family, no FileSystem.resource entry). */
    if (model) {
        int i;
        for (i = 0; i < model->num_fs; i++) {
            const RdbFileSys *fs = &model->fs[i];
            BPTR seg;
            if ((fs->dos_type & 0xFFFFFF00u) != fam) continue;
            if (!fs->seg_data || fs->seg_len == 0) continue;
            seg = embedded_loadseg(fs->seg_data, fs->seg_len);
            if (!seg) continue;
            dn->dn_SegList   = seg;
            dn->dn_GlobalVec = (BPTR)(ULONG)(fs->dn_globalvec ? fs->dn_globalvec : (uint32_t)-1);
            dn->dn_StackSize = fs->dn_stack ? (LONG)fs->dn_stack : 4096;
            dn->dn_Priority  = fs->dn_pri   ? (LONG)fs->dn_pri   : 10;
            return 1;
        }
    }

    return 0;
}

FmtResult format_partition(const char *driver, uint32_t unit,
                           const RdbModel *m, int part_index, const char *volname)
{
    struct Library *ExpansionBase;
    uint32_t env[FMT_ENV_LONGS];
    static ULONG pp[4 + FMT_ENV_LONGS];   /* static: keep off the 4KB stack */
    static char  driverbuf[40];
    static char  devname[10];              /* ephemeral "HDP0" */
    static char  devcolon[12];             /* ephemeral "HDP0:" */
    static UBYTE volbstr[34];
    struct DeviceNode *dn;
    const RdbPartition *p;
    int i, k;

    if (format_build_envec(m, part_index, env) != 0) return FMT_ERR_RANGE;
    p = &m->parts[part_index];

    /* If THIS partition's blocks are already mounted (matched name-independently
       by driver+unit+cylinder overlap — handles auto-mounted and renamed/_N
       collisions), reformat the EXISTING live volume the way C:Format/Workbench
       do: inhibit it, ACTION_FORMAT, un-inhibit. We must NOT add a second node on
       top of mounted blocks. The boot device is already hard-blocked upstream
       (safety_classify in the GUI), and the user has confirmed the destructive
       action, so reformatting a non-boot mounted volume is the intended path. */
    {
        static char livecolon[10];
        char livename[8];
        if (safety_partition_mounted(driver, unit, p->low_cyl, p->high_cyl, livename)) {
            struct MsgPort *port;
            BPTR vb; LONG ok; int kk;
            for (kk = 0; kk < 7 && livename[kk]; kk++) livecolon[kk] = livename[kk];
            livecolon[kk++] = ':'; livecolon[kk] = 0;
            /* Reformat the EXISTING mount only if it has a live handler to talk
               to. DeviceProc returns the handler's port, or NULL when the DOS
               node exists with NO handler bound (dn_SegList == 0) — which is what
               happens when the controller mounts an RDB partition but does NOT
               load the embedded filesystem from its FSHD (confirmed with
               FS-UAE's uaehf.device: the node mounts as NDOS, DeviceProc == NULL).
               In that case there is nothing to ACTION_FORMAT, so fall through to
               the fresh path: it binds OUR handler via Strategy-3 (InternalLoadSeg
               the embedded FSHD/LSEG) and formats via an ephemeral mount — exactly
               what the embedded-FS support is for. The handler-less node has no
               process touching the blocks, so an ephemeral handler over them is
               safe. (Verified on-target: PFS3 embedded -> Format writes a valid
               PFS\1 volume via this path.) */
            port = DeviceProc((STRPTR)livecolon);
            if (port) {
                Inhibit((STRPTR)livecolon, DOSTRUE);
                vb = cstr_to_bstr(volname && volname[0] ? volname : p->name, volbstr);
                ok = DoPkt(port, ACTION_FORMAT, (LONG)vb, (LONG)p->dos_type, 0L, 0L, 0L);
                Inhibit((STRPTR)livecolon, DOSFALSE);
                return ok ? FMT_OK : FMT_ERR_FORMAT;
            }
            /* else: no live handler -> fall through to the fresh/Strategy-3 path */
        }
    }

    /* Copy driver into a stable static buffer for MakeDosNode parmPacket. */
    for (k = 0; k < 39 && driver[k]; k++) driverbuf[k] = driver[k];
    driverbuf[k] = 0;

    /* Pick a guaranteed-unique ephemeral device name (HDP0..HDP999).
       Never touches the RDB partition name; the partition name is used only
       as the volume label written by ACTION_FORMAT (persistent; user-visible).
       On reboot the RDB name mounts normally — the ephemeral name is discarded. */
    if (!pick_free_devname(devname, devcolon)) return FMT_ERR_ADDNODE;

    ExpansionBase = OpenLibrary((STRPTR)"expansion.library", 37);
    if (!ExpansionBase) return FMT_ERR_MAKENODE;

    /* MakeDosNode parmPacket: pp[0]=devname (C string), pp[1]=driver (C string),
       pp[2]=unit, pp[3]=flags=0, pp[4..]=env starting at de_TableSize.
       pp[0] is the ephemeral name (HDP0), NOT the partition name (DH5). */
    pp[0] = (ULONG)devname;
    pp[1] = (ULONG)driverbuf;
    pp[2] = (ULONG)unit;
    pp[3] = 0;
    for (i = 0; i < FMT_ENV_LONGS; i++) pp[4 + i] = env[i];

    dn = (struct DeviceNode *)MakeDosNode((APTR)pp);
    if (!dn) { CloseLibrary(ExpansionBase); return FMT_ERR_MAKENODE; }

    if (!bind_handler(dn, (ULONG)p->dos_type, m)) {
        /* No handler found for this dostype's family (no live mount of that
           family, no FileSystem.resource entry, and no embedded FSHD/LSEG in
           this RDB model). The node is not added to DOS; do not call AddDosNode.
           MakeDosNode allocation is intentionally not freed here: there is no
           public FreeDosNode API, this path is rare and one-shot, and the
           caller can correct the error (e.g. install the FS first). */
        CloseLibrary(ExpansionBase);
        return FMT_ERR_NO_HANDLER;
    }
    /* A1 lifetime note for Strategy-3: if bind_handler used embedded_loadseg,
       dn->dn_SegList now points to a freshly InternalLoadSeg'd seglist.  If
       AddDosNode below fails or ACTION_FORMAT later fails, that seglist would
       leak.  Consistent with this file's existing error-handling philosophy
       (no FreeDosNode API; rare one-shot path; see comment above), we accept
       the leak on those further error exits rather than add fragile tracking.
       The seglist is small (<< 1 KB for a typical PFS handler) and the error
       paths are exceptional; they do not loop.  A full teardown would call
       InternalUnLoadSeg(dn->dn_SegList, mem_free_for_unload) on those exits,
       but we deliberately match the existing "intentionally not freed" policy. */

    if (!AddDosNode(0, ADNF_STARTPROC, dn)) {
        CloseLibrary(ExpansionBase);
        return FMT_ERR_ADDNODE;
    }
    CloseLibrary(ExpansionBase);   /* node is now owned by DOS */

    /* Format via the ephemeral device (devcolon = "HDP0:").
       All three of DeviceProc, Inhibit(TRUE), Inhibit(FALSE) use devcolon.
       The volume LABEL (written to disk by ACTION_FORMAT, user-visible) is the
       partition name (p->name) or the caller-supplied volname — never the
       ephemeral device name. */
    {
        struct MsgPort *port = DeviceProc((STRPTR)devcolon);
        BPTR  vb;
        LONG  ok;
        if (!port) return FMT_ERR_FORMAT;
        Inhibit((STRPTR)devcolon, DOSTRUE);
        vb = cstr_to_bstr(volname && volname[0] ? volname : p->name, volbstr);
        ok = DoPkt(port, ACTION_FORMAT, (LONG)vb, (LONG)p->dos_type, 0L, 0L, 0L);
        Inhibit((STRPTR)devcolon, DOSFALSE);

        /* D1 fix: remove the ephemeral node from the DosList so "HDP0:" does not
           persist as a duplicate live mount for the rest of the session.
           NOTE — UNVERIFIED FRESH PATH: this code has not been exercised on real
           hardware.  RemDosEntry only removes the list entry (stops new opens of
           "HDPn:"); it does NOT terminate the handler process that was started by
           ADNF_STARTPROC.  A full teardown would additionally require sending
           ACTION_DIE to the handler's port, but not all handlers implement it and
           the risk of a hang outweighs the benefit for this one-shot tool.  The
           handler process will exit naturally when it receives no further packets
           and its port is forgotten (or at the next reboot). */
        LockDosList(LDF_DEVICES | LDF_WRITE);
        RemDosEntry((struct DosList *)dn);
        UnLockDosList(LDF_DEVICES | LDF_WRITE);

        if (!ok) return FMT_ERR_FORMAT;
    }
    return FMT_OK;
}

#endif /* HDPART_AMIGA */
