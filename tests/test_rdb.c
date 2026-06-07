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

typedef struct { FILE *f; } FileCtx;
static int file_io(void *ctx, uint32_t block, uint8_t *buf, int write)
{
    FileCtx *c = (FileCtx *)ctx;
    if (fseek(c->f, (long)block * RDB_BLOCK_BYTES, SEEK_SET)) return 1;
    if (write) { if (fwrite(buf, 1, RDB_BLOCK_BYTES, c->f) != RDB_BLOCK_BYTES) return 1; }
    else       { if (fread (buf, 1, RDB_BLOCK_BYTES, c->f) != RDB_BLOCK_BYTES) return 1; }
    return 0;
}

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

static void test_init_model(void)
{
    RdbModel m;
    rdb_init_model(&m, /*cyl*/996, /*heads*/16, /*sectors*/63);
    CHECK(m.cylinders == 996);
    CHECK(m.heads == 16);
    CHECK(m.sectors == 63);
    CHECK(m.block_bytes == RDB_BLOCK_BYTES);
    CHECK(m.cyl_blocks == 16u * 63u);          /* 1008 */
    CHECK(m.lo_cyl == 2);                       /* reserve 2 cyl for RDB */
    CHECK(m.hi_cyl == 995);                     /* cylinders-1 */
    CHECK(m.rdb_blocks_lo == 0);
    CHECK(m.rdb_blocks_hi == 2u * 1008u - 1u);  /* 2015 */
    CHECK(m.num_parts == 0);
}

static void test_add_partition(void)
{
    RdbModel m;
    rdb_init_model(&m, 996, 16, 63);

    /* First partition: 200 MB -> 407 cyl, starts at lo_cyl=2 */
    int r = rdb_add_partition(&m, "DH0", 200, RDB_DOSTYPE_FFS_INTL);
    CHECK(r == RDB_OK);
    CHECK(m.num_parts == 1);
    CHECK(m.parts[0].low_cyl == 2);
    CHECK(m.parts[0].high_cyl == 2 + 407 - 1);          /* 408 */
    CHECK(strcmp(m.parts[0].name, "DH0") == 0);
    CHECK(m.parts[0].dos_type == RDB_DOSTYPE_FFS_INTL);

    /* Second partition follows immediately */
    r = rdb_add_partition(&m, "DH1", 200, RDB_DOSTYPE_FFS_INTL);
    CHECK(r == RDB_OK);
    CHECK(m.parts[1].low_cyl == 409);
    CHECK(m.parts[1].high_cyl == 409 + 407 - 1);        /* 815 */

    /* Duplicate name rejected */
    r = rdb_add_partition(&m, "DH0", 10, RDB_DOSTYPE_FFS_INTL);
    CHECK(r == RDB_ERR_DUP_NAME);

    /* Too big to fit (remaining cyls 816..995 = 180 cyl ~ 88 MB) */
    r = rdb_add_partition(&m, "DH2", 500, RDB_DOSTYPE_FFS_INTL);
    CHECK(r == RDB_ERR_NO_SPACE);

    /* validate passes on the good model */
    CHECK(rdb_validate(&m) == RDB_OK);
}

/* Simple RAM-backed BlockIO for tests: 4096 blocks * 512 bytes. */
#define RAMDISK_BLOCKS 4096
static uint8_t g_ram[RAMDISK_BLOCKS][RDB_BLOCK_BYTES];

static int ram_io(void *ctx, uint32_t block, uint8_t *buf, int write)
{
    (void)ctx;
    if (block >= RAMDISK_BLOCKS) return 1;
    if (write) memcpy(g_ram[block], buf, RDB_BLOCK_BYTES);
    else       memcpy(buf, g_ram[block], RDB_BLOCK_BYTES);
    return 0;
}

static void test_serialize_parse(void)
{
    RdbModel m, back;
    int r;
    memset(g_ram, 0, sizeof(g_ram));

    rdb_init_model(&m, 996, 16, 63);
    CHECK(rdb_add_partition(&m, "DH0", 200, RDB_DOSTYPE_FFS_INTL) == RDB_OK);
    CHECK(rdb_add_partition(&m, "DH1", 200, RDB_DOSTYPE_FFS_INTL) == RDB_OK);

    r = rdb_serialize(&m, ram_io, 0);
    CHECK(r == RDB_OK);

    /* RDSK lives at block 0 with a valid checksum */
    CHECK(be_get32(g_ram[0] + 0) == 0x5244534Bu);          /* 'RDSK' */
    CHECK(rdb_checksum_ok(g_ram[0], be_get32(g_ram[0] + 4)));

    /* Parse it back */
    r = rdb_parse(&back, ram_io, 0);
    CHECK(r == RDB_OK);
    CHECK(back.cylinders == 996);
    CHECK(back.heads == 16);
    CHECK(back.sectors == 63);
    CHECK(back.num_parts == 2);
    CHECK(strcmp(back.parts[0].name, "DH0") == 0);
    CHECK(back.parts[0].low_cyl == 2);
    CHECK(back.parts[0].high_cyl == 408);
    CHECK(strcmp(back.parts[1].name, "DH1") == 0);
    CHECK(back.parts[1].low_cyl == 409);
    CHECK(back.parts[1].dos_type == RDB_DOSTYPE_FFS_INTL);
}

int main(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "--emit") == 0) {
        /* Small, self-consistent geometry so the image is only a few MB and
           its size matches the geometry declared in the RDB (keeps rdbtool
           happy): 64 cyl * 4 heads * 32 sec = 8192 blocks = 4 MB.
           Output path is fixed to /tmp to avoid path-traversal concerns. */
        const char *outpath = "/tmp/hdpart_test.img";
        FILE *f = fopen(outpath, "wb+");
        static uint8_t z[RDB_BLOCK_BYTES];
        uint32_t i, total = 64u * 4u * 32u;   /* cylinders * cyl_blocks */
        FileCtx c;
        RdbModel m;
        if (!f) { perror("fopen"); return 2; }
        for (i = 0; i < total; i++) fwrite(z, 1, RDB_BLOCK_BYTES, f);
        c.f = f;
        rdb_init_model(&m, 64, 4, 32);
        rdb_add_partition(&m, "DH0", 1, RDB_DOSTYPE_FFS_INTL);
        rdb_add_partition(&m, "DH1", 1, RDB_DOSTYPE_FFS_INTL);
        if (rdb_serialize(&m, file_io, &c) != RDB_OK) { fclose(f); return 3; }
        fclose(f);
        printf("wrote %s (%u blocks)\n", outpath, total);
        return 0;
    }

    test_endian();
    test_checksum();
    test_geometry();
    test_init_model();
    test_add_partition();
    test_serialize_parse();
    if (g_fail) { printf("%d CHECK(s) FAILED\n", g_fail); return 1; }
    printf("ALL TESTS PASSED\n");
    return 0;
}
