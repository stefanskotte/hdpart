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
    uint8_t  bootable;           /* 0/1 (UI greyed in phase 1) */
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

void rdb_init_model(RdbModel *m, uint32_t cyl, uint32_t heads, uint32_t sectors);

enum {
    RDB_OK = 0,
    RDB_ERR_NO_SPACE,
    RDB_ERR_DUP_NAME,
    RDB_ERR_TOO_MANY,
    RDB_ERR_BAD_NAME,
    RDB_ERR_OVERLAP,
    RDB_ERR_IO,
    RDB_ERR_NO_RDB,
    RDB_ERR_RANGE
};

/* Append a partition sized in MB, placed immediately after the last one
   (or at lo_cyl). Returns RDB_OK or an RDB_ERR_*. */
int rdb_add_partition(RdbModel *m, const char *name, uint32_t size_mb,
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
