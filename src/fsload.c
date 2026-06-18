#include "fsload.h"
#include <string.h>

#ifdef HDPART_AMIGA
#include <proto/dos.h>
#include <exec/memory.h>
#include <dos/dos.h>

/* PatchFlags bit positions — from mounter.c ProcessPatchFlags() and
   devices/hardblocks.h comment (e.g. 0x180 = SegList|GlobalVec).
   Bit 4 = dn_StackSize, bit 5 = dn_Priority, bit 8 = dn_GlobalVec. */
#define FSB_STACKSIZE  4
#define FSB_PRIORITY   5
#define FSB_GLOBALVEC  8
#define FSF_STACKSIZE  (1u << FSB_STACKSIZE)   /* 0x010 */
#define FSF_PRIORITY   (1u << FSB_PRIORITY)    /* 0x020 */
#define FSF_GLOBALVEC  (1u << FSB_GLOBALVEC)   /* 0x100 */

int fsload_from_file(const char *path, RdbFileSys *out)
{
    BPTR fh;
    LONG flen, got;
    uint8_t *buf;
    const char *fp;

    memset(out, 0, sizeof *out);

    fh = Open((CONST_STRPTR)path, MODE_OLDFILE);
    if (!fh) return FSL_EOPEN;

    /* Determine file size: seek to end, capture position, seek back. */
    Seek(fh, 0, OFFSET_END);
    flen = Seek(fh, 0, OFFSET_BEGINNING);  /* returns previous (end) position */
    if (flen <= 0) { Close(fh); return FSL_EREAD; }
    if ((uint32_t)flen > FSLOAD_MAX_BYTES) { Close(fh); return FSL_ETOOBIG; }

    buf = (uint8_t *)rdb_seg_alloc((uint32_t)flen);
    if (!buf) { Close(fh); return FSL_ENOMEM; }

    got = Read(fh, buf, flen);
    Close(fh);
    if (got != flen) { rdb_seg_free(buf); return FSL_EREAD; }

    if (!fsload_is_hunk_file(buf, (uint32_t)flen)) {
        rdb_seg_free(buf);
        return FSL_ENOTLOADFILE;
    }

    out->seg_data    = buf;
    out->seg_len     = (uint32_t)flen;
    out->dos_type    = 0x50465303u;   /* placeholder PFS\3; caller sets the real type */
    out->version     = 0;
    /* PatchFlags: StackSize | Priority | GlobalVec — the three handler params
       we provide.  Bit positions from mounter.c ProcessPatchFlags():
       FSF_STACKSIZE (0x010) | FSF_PRIORITY (0x020) | FSF_GLOBALVEC (0x100). */
    out->patch_flags = FSF_STACKSIZE | FSF_PRIORITY | FSF_GLOBALVEC; /* 0x130 */
    out->dn_stack    = 4096;
    out->dn_pri      = 10;
    out->dn_globalvec = (uint32_t)-1;  /* -1 = not a BCPL program */
    out->source      = RDB_FS_FILE;

    /* Derive display name from the file part of path (after last '/' or ':'). */
    fp = path;
    { const char *p = path; for (; *p; p++) if (*p == '/' || *p == ':') fp = p + 1; }
    { int i = 0;
      for (; fp[i] && i < RDB_NAME_LEN - 1; i++) out->name[i] = fp[i];
      out->name[i] = 0; }

    return FSL_OK;
}
#endif /* HDPART_AMIGA */

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
