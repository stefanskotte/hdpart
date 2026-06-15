/* Host unit tests for safety_decide (pure). The OS-bound safety_classify()
   needs AmigaOS and is not exercised here. Built by run-host-tests.sh. */
#include <stdio.h>
#include <string.h>
#include "../src/safety.h"

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); g_fail++; } } while (0)

static MountEntry mk(const char *drv, uint32_t unit, const char *name)
{
    MountEntry m; memset(&m, 0, sizeof(m));
    strncpy(m.driver, drv, sizeof(m.driver) - 1);
    m.unit = unit;
    strncpy(m.name, name, sizeof(m.name) - 1);
    return m;
}

static MountedPart mkp(const char *drv, uint32_t unit,
                        uint32_t lo, uint32_t hi, const char *name)
{
    MountedPart p; memset(&p, 0, sizeof(p));
    strncpy(p.driver, drv, sizeof(p.driver) - 1);
    p.unit = unit;
    p.low_cyl  = lo;
    p.high_cyl = hi;
    strncpy(p.name, name, sizeof(p.name) - 1);
    return p;
}

static void test_boot_match(void)
{
    char names[8][8]; int nn = -1;
    MountEntry mounts[1] = { mk("scsi.device", 0, "DH0") };
    DevLiveness r = safety_decide("scsi.device", 0, 1, "scsi.device", 0,
                                  mounts, 1, names, 8, &nn);
    CHECK(r == DEV_BOOT);
}

static void test_mounted_not_boot(void)
{
    char names[8][8]; int nn = -1;
    MountEntry mounts[2] = { mk("scsi.device", 0, "DH4"), mk("scsi.device", 0, "DH5") };
    /* boot is the floppy (trackdisk.device), target is scsi unit 0 with mounts */
    DevLiveness r = safety_decide("scsi.device", 0, 1, "trackdisk.device", 0,
                                  mounts, 2, names, 8, &nn);
    CHECK(r == DEV_MOUNTED);
    CHECK(nn == 2);
    CHECK(strcmp(names[0], "DH4") == 0);
    CHECK(strcmp(names[1], "DH5") == 0);
}

static void test_clear(void)
{
    char names[8][8]; int nn = -1;
    MountEntry mounts[1] = { mk("scsi.device", 1, "DH0") };  /* different unit */
    DevLiveness r = safety_decide("scsi.device", 0, 1, "trackdisk.device", 0,
                                  mounts, 1, names, 8, &nn);
    CHECK(r == DEV_CLEAR);
    CHECK(nn == 0);
}

static void test_boot_unknown_failsafe(void)
{
    char names[8][8]; int nn = -1;
    /* No mounts on target and boot unknown -> cannot prove not-boot -> MOUNTED. */
    DevLiveness r = safety_decide("scsi.device", 0, 0, "", 0,
                                  0, 0, names, 8, &nn);
    CHECK(r == DEV_MOUNTED);
}

static void test_case_insensitive_driver(void)
{
    char names[8][8]; int nn = -1;
    MountEntry mounts[1] = { mk("scsi.device", 0, "DH0") };
    DevLiveness r = safety_decide("SCSI.DEVICE", 0, 1, "SCSI.DEVICE", 0,
                                  mounts, 1, names, 8, &nn);
    CHECK(r == DEV_BOOT);
}

static void test_part_conflict(void)
{
    char out[8];

    /* Same driver+unit, overlapping range -> 1 + correct name. */
    {
        MountedPart parts[1] = { mkp("scsi.device", 0, 10, 50, "DH1") };
        memset(out, 0, sizeof(out));
        CHECK(safety_part_conflict("scsi.device", 0, 20, 40, parts, 1, out) == 1);
        CHECK(strcmp(out, "DH1") == 0);
    }

    /* Different unit -> 0. */
    {
        MountedPart parts[1] = { mkp("scsi.device", 1, 10, 50, "DH2") };
        CHECK(safety_part_conflict("scsi.device", 0, 20, 40, parts, 1, NULL) == 0);
    }

    /* Different driver -> 0. */
    {
        MountedPart parts[1] = { mkp("ide.device", 0, 10, 50, "DH3") };
        CHECK(safety_part_conflict("scsi.device", 0, 20, 40, parts, 1, NULL) == 0);
    }

    /* Same unit, non-overlapping ranges (target entirely after) -> 0. */
    {
        MountedPart parts[1] = { mkp("scsi.device", 0, 10, 50, "DH4") };
        CHECK(safety_part_conflict("scsi.device", 0, 60, 80, parts, 1, NULL) == 0);
    }

    /* Same unit, non-overlapping ranges (target entirely before) -> 0. */
    {
        MountedPart parts[1] = { mkp("scsi.device", 0, 60, 80, "DH4b") };
        CHECK(safety_part_conflict("scsi.device", 0, 10, 50, parts, 1, NULL) == 0);
    }

    /* Identical range -> 1. */
    {
        MountedPart parts[1] = { mkp("scsi.device", 0, 10, 50, "DH5") };
        memset(out, 0, sizeof(out));
        CHECK(safety_part_conflict("scsi.device", 0, 10, 50, parts, 1, out) == 1);
        CHECK(strcmp(out, "DH5") == 0);
    }

    /* Partial overlap (target starts inside existing) -> 1. */
    {
        MountedPart parts[1] = { mkp("scsi.device", 0, 10, 50, "DH6") };
        CHECK(safety_part_conflict("scsi.device", 0, 40, 70, parts, 1, NULL) == 1);
    }

    /* Adjacent (a_high+1 == b_low): target high_cyl=9, existing low_cyl=10 -> 0. */
    {
        MountedPart parts[1] = { mkp("scsi.device", 0, 10, 50, "DH7") };
        CHECK(safety_part_conflict("scsi.device", 0, 0, 9, parts, 1, NULL) == 0);
    }

    /* Case-insensitive driver match -> 1. */
    {
        MountedPart parts[1] = { mkp("SCSI.DEVICE", 0, 10, 50, "DH8") };
        memset(out, 0, sizeof(out));
        CHECK(safety_part_conflict("scsi.device", 0, 10, 50, parts, 1, out) == 1);
        CHECK(strcmp(out, "DH8") == 0);
    }
}

int main(void)
{
    test_boot_match();
    test_mounted_not_boot();
    test_clear();
    test_boot_unknown_failsafe();
    test_case_insensitive_driver();
    test_part_conflict();
    if (g_fail) { printf("SAFETY TESTS FAILED (%d)\n", g_fail); return 1; }
    printf("SAFETY TESTS PASSED\n");
    return 0;
}
