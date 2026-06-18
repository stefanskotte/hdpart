/* Host unit tests for FSHD/LSEG embedded-filesystem support in rdb.c. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/rdb.h"
#include "../src/fsload.h"

static int tests_run, tests_failed;
#define CHECK(c) do { tests_run++; if(!(c)){ tests_failed++; \
    printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c);} } while(0)

static void test_lseg_block_count(void)
{
    CHECK(rdb_lseg_block_count(0) == 0);
    CHECK(rdb_lseg_block_count(1) == 1);
    CHECK(rdb_lseg_block_count(492) == 1);
    CHECK(rdb_lseg_block_count(493) == 2);
    CHECK(rdb_lseg_block_count(984) == 2);
    CHECK(rdb_lseg_block_count(985) == 3);
}

static void test_model_free_safe(void)
{
    RdbModel m;
    memset(&m, 0, sizeof m);
    rdb_model_free(&m);            /* zero model: must not crash */
    CHECK(m.num_fs == 0);
}

/* Simple RAM block device for tests: 4096 blocks of 512 bytes. */
#define RAM_BLOCKS 4096
typedef struct { uint8_t b[RAM_BLOCKS][512]; } RamDisk;
static int ram_io(void *ctx, uint32_t blk, uint8_t *buf, int write)
{
    RamDisk *d = (RamDisk *)ctx;
    if (blk >= RAM_BLOCKS) return 1;
    if (write) memcpy(d->b[blk], buf, 512);
    else       memcpy(buf, d->b[blk], 512);
    return 0;
}

/* Build a tiny fake hunk image of n bytes with the HUNK_HEADER magic. */
static uint8_t *fake_seg(uint32_t n)
{
    uint8_t *p = (uint8_t *)calloc(1, n < 4 ? 4 : n);
    p[0]=0x00; p[1]=0x00; p[2]=0x03; p[3]=0xF3;   /* HUNK_HEADER */
    { uint32_t i; for (i = 4; i < n; i++) p[i] = (uint8_t)(i * 7u + 1u); }
    return p;
}

static void test_serialize_with_fs(void)
{
    RamDisk *d = (RamDisk *)calloc(1, sizeof *d);
    RdbModel m; memset(&m, 0, sizeof m);
    rdb_init_model(&m, 100, 16, 63);
    CHECK(rdb_add_partition(&m, "DH0", 20, RDB_DOSTYPE_FFS_INTL) == RDB_OK);

    m.num_fs = 1;
    m.fs[0].dos_type = 0x50465303u;       /* PFS\3 */
    m.fs[0].version  = (53u << 16) | 3u;
    m.fs[0].seg_len  = 1000;              /* -> 3 LSEG blocks */
    m.fs[0].seg_data = fake_seg(1000);
    strcpy(m.fs[0].name, "PFSFileSystem");

    CHECK(rdb_serialize(&m, ram_io, d) == RDB_OK);

    /* RDSK FileSysHdr (offset 32) must point at a block whose ID is 'FSHD'. */
    {
        uint8_t blk[512]; uint32_t fshd;
        ram_io(d, 0, blk, 0);
        fshd = (blk[32]<<24)|(blk[33]<<16)|(blk[34]<<8)|blk[35];
        CHECK(fshd != 0xFFFFFFFFu);
        ram_io(d, fshd, blk, 0);
        CHECK(blk[0]=='F' && blk[1]=='S' && blk[2]=='H' && blk[3]=='D');
    }
    free(m.fs[0].seg_data); free(d);
}

static void test_capacity_guard(void)
{
    RamDisk *d = (RamDisk *)calloc(1, sizeof *d);
    RdbModel m; memset(&m, 0, sizeof m);
    /* 2 reserved cyls * (4 heads * 4 sectors) = 32 reserved blocks: tiny. */
    rdb_init_model(&m, 50, 4, 4);
    m.num_fs = 1;
    m.fs[0].dos_type = 0x50465303u;
    m.fs[0].seg_len  = 200000;            /* needs ~407 LSEG blocks, won't fit */
    m.fs[0].seg_data = fake_seg(200000);
    CHECK(rdb_serialize(&m, ram_io, d) == RDB_ERR_NO_RDB_SPACE);
    free(m.fs[0].seg_data); free(d);
}

static void test_roundtrip(void)
{
    RamDisk *d = (RamDisk *)calloc(1, sizeof *d);
    RdbModel m; memset(&m, 0, sizeof m);
    rdb_init_model(&m, 100, 16, 63);
    CHECK(rdb_add_partition(&m, "DH0", 20, RDB_DOSTYPE_FFS_INTL) == RDB_OK);
    m.num_fs = 1;
    m.fs[0].dos_type = 0x50465303u;
    m.fs[0].version  = (53u << 16) | 3u;
    m.fs[0].seg_len  = 1000;
    m.fs[0].seg_data = fake_seg(1000);
    strcpy(m.fs[0].name, "PFSFileSystem");
    CHECK(rdb_serialize(&m, ram_io, d) == RDB_OK);

    RdbModel r; memset(&r, 0, sizeof r);
    CHECK(rdb_parse(&r, ram_io, d) == RDB_OK);
    CHECK(r.num_fs == 1);
    CHECK(r.fs[0].dos_type == 0x50465303u);
    CHECK(r.fs[0].version  == ((53u<<16)|3u));
    CHECK(r.fs[0].seg_len  >= 1000);   /* padded to LSEG_PAYLOAD multiples */
    CHECK(r.fs[0].seg_data != 0);
    {
        uint8_t *orig = fake_seg(1000);
        CHECK(memcmp(r.fs[0].seg_data, orig, 1000) == 0);  /* bytes preserved */
        free(orig);
    }
    rdb_model_free(&r);
    free(m.fs[0].seg_data); free(d);
}

static void test_preserve_on_resave(void)
{
    /* A disk with an embedded FS, parsed and re-saved, keeps the FS. */
    RamDisk *d = (RamDisk *)calloc(1, sizeof *d);
    RdbModel m; memset(&m, 0, sizeof m);
    rdb_init_model(&m, 100, 16, 63);
    CHECK(rdb_add_partition(&m, "DH0", 20, RDB_DOSTYPE_FFS_INTL) == RDB_OK);
    m.num_fs = 1; m.fs[0].dos_type = 0x50465303u; m.fs[0].seg_len = 600;
    m.fs[0].seg_data = fake_seg(600);
    CHECK(rdb_serialize(&m, ram_io, d) == RDB_OK);

    RdbModel r; memset(&r, 0, sizeof r);
    CHECK(rdb_parse(&r, ram_io, d) == RDB_OK);
    CHECK(r.num_fs == 1);
    /* re-save the parsed model to a second disk, parse again */
    RamDisk *d2 = (RamDisk *)calloc(1, sizeof *d2);
    CHECK(rdb_serialize(&r, ram_io, d2) == RDB_OK);
    RdbModel r2; memset(&r2, 0, sizeof r2);
    CHECK(rdb_parse(&r2, ram_io, d2) == RDB_OK);
    CHECK(r2.num_fs == 1);
    CHECK(r2.fs[0].seg_len >= 600);   /* padded to LSEG_PAYLOAD multiples */
    rdb_model_free(&r); rdb_model_free(&r2);
    free(m.fs[0].seg_data); free(d); free(d2);
}

static void test_lseg_bad_checksum_rejected(void)
{
    RamDisk *d = (RamDisk *)calloc(1, sizeof *d);
    RdbModel m; memset(&m, 0, sizeof m);
    rdb_init_model(&m, 100, 16, 63);
    CHECK(rdb_add_partition(&m, "DH0", 20, RDB_DOSTYPE_FFS_INTL) == RDB_OK);
    m.num_fs = 1; m.fs[0].dos_type = 0x50465303u; m.fs[0].seg_len = 600;
    m.fs[0].seg_data = fake_seg(600);
    CHECK(rdb_serialize(&m, ram_io, d) == RDB_OK);
    /* find first LSEG block and corrupt a payload byte without fixing checksum */
    { uint8_t blk[512]; uint32_t fshd, lseg;
      ram_io(d, 0, blk, 0);
      fshd = (blk[32]<<24)|(blk[33]<<16)|(blk[34]<<8)|blk[35];
      ram_io(d, fshd, blk, 0);
      lseg = (blk[72]<<24)|(blk[73]<<16)|(blk[74]<<8)|blk[75]; /* FSHD_o_SegList=72 */
      ram_io(d, lseg, blk, 0);
      blk[20] ^= 0xFF;            /* LSEG_o_LoadData=20: flip a byte, leave chksum stale */
      ram_io(d, lseg, blk, 1);
    }
    RdbModel r; memset(&r, 0, sizeof r);
    rdb_parse(&r, ram_io, d);
    CHECK(r.num_fs == 0);          /* corrupt LSEG -> FS rejected, not silently copied */
    rdb_model_free(&r);
    free(m.fs[0].seg_data); free(d);
}

/* M3: PBFF_NOMOUNT (bit 1) must survive serialize→parse round-trip.
   Bootable bit must still work and toggling it on one partition must not
   clobber flags on another partition. */
static void test_pbff_nomount_roundtrip(void)
{
    RamDisk *d = (RamDisk *)calloc(1, sizeof *d);
    RdbModel m; memset(&m, 0, sizeof m);
    rdb_init_model(&m, 100, 16, 63);

    /* DH0: NOMOUNT only (not bootable) */
    CHECK(rdb_add_partition(&m, "DH0", 20, RDB_DOSTYPE_FFS_INTL) == RDB_OK);
    m.parts[0].flags    = RDB_PBFF_NOMOUNT;
    m.parts[0].bootable = 0;

    /* DH1: bootable, no NOMOUNT */
    CHECK(rdb_add_partition(&m, "DH1", 20, RDB_DOSTYPE_FFS_INTL) == RDB_OK);
    m.parts[1].flags    = RDB_PBFF_BOOTABLE;
    m.parts[1].bootable = 1;

    CHECK(rdb_serialize(&m, ram_io, d) == RDB_OK);

    /* First parse */
    RdbModel r; memset(&r, 0, sizeof r);
    CHECK(rdb_parse(&r, ram_io, d) == RDB_OK);
    CHECK(r.num_parts == 2);
    /* DH0: NOMOUNT preserved, bootable=0 */
    CHECK((r.parts[0].flags & RDB_PBFF_NOMOUNT) != 0);
    CHECK(r.parts[0].bootable == 0);
    /* DH1: bootable flag correct */
    CHECK((r.parts[1].flags & RDB_PBFF_BOOTABLE) != 0);
    CHECK(r.parts[1].bootable == 1);
    /* DH1 bootable must not have bled NOMOUNT onto it */
    CHECK((r.parts[1].flags & RDB_PBFF_NOMOUNT) == 0);

    /* Second serialize→parse (NOMOUNT must still be there) */
    RamDisk *d2 = (RamDisk *)calloc(1, sizeof *d2);
    CHECK(rdb_serialize(&r, ram_io, d2) == RDB_OK);
    RdbModel r2; memset(&r2, 0, sizeof r2);
    CHECK(rdb_parse(&r2, ram_io, d2) == RDB_OK);
    CHECK(r2.num_parts == 2);
    CHECK((r2.parts[0].flags & RDB_PBFF_NOMOUNT) != 0);
    CHECK(r2.parts[0].bootable == 0);

    /* Toggle bootable on DH1 via the bootable field alone; NOMOUNT on DH0
       must remain unaffected across a third serialize→parse */
    r2.parts[1].bootable = 0;   /* un-boot DH1 */
    RamDisk *d3 = (RamDisk *)calloc(1, sizeof *d3);
    CHECK(rdb_serialize(&r2, ram_io, d3) == RDB_OK);
    RdbModel r3; memset(&r3, 0, sizeof r3);
    CHECK(rdb_parse(&r3, ram_io, d3) == RDB_OK);
    CHECK((r3.parts[0].flags & RDB_PBFF_NOMOUNT) != 0);   /* still set */
    CHECK(r3.parts[1].bootable == 0);                      /* correctly cleared */
    CHECK((r3.parts[1].flags & RDB_PBFF_BOOTABLE) == 0);  /* bit0 reflects bootable */

    /* Fresh partitions (created via rdb_add_partition) must have flags=0 */
    CHECK(m.parts[0].flags == RDB_PBFF_NOMOUNT); /* explicitly set above is intact */
    {
        RdbModel fresh; memset(&fresh, 0, sizeof fresh);
        rdb_init_model(&fresh, 100, 16, 63);
        CHECK(rdb_add_partition(&fresh, "DH0", 20, RDB_DOSTYPE_FFS_INTL) == RDB_OK);
        CHECK(fresh.parts[0].flags == 0);
    }

    free(d); free(d2); free(d3);
}

static void test_is_hunk_file(void)
{
    uint8_t good[8] = {0x00,0x00,0x03,0xF3, 0,0,0,0};
    uint8_t bad[8]  = {0xDE,0xAD,0xBE,0xEF, 0,0,0,0};
    CHECK(fsload_is_hunk_file(good, 8) == 1);
    CHECK(fsload_is_hunk_file(bad, 8) == 0);
    CHECK(fsload_is_hunk_file(good, 3) == 0);   /* too short */
    CHECK(fsl_err_text(FSL_ENOTLOADFILE) != 0);
}

/* ---- Adaptive RDB reserve tests ---- */

/* Real geometry (16h x 63s = 1008 blocks/cyl): 2 cyls = 2016 blocks >= 1024,
   so the floor is not engaged and lo_cyl stays at RDB_RESERVED_CYLS (2). */
static void test_reserve_real_geometry(void)
{
    RdbModel m;
    rdb_init_model(&m, 1000, 16, 63);
    CHECK(m.cyl_blocks == 1008u);
    CHECK(m.lo_cyl == RDB_RESERVED_CYLS);                /* 2: unchanged */
    CHECK(m.rdb_blocks_hi == 2u * 1008u - 1u);           /* 2015 */
}

/* Small geometry (1h x 32s = 32 blocks/cyl): floor kicks in.
   ceil(1024/32)=32 reserved cylinders.  A 58000-byte FS should now fit. */
static void test_reserve_small_geometry(void)
{
    RamDisk *d = (RamDisk *)calloc(1, sizeof *d);
    RdbModel m;
    rdb_init_model(&m, 9600, 1, 32);
    CHECK(m.cyl_blocks == 32u);
    /* Reserved area must hold >= RDB_RESERVED_MIN_BLOCKS metadata blocks. */
    CHECK((m.rdb_blocks_hi + 1u) >= RDB_RESERVED_MIN_BLOCKS);
    /* lo_cyl must be ceil(1024/32) = 32. */
    CHECK(m.lo_cyl == 32u);

    /* A ~58000-byte FS that previously caused RDB_ERR_NO_RDB_SPACE must now fit. */
    CHECK(rdb_add_partition(&m, "DH0", 1, RDB_DOSTYPE_FFS_INTL) >= 0);
    m.num_fs = 1;
    m.fs[0].dos_type = 0x50465303u;  /* PFS\3 */
    m.fs[0].seg_len  = 58000;
    m.fs[0].seg_data = fake_seg(58000);
    CHECK(rdb_serialize(&m, ram_io, d) == RDB_OK);   /* was RDB_ERR_NO_RDB_SPACE before fix */
    free(m.fs[0].seg_data);
    free(d);
}

/* Tiny disk (50 cyl, 4h x 4s = 16 blocks/cyl): clamp must prevent reserving
   more than cyl/4 cylinders so the disk retains partitionable room. */
static void test_reserve_tiny_disk_clamp(void)
{
    RdbModel m;
    rdb_init_model(&m, 50, 4, 4);
    CHECK(m.cyl_blocks == 16u);
    /* Clamp: lo_cyl must not exceed 50/4 = 12. */
    CHECK(m.lo_cyl <= 50u / 4u);
    /* Disk must have at least one partitionable cylinder above the reserve. */
    CHECK(m.lo_cyl < m.hi_cyl);
}

int main(void)
{
    test_lseg_block_count();
    test_model_free_safe();
    test_serialize_with_fs();
    test_capacity_guard();
    test_roundtrip();
    test_preserve_on_resave();
    test_lseg_bad_checksum_rejected();
    test_pbff_nomount_roundtrip();
    test_is_hunk_file();
    test_reserve_real_geometry();
    test_reserve_small_geometry();
    test_reserve_tiny_disk_clamp();
    printf("test_fshd: %d run, %d failed\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
