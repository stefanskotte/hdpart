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

   Returns 1 if a SegList was bound, else 0 (caller -> FMT_ERR_NO_HANDLER). */
static int bind_handler(struct DeviceNode *dn, ULONG dos_type)
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
            return 1;
        }
    }

    /* Strategy 2: FileSystem.resource fse_SegList (PatchFlags is 0 on 3.2). */
    {
        struct FileSysResource *fsr = (struct FileSysResource *)OpenResource((STRPTR)FSRNAME);
        struct FileSysEntry *fse;
        if (!fsr) return 0;
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
            port = DeviceProc((STRPTR)livecolon);
            if (!port) return FMT_ERR_FORMAT;
            Inhibit((STRPTR)livecolon, DOSTRUE);
            vb = cstr_to_bstr(volname && volname[0] ? volname : p->name, volbstr);
            ok = DoPkt(port, ACTION_FORMAT, (LONG)vb, (LONG)p->dos_type, 0L, 0L, 0L);
            Inhibit((STRPTR)livecolon, DOSFALSE);
            return ok ? FMT_OK : FMT_ERR_FORMAT;
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

    if (!bind_handler(dn, (ULONG)p->dos_type)) {
        /* No handler found for this dostype's family (no live mount of that
           family and no FileSystem.resource entry). For a custom FS not yet
           loaded this needs embedded-FS support (feature B).
           The node is not added to DOS; do not call AddDosNode.
           MakeDosNode allocation is intentionally not freed here: there is no
           public FreeDosNode API, this path is rare and one-shot, and the
           caller can correct the error (e.g. load the FS first). */
        CloseLibrary(ExpansionBase);
        return FMT_ERR_NO_HANDLER;
    }

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
        if (!ok) return FMT_ERR_FORMAT;
    }
    return FMT_OK;
}

#endif /* HDPART_AMIGA */
