/* Host unit tests for discover.c pure helpers. Built and run by run-host-tests.sh.
   The OS-bound discover_disks() is NOT exercised here (it needs AmigaOS). */
#include <stdio.h>
#include <string.h>
#include "../src/discover.h"

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); g_fail++; } } while (0)

static void test_bcpl_to_c(void)
{
    /* BCPL string: length byte 4, then "scsi" */
    unsigned char b[8] = { 4, 's', 'c', 's', 'i', 0, 0, 0 };
    char out[16];
    disc_bcpl_to_c(b, out, sizeof(out));
    CHECK(strcmp(out, "scsi") == 0);

    /* truncation to outsz-1 */
    unsigned char b2[6] = { 5, 'a','b','c','d','e' };
    char small[3];
    disc_bcpl_to_c(b2, small, sizeof(small));
    CHECK(strcmp(small, "ab") == 0);
}

static void test_find(void)
{
    DiscDisk list[2];
    memset(list, 0, sizeof(list));
    strcpy(list[0].driver, "scsi.device"); list[0].unit = 0;
    strcpy(list[1].driver, "scsi.device"); list[1].unit = 1;
    CHECK(disc_find(list, 2, "scsi.device", 0) == 0);
    CHECK(disc_find(list, 2, "scsi.device", 1) == 1);
    CHECK(disc_find(list, 2, "scsi.device", 2) == -1);
    CHECK(disc_find(list, 2, "lide.device", 0) == -1);
}

static void test_blocks_to_mb(void)
{
    /* 8192 blocks * 512 = 4 MiB */
    CHECK(disc_blocks_to_mb(8192, 512) == 4);
    /* 1,000,000 blocks * 512 = 488 MiB (floor) */
    CHECK(disc_blocks_to_mb(1000000, 512) == 488);
    CHECK(disc_blocks_to_mb(0, 512) == 0);
    CHECK(disc_blocks_to_mb(100, 0) == 0);   /* guard against /0 */
}

static void test_is_device_name(void)
{
    CHECK(disc_is_device_name("scsi.device") == 1);
    CHECK(disc_is_device_name("uaehf.device") == 1);
    CHECK(disc_is_device_name("a.device") == 1);          /* min valid length 8 */
    CHECK(disc_is_device_name("TRACKDISK.DEVICE") == 1);  /* case-insensitive */
    CHECK(disc_is_device_name("RAW") == 0);               /* handler, not a device */
    CHECK(disc_is_device_name("PRT") == 0);
    CHECK(disc_is_device_name("") == 0);
    CHECK(disc_is_device_name(".device") == 0);           /* nothing before suffix */
    CHECK(disc_is_device_name("foo.handler") == 0);
    {
        char bad[5]; bad[0]='g'; bad[1]=(char)0x01; bad[2]='B'; bad[3]=0;
        CHECK(disc_is_device_name(bad) == 0);             /* non-printable */
    }
}

static void test_is_partitionable(void)
{
    /* Real disks with media: partitionable. */
    CHECK(disc_is_partitionable("scsi.device",  100) == 1);
    CHECK(disc_is_partitionable("uaehf.device", 8192) == 1);
    CHECK(disc_is_partitionable("lide.device",  1) == 1);
    /* Floppies are never RDB-partition targets. */
    CHECK(disc_is_partitionable("trackdisk.device", 1760) == 0);
    /* exact-match, not prefix: a different driver starting with the same text is OK */
    CHECK(disc_is_partitionable("trackdisk.device2", 100) == 1);
    /* Directory drives / no-media report 0 blocks. */
    CHECK(disc_is_partitionable("uae.device", 0) == 0);
    CHECK(disc_is_partitionable("scsi.device", 0) == 0);
}

int main(void)
{
    test_bcpl_to_c();
    test_find();
    test_blocks_to_mb();
    test_is_device_name();
    test_is_partitionable();
    if (g_fail) { printf("%d CHECK(s) FAILED\n", g_fail); return 1; }
    printf("DISCOVER TESTS PASSED\n");
    return 0;
}
