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

int main(void)
{
    test_boot_match();
    test_mounted_not_boot();
    test_clear();
    test_boot_unknown_failsafe();
    test_case_insensitive_driver();
    if (g_fail) { printf("SAFETY TESTS FAILED (%d)\n", g_fail); return 1; }
    printf("SAFETY TESTS PASSED\n");
    return 0;
}
