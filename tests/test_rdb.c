/* HDPart host unit tests. Assert-based, no framework.
 * Compiled with system cc; includes the engine source directly. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "../src/endian.h"
#include "../src/rdb.h"

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); g_fail++; } } while (0)

static void test_endian(void)
{
    uint8_t b[4];
    be_put32(b, 0x12345678u);
    CHECK(b[0]==0x12 && b[1]==0x34 && b[2]==0x56 && b[3]==0x78);
    CHECK(be_get32(b) == 0x12345678u);
}

static void test_checksum(void)
{
    uint8_t blk[RDB_BLOCK_BYTES];
    memset(blk, 0, sizeof(blk));
    be_put32(blk + 0, 0x5244534Bu); /* 'RDSK' */
    be_put32(blk + 4, 64);          /* summed longs */
    /* checksum field at offset 8 left zero, then computed */
    rdb_set_checksum(blk, 64, 8);
    CHECK(rdb_checksum_ok(blk, 64));
    /* corrupt a byte -> checksum must fail */
    blk[20] ^= 0xFF;
    CHECK(!rdb_checksum_ok(blk, 64));
}

static void test_geometry(void)
{
    /* 16 heads * 63 sectors = 1008 blocks/cyl; 512 b/blk => 504 KB/cyl
       => ~0.4922 MB/cyl. 200 MB -> ceil(200*1024*1024 / (1008*512)) cyls. */
    uint32_t cyl_blocks = 16u * 63u;            /* 1008 */
    uint32_t cyls = rdb_mb_to_cyls(200, cyl_blocks, RDB_BLOCK_BYTES);
    CHECK(cyls == 407);                         /* 200MB rounds up to 407 cyl */
    /* round-trip: cyls back to MB (floor) */
    uint32_t mb = rdb_cyls_to_mb(cyls, cyl_blocks, RDB_BLOCK_BYTES);
    CHECK(mb == 200);                           /* 407 cyl -> 200 MB (floor) */
}

int main(void)
{
    test_endian();
    test_checksum();
    test_geometry();
    /* further test functions appended in later tasks */
    if (g_fail) { printf("%d CHECK(s) FAILED\n", g_fail); return 1; }
    printf("ALL TESTS PASSED\n");
    return 0;
}
