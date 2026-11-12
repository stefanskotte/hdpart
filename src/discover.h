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
    int        used_pct;                  /* partitioned share 0..100, -1 if no RDB */
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

/* Parse an 8-byte SCSI READ CAPACITY(10) response (big-endian: bytes 0..3 =
   last logical block address, bytes 4..7 = block length). On success fills
   *block_bytes (>=512; 0 in the response defaults to 512) and *total_blocks
   (= last_lba + 1) and returns 1. Returns 0 when last_lba == 0xFFFFFFFF (the
   ">=2TB, use READ CAPACITY(16)" sentinel, beyond HDPart's 32-bit range). */
int disc_parse_read_capacity10(const uint8_t resp[8],
                               uint32_t *block_bytes, uint32_t *total_blocks);

/* Synthesize a self-consistent CHS geometry from a raw block count, for devices
   whose driver does not implement TD_GETGEOMETRY (e.g. the A3000 ROM
   scsi.device 37.x). RDB only needs heads*sectors (= blocks/cylinder) to be
   self-consistent — the controller addresses LBA-style — so a standard 16x63
   translation is used (1 block/cyl for media smaller than one cylinder).
   total_blocks==0 yields all zeros. */
void disc_synth_chs(uint32_t total_blocks,
                    uint32_t *cyl, uint32_t *heads, uint32_t *sectors);

/* ---- SCSI unit numbering (pure; matches a4091-software attach.c) ----
   Classic Commodore scheme, ID and LUN 0..7:  unit = ID + LUN*10.
   Phase V wide scheme, ID or LUN above 7:     unit = LUN*10000 + ID*10 + 8.
   IDE drivers use the same numbers (unit 0 master, 1 slave = ID 0/1, LUN 0). */
#define DISC_MAX_LUN 8          /* LUNs 0..7 are probed */
#define DISC_NARROW_IDS 8
#define DISC_WIDE_IDS 16

uint32_t disc_unit_encode(uint32_t id, uint32_t lun);
void     disc_unit_decode(uint32_t unit, uint32_t *id, uint32_t *lun);

/* "ID" or "ID.LUN" (LUN 0 is left off), e.g. "3", "3.1", "12". NUL-terminated,
   truncated to outsz-1. Returns the length. */
int disc_unit_label(uint32_t unit, char *out, int outsz);

/* True if this driver is known to take Phase V wide unit numbers (IDs 8..15).
   Only these are probed beyond ID 7: an unknown driver may alias or misread them. */
int disc_driver_wide(const char *name);

/* SCSI INQUIRY byte 0: true if the peripheral qualifier says no device is on this
   LUN (qualifier 1 = not connected, 3 = not supported) or the type is 0x1F (none). */
int disc_inquiry_absent(uint8_t byte0);

/* What a unit looks like from outside, to catch a target that ignores the LUN and
   answers the same on all of them ("ghost" LUNs). */
typedef struct {
    int      has_inq;           /* inq[] valid (driver supports HD_SCSICMD) */
    uint8_t  inq[36];
    uint32_t total_blocks;      /* 0 if no media / unknown */
    uint32_t sum;               /* additive sum of blocks 0..15, if media */
} DiscIdent;

/* True if `lun` answered exactly like `lun0`: same inquiry (or both without one),
   same capacity and same first 16 blocks. Two genuinely separate but blank and
   identical LUNs also match: they are dropped, which is the safe way to be wrong. */
int disc_ident_same(const DiscIdent *lun0, const DiscIdent *lun);

/* ---- OS-bound entry point (implemented in discover.c) ---- */
/* Fill out[] with up to `max` discovered disks; return the count. */
int discover_disks(DiscDisk out[], int max);

/* Register a user-loaded driver name so the curated probe (scan_probe) also
   probes it on every subsequent discover_disks(). Deduped; ignored past
   capacity. Plain C (no OS calls) so it is host-testable. */
void disc_add_extra_driver(const char *name);

/* Number of registered extra drivers (for tests / introspection). */
int disc_extra_count(void);

/* Fill out[] with candidate driver NAMES for the picker: the curated disk
   drivers that are RESIDENT in exec's device list, plus user-loaded extras
   (deduped). Returns the count (<= max). Pointers reference static storage; do
   not free or modify. OS-bound (reads exec's device list); not host-testable. */
int disc_candidate_drivers(const char *out[], int max);

/* Targeted probe of ONE driver, the way a4091/mounter scans: IDs 0..7 (0..15 on a
   wide driver) at LUN 0; LUNs 1..7 only behind an ID whose LUN 0 opened, stopping
   at the first LUN that does not open, has no device, mirrors LUN 0, or follows an
   RDB with RDBFF_LASTLUN. Adds what it finds to out[] (starting at *count, capacity
   max) and classifies just those new entries (geometry + RDB). *count is updated
   in place. Returns the number of entries added. OS-bound. */
int discover_probe_driver(DiscDisk out[], int *count, int max, const char *driver);

#endif /* HDPART_DISCOVER_H */
