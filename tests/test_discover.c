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

int main(void)
{
    test_bcpl_to_c();
    test_find();
    test_blocks_to_mb();
    if (g_fail) { printf("%d CHECK(s) FAILED\n", g_fail); return 1; }
    printf("DISCOVER TESTS PASSED\n");
    return 0;
}
