#include "fsload.h"
#include <string.h>

#ifdef HDPART_AMIGA
#include <proto/dos.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include "device.h"

/* PatchFlags bit positions — from mounter.c ProcessPatchFlags() and
   devices/hardblocks.h comment (e.g. 0x180 = SegList|GlobalVec).
   Bit 4 = dn_StackSize, bit 5 = dn_Priority, bit 8 = dn_GlobalVec. */
#define FSB_STACKSIZE  4
#define FSB_PRIORITY   5
#define FSB_SEGLIST    7
#define FSB_GLOBALVEC  8
#define FSF_STACKSIZE  (1u << FSB_STACKSIZE)   /* 0x010 */
#define FSF_PRIORITY   (1u << FSB_PRIORITY)    /* 0x020 */
#define FSF_SEGLIST    (1u << FSB_SEGLIST)     /* 0x080 */
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
    /* Detect the DosType from the handler binary (PFS\3 / SFS\2 / PDS\3); fall
       back to a PFS\3 placeholder if no known family is found (user can edit). */
    { uint32_t dt = fsload_detect_dostype(buf, (uint32_t)flen);
      out->dos_type = dt ? dt : 0x50465303u; }
    out->version     = fsload_parse_version(buf, (uint32_t)flen);
    /* PatchFlags: SegList | StackSize | Priority | GlobalVec.  SegList (0x80) is
       mandatory — the handler code lives in this RDB's LSEG chain, so the boot
       loader must patch dn_SegList from it; without it the FS is unusable and the
       partition will not mount after a reboot (field report 2026-06-25).  The
       other three are the handler params we provide.  Bit positions from
       mounter.c ProcessPatchFlags(). Matches fsres_register()'s live value. */
    out->patch_flags = FSF_SEGLIST | FSF_STACKSIZE | FSF_PRIORITY | FSF_GLOBALVEC; /* 0x1B0 */
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

int fsload_from_disk(const char *driver, uint32_t unit, int which, RdbFileSys *out)
{
    RdbModel scratch;
    int rc;
    RdbFileSys *src;
    DeviceHandle *h;

    memset(&scratch, 0, sizeof scratch);
    h = dev_open(driver, (ULONG)unit);
    if (!h) return FSL_EOPEN;
    rc = rdb_parse(&scratch, dev_block_io, h);
    dev_close(h);
    if (rc != RDB_OK) { rdb_model_free(&scratch); return FSL_ENOFS; }
    if (scratch.num_fs <= 0 || which < 0 || which >= scratch.num_fs) {
        rdb_model_free(&scratch); return FSL_ENOFS;
    }
    src = &scratch.fs[which];
    memset(out, 0, sizeof *out);
    *out = *src;                        /* shallow copy: scalars + name */
    out->seg_data = (uint8_t *)rdb_seg_alloc(src->seg_len ? src->seg_len : 1);
    if (!out->seg_data) { rdb_model_free(&scratch); return FSL_ENOMEM; }
    memcpy(out->seg_data, src->seg_data, src->seg_len);
    out->source = RDB_FS_COPIED;
    rdb_model_free(&scratch);           /* frees scratch's own seg_data; out->seg_data is safe */
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

/* Parse "<digits>.<digits>" starting at buf[pos]. On success returns 1 and
   writes (major<<16|minor) to *ver; else returns 0. *endp gets the index just
   past the digits scanned (so the caller can advance). */
static int parse_nm(const uint8_t *buf, uint32_t pos, uint32_t end,
                    uint32_t *ver, uint32_t *endp)
{
    uint32_t major = 0, minor = 0, p = pos, p2;
    if (!(p < end && buf[p] >= '0' && buf[p] <= '9')) { *endp = pos + 1; return 0; }
    while (p < end && buf[p] >= '0' && buf[p] <= '9') {
        major = (major <= 6553u) ? major * 10u + (uint32_t)(buf[p] - '0') : 65535u;
        p++;
    }
    if (!(p < end && buf[p] == '.')) { *endp = p; return 0; }
    p2 = p + 1;
    if (!(p2 < end && buf[p2] >= '0' && buf[p2] <= '9')) { *endp = p2; return 0; }
    while (p2 < end && buf[p2] >= '0' && buf[p2] <= '9') {
        minor = (minor <= 6553u) ? minor * 10u + (uint32_t)(buf[p2] - '0') : 65535u;
        p2++;
    }
    if (major > 65535u) major = 65535u;
    if (minor > 65535u) minor = 65535u;
    *ver = (major << 16) | minor;
    *endp = p2;
    return 1;
}

uint32_t fsload_parse_version(const uint8_t *buf, uint32_t len)
{
    static const uint8_t cookie[5] = {0x24,0x56,0x45,0x52,0x3A};  /* "$VER:" */
    uint32_t i, end, ver, nx;

    if (!buf || len < 5) return 0;

    /* Pass 1: the "$VER:" cookie — first N.M within 200 bytes after it. */
    for (i = 0; i <= len - 5; i++) {
        if (buf[i]==cookie[0] && buf[i+1]==cookie[1] && buf[i+2]==cookie[2] &&
            buf[i+3]==cookie[3] && buf[i+4]==cookie[4]) {
            uint32_t pos = i + 5;
            end = pos + 200u; if (end > len) end = len;
            while (pos < end) {
                if (buf[pos] >= '0' && buf[pos] <= '9') {
                    if (parse_nm(buf, pos, end, &ver, &nx)) return ver;
                    pos = nx;
                } else pos++;
            }
            break;   /* cookie found but no N.M — try pass 2 */
        }
    }

    /* Pass 2 (no "$VER:" cookie, e.g. SmartFilesystem): the romtag idString
       form "<name> <major>.<minor> (<date>)" — a space, then N.M, then " (".
       The trailing " (" (a date) makes the match specific and avoids picking up
       unrelated "N.M" numbers. */
    if (len >= 4) {
        for (i = 0; i + 1 < len; i++) {
            if (buf[i] != ' ') continue;
            if (parse_nm(buf, i + 1, len, &ver, &nx)) {
                if (nx + 1 < len && buf[nx] == ' ' && buf[nx+1] == '(')
                    return ver;
            }
        }
    }
    return 0;
}

uint32_t fsload_detect_dostype(const uint8_t *buf, uint32_t len)
{
    uint32_t cPFS = 0, cSFS = 0, cPDS = 0, i;
    if (!buf || len < 4) return 0;
    /* Count DosType-family signatures: a 3-letter family tag followed by a low
       version byte (a handler embeds the DosType longwords it recognises). */
    for (i = 0; i + 4 <= len; i++) {
        uint8_t a = buf[i], b = buf[i+1], c = buf[i+2], v = buf[i+3];
        if (v > 15) continue;
        if      (a=='P' && b=='F' && c=='S') cPFS++;
        else if (a=='S' && b=='F' && c=='S') cSFS++;
        else if (a=='P' && b=='D' && c=='S') cPDS++;
    }
    /* Dominant family -> its canonical DosType. Require >= 2 hits so a single
       stray longword in unrelated data does not misclassify. */
    if (cSFS >= 2 && cSFS >= cPFS && cSFS >= cPDS) return 0x53465302u; /* SFS\2 */
    if (cPFS >= 2 && cPFS >= cPDS)                 return 0x50465303u; /* PFS\3 */
    if (cPDS >= 2)                                 return 0x50445303u; /* PDS\3 */
    return 0;
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
