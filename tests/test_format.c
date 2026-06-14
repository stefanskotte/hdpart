/* Host unit tests for format_build_envec (pure). OS-bound format_partition()
   needs AmigaOS and is not exercised here. Built by run-host-tests.sh. */
#include <stdio.h>
#include <string.h>
#include "../src/format.h"
#include "../src/rdb.h"

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); g_fail++; } } while (0)

/* DE_ indexes (relative to env[0]=DE_TABLESIZE). */
enum { DE_TABLESIZE=0, DE_SIZEBLOCK=1, DE_SECORG=2, DE_NUMHEADS=3, DE_SECSPERBLK=4,
       DE_BLKSPERTRACK=5, DE_RESERVEDBLKS=6, DE_PREFAC=7, DE_INTERLEAVE=8,
       DE_LOWCYL=9, DE_HIGHCYL=10, DE_NUMBUFFERS=11, DE_BUFMEMTYPE=12,
       DE_MAXTRANSFER=13, DE_MASK=14, DE_BOOTPRI=15, DE_DOSTYPE=16 };

static void test_envec(void)
{
    RdbModel m;
    uint32_t env[FMT_ENV_LONGS];
    int rc;

    /* 100 cyl, 4 heads, 32 sectors, 512-byte blocks. */
    rdb_init_model(&m, 100, 4, 32);
    rc = rdb_add_partition_cyl(&m, "DH4", 10, 49, 0x444F5303u); /* FFS Intl */
    CHECK(rc >= 0);
    /* set flag fields the env carries */
    m.parts[0].num_buffers = 30;
    m.parts[0].maxtransfer = 0x0001FE00u;
    m.parts[0].mask        = 0x7FFFFFFEu;
    m.parts[0].boot_pri    = 0;

    CHECK(format_build_envec(&m, 0, env) == 0);
    CHECK(env[DE_TABLESIZE]    == 16);
    CHECK(env[DE_SIZEBLOCK]    == 128);   /* 512 / 4 longwords */
    CHECK(env[DE_SECORG]       == 0);
    CHECK(env[DE_NUMHEADS]     == 4);
    CHECK(env[DE_SECSPERBLK]   == 1);
    CHECK(env[DE_BLKSPERTRACK] == 32);
    CHECK(env[DE_RESERVEDBLKS] == 2);
    CHECK(env[DE_PREFAC]       == 0);
    CHECK(env[DE_INTERLEAVE]   == 0);
    CHECK(env[DE_LOWCYL]       == 10);
    CHECK(env[DE_HIGHCYL]      == 49);
    CHECK(env[DE_NUMBUFFERS]   == 30);
    CHECK(env[DE_BUFMEMTYPE]   == 0);
    CHECK(env[DE_MAXTRANSFER]  == 0x0001FE00u);
    CHECK(env[DE_MASK]         == 0x7FFFFFFEu);
    CHECK(env[DE_BOOTPRI]      == 0);
    CHECK(env[DE_DOSTYPE]      == 0x444F5303u);

    CHECK(format_build_envec(&m, 5, env) == -1); /* out of range */
}

int main(void)
{
    test_envec();
    if (g_fail) { printf("FORMAT TESTS FAILED (%d)\n", g_fail); return 1; }
    printf("FORMAT TESTS PASSED\n");
    return 0;
}
