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

#ifdef HDPART_AMIGA
#include <proto/exec.h>
#include <proto/dos.h>
#include <exec/memory.h>
#include <exec/lists.h>
#include <dos/filehandler.h>
#include <resources/filesysres.h>

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
