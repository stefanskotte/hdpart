/* HDPart Quick setup: layout arithmetic and setup-pending check. */
#include <string.h>
#include "quick.h"

#define QS_MAX_BLOCKS   8388608u      /* 4 GiB at 512 bytes */
#define QS_MIN_MB       128u
#define QS_SYSTEM_MB    500u
#define QS_PART_CAP_MB  2048u         /* 2 GiB per partition (Task 1: unchanged) */

static const char *const QS_NAMES[3] = { "Work", "Games", "Demos" };

static int ticked(uint32_t mask)
{
    int n = 0;
    if (mask & QS_WORK)  n++;
    if (mask & QS_GAMES) n++;
    if (mask & QS_DEMOS) n++;
    return n;
}

const char *quick_volume_name(uint32_t mask, int part_index)
{
    int i, k = 0;
    if (part_index < 0) return 0;
    if (part_index == 0) return "System";
    for (i = 0; i < 3; i++) {
        if (mask & (1u << i)) {
            if (++k == part_index) return QS_NAMES[i];
        }
    }
    return 0;
}

static int layout_inner(RdbModel *m, uint32_t mask)
{
    uint32_t bb = m->block_bytes ? m->block_bytes : RDB_BLOCK_BYTES;
    uint32_t mb_blocks = 1048576u / bb;               /* blocks per MB */
    uint32_t max_blocks = QS_MAX_BLOCKS / (bb >= 512u ? bb / 512u : 1u);  /* 4 GiB in this block size */
    uint32_t cb = m->cyl_blocks;
    uint32_t cyls, usable_blocks, sys_blocks, sys_cyls, cap_cyls;
    uint32_t next, rem, share, extra, i;
    int n = ticked(mask), idx;
    char name[8];

    if (cb == 0 || m->cylinders == 0) return QS_ERR_TOO_SMALL;
    cyls = m->cylinders;
    /* Whole cylinders ending below 4 GiB: ending exactly on it wraps a transfer's end offset to 0. */
    if (cyls > (max_blocks - 1u) / cb) cyls = (max_blocks - 1u) / cb;
    usable_blocks = cyls * cb;
    if (usable_blocks / mb_blocks < QS_MIN_MB) return QS_ERR_TOO_SMALL;

    m->num_parts = 0;
    m->hi_cyl = cyls - 1u;

    sys_blocks = QS_SYSTEM_MB * mb_blocks;
    if (sys_blocks > usable_blocks / 2u) sys_blocks = usable_blocks / 2u;
    sys_cyls = (sys_blocks + cb - 1u) / cb;
    if (sys_cyls < 1u) sys_cyls = 1u;
    if (m->lo_cyl + sys_cyls - 1u > m->hi_cyl) return QS_ERR_TOO_SMALL;

    idx = rdb_add_partition_cyl(m, "DH0", m->lo_cyl, m->lo_cyl + sys_cyls - 1u,
                                RDB_DOSTYPE_FFS_INTL);
    if (idx < 0) return idx;
    m->parts[idx].bootable = 1;
    m->parts[idx].flags |= RDB_PBFF_BOOTABLE;
    m->parts[idx].boot_pri = 0;

    next = m->lo_cyl + sys_cyls;
    if (n == 0) return RDB_OK;
    rem = m->hi_cyl + 1u - next;
    cap_cyls = (QS_PART_CAP_MB * mb_blocks) / cb;
    share = rem / (uint32_t)n;
    extra = rem % (uint32_t)n;
    if (share < 1u || cap_cyls < 1u) return QS_ERR_TOO_SMALL;

    for (i = 0; i < (uint32_t)n; i++) {
        uint32_t len = share + ((i + 1u == (uint32_t)n) ? extra : 0u);
        if (len > cap_cyls) len = cap_cyls;
        name[0] = 'D'; name[1] = 'H'; name[2] = (char)('1' + i); name[3] = 0;
        idx = rdb_add_partition_cyl(m, name, next, next + len - 1u, RDB_DOSTYPE_FFS_INTL);
        if (idx < 0) return idx;
        next += len;
    }
    return RDB_OK;
}

int quick_layout(RdbModel *m, uint32_t mask)
{
    int rc;
    m->num_parts = 0;
    rc = layout_inner(m, mask);
    if (rc != RDB_OK) m->num_parts = 0;      /* never leave stale or partial partitions */
    return rc;
}

int quick_geometry_agrees(uint32_t cylinders, uint32_t heads, uint32_t sectors, uint32_t total)
{
    uint32_t cb, product, diff;
    if (!cylinders || !heads || !sectors || !total) return 0;
    if (heads > 0xFFFFFFFFu / sectors) return 0;
    cb = heads * sectors;
    if (cylinders > 0xFFFFFFFFu / cb) return 0;
    product = cylinders * cb;
    diff = product > total ? product - total : total - product;
    return diff <= total / 64u;
}

uint32_t quick_cap_cylinders(uint32_t cylinders, uint32_t heads, uint32_t sectors, uint32_t total)
{
    uint32_t most;
    if (!heads || !sectors || heads > 0xFFFFFFFFu / sectors) return cylinders;
    most = total / (heads * sectors);
    return cylinders > most ? most : cylinders;
}

int quick_pending_valid(const RdbModel *m)
{
    uint32_t mask;
    int i;
    if (!(m->setup_pending & 0x80u) || (m->setup_pending & 0x78u)) return 0;
    mask = m->setup_pending & 7u;
    if (m->num_parts != 1 + ticked(mask)) return 0;
    for (i = 0; i < m->num_parts; i++) {
        const RdbPartition *p = &m->parts[i];
        if (p->name[0] != 'D' || p->name[1] != 'H' || p->name[2] != (char)('0' + i) ||
            p->name[3] != 0) return 0;
        if (p->dos_type != RDB_DOSTYPE_FFS_INTL) return 0;
    }
    if (!m->parts[0].bootable) return 0;
    return 1;
}
