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

static void test_geometry_large(void)
{
    /* Larger size must stay correct with 32-bit math (no overflow at these
       sizes) and round up. 1024 MB @ cyl_blocks=1008, 512 b/blk:
       blocks_per_mb=2048, total=2097152, ceil(2097152/1008)=2081. */
    CHECK(rdb_mb_to_cyls(1024, 1008, 512) == 2081);
    /* monotonic: bigger request -> at least as many cylinders */
    CHECK(rdb_mb_to_cyls(2048, 1008, 512) >= rdb_mb_to_cyls(1024, 1008, 512));
}

static void test_validate_negative(void)
{
    RdbModel m;

    /* overlap */
    rdb_init_model(&m, 996, 16, 63);
    CHECK(rdb_add_partition(&m, "DH0", 200, RDB_DOSTYPE_FFS_INTL) == RDB_OK);
    CHECK(rdb_add_partition(&m, "DH1", 200, RDB_DOSTYPE_FFS_INTL) == RDB_OK);
    CHECK(rdb_validate(&m) == RDB_OK);
    m.parts[1].low_cyl = m.parts[0].high_cyl;      /* force overlap */
    CHECK(rdb_validate(&m) == RDB_ERR_OVERLAP);

    /* inverted range */
    rdb_init_model(&m, 996, 16, 63);
    CHECK(rdb_add_partition(&m, "DH0", 200, RDB_DOSTYPE_FFS_INTL) == RDB_OK);
    m.parts[0].high_cyl = m.parts[0].low_cyl - 1;  /* low > high */
    CHECK(rdb_validate(&m) == RDB_ERR_RANGE);

    /* out of bounds (beyond hi_cyl) */
    rdb_init_model(&m, 996, 16, 63);
    CHECK(rdb_add_partition(&m, "DH0", 200, RDB_DOSTYPE_FFS_INTL) == RDB_OK);
    m.parts[0].high_cyl = m.hi_cyl + 5;
    CHECK(rdb_validate(&m) == RDB_ERR_NO_SPACE);
}

static void test_add_at_and_set(void)
{
    RdbModel m;
    int r, idx;
    rdb_init_model(&m, 996, 16, 63);
    /* add at an explicit start cylinder */
    idx = rdb_add_partition_at(&m, "DH0", 100, 50, RDB_DOSTYPE_FFS_INTL);
    CHECK(idx == 0);
    CHECK(m.parts[0].low_cyl == 100);
    CHECK(m.parts[0].high_cyl == 100 + rdb_mb_to_cyls(50, m.cyl_blocks, m.block_bytes) - 1);
    /* resize partition 0 to 30 MB, keeping its start */
    r = rdb_set_partition(&m, 0, "WORK", 30, RDB_DOSTYPE_FFS_INTL);
    CHECK(r == RDB_OK);
    CHECK(m.parts[0].low_cyl == 100);
    CHECK(strcmp(m.parts[0].name, "WORK") == 0);
    CHECK(m.parts[0].high_cyl == 100 + rdb_mb_to_cyls(30, m.cyl_blocks, m.block_bytes) - 1);
    /* a resize that would overflow the disk is rejected, leaving the model valid */
    r = rdb_set_partition(&m, 0, "WORK", 100000, RDB_DOSTYPE_FFS_INTL);
    CHECK(r == RDB_ERR_NO_SPACE);
    CHECK(rdb_validate(&m) == RDB_OK);
    /* add_at that overlaps an existing partition is rejected */
    idx = rdb_add_partition_at(&m, "DH1", 100, 10, RDB_DOSTYPE_FFS_INTL);
    CHECK(idx == RDB_ERR_OVERLAP);
}

static void test_add_cyl_and_rename(void)
{
    RdbModel m;
    int idx, r;
    rdb_init_model(&m, 996, 16, 63);          /* lo_cyl=2, hi_cyl=995 */
    /* add an exact cylinder range (fills a gap with NO MB rounding loss) */
    idx = rdb_add_partition_cyl(&m, "DH0", 2, 500, RDB_DOSTYPE_FFS_INTL);
    CHECK(idx == 0);
    CHECK(m.parts[0].low_cyl == 2 && m.parts[0].high_cyl == 500);   /* exact, not MB-rounded */
    /* rename keeps the exact cylinder range untouched */
    r = rdb_rename_partition(&m, 0, "SYSTEM", RDB_DOSTYPE_FFS_INTL);
    CHECK(r == RDB_OK);
    CHECK(strcmp(m.parts[0].name, "SYSTEM") == 0);
    CHECK(m.parts[0].low_cyl == 2 && m.parts[0].high_cyl == 500);   /* unchanged */
    /* out-of-bounds / overlap rejected; model stays valid */
    CHECK(rdb_add_partition_cyl(&m, "X", 400, 600, RDB_DOSTYPE_FFS_INTL) == RDB_ERR_OVERLAP);
    CHECK(rdb_add_partition_cyl(&m, "Y", 900, 1000, RDB_DOSTYPE_FFS_INTL) == RDB_ERR_NO_SPACE);
    CHECK(rdb_rename_partition(&m, 0, "SYSTEM", RDB_DOSTYPE_FFS_INTL) == RDB_OK); /* self-rename ok */
    CHECK(rdb_validate(&m) == RDB_OK);
}

static void test_largest_free_gap(void)
{
    RdbModel m;
    uint32_t s = 0, e = 0;
    rdb_init_model(&m, 996, 16, 63);            /* lo_cyl=2, hi_cyl=995 */
    /* empty disk: whole partitionable area is the gap */
    CHECK(rdb_largest_free_gap(&m, &s, &e) == 1);
    CHECK(s == 2 && e == 995);
    /* one partition at the front leaves the tail as the gap */
    rdb_add_partition(&m, "DH0", 100, RDB_DOSTYPE_FFS_INTL);   /* 2..N */
    CHECK(rdb_largest_free_gap(&m, &s, &e) == 1);
    CHECK(s == m.parts[0].high_cyl + 1 && e == 995);
    /* tiny partitions at front and tail leave the big MIDDLE gap as the largest */
    {
        RdbModel m2;
        uint32_t s2 = 0, e2 = 0;
        rdb_init_model(&m2, 996, 16, 63);
        CHECK(rdb_add_partition_at(&m2, "A", 2,   1, RDB_DOSTYPE_FFS_INTL) == 0);
        CHECK(rdb_add_partition_at(&m2, "B", 990, 1, RDB_DOSTYPE_FFS_INTL) == 1);
        CHECK(rdb_largest_free_gap(&m2, &s2, &e2) == 1);
        CHECK(s2 == m2.parts[0].high_cyl + 1 && e2 == 989);
    }
    /* fill the rest; no gap remains — place DH1 directly to reach hi_cyl */
    {
        RdbPartition *p;
        if (m.num_parts < RDB_MAX_PARTS) {
            p = &m.parts[m.num_parts++];
            p->low_cyl  = m.parts[0].high_cyl + 1;
            p->high_cyl = m.hi_cyl;
            p->dos_type = RDB_DOSTYPE_FFS_INTL;
            p->num_buffers = 30; p->boot_pri = 0; p->bootable = 0;
            { int _i; for (_i=0;_i<RDB_NAME_LEN-1&&"DH1"[_i];_i++) p->name[_i]="DH1"[_i]; p->name[_i]=0; }
        }
    }
    CHECK(rdb_largest_free_gap(&m, &s, &e) == 0);
}

/* Partition flag fields: defaults on add, and maxtransfer/mask/bootable/boot_pri
   round-tripping through serialize -> parse. */
static void test_part_flags(void)
{
    RdbModel m, back;
    memset(g_ram, 0, sizeof(g_ram));

    rdb_init_model(&m, 996, 16, 63);
    CHECK(rdb_add_partition(&m, "DH0", 200, RDB_DOSTYPE_FFS_INTL) == RDB_OK);

    /* defaults on a fresh partition */
    CHECK(m.parts[0].maxtransfer == 0x7FFFFFFFu);
    CHECK(m.parts[0].mask        == 0x7FFFFFFEu);
    CHECK(m.parts[0].bootable    == 0);
    CHECK(m.parts[0].boot_pri    == 0);

    /* set custom flag values, serialize, parse back */
    m.parts[0].maxtransfer = 0x0001FE00u;
    m.parts[0].mask        = 0xFFFFFFFEu;
    m.parts[0].bootable    = 1;
    m.parts[0].boot_pri    = 5;
    CHECK(rdb_serialize(&m, ram_io, 0) == RDB_OK);
    CHECK(rdb_parse(&back, ram_io, 0) == RDB_OK);
    CHECK(back.parts[0].maxtransfer == 0x0001FE00u);
    CHECK(back.parts[0].mask        == 0xFFFFFFFEu);
    CHECK(back.parts[0].bootable    == 1);
    CHECK(back.parts[0].boot_pri    == 5);
}

/* rdb_split_equal: N equal cylinder-slices covering the whole partitionable
   range, named DH0..DH(N-1), with error handling. */
static void test_split_equal(void)
{
    RdbModel m;
    int i;

    rdb_init_model(&m, 996, 16, 63);                 /* lo=2, hi=995, span=994 */
    CHECK(rdb_split_equal(&m, 4, RDB_DOSTYPE_FFS_INTL) == RDB_OK);
    CHECK(m.num_parts == 4);
    CHECK(m.parts[0].low_cyl == 2);                  /* starts at lo */
    CHECK(m.parts[3].high_cyl == 995);               /* last reaches hi exactly */
    for (i = 1; i < 4; i++)                           /* contiguous, no gaps */
        CHECK(m.parts[i].low_cyl == m.parts[i-1].high_cyl + 1);
    CHECK(strcmp(m.parts[0].name, "DH0") == 0);
    CHECK(strcmp(m.parts[3].name, "DH3") == 0);
    CHECK(rdb_validate(&m) == RDB_OK);

    /* N=1 covers the whole range */
    CHECK(rdb_split_equal(&m, 1, RDB_DOSTYPE_FFS_INTL) == RDB_OK);
    CHECK(m.num_parts == 1);
    CHECK(m.parts[0].low_cyl == 2 && m.parts[0].high_cyl == 995);

    /* errors: bad count */
    CHECK(rdb_split_equal(&m, 0, RDB_DOSTYPE_FFS_INTL) != RDB_OK);
    CHECK(rdb_split_equal(&m, RDB_MAX_PARTS + 1, RDB_DOSTYPE_FFS_INTL) != RDB_OK);

    /* error: more partitions than cylinders (span = 3, ask 4) */
    { RdbModel s; rdb_init_model(&s, 5, 1, 1);
      CHECK(rdb_split_equal(&s, 4, RDB_DOSTYPE_FFS_INTL) != RDB_OK); }
}

/* rdb_split_range: append N equal slices into a free gap, auto-named (lowest
   unused DH<n>), keeping existing partitions. */
static void test_split_range(void)
{
    RdbModel m;
    int i;

    rdb_init_model(&m, 996, 16, 63);                 /* lo=2, hi=995 */
    CHECK(rdb_add_partition_cyl(&m, "DH0", 2, 51, RDB_DOSTYPE_FFS_INTL) >= 0); /* boot */
    CHECK(rdb_split_range(&m, 52, 995, 3, RDB_DOSTYPE_FFS_INTL) == RDB_OK);
    CHECK(m.num_parts == 4);                          /* DH0 + 3 new */
    CHECK(strcmp(m.parts[0].name, "DH0") == 0);
    CHECK(strcmp(m.parts[1].name, "DH1") == 0);       /* auto-named, skips DH0 */
    CHECK(strcmp(m.parts[3].name, "DH3") == 0);
    CHECK(m.parts[1].low_cyl == 52);                  /* covers the gap exactly */
    CHECK(m.parts[3].high_cyl == 995);
    for (i = 2; i < 4; i++)
        CHECK(m.parts[i].low_cyl == m.parts[i-1].high_cyl + 1);   /* contiguous */
    CHECK(rdb_validate(&m) == RDB_OK);

    CHECK(rdb_split_range(&m, 52, 995, 0, RDB_DOSTYPE_FFS_INTL) != RDB_OK);  /* count<1 */
    { RdbModel s; rdb_init_model(&s, 996, 16, 63);
      CHECK(rdb_split_range(&s, 2, 4, 4, RDB_DOSTYPE_FFS_INTL) != RDB_OK); } /* 4 > span 3 */
}

static void test_resize_cyl(void)
{
    RdbModel m;
    rdb_init_model(&m, 996, 16, 63);            /* lo_cyl=2, hi_cyl=995 */
    /* DH0 2..51, DH1 100..199, DH2 300..399 — gaps before/after DH1 */
    CHECK(rdb_add_partition_cyl(&m, "DH0",   2,  51, RDB_DOSTYPE_FFS_INTL) == 0);
    CHECK(rdb_add_partition_cyl(&m, "DH1", 100, 199, RDB_DOSTYPE_FFS_INTL) == 1);
    CHECK(rdb_add_partition_cyl(&m, "DH2", 300, 399, RDB_DOSTYPE_FFS_INTL) == 2);

    /* grow End edge into trailing gap: low fixed, high up */
    CHECK(rdb_resize_cyl(&m, 1, 100, 250) == RDB_OK);
    CHECK(m.parts[1].low_cyl == 100 && m.parts[1].high_cyl == 250);

    /* grow Start edge into leading gap: high fixed, low down */
    CHECK(rdb_resize_cyl(&m, 1, 60, 250) == RDB_OK);
    CHECK(m.parts[1].low_cyl == 60 && m.parts[1].high_cyl == 250);

    /* shrink End edge */
    CHECK(rdb_resize_cyl(&m, 1, 60, 120) == RDB_OK);
    CHECK(m.parts[1].low_cyl == 60 && m.parts[1].high_cyl == 120);

    /* shrink Start edge */
    CHECK(rdb_resize_cyl(&m, 1, 110, 120) == RDB_OK);
    CHECK(m.parts[1].low_cyl == 110 && m.parts[1].high_cyl == 120);

    /* overlap rejection -> model unchanged (rollback) */
    CHECK(rdb_resize_cyl(&m, 1, 110, 300) == RDB_ERR_OVERLAP);   /* hits DH2@300 */
    CHECK(m.parts[1].low_cyl == 110 && m.parts[1].high_cyl == 120);

    /* out-of-bounds rejection -> rollback */
    CHECK(rdb_resize_cyl(&m, 2, 300, 996) == RDB_ERR_NO_SPACE);  /* hi_cyl=995 */
    CHECK(m.parts[2].low_cyl == 300 && m.parts[2].high_cyl == 399);

    /* inverted range and bad index */
    CHECK(rdb_resize_cyl(&m, 1, 200, 199) == RDB_ERR_RANGE);
    CHECK(rdb_resize_cyl(&m, 99, 100, 200) == RDB_ERR_RANGE);

    /* 1-cylinder floor round-trips */
    CHECK(rdb_resize_cyl(&m, 1, 150, 150) == RDB_OK);
    CHECK(m.parts[1].low_cyl == 150 && m.parts[1].high_cyl == 150);
}

static void test_gap_helpers(void)
{
    RdbModel m;
    rdb_init_model(&m, 996, 16, 63);            /* lo_cyl=2, hi_cyl=995 */
    CHECK(rdb_add_partition_cyl(&m, "DH0",   2,  51, RDB_DOSTYPE_FFS_INTL) == 0);
    CHECK(rdb_add_partition_cyl(&m, "DH1", 100, 199, RDB_DOSTYPE_FFS_INTL) == 1);
    CHECK(rdb_add_partition_cyl(&m, "DH2", 300, 399, RDB_DOSTYPE_FFS_INTL) == 2);

    /* DH1 sits between DH0 (..51) and DH2 (300..) */
    CHECK(rdb_gap_end_after(&m, 1)    == 300);  /* next occupied cyl after high */
    CHECK(rdb_gap_start_before(&m, 1) == 52);   /* first free cyl before low   */

    /* DH0 is first: nothing before -> lo_cyl */
    CHECK(rdb_gap_start_before(&m, 0) == 2);
    /* DH2 is last: nothing after -> hi_cyl+1 */
    CHECK(rdb_gap_end_after(&m, 2)    == 996);

    CHECK(rdb_gap_end_after(&m, 0)    == 100);   /* DH0 last in gap: next is DH1@100 */
    CHECK(rdb_gap_start_before(&m, 2) == 200);   /* DH2: prev partition DH1 ends @199 */
    CHECK(rdb_gap_end_after(&m, 99)   == 996);   /* invalid index -> full-disk sentinel */
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
    if (argc >= 2 && strcmp(argv[1], "--emit-fs") == 0) {
        /* Like --emit but embeds a fake PFS\3 filesystem in the RDB so that
           rdbtool can independently verify the FSHD/LSEG chain.
           Same geometry: 64 cyl * 4 heads * 32 sec = 8192 blocks = 4 MB. */
        const char *outpath = "/tmp/hdpart_test_fs.img";
        FILE *f = fopen(outpath, "wb+");
        static uint8_t z2[RDB_BLOCK_BYTES];
        /* Fake hunk image: HUNK_HEADER magic + filler, 984 bytes (2 LSEG blocks). */
        static uint8_t fake_hunk[984];
        uint32_t i2, total2 = 64u * 4u * 32u;
        FileCtx c2;
        RdbModel m2;
        if (!f) { perror("fopen"); return 2; }
        for (i2 = 0; i2 < total2; i2++) fwrite(z2, 1, RDB_BLOCK_BYTES, f);
        c2.f = f;
        fake_hunk[0]=0x00; fake_hunk[1]=0x00; fake_hunk[2]=0x03; fake_hunk[3]=0xF3;
        for (i2 = 4; i2 < 984; i2++) fake_hunk[i2] = (uint8_t)(i2 * 7u + 1u);
        rdb_init_model(&m2, 64, 4, 32);
        rdb_add_partition(&m2, "DH0", 1, RDB_DOSTYPE_FFS_INTL);
        m2.num_fs = 1;
        m2.fs[0].dos_type = 0x50465303u;   /* PFS\3 */
        m2.fs[0].version  = (53u << 16) | 3u;
        m2.fs[0].seg_len  = 984;
        m2.fs[0].seg_data = fake_hunk;
        if (rdb_serialize(&m2, file_io, &c2) != RDB_OK) { fclose(f); return 3; }
        m2.fs[0].seg_data = 0;             /* not heap-owned; prevent free */
        fclose(f);
        printf("wrote %s (%u blocks, embedded PFS\\3 FS)\n", outpath, total2);
        return 0;
    }

    test_endian();
    test_checksum();
    test_geometry();
    test_geometry_large();
    test_init_model();
    test_add_partition();
    test_serialize_parse();
    test_validate_negative();
    test_add_at_and_set();
    test_resize_cyl();
    test_gap_helpers();
    test_add_cyl_and_rename();
    test_largest_free_gap();
    test_part_flags();
    test_split_equal();
    test_split_range();
    if (g_fail) { printf("%d CHECK(s) FAILED\n", g_fail); return 1; }
    printf("ALL TESTS PASSED\n");
    return 0;
}
