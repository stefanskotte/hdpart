#ifndef HDPART_DISCOVER_H
#define HDPART_DISCOVER_H

#include <stdint.h>

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
    uint32_t   unit;
    uint32_t   size_mb;                   /* from geometry, 0 if unknown */
    DiskStatus status;
    int        partitionable;             /* 1 if this can hold an RDB (for the GUI picker) */
    char       label[DISC_LABEL_LEN];     /* model string / DOS name for display */
} DiscDisk;

/* ---- pure helpers (no OS calls; unit-tested on host) ---- */

/* Convert a BCPL string (length byte followed by chars) to a C string.
   `bcpl` points at the length byte. Truncates to outsz-1. */
void disc_bcpl_to_c(const unsigned char *bcpl, char *out, int outsz);

/* Find driver+unit in list; return index or -1. Case-sensitive on driver. */
int disc_find(const DiscDisk list[], int count, const char *driver, uint32_t unit);

/* Convert a block count + block size to whole MB (floor), 32-bit safe. */
uint32_t disc_blocks_to_mb(uint32_t total_blocks, uint32_t block_bytes);

/* True if `s` looks like an exec device name: non-empty, all printable, and
   ending in ".device" (case-insensitive). Used to reject non-disk DOS handlers
   (RAW:, PRT:, SER:, ...) whose dol_Startup is not a FileSysStartupMsg. */
int disc_is_device_name(const char *s);

/* True if a (driver,total_blocks) pair represents an RDB-partitionable disk:
   has media (total_blocks>0) and is not a floppy (trackdisk.device). */
int disc_is_partitionable(const char *driver, uint32_t total_blocks);

/* ---- OS-bound entry point (implemented in discover.c) ---- */
/* Fill out[] with up to `max` discovered disks; return the count. */
int discover_disks(DiscDisk out[], int max);

/* Register a user-loaded driver name so the curated probe (scan_probe) also
   probes it on every subsequent discover_disks(). Deduped; ignored past
   capacity. Plain C (no OS calls) so it is host-testable. */
void disc_add_extra_driver(const char *name);

/* Number of registered extra drivers (for tests / introspection). */
int disc_extra_count(void);

/* Fill out[] with candidate driver NAMES for the picker: the curated probe list
   followed by user-loaded extras (deduped against the curated names). Returns
   the count (<= max). Pointers reference static storage; do not free or modify.
   Plain C, no hardware access — host-testable. */
int disc_candidate_drivers(const char *out[], int max);

/* Targeted probe of ONE driver: open units 0..PROBE_UNITS-1, add the ones that
   open to out[] (starting at *count, capacity max), classify just those new
   entries (geometry + RDB). *count is updated in place. Returns the number of
   entries added. OS-bound. */
int discover_probe_driver(DiscDisk out[], int *count, int max, const char *driver);

#endif /* HDPART_DISCOVER_H */
