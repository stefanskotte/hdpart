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

/* Find the ROM FFS handler seglist for dos_type and patch it into dn. Returns 1
   if a handler was found and applied, 0 otherwise.
   Verified against real headers:
   - FileSysEntry patchable run: fse_Type .. fse_GlobalVec (9 longwords, bits 0-8)
   - DeviceNode:                 dn_Type  .. dn_GlobalVec  (same layout, same count)
   Copying src[bit] -> dst[bit] where bit is set in fse_PatchFlags is valid. */
static int bind_rom_handler(struct DeviceNode *dn, ULONG dos_type)
{
    struct FileSysResource *fsr;
    struct FileSysEntry *fse;
    ULONG *src, *dst;
    int applied = 0;

    fsr = (struct FileSysResource *)OpenResource((STRPTR)FSRNAME);
    if (!fsr) return 0;

    Forbid();
    for (fse = (struct FileSysEntry *)fsr->fsr_FileSysEntries.lh_Head;
         fse->fse_Node.ln_Succ;
         fse = (struct FileSysEntry *)fse->fse_Node.ln_Succ) {
        if (fse->fse_DosType != dos_type) continue;
        /* Patch DeviceNode fields selected by fse_PatchFlags. The patchable run
           starts at fse_Type (mirrors dn_Type) and is contiguous (9 longwords). */
        src = (ULONG *)&fse->fse_Type;
        dst = (ULONG *)&dn->dn_Type;
        {
            ULONG flags = fse->fse_PatchFlags;
            int bit;
            for (bit = 0; bit < 9; bit++)
                if (flags & (1u << bit)) dst[bit] = src[bit];
        }
        /* Bit 7 of PatchFlags = SegList must be patched in for the handler to
           launch. Reject entries that matched DosType but carry no SegList;
           keep scanning so a later (better) entry can still satisfy the request. */
        if (fse->fse_PatchFlags & (1u << 7)) applied = 1;
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
    static char  devname[10];              /* ephemeral "HDP0" */
    static char  devcolon[12];             /* ephemeral "HDP0:" */
    static UBYTE volbstr[34];
    struct DeviceNode *dn;
    const RdbPartition *p;
    int i, k;

    if (format_build_envec(m, part_index, env) != 0) return FMT_ERR_RANGE;
    p = &m->parts[part_index];

    /* Refuse if THIS partition (these blocks) is already mounted — name-
       independent: matches by driver+unit+cylinder overlap, so an unrelated
       same-named device on another disk does not false-trigger, and a renamed
       collision (DH5_1) is still detected. */
    {
        char livename[8];
        if (safety_partition_mounted(driver, unit, p->low_cyl, p->high_cyl, livename))
            return FMT_ERR_ALREADY_MOUNTED;
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

    if (!bind_rom_handler(dn, (ULONG)p->dos_type)) {
        /* No ROM handler for this dostype: non-ROM FS, not supported in v1.
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
