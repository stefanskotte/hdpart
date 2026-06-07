#ifndef HDPART_DISCOVER_H
#define HDPART_DISCOVER_H

#include <exec/types.h>

#define DISC_MAX        32
#define DISC_DRIVER_LEN 40
#define DISC_LABEL_LEN  40

typedef enum {
    DST_UNKNOWN = 0,
    DST_NOMEDIA,   /* device opened but no media / geometry failed */
    DST_BLANK,     /* media present, no valid RDB */
    DST_RDB,       /* media present, valid RDB found */
    DST_MOUNTED    /* referenced by a mounted AmigaDOS device */
} DiskStatus;

typedef struct {
    char       driver[DISC_DRIVER_LEN];  /* exec device name, e.g. "scsi.device" */
    ULONG      unit;
    ULONG      size_mb;                   /* from geometry, 0 if unknown */
    DiskStatus status;
    char       label[DISC_LABEL_LEN];     /* model string / DOS name for display */
} DiscDisk;

/* ---- pure helpers (no OS calls; unit-tested on host) ---- */

/* Convert a BCPL string (length byte followed by chars) to a C string.
   `bcpl` points at the length byte. Truncates to outsz-1. */
void disc_bcpl_to_c(const unsigned char *bcpl, char *out, int outsz);

/* Find driver+unit in list; return index or -1. Case-sensitive on driver. */
int disc_find(const DiscDisk list[], int count, const char *driver, ULONG unit);

/* Convert a block count + block size to whole MB (floor), 32-bit safe. */
ULONG disc_blocks_to_mb(ULONG total_blocks, ULONG block_bytes);

/* ---- OS-bound entry point (implemented in discover.c) ---- */
/* Fill out[] with up to `max` discovered disks; return the count. */
int discover_disks(DiscDisk out[], int max);

#endif /* HDPART_DISCOVER_H */
