#include "rdb.h"
#include "endian.h"
#include <string.h>
/* implementation grows over the following tasks */

static int str_eq(const char *a, const char *b)
{
    while (*a && (*a == *b)) { a++; b++; }
    return *a == *b;
}

uint32_t rdb_sum_longs(const uint8_t *blk, uint32_t summed_longs)
{
    uint32_t sum = 0, i;
    for (i = 0; i < summed_longs; i++)
        sum += be_get32(blk + i * 4);
    return sum;
}

void rdb_set_checksum(uint8_t *blk, uint32_t summed_longs, uint32_t chk_off)
{
    uint32_t sum;
    be_put32(blk + chk_off, 0);
    sum = rdb_sum_longs(blk, summed_longs);
    be_put32(blk + chk_off, (uint32_t)(0u - sum)); /* two's complement */
}

int rdb_checksum_ok(const uint8_t *blk, uint32_t summed_longs)
{
    return rdb_sum_longs(blk, summed_longs) == 0u;
}

uint32_t rdb_mb_to_cyls(uint32_t mb, uint32_t cyl_blocks, uint32_t block_bytes)
{
    /* Use 64-bit to avoid overflow on large sizes. */
    uint64_t bytes      = (uint64_t)mb * 1024u * 1024u;
    uint64_t cyl_bytes  = (uint64_t)cyl_blocks * block_bytes;
    uint64_t cyls       = (bytes + cyl_bytes - 1) / cyl_bytes; /* ceil */
    if (cyls == 0) cyls = 1;
    return (uint32_t)cyls;
}

uint32_t rdb_cyls_to_mb(uint32_t cyls, uint32_t cyl_blocks, uint32_t block_bytes)
{
    uint64_t bytes = (uint64_t)cyls * cyl_blocks * block_bytes;
    return (uint32_t)(bytes / (1024u * 1024u)); /* floor */
}

void rdb_init_model(RdbModel *m, uint32_t cyl, uint32_t heads, uint32_t sectors)
{
    memset(m, 0, sizeof(*m));
    m->cylinders   = cyl;
    m->heads       = heads;
    m->sectors     = sectors;
    m->block_bytes = RDB_BLOCK_BYTES;
    m->cyl_blocks  = heads * sectors;
    m->lo_cyl      = RDB_RESERVED_CYLS;
    m->hi_cyl      = (cyl > 0) ? cyl - 1 : 0;
    m->rdb_blocks_lo = 0;
    m->rdb_blocks_hi = RDB_RESERVED_CYLS * m->cyl_blocks - 1;
    m->num_parts   = 0;
}

static uint32_t next_free_cyl(const RdbModel *m)
{
    uint32_t top = m->lo_cyl;
    int i;
    for (i = 0; i < m->num_parts; i++)
        if (m->parts[i].high_cyl + 1 > top)
            top = m->parts[i].high_cyl + 1;
    return top;
}

static int name_taken(const RdbModel *m, const char *name, int skip)
{
    int i;
    for (i = 0; i < m->num_parts; i++) {
        if (i == skip) continue;
        if (str_eq(m->parts[i].name, name)) return 1;
    }
    return 0;
}

static void copy_name(char *dst, const char *src)
{
    int i;
    for (i = 0; i < RDB_NAME_LEN - 1 && src[i]; i++) dst[i] = src[i];
    dst[i] = 0;
}

int rdb_add_partition(RdbModel *m, const char *name, uint32_t size_mb,
                      uint32_t dos_type)
{
    uint32_t start, cyls, end;
    RdbPartition *p;

    if (m->num_parts >= RDB_MAX_PARTS) return RDB_ERR_TOO_MANY;
    if (!name || !name[0])             return RDB_ERR_BAD_NAME;
    if (name_taken(m, name, -1))       return RDB_ERR_DUP_NAME;

    start = next_free_cyl(m);
    cyls  = rdb_mb_to_cyls(size_mb, m->cyl_blocks, m->block_bytes);
    if (cyls == 0) cyls = 1;
    end   = start + cyls - 1;
    if (start > m->hi_cyl || end > m->hi_cyl) return RDB_ERR_NO_SPACE;

    p = &m->parts[m->num_parts++];
    copy_name(p->name, name);
    p->low_cyl     = start;
    p->high_cyl    = end;
    p->dos_type    = dos_type;
    p->num_buffers = 30;
    p->boot_pri    = 0;
    p->bootable    = 0;
    return RDB_OK;
}

int rdb_delete_partition(RdbModel *m, int index)
{
    int i;
    if (index < 0 || index >= m->num_parts) return RDB_ERR_BAD_NAME;
    for (i = index; i < m->num_parts - 1; i++)
        m->parts[i] = m->parts[i + 1];
    m->num_parts--;
    return RDB_OK;
}

int rdb_validate(const RdbModel *m)
{
    int i, j;
    for (i = 0; i < m->num_parts; i++) {
        const RdbPartition *a = &m->parts[i];
        if (a->low_cyl < m->lo_cyl || a->high_cyl > m->hi_cyl)
            return RDB_ERR_NO_SPACE;
        if (a->low_cyl > a->high_cyl)
            return RDB_ERR_OVERLAP;
        if (!a->name[0]) return RDB_ERR_BAD_NAME;
        for (j = i + 1; j < m->num_parts; j++) {
            const RdbPartition *b = &m->parts[j];
            if (str_eq(a->name, b->name)) return RDB_ERR_DUP_NAME;
            if (a->low_cyl <= b->high_cyl && b->low_cyl <= a->high_cyl)
                return RDB_ERR_OVERLAP;
        }
    }
    return RDB_OK;
}
