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
#include <dos/dos.h>
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
