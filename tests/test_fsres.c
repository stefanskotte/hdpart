/* Host unit tests for the pure fsres helpers. */
#include <stdio.h>
#include <string.h>
#include "../src/fsres.h"
#include "../src/rdb.h"

static int fails;
#define CHECK(c) do { if(!(c)){ printf("FAIL %s:%d %s\n",__FILE__,__LINE__,#c); fails++; } } while(0)

static void test_find_embedded(void)
{
    RdbModel m; memset(&m, 0, sizeof m);
    static uint8_t seg[8] = {0,0,3,0xF3, 1,2,3,4};
    m.num_fs = 1;
    m.fs[0].dos_type = 0x50465303u;   /* PFS\3 */
    m.fs[0].seg_data = seg; m.fs[0].seg_len = 8;
    /* family match: query DosType differs in low byte but same family */
    CHECK(fsres_find_embedded(&m, 0x50465300u) == &m.fs[0]);
    CHECK(fsres_find_embedded(&m, 0x50465303u) == &m.fs[0]);
    /* different family -> NULL */
    CHECK(fsres_find_embedded(&m, 0x53465302u) == 0);
    /* empty seg_data -> NULL */
    m.fs[0].seg_data = 0;
    CHECK(fsres_find_embedded(&m, 0x50465303u) == 0);
}

static void test_resolve_fields_patched(void)
{
    RdbFileSys fs; memset(&fs, 0, sizeof fs);
    fs.dos_type = 0x50465303u; fs.version = (19u<<16)|2u;
    fs.patch_flags = FSPF_STACKSIZE | FSPF_PRIORITY | FSPF_GLOBALVEC; /* 0x130 */
    fs.dn_stack = 8192; fs.dn_pri = 5; fs.dn_globalvec = 0xFFFFFFFFu;
    FsHandlerFields f; memset(&f, 0, sizeof f);
    fsres_resolve_fields(&fs, &f);
    CHECK(f.dos_type == 0x50465303u);
    CHECK(f.patch_flags == 0x130u);
    CHECK(f.dn_stack == 8192);       /* patched value honoured */
    CHECK(f.dn_pri == 5);
    CHECK(f.dn_globalvec == 0xFFFFFFFFu);
    CHECK(f.dn_type == 0 && f.dn_handler == 0); /* not patched -> 0 */
}

static void test_resolve_fields_defaults(void)
{
    RdbFileSys fs; memset(&fs, 0, sizeof fs);
    fs.dos_type = 0x444F5303u;        /* DOS\3 */
    fs.patch_flags = 0;               /* nothing patched (e.g. OS3.2 FSHD) */
    fs.dn_stack = 0; fs.dn_pri = 0; fs.dn_globalvec = 0;
    FsHandlerFields f; memset(&f, 0, sizeof f);
    fsres_resolve_fields(&fs, &f);
    CHECK(f.dn_stack == 4096);        /* default */
    CHECK(f.dn_pri == 10);            /* default */
    CHECK(f.dn_globalvec == 0xFFFFFFFFu); /* default -1 */
}

static void test_bstr_eq(void)
{
    uint8_t a[5] = {3,'D','H','5',0};
    CHECK(fsres_bstr_eq(a, "DH5") == 1);
    CHECK(fsres_bstr_eq(a, "DH6") == 0);
    CHECK(fsres_bstr_eq(a, "DH")  == 0);   /* length mismatch */
    CHECK(fsres_bstr_eq(a, "DH50")== 0);
}

int main(void)
{
    test_find_embedded();
    test_resolve_fields_patched();
    test_resolve_fields_defaults();
    test_bstr_eq();
    printf("test_fsres: %s\n", fails ? "FAILURES" : "ALL TESTS PASSED");
    return fails ? 1 : 0;
}
