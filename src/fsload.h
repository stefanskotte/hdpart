#ifndef HDPART_FSLOAD_H
#define HDPART_FSLOAD_H
#include <stdint.h>
#include "rdb.h"

enum {
    FSL_OK = 0,
    FSL_EOPEN = -1,        /* could not open the file */
    FSL_EREAD = -2,        /* read error */
    FSL_ETOOBIG = -3,      /* exceeds FSLOAD_MAX_BYTES */
    FSL_ENOTLOADFILE = -4, /* not an AmigaDOS hunk load file */
    FSL_ENOMEM = -5,
    FSL_EFULL = -6,        /* model already has RDB_MAX_FS filesystems */
    FSL_ENOFS = -7         /* source disk has no embedded filesystem */
};

#define FSLOAD_MAX_BYTES (512u * 1024u)
#define HUNK_HEADER 0x000003F3u

const char *fsl_err_text(int rc);

/* Pure: 1 if buf starts with HUNK_HEADER and len is plausible, else 0. */
int fsload_is_hunk_file(const uint8_t *buf, uint32_t len);

/* Scan a loaded handler image for a version: the "$VER:" cookie, or failing
   that a "<name> <major>.<minor> (<date>)" idString. Returns the packed version
   (major<<16 | minor), or 0 if not found / unparseable. */
uint32_t fsload_parse_version(const uint8_t *buf, uint32_t len);

/* Detect the filesystem's DosType by scanning the handler binary for the
   dominant non-ROM FS family signature (PFS/SFS/PDS appearing as the high 3
   bytes of a DosType longword), mapped to that family's canonical DosType
   (PFS\3 / SFS\2 / PDS\3). Returns 0 if no known family is found. Pure. */
uint32_t fsload_detect_dostype(const uint8_t *buf, uint32_t len);

#ifdef HDPART_AMIGA
/* Load a filesystem handler from a file into out (out->seg_data heap-owned).
   dos_type is left as a placeholder (set later in the GUI). */
int fsload_from_file(const char *path, RdbFileSys *out);

/* Copy an embedded FS from another disk's RDB. Opens driver/unit transiently,
   parses, copies fs index `which` into out. */
int fsload_from_disk(const char *driver, uint32_t unit, int which, RdbFileSys *out);
#endif

#endif
