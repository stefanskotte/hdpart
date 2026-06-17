/* Host unit tests for FSHD/LSEG embedded-filesystem support in rdb.c. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/rdb.h"

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

int main(void)
{
    test_lseg_block_count();
    test_model_free_safe();
    printf("test_fshd: %d run, %d failed\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
