#ifndef HDPART_RDB_H
#define HDPART_RDB_H
#include <stdint.h>

#define RDB_BLOCK_BYTES 512
#define RDB_MAX_PARTS   32
#define RDB_NAME_LEN    32

typedef int (*BlockIO)(void *ctx, uint32_t block, uint8_t *buf, int write);

/* Checksum: sum of `summed_longs` big-endian longwords must equal 0.
   chk_off is the byte offset of the checksum field within the block. */
void     rdb_set_checksum(uint8_t *blk, uint32_t summed_longs, uint32_t chk_off);
int      rdb_checksum_ok(const uint8_t *blk, uint32_t summed_longs);
uint32_t rdb_sum_longs(const uint8_t *blk, uint32_t summed_longs);

/* MB<->cylinder conversions. 1 MB = 1024*1024 bytes.
   mb_to_cyls rounds UP (so the partition is at least the requested size).
   cyls_to_mb rounds DOWN (reported usable size). */
uint32_t rdb_mb_to_cyls(uint32_t mb, uint32_t cyl_blocks, uint32_t block_bytes);
uint32_t rdb_cyls_to_mb(uint32_t cyls, uint32_t cyl_blocks, uint32_t block_bytes);

typedef struct {
    char     name[RDB_NAME_LEN]; /* NUL-terminated device name, e.g. "DH0" */
    uint32_t low_cyl;
    uint32_t high_cyl;
    uint32_t dos_type;           /* e.g. 0x444F5303 = DOS\3 (FFS Intl) */
    uint32_t num_buffers;
    int32_t  boot_pri;
    uint8_t  bootable;           /* 0/1 */
    uint32_t maxtransfer;        /* DOSEnvVec de_MaxTransfer */
    uint32_t mask;               /* DOSEnvVec de_Mask */
} RdbPartition;

typedef struct {
    uint32_t cylinders, heads, sectors;
    uint32_t block_bytes;
    uint32_t cyl_blocks;         /* heads*sectors */
    uint32_t lo_cyl, hi_cyl;     /* partitionable cylinder range */
    uint32_t rdb_blocks_lo, rdb_blocks_hi;
    int          num_parts;
    RdbPartition parts[RDB_MAX_PARTS];
} RdbModel;

#define RDB_RESERVED_CYLS 2u     /* cylinders reserved for RDB metadata */
#define RDB_DOSTYPE_FFS_INTL 0x444F5303u
#define RDB_DEFAULT_MAXTRANSFER 0x7FFFFFFFu  /* DOSEnvVec de_MaxTransfer default */
#define RDB_DEFAULT_MASK        0x7FFFFFFEu  /* DOSEnvVec de_Mask default */

void rdb_init_model(RdbModel *m, uint32_t cyl, uint32_t heads, uint32_t sectors);

/* Error codes are NEGATIVE so that functions returning a non-negative index on
   success (rdb_add_partition_at) can signal errors unambiguously via `< 0`.
   Status-returning functions still test `== RDB_OK` / `!= RDB_OK` correctly. */
enum {
    RDB_OK = 0,
    RDB_ERR_NO_SPACE = -1,
    RDB_ERR_DUP_NAME = -2,
    RDB_ERR_TOO_MANY = -3,
    RDB_ERR_BAD_NAME = -4,
    RDB_ERR_OVERLAP  = -5,
    RDB_ERR_IO       = -6,
    RDB_ERR_NO_RDB   = -7,
    RDB_ERR_RANGE    = -8
};

/* Find the largest unallocated cylinder range within [lo_cyl, hi_cyl].
   On success writes start/end (inclusive) and returns 1; returns 0 if the
   disk is full. Handles unsorted/overlapping partitions defensively. */
int rdb_largest_free_gap(const RdbModel *m, uint32_t *start, uint32_t *end);

/* Append a partition sized in MB, placed immediately after the last one
   (or at lo_cyl). Returns RDB_OK or an RDB_ERR_*. */
int rdb_add_partition(RdbModel *m, const char *name, uint32_t size_mb,
                      uint32_t dos_type);

/* Add a partition at an explicit start cylinder, size in MB. Returns the new
   index (>=0) or a negative RDB_ERR_* (e.g. RDB_ERR_OVERLAP / RDB_ERR_NO_SPACE
   / RDB_ERR_DUP_NAME / RDB_ERR_TOO_MANY). */
int rdb_add_partition_at(RdbModel *m, const char *name, uint32_t start_cyl,
                         uint32_t size_mb, uint32_t dos_type);

/* Update partition `index` in place: rename, set dos_type, and resize to
   size_mb keeping its low_cyl fixed (high_cyl recomputed). Validates the whole
   model afterward; on a validation failure the partition is restored and an
   RDB_ERR_* is returned. RDB_OK on success. */
int rdb_set_partition(RdbModel *m, int index, const char *name,
                      uint32_t size_mb, uint32_t dos_type);

/* Resize partition `index` to the cylinder range [low, high], keeping every
   other partition fixed. Validates the whole table (bounds / overlap / range /
   duplicate-name) via rdb_validate and rolls back to the prior extent on any
   error. Name, dos_type and flag fields are left untouched. Returns RDB_OK,
   RDB_ERR_RANGE (bad index or low > high), or rdb_validate's error. */
int rdb_resize_cyl(RdbModel *m, int index, uint32_t low, uint32_t high);

/* Add a partition spanning an exact cylinder range [start_cyl, end_cyl]
   (inclusive). Use this to fill a free gap precisely without MB rounding loss.
   Returns the new index (>=0) or a negative RDB_ERR_*. */
int rdb_add_partition_cyl(RdbModel *m, const char *name, uint32_t start_cyl,
                          uint32_t end_cyl, uint32_t dos_type);

/* Change only a partition's name + dos_type, leaving its cylinder range exactly
   as-is (so editing a gap-filling partition without changing its size does not
   re-round it). Returns RDB_OK or a negative RDB_ERR_*. */
int rdb_rename_partition(RdbModel *m, int index, const char *name, uint32_t dos_type);

/* Replace the whole partition table with `count` equal cylinder-slices spanning
   the partitionable range (lo_cyl..hi_cyl), named DH0..DH(count-1); the last
   slice absorbs any remainder cylinders so the disk is covered exactly. The
   model's geometry must already be set (rdb_init_model). Returns RDB_OK, or
   RDB_ERR_RANGE (count<1 or >RDB_MAX_PARTS) / RDB_ERR_NO_SPACE (fewer cylinders
   than partitions). */
int rdb_split_equal(RdbModel *m, int count, uint32_t dos_type);

/* Append `count` equal cylinder-slices over [lo, hi] (within lo_cyl..hi_cyl),
   auto-named with the lowest unused DH<n>, keeping existing partitions. The last
   slice absorbs remainder cylinders. Returns RDB_OK, RDB_ERR_RANGE (count<1 or
   bad range), RDB_ERR_TOO_MANY (would exceed RDB_MAX_PARTS), or RDB_ERR_NO_SPACE
   (fewer cylinders than partitions). Used to split a free gap. */
int rdb_split_range(RdbModel *m, uint32_t lo, uint32_t hi, int count,
                    uint32_t dos_type);

/* Remove partition by index, shifting the rest down. */
int rdb_delete_partition(RdbModel *m, int index);
/* Validate all partitions: bounds within [lo_cyl,hi_cyl], no overlaps,
   unique names. Returns RDB_OK or first error found. */
int rdb_validate(const RdbModel *m);

/* Serialize the model to disk via io: writes RDSK at block 0 and a PART
   chain in the reserved area. Returns RDB_OK or RDB_ERR_*. Validates first. */
int rdb_serialize(const RdbModel *m, BlockIO io, void *ctx);

/* Parse an existing RDB from disk via io: scans blocks 0..RDB_LOCATION_LIMIT
   for RDSK, reads geometry, walks the PART chain. Returns RDB_OK,
   RDB_ERR_NO_RDB, or RDB_ERR_IO. */
int rdb_parse(RdbModel *m, BlockIO io, void *ctx);

#endif
