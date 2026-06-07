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

static void test_endian(void)
{
    uint8_t b[4];
    be_put32(b, 0x12345678u);
    CHECK(b[0]==0x12 && b[1]==0x34 && b[2]==0x56 && b[3]==0x78);
    CHECK(be_get32(b) == 0x12345678u);
}

int main(void)
{
    test_endian();
    /* further test functions appended in later tasks */
    if (g_fail) { printf("%d CHECK(s) FAILED\n", g_fail); return 1; }
    printf("ALL TESTS PASSED\n");
    return 0;
}
