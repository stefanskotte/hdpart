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
