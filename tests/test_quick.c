/* HDPart host tests for Quick setup layout. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "../src/endian.h"
#include "../src/rdb.h"
#include "../src/quick.h"

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); g_fail++; } } while (0)

#define MB_BLOCKS 2048u
static uint32_t pblocks(const RdbModel *m, int i)
{ return (m->parts[i].high_cyl - m->parts[i].low_cyl + 1u) * m->cyl_blocks; }
static uint32_t abs_diff(uint32_t a, uint32_t b) { return a > b ? a - b : b - a; }

/* card of `mb` megabytes with the given cylinder size (heads*sectors) */
static void card(RdbModel *m, uint32_t mb, uint32_t heads, uint32_t sectors)
{ rdb_init_model(m, mb * MB_BLOCKS / (heads * sectors), heads, sectors); }

static void test_layout_8gb_two_ticked(void)
{
    RdbModel m; int i;
    rdb_init_model(&m, 16644, 16, 63);          /* ~8 GB at 1008 blocks per cylinder */
    CHECK(quick_layout(&m, QS_WORK | QS_GAMES) == RDB_OK);
    CHECK(m.num_parts == 3);
    CHECK(strcmp(m.parts[0].name, "DH0") == 0 && strcmp(m.parts[1].name, "DH1") == 0 &&
          strcmp(m.parts[2].name, "DH2") == 0);
    CHECK(m.parts[0].bootable == 1 && m.parts[0].boot_pri == 0);
    CHECK(m.parts[1].bootable == 0 && m.parts[2].bootable == 0);
    CHECK(abs_diff(pblocks(&m, 1), pblocks(&m, 2)) <= m.cyl_blocks);
    CHECK((m.parts[2].high_cyl + 1u) * m.cyl_blocks <= 8388608u);
    CHECK(m.cylinders == 16644);                 /* the card's own */
    CHECK(m.hi_cyl + 1u == 8388608u / 1008u);
    CHECK(abs_diff(pblocks(&m, 0), 500u * MB_BLOCKS) <= m.cyl_blocks);
    for (i = 0; i < 3; i++) {
        CHECK(m.parts[i].dos_type == RDB_DOSTYPE_FFS_INTL);
        CHECK(m.parts[i].maxtransfer == RDB_DEFAULT_MAXTRANSFER);
    }
    CHECK(rdb_validate(&m) == RDB_OK);
}

static void test_layout_partition_cap(void)
{
    RdbModel m;
    rdb_init_model(&m, 16644, 16, 63);
    CHECK(quick_layout(&m, QS_WORK) == RDB_OK);
    CHECK(m.num_parts == 2);
    CHECK(pblocks(&m, 1) <= 4194304u);           /* 2 GiB */
    CHECK(pblocks(&m, 1) > 4194304u - m.cyl_blocks);
    CHECK(m.parts[1].high_cyl + 1u < m.hi_cyl + 1u);   /* remainder stays unused */
    CHECK(rdb_validate(&m) == RDB_OK);
}

static void test_layout_system_only(void)
{
    RdbModel m;
    rdb_init_model(&m, 16644, 16, 63);
    CHECK(quick_layout(&m, 0) == RDB_OK);
    CHECK(m.num_parts == 1);
    CHECK(abs_diff(pblocks(&m, 0), 500u * MB_BLOCKS) <= m.cyl_blocks);
    CHECK(m.parts[0].bootable == 1);
}

static void test_layout_small_cards(void)
{
    RdbModel m;
    card(&m, 129, 16, 63);
    CHECK(quick_layout(&m, QS_WORK | QS_GAMES | QS_DEMOS) == RDB_OK);
    CHECK(abs_diff(pblocks(&m, 0), 64u * MB_BLOCKS) <= 2u * m.cyl_blocks);
    CHECK(rdb_validate(&m) == RDB_OK);
    card(&m, 64, 16, 63);
    CHECK(quick_layout(&m, QS_WORK | QS_GAMES | QS_DEMOS) == QS_ERR_TOO_SMALL);
    card(&m, 127, 16, 63);
    CHECK(quick_layout(&m, 0) == QS_ERR_TOO_SMALL);
}

/* The same card has different geometry per Kickstart: sizes must not drift. */
static void test_layout_geometry_independent(void)
{
    static const uint32_t geo[][2] = { {16, 63}, {1, 63}, {15, 63}, {16, 255}, {4, 32}, {255, 63} };
    unsigned g, mbs[] = { 256, 1024, 8192 }, k;
    for (k = 0; k < 3; k++) for (g = 0; g < sizeof geo / sizeof geo[0]; g++) {
        RdbModel m; uint32_t cb;
        card(&m, mbs[k], geo[g][0], geo[g][1]);
        cb = m.cyl_blocks;
        CHECK(quick_layout(&m, QS_WORK | QS_DEMOS) == RDB_OK);
        CHECK(m.num_parts == 3);
        CHECK(rdb_validate(&m) == RDB_OK);
        CHECK((m.hi_cyl + 1u) * cb <= 8388608u);
        {   /* System: min(500 MB, half the usable card), within one cylinder */
            uint32_t usable = (m.hi_cyl + 1u) * cb, want = 500u * MB_BLOCKS;
            if (want > usable / 2u) want = usable / 2u;
            CHECK(pblocks(&m, 0) >= want && pblocks(&m, 0) < want + cb);
        }
        {   /* the two others stay within one cylinder of each other and under 2 GiB */
            CHECK(abs_diff(pblocks(&m, 1), pblocks(&m, 2)) <= cb || pblocks(&m, 2) > pblocks(&m, 1));
            CHECK(pblocks(&m, 1) <= 4194304u && pblocks(&m, 2) <= 4194304u);
            if (mbs[k] == 256) {   /* the rest splits in half, whatever the geometry */
                uint32_t rest = (m.hi_cyl + 1u - m.lo_cyl) * cb - pblocks(&m, 0);   /* after the reserved cylinders */
                CHECK(abs_diff(pblocks(&m, 1), rest / 2u) <= 2u * cb);
            }
        }
        CHECK(m.parts[2].high_cyl <= m.hi_cyl);
    }
}

/* A transfer's end offset is 32 bits: a partition ending exactly on 4 GiB wraps it to 0. */
static uint64_t last_end_byte(const RdbModel *m)
{ return ((uint64_t)m->parts[m->num_parts - 1].high_cyl + 1u) * m->cyl_blocks * 512u; }

/* Cylinder sizes that divide 2^23: 512 and 2048 blocks. */
static void test_layout_below_4gib_exact_cylinders(void)
{
    static const uint32_t geo[][2] = { {16, 32}, {32, 64} };
    unsigned g;
    for (g = 0; g < 2; g++) {
        RdbModel m;
        card(&m, 8192, geo[g][0], geo[g][1]);
        CHECK(quick_layout(&m, QS_WORK | QS_GAMES | QS_DEMOS) == RDB_OK);
        CHECK(m.num_parts == 4);
        CHECK(m.parts[3].high_cyl == m.hi_cyl);
        CHECK(last_end_byte(&m) < 4294967296ull);
        CHECK(last_end_byte(&m) == 4294967296ull - (uint64_t)m.cyl_blocks * 512u);   /* one cylinder given up */
        CHECK(rdb_validate(&m) == RDB_OK);
    }
}

/* 1008 blocks per cylinder never lands on the boundary: the table is what it always was. */
static void test_layout_1008_unchanged(void)
{
    RdbModel m;
    rdb_init_model(&m, 16644, 16, 63);
    CHECK(quick_layout(&m, QS_WORK | QS_GAMES | QS_DEMOS) == RDB_OK);
    CHECK(m.hi_cyl == 8321u);
    CHECK(m.num_parts == 4);
    CHECK(m.parts[0].low_cyl == 2u    && m.parts[0].high_cyl == 1017u);
    CHECK(m.parts[1].low_cyl == 1018u && m.parts[1].high_cyl == 3451u);
    CHECK(m.parts[2].low_cyl == 3452u && m.parts[2].high_cyl == 5885u);
    CHECK(m.parts[3].low_cyl == 5886u && m.parts[3].high_cyl == 8321u);
    CHECK(last_end_byte(&m) == 4294950912ull);
}

static void test_volume_names(void)
{
    uint32_t mask = QS_WORK | QS_DEMOS;
    CHECK(strcmp(quick_volume_name(mask, 0), "System") == 0);
    CHECK(strcmp(quick_volume_name(mask, 1), "Work") == 0);
    CHECK(strcmp(quick_volume_name(mask, 2), "Demos") == 0);
    CHECK(quick_volume_name(mask, 3) == NULL);
    CHECK(strcmp(quick_volume_name(QS_GAMES, 1), "Games") == 0);
    CHECK(quick_volume_name(0, 1) == NULL);
}

static void test_layout_failure_clears(void)
{
    RdbModel m;
    rdb_init_model(&m, 16644, 16, 63);
    CHECK(quick_layout(&m, QS_WORK) == RDB_OK && m.num_parts == 2);
    card(&m, 64, 16, 63);                         /* too small, but hold stale parts */
    m.num_parts = 2; strcpy(m.parts[0].name, "DH0"); strcpy(m.parts[1].name, "DH1");
    CHECK(quick_layout(&m, QS_WORK) == QS_ERR_TOO_SMALL);
    CHECK(m.num_parts == 0);
    rdb_init_model(&m, 16644, 16, 63);
    quick_layout(&m, 7);
    m.cylinders = 0;                              /* degenerate geometry */
    CHECK(quick_layout(&m, 7) == QS_ERR_TOO_SMALL && m.num_parts == 0);
}

int main(void)
{
    test_layout_8gb_two_ticked();
    test_layout_partition_cap();
    test_layout_system_only();
    test_layout_small_cards();
    test_layout_geometry_independent();
    test_layout_below_4gib_exact_cylinders();
    test_layout_1008_unchanged();
    test_volume_names();
    test_layout_failure_clears();
    if (g_fail) { printf("%d CHECK(s) FAILED\n", g_fail); return 1; }
    printf("ALL TESTS PASSED\n");
    return 0;
}
