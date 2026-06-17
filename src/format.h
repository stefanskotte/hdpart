#ifndef HDPART_FORMAT_H
#define HDPART_FORMAT_H
#include <stdint.h>
#include "rdb.h"

typedef enum {
    FMT_OK = 0,
    FMT_ERR_RANGE,           /* bad part_index */
    FMT_ERR_NO_HANDLER,      /* no FS handler for this DosType's family */
    FMT_ERR_MAKENODE,        /* MakeDosNode / OpenLibrary failed */
    FMT_ERR_ADDNODE,         /* AddDosNode failed / no free ephemeral name */
    FMT_ERR_FORMAT           /* DeviceProc or ACTION_FORMAT packet failed */
} FmtResult;

/* DosEnvec built up to and including de_DosType (index DE_DOSTYPE=16).
   env[0] = de_TableSize = 16; env[1..16] = the fields. 17 longwords total. */
#define FMT_ENV_LONGS 17

/* Pure: build the DOS environment vector for a partition from the model+geometry.
   Returns 0 on success, -1 if part_index is out of range. Host-testable. */
int format_build_envec(const RdbModel *m, int part_index, uint32_t env[FMT_ENV_LONGS]);

#ifdef HDPART_AMIGA
/* OS: format the partition live (no reboot). If its blocks are already mounted,
   reformat the existing volume the C:Format/Workbench way (Inhibit +
   ACTION_FORMAT + Inhibit); otherwise MakeDosNode + bind the FS-family handler +
   AddDosNode(STARTPROC) + Inhibit + ACTION_FORMAT + Inhibit. The volume is always
   formatted as the partition's selected DosType; volname = volume label (no
   colon). Returns FmtResult. */
FmtResult format_partition(const char *driver, uint32_t unit,
                           const RdbModel *m, int part_index, const char *volname);
#endif

#endif /* HDPART_FORMAT_H */
