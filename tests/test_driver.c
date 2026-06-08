/* Host unit tests for driver.c pure helper drv_find_romtag. The OS-bound
   driver_load_file() is NOT exercised here (it needs AmigaOS). */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "../src/driver.h"
#include "../src/endian.h"

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); g_fail++; } } while (0)

/* Lay a valid Resident romtag into buf at offset off, of the given rt_Type.
   The self-pointer (rt_MatchTag) is the low 32 bits of the tag's own address. */
static void put_romtag(unsigned char *buf, uint32_t off, int type)
{
    memset(buf + off, 0, 26);
    be_put16(buf + off + 0, 0x4AFC);                              /* rt_MatchWord */
    be_put32(buf + off + 2, (uint32_t)(uintptr_t)(buf + off));    /* rt_MatchTag (self) */
    buf[off + 12] = (unsigned char)type;                         /* rt_Type */
}

static void test_finds_valid_romtag(void)
{
    unsigned char buf[128];
    uint32_t off = 99; int type = -1;
    memset(buf, 0, sizeof(buf));
    put_romtag(buf, 8, DRV_NT_DEVICE);   /* place at an even offset */
    CHECK(drv_find_romtag(buf, sizeof(buf), 0, &off, &type) == 1);
    CHECK(off == 8);
    CHECK(type == DRV_NT_DEVICE);
}

static void test_rejects_bare_matchword(void)
{
    /* 0x4AFC present but rt_MatchTag is NOT the self-pointer -> not a romtag. */
    unsigned char buf[128];
    uint32_t off = 99; int type = -1;
    memset(buf, 0, sizeof(buf));
    be_put16(buf + 8, 0x4AFC);
    be_put32(buf + 10, 0x12345678u);     /* bogus, not self */
    CHECK(drv_find_romtag(buf, sizeof(buf), 0, &off, &type) == 0);
}

static void test_none_when_absent(void)
{
    unsigned char buf[128];
    uint32_t off = 99; int type = -1;
    memset(buf, 0, sizeof(buf));
    CHECK(drv_find_romtag(buf, sizeof(buf), 0, &off, &type) == 0);
}

static void test_reports_non_device_type(void)
{
    /* A valid romtag that is NOT a device (e.g. NT_LIBRARY=9): found, type 9. */
    unsigned char buf[128];
    uint32_t off = 99; int type = -1;
    memset(buf, 0, sizeof(buf));
    put_romtag(buf, 16, 9);
    CHECK(drv_find_romtag(buf, sizeof(buf), 0, &off, &type) == 1);
    CHECK(off == 16);
    CHECK(type == 9);
}

static void test_respects_bounds(void)
{
    /* A romtag whose 26 bytes would straddle the end must not be reported.
       buf is large enough to write the tag safely; we pass size=40 so that
       20+26=46 > 40 makes it out of bounds for drv_find_romtag. */
    unsigned char buf[64];
    uint32_t off = 99; int type = -1;
    memset(buf, 0, sizeof(buf));
    put_romtag(buf, 20, DRV_NT_DEVICE);  /* 20+26 = 46 > 40: out of bounds */
    CHECK(drv_find_romtag(buf, 40, 0, &off, &type) == 0);
}

static void test_finds_second_when_start_advanced(void)
{
    /* Two romtags; starting past the first returns the second. */
    unsigned char buf[128];
    uint32_t off = 99; int type = -1;
    memset(buf, 0, sizeof(buf));
    put_romtag(buf, 8,  9);              /* first: a library */
    put_romtag(buf, 48, DRV_NT_DEVICE);  /* second: the device */
    CHECK(drv_find_romtag(buf, sizeof(buf), 0,  &off, &type) == 1);
    CHECK(off == 8 && type == 9);
    CHECK(drv_find_romtag(buf, sizeof(buf), 10, &off, &type) == 1);
    CHECK(off == 48 && type == DRV_NT_DEVICE);
}

int main(void)
{
    test_finds_valid_romtag();
    test_rejects_bare_matchword();
    test_none_when_absent();
    test_reports_non_device_type();
    test_respects_bounds();
    test_finds_second_when_start_advanced();
    if (g_fail) { printf("%d driver test(s) FAILED\n", g_fail); return 1; }
    printf("DRIVER TESTS PASSED\n");
    return 0;
}
