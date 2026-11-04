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

static MountedPart mkr(const char *drv, uint32_t unit, uint32_t lo, uint32_t hi, const char *name,
                       uint32_t cb, uint32_t type)
{
    MountedPart p = mkp(drv, unit, lo, hi, name);
    p.cyl_blocks = cb;
    p.dos_type = type;
    return p;
}

static void test_resume_decide(void)
{
    const uint32_t T = 0x444F5303u, CB = 1008;
#define RD(parts, n) safety_resume_decide("scsi.device", 0, "DH1", 100, 199, CB, T, parts, n)
    MountedPart a[3];

    CHECK(RD(a, 0) == SAFETY_RESUME_NONE);
    a[0] = mkr("scsi.device", 0, 2, 99, "DH0", CB, T);            /* a neighbour is no concern */
    CHECK(RD(a, 1) == SAFETY_RESUME_NONE);
    a[1] = mkr("scsi.device", 0, 100, 199, "DH1", CB, T);
    CHECK(RD(a, 2) == SAFETY_RESUME_MATCH);
    a[1] = mkr("SCSI.DEVICE", 0, 100, 199, "dh1", CB, T);         /* DOS and exec ignore case */
    CHECK(RD(a, 2) == SAFETY_RESUME_MATCH);
    a[1] = mkr("scsi.device", 0, 100, 198, "DH1", CB, T);         /* bounds */
    CHECK(RD(a, 2) == SAFETY_RESUME_MISMATCH);
    a[1] = mkr("scsi.device", 0, 101, 199, "DH1", CB, T);
    CHECK(RD(a, 2) == SAFETY_RESUME_MISMATCH);
    a[1] = mkr("scsi.device", 0, 100, 199, "DH1", 512, T);        /* same cylinders, other blocks */
    CHECK(RD(a, 2) == SAFETY_RESUME_MISMATCH);
    a[1] = mkr("scsi.device", 0, 100, 199, "DH1", CB, 0x50465303u); /* another filesystem */
    CHECK(RD(a, 2) == SAFETY_RESUME_MISMATCH);
    a[1] = mkr("scsi.device", 0, 100, 199, "OLD", CB, T);         /* another name on these blocks */
    CHECK(RD(a, 2) == SAFETY_RESUME_MISMATCH);
    a[1] = mkr("scsi.device", 0, 150, 160, "OLD", CB, T);         /* part of them */
    CHECK(RD(a, 2) == SAFETY_RESUME_MISMATCH);
    a[1] = mkr("scsi.device", 1, 100, 199, "DH1", CB, T);         /* this name, another unit */
    CHECK(RD(a, 2) == SAFETY_RESUME_MISMATCH);
    a[1] = mkr("other.device", 0, 100, 199, "DH1", CB, T);        /* this name, another driver */
    CHECK(RD(a, 2) == SAFETY_RESUME_MISMATCH);
    a[1] = mkr("scsi.device", 1, 100, 199, "OTHER", CB, T);       /* someone else's disk entirely */
    CHECK(RD(a, 2) == SAFETY_RESUME_NONE);
    a[1] = mkr("scsi.device", 0, 100, 199, "DH1", CB, T);         /* a match, and a second mount */
    a[2] = mkr("scsi.device", 0, 100, 199, "DH1", CB, T);
    CHECK(RD(a, 3) == SAFETY_RESUME_MISMATCH);
#undef RD
}

/* A driver module, as bytes: padding, then the given 6-byte instructions. */
static uint32_t put6(uint8_t *m, uint32_t at, uint16_t op, uint32_t arg)
{
    m[at] = (uint8_t)(op >> 8); m[at + 1] = (uint8_t)op;
    m[at + 2] = (uint8_t)(arg >> 24); m[at + 3] = (uint8_t)(arg >> 16);
    m[at + 4] = (uint8_t)(arg >> 8);  m[at + 5] = (uint8_t)arg;
    return at + 6;
}

static void test_driver_marker(void)
{
    uint8_t m[256];
    const uint16_t LEA_A4 = 0x49F9, MOVEL = 0x2B7C;

    memset(m, 0x4E, sizeof(m));                 /* no base at all: doubt */
    CHECK(safety_driver_is_ide(m, sizeof(m)) == 0);
    CHECK(safety_driver_is_ide(m, 0) == 0);
    CHECK(safety_driver_is_ide(NULL, 100) == 0);

    memset(m, 0x4E, sizeof(m));                 /* Gayle build (A600, A1200) */
    put6(m, 40, LEA_A4, 0x00DA2000);
    CHECK(safety_driver_is_ide(m, sizeof(m)) == 1);

    memset(m, 0x4E, sizeof(m));                 /* A4000 and A4000T IDE build */
    put6(m, 40, LEA_A4, 0x00DD2020);
    CHECK(safety_driver_is_ide(m, sizeof(m)) == 1);

    memset(m, 0x4E, sizeof(m));                 /* A3000: SDMAC */
    put6(m, 40, LEA_A4, 0x00DD0000);
    CHECK(safety_driver_is_ide(m, sizeof(m)) == 0);

    memset(m, 0x4E, sizeof(m));                 /* A4000T: NCR */
    put6(m, 40, LEA_A4, 0x00DD0040);
    CHECK(safety_driver_is_ide(m, sizeof(m)) == 0);

    memset(m, 0x4E, sizeof(m));                 /* IDE base with a SCSI one anywhere: doubt */
    put6(m, 40, LEA_A4, 0x00DA2000);
    put6(m, 80, MOVEL, 0x00DD0000);
    CHECK(safety_driver_is_ide(m, sizeof(m)) == 0);
    put6(m, 80, MOVEL, 0x00DD0040);
    CHECK(safety_driver_is_ide(m, sizeof(m)) == 0);

    memset(m, 0x4E, sizeof(m));                 /* both IDE bases: not a build that exists */
    put6(m, 40, LEA_A4, 0x00DA2000);
    put6(m, 80, LEA_A4, 0x00DD2020);
    CHECK(safety_driver_is_ide(m, sizeof(m)) == 0);

    memset(m, 0x4E, sizeof(m));                 /* the address not as this instruction */
    put6(m, 40, MOVEL, 0x00DA2000);
    CHECK(safety_driver_is_ide(m, sizeof(m)) == 0);

    memset(m, 0x4E, sizeof(m));                 /* odd offset is not an instruction */
    put6(m, 41, LEA_A4, 0x00DA2000);
    CHECK(safety_driver_is_ide(m, sizeof(m)) == 0);

    memset(m, 0x4E, sizeof(m));                 /* ends exactly at the buffer's end, and just past it */
    put6(m, 250, LEA_A4, 0x00DA2000);
    CHECK(safety_driver_is_ide(m, 256) == 1);
    CHECK(safety_driver_is_ide(m, 255) == 0);
}

int main(void)
{
    test_boot_match();
    test_mounted_not_boot();
    test_clear();
    test_boot_unknown_failsafe();
    test_case_insensitive_driver();
    test_part_conflict();
    test_resume_decide();
    test_driver_marker();
    if (g_fail) { printf("SAFETY TESTS FAILED (%d)\n", g_fail); return 1; }
    printf("SAFETY TESTS PASSED\n");
    return 0;
}
