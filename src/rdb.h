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

#endif
