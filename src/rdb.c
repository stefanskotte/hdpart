#include "rdb.h"
#include "endian.h"
/* implementation grows over the following tasks */

uint32_t rdb_sum_longs(const uint8_t *blk, uint32_t summed_longs)
{
    uint32_t sum = 0, i;
    for (i = 0; i < summed_longs; i++)
        sum += be_get32(blk + i * 4);
    return sum;
}

void rdb_set_checksum(uint8_t *blk, uint32_t summed_longs, uint32_t chk_off)
{
    uint32_t sum;
    be_put32(blk + chk_off, 0);
    sum = rdb_sum_longs(blk, summed_longs);
    be_put32(blk + chk_off, (uint32_t)(0u - sum)); /* two's complement */
}

int rdb_checksum_ok(const uint8_t *blk, uint32_t summed_longs)
{
    return rdb_sum_longs(blk, summed_longs) == 0u;
}

uint32_t rdb_mb_to_cyls(uint32_t mb, uint32_t cyl_blocks, uint32_t block_bytes)
{
    /* Use 64-bit to avoid overflow on large sizes. */
    uint64_t bytes      = (uint64_t)mb * 1024u * 1024u;
    uint64_t cyl_bytes  = (uint64_t)cyl_blocks * block_bytes;
    uint64_t cyls       = (bytes + cyl_bytes - 1) / cyl_bytes; /* ceil */
    if (cyls == 0) cyls = 1;
    return (uint32_t)cyls;
}

uint32_t rdb_cyls_to_mb(uint32_t cyls, uint32_t cyl_blocks, uint32_t block_bytes)
{
    uint64_t bytes = (uint64_t)cyls * cyl_blocks * block_bytes;
    return (uint32_t)(bytes / (1024u * 1024u)); /* floor */
}
