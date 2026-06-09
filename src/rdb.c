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
    /* 1 MB = 1024*1024 bytes => (1024*1024)/block_bytes blocks per MB
       (2048 for 512-byte blocks). Computed in 32-bit so the freestanding
       m68000 build needs no libgcc 64-bit helpers. mb is capped well above
       any real Amiga drive (~2 TB before 32-bit overflow). Rounds UP. */
    uint32_t blocks_per_mb, total_blocks, cyls;
    if (block_bytes == 0) block_bytes = RDB_BLOCK_BYTES;
    if (cyl_blocks == 0)  return 1;
    blocks_per_mb = (1024u * 1024u) / block_bytes;
    total_blocks  = mb * blocks_per_mb;
    cyls = (total_blocks + cyl_blocks - 1u) / cyl_blocks; /* ceil */
    if (cyls == 0) cyls = 1;
    return cyls;
}

uint32_t rdb_cyls_to_mb(uint32_t cyls, uint32_t cyl_blocks, uint32_t block_bytes)
{
    /* Floor. Pure 32-bit (see rdb_mb_to_cyls). */
    uint32_t blocks_per_mb, total_blocks;
    if (block_bytes == 0) block_bytes = RDB_BLOCK_BYTES;
    blocks_per_mb = (1024u * 1024u) / block_bytes;
    if (blocks_per_mb == 0) return 0;
    total_blocks = cyls * cyl_blocks;
    return total_blocks / blocks_per_mb;
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

int rdb_largest_free_gap(const RdbModel *m, uint32_t *start, uint32_t *end)
{
    uint32_t cur = m->lo_cyl, best_s = 0, best_e = 0, best_len = 0;
    while (cur <= m->hi_cyl) {
        uint32_t next_start = m->hi_cyl + 1;
        int covering = -1, i;
        for (i = 0; i < m->num_parts; i++) {
            const RdbPartition *p = &m->parts[i];
            if (p->low_cyl <= cur && cur <= p->high_cyl) { covering = i; break; }
            if (p->low_cyl > cur && p->low_cyl < next_start) next_start = p->low_cyl;
        }
        if (covering >= 0) { cur = m->parts[covering].high_cyl + 1; continue; }
        {
            uint32_t gs = cur, ge = (next_start > 0 ? next_start - 1 : 0);
            if (ge > m->hi_cyl) ge = m->hi_cyl;
            if (ge >= gs) {
                uint32_t len = ge - gs + 1;
                if (len > best_len) { best_len = len; best_s = gs; best_e = ge; }
            }
        }
        cur = next_start;
    }
    if (best_len == 0) return 0;
    *start = best_s; *end = best_e;
    return 1;
}

int rdb_add_partition_at(RdbModel *m, const char *name, uint32_t start_cyl,
                         uint32_t size_mb, uint32_t dos_type)
{
    uint32_t cyls, end;
    RdbPartition *p;
    if (m->num_parts >= RDB_MAX_PARTS) return RDB_ERR_TOO_MANY;
    if (!name || !name[0])             return RDB_ERR_BAD_NAME;
    if (name_taken(m, name, -1))       return RDB_ERR_DUP_NAME;
    cyls = rdb_mb_to_cyls(size_mb, m->cyl_blocks, m->block_bytes);
    if (cyls == 0) cyls = 1;
    end = start_cyl + cyls - 1;
    if (start_cyl < m->lo_cyl || end > m->hi_cyl) return RDB_ERR_NO_SPACE;

    p = &m->parts[m->num_parts];        /* tentative */
    copy_name(p->name, name);
    p->low_cyl = start_cyl; p->high_cyl = end;
    p->dos_type = dos_type; p->num_buffers = 30; p->boot_pri = 0; p->bootable = 0;
    p->maxtransfer = RDB_DEFAULT_MAXTRANSFER; p->mask = RDB_DEFAULT_MASK;
    m->num_parts++;
    if (rdb_validate(m) != RDB_OK) { m->num_parts--; return RDB_ERR_OVERLAP; }
    return m->num_parts - 1;
}

int rdb_set_partition(RdbModel *m, int index, const char *name,
                      uint32_t size_mb, uint32_t dos_type)
{
    RdbPartition saved, *p;
    uint32_t cyls;
    int v;
    if (index < 0 || index >= m->num_parts) return RDB_ERR_RANGE;
    if (!name || !name[0])                  return RDB_ERR_BAD_NAME;
    if (name_taken(m, name, index))         return RDB_ERR_DUP_NAME;
    p = &m->parts[index];
    saved = *p;
    cyls = rdb_mb_to_cyls(size_mb, m->cyl_blocks, m->block_bytes);
    if (cyls == 0) cyls = 1;
    copy_name(p->name, name);
    p->dos_type = dos_type;
    p->high_cyl = p->low_cyl + cyls - 1;
    if (p->high_cyl > m->hi_cyl) { *p = saved; return RDB_ERR_NO_SPACE; }
    v = rdb_validate(m);
    if (v != RDB_OK) { *p = saved; return v; }
    return RDB_OK;
}

int rdb_add_partition_cyl(RdbModel *m, const char *name, uint32_t start_cyl,
                          uint32_t end_cyl, uint32_t dos_type)
{
    RdbPartition *p;
    if (m->num_parts >= RDB_MAX_PARTS) return RDB_ERR_TOO_MANY;
    if (!name || !name[0])             return RDB_ERR_BAD_NAME;
    if (name_taken(m, name, -1))       return RDB_ERR_DUP_NAME;
    if (end_cyl < start_cyl)           return RDB_ERR_RANGE;
    if (start_cyl < m->lo_cyl || end_cyl > m->hi_cyl) return RDB_ERR_NO_SPACE;

    p = &m->parts[m->num_parts];
    copy_name(p->name, name);
    p->low_cyl = start_cyl; p->high_cyl = end_cyl;
    p->dos_type = dos_type; p->num_buffers = 30; p->boot_pri = 0; p->bootable = 0;
    p->maxtransfer = RDB_DEFAULT_MAXTRANSFER; p->mask = RDB_DEFAULT_MASK;
    m->num_parts++;
    if (rdb_validate(m) != RDB_OK) { m->num_parts--; return RDB_ERR_OVERLAP; }
    return m->num_parts - 1;
}

int rdb_rename_partition(RdbModel *m, int index, const char *name, uint32_t dos_type)
{
    RdbPartition *p;
    if (index < 0 || index >= m->num_parts) return RDB_ERR_RANGE;
    if (!name || !name[0])                  return RDB_ERR_BAD_NAME;
    if (name_taken(m, name, index))         return RDB_ERR_DUP_NAME;
    p = &m->parts[index];
    copy_name(p->name, name);
    p->dos_type = dos_type;
    return RDB_OK;
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
    p->maxtransfer = RDB_DEFAULT_MAXTRANSFER;
    p->mask        = RDB_DEFAULT_MASK;
    return RDB_OK;
}

int rdb_delete_partition(RdbModel *m, int index)
{
    int i;
    if (index < 0 || index >= m->num_parts) return RDB_ERR_RANGE;
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
            return RDB_ERR_RANGE;
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

/* ---- RDB on-disk constants (byte offsets within a 512-byte block) ---- */
#define ID_RDSK 0x5244534Bu
#define ID_PART 0x50415254u
#define RDB_LOCATION_LIMIT 16u
#define NULLPTR 0xFFFFFFFFu

/* RigidDiskBlock field offsets */
#define RDB_o_ID            0
#define RDB_o_SummedLongs   4
#define RDB_o_ChkSum        8
#define RDB_o_HostID        12
#define RDB_o_BlockBytes    16
#define RDB_o_Flags         20
#define RDB_o_BadBlockList  24
#define RDB_o_PartitionList 28
#define RDB_o_FileSysHdr    32
#define RDB_o_DriveInit     36
#define RDB_o_Cylinders     64
#define RDB_o_Sectors       68
#define RDB_o_Heads         72
#define RDB_o_Interleave    76
#define RDB_o_Park          80
#define RDB_o_WritePreComp  96
#define RDB_o_ReducedWrite  100
#define RDB_o_StepRate      104
#define RDB_o_RDBBlocksLo   128
#define RDB_o_RDBBlocksHi   132
#define RDB_o_LoCylinder    136
#define RDB_o_HiCylinder    140
#define RDB_o_CylBlocks     144
#define RDB_o_AutoParkSecs  148
#define RDB_o_HighRDSKBlock 152
#define RDB_SUMMEDLONGS     64u

/* PartitionBlock field offsets */
#define PART_o_ID           0
#define PART_o_SummedLongs  4
#define PART_o_ChkSum       8
#define PART_o_HostID       12
#define PART_o_Next         16
#define PART_o_Flags        20
#define PART_o_DevFlags     32
#define PART_o_DriveName    36   /* BSTR: length byte then chars */
#define PART_o_Environment  128  /* DosEnvec, 20 longs */
#define PART_SUMMEDLONGS    64u

/* DosEnvec indices (longword index within pb_Environment) */
#define DE_TableSize     0
#define DE_SizeBlock     1
#define DE_SecOrg        2
#define DE_Surfaces      3
#define DE_SectorPerBlk  4
#define DE_BlocksPerTrk  5
#define DE_Reserved      6
#define DE_PreAlloc      7
#define DE_Interleave    8
#define DE_LowCyl        9
#define DE_HighCyl       10
#define DE_NumBuffers    11
#define DE_BufMemType    12
#define DE_MaxTransfer   13
#define DE_Mask          14
#define DE_BootPri       15
#define DE_DosType       16

static void env_put(uint8_t *blk, int idx, uint32_t v)
{
    be_put32(blk + PART_o_Environment + idx * 4, v);
}
static uint32_t env_get(const uint8_t *blk, int idx)
{
    return be_get32(blk + PART_o_Environment + idx * 4);
}

static void write_partition_block(uint8_t *blk, const RdbModel *m,
                                  const RdbPartition *p, uint32_t next)
{
    int i, n;
    memset(blk, 0, RDB_BLOCK_BYTES);
    be_put32(blk + PART_o_ID, ID_PART);
    be_put32(blk + PART_o_SummedLongs, PART_SUMMEDLONGS);
    be_put32(blk + PART_o_HostID, 7);
    be_put32(blk + PART_o_Next, next);
    be_put32(blk + PART_o_Flags, p->bootable ? 1u : 0u); /* PBFF_BOOTABLE */
    be_put32(blk + PART_o_DevFlags, 0);

    /* pb_DriveName as BSTR: length byte then characters */
    for (n = 0; n < RDB_NAME_LEN - 1 && p->name[n]; n++) ;
    blk[PART_o_DriveName] = (uint8_t)n;
    for (i = 0; i < n; i++) blk[PART_o_DriveName + 1 + i] = (uint8_t)p->name[i];

    env_put(blk, DE_TableSize,    16);
    env_put(blk, DE_SizeBlock,    m->block_bytes / 4);   /* longs per block */
    env_put(blk, DE_SecOrg,       0);
    env_put(blk, DE_Surfaces,     m->heads);
    env_put(blk, DE_SectorPerBlk, 1);
    env_put(blk, DE_BlocksPerTrk, m->sectors);
    env_put(blk, DE_Reserved,     2);
    env_put(blk, DE_PreAlloc,     0);
    env_put(blk, DE_Interleave,   0);
    env_put(blk, DE_LowCyl,       p->low_cyl);
    env_put(blk, DE_HighCyl,      p->high_cyl);
    env_put(blk, DE_NumBuffers,   p->num_buffers);
    env_put(blk, DE_BufMemType,   0);
    env_put(blk, DE_MaxTransfer,  p->maxtransfer);
    env_put(blk, DE_Mask,         p->mask);
    env_put(blk, DE_BootPri,      (uint32_t)p->boot_pri);
    env_put(blk, DE_DosType,      p->dos_type);

    rdb_set_checksum(blk, PART_SUMMEDLONGS, PART_o_ChkSum);
}

int rdb_serialize(const RdbModel *m, BlockIO io, void *ctx)
{
    uint8_t blk[RDB_BLOCK_BYTES];
    uint32_t part_block_first, i;
    int v = rdb_validate(m);
    if (v != RDB_OK) return v;

    /* PART blocks occupy blocks 1..num_parts (block 0 is RDSK). */
    part_block_first = (m->num_parts > 0) ? 1u : NULLPTR;

    /* Write RDSK */
    memset(blk, 0, sizeof(blk));
    be_put32(blk + RDB_o_ID, ID_RDSK);
    be_put32(blk + RDB_o_SummedLongs, RDB_SUMMEDLONGS);
    be_put32(blk + RDB_o_HostID, 7);
    be_put32(blk + RDB_o_BlockBytes, m->block_bytes);
    be_put32(blk + RDB_o_Flags, 0);
    be_put32(blk + RDB_o_BadBlockList, NULLPTR);
    be_put32(blk + RDB_o_PartitionList, part_block_first);
    be_put32(blk + RDB_o_FileSysHdr, NULLPTR);
    be_put32(blk + RDB_o_DriveInit, NULLPTR);
    be_put32(blk + RDB_o_Cylinders, m->cylinders);
    be_put32(blk + RDB_o_Sectors, m->sectors);
    be_put32(blk + RDB_o_Heads, m->heads);
    be_put32(blk + RDB_o_Interleave, 1);
    be_put32(blk + RDB_o_Park, m->cylinders);
    be_put32(blk + RDB_o_WritePreComp, m->cylinders);
    be_put32(blk + RDB_o_ReducedWrite, m->cylinders);
    be_put32(blk + RDB_o_StepRate, 3);
    be_put32(blk + RDB_o_RDBBlocksLo, m->rdb_blocks_lo);
    be_put32(blk + RDB_o_RDBBlocksHi, m->rdb_blocks_hi);
    be_put32(blk + RDB_o_LoCylinder, m->lo_cyl);
    be_put32(blk + RDB_o_HiCylinder, m->hi_cyl);
    be_put32(blk + RDB_o_CylBlocks, m->cyl_blocks);
    be_put32(blk + RDB_o_AutoParkSecs, 0);
    be_put32(blk + RDB_o_HighRDSKBlock, m->num_parts > 0 ? (uint32_t)m->num_parts : 0u);
    rdb_set_checksum(blk, RDB_SUMMEDLONGS, RDB_o_ChkSum);
    if (io(ctx, 0, blk, 1)) return RDB_ERR_IO;

    /* Write PART chain at blocks 1..num_parts */
    for (i = 0; i < (uint32_t)m->num_parts; i++) {
        uint32_t next = (i + 1 < (uint32_t)m->num_parts) ? (i + 2) : NULLPTR;
        write_partition_block(blk, m, &m->parts[i], next);
        if (io(ctx, 1 + i, blk, 1)) return RDB_ERR_IO;
    }
    return RDB_OK;
}

int rdb_parse(RdbModel *m, BlockIO io, void *ctx)
{
    uint8_t blk[RDB_BLOCK_BYTES];
    uint32_t b, part_ptr;
    int found = 0;

    memset(m, 0, sizeof(*m));

    for (b = 0; b < RDB_LOCATION_LIMIT; b++) {
        if (io(ctx, b, blk, 0)) return RDB_ERR_IO;
        if (be_get32(blk + RDB_o_ID) == ID_RDSK &&
            rdb_checksum_ok(blk, be_get32(blk + RDB_o_SummedLongs))) {
            found = 1;
            break;
        }
    }
    if (!found) return RDB_ERR_NO_RDB;

    m->block_bytes   = be_get32(blk + RDB_o_BlockBytes);
    if (m->block_bytes == 0) m->block_bytes = RDB_BLOCK_BYTES;
    m->cylinders     = be_get32(blk + RDB_o_Cylinders);
    m->sectors       = be_get32(blk + RDB_o_Sectors);
    m->heads         = be_get32(blk + RDB_o_Heads);
    m->cyl_blocks    = be_get32(blk + RDB_o_CylBlocks);
    if (m->cyl_blocks == 0) m->cyl_blocks = m->heads * m->sectors;
    m->lo_cyl        = be_get32(blk + RDB_o_LoCylinder);
    m->hi_cyl        = be_get32(blk + RDB_o_HiCylinder);
    m->rdb_blocks_lo = be_get32(blk + RDB_o_RDBBlocksLo);
    m->rdb_blocks_hi = be_get32(blk + RDB_o_RDBBlocksHi);
    part_ptr         = be_get32(blk + RDB_o_PartitionList);

    while (part_ptr != NULLPTR && part_ptr != 0 && m->num_parts < RDB_MAX_PARTS) {
        RdbPartition *p;
        int len, i;
        if (io(ctx, part_ptr, blk, 0)) return RDB_ERR_IO;
        if (be_get32(blk + PART_o_ID) != ID_PART) break;
        if (!rdb_checksum_ok(blk, be_get32(blk + PART_o_SummedLongs))) break;

        p = &m->parts[m->num_parts++];
        len = blk[PART_o_DriveName];
        if (len > RDB_NAME_LEN - 1) len = RDB_NAME_LEN - 1;
        for (i = 0; i < len; i++) p->name[i] = (char)blk[PART_o_DriveName + 1 + i];
        p->name[len] = 0;

        p->low_cyl     = env_get(blk, DE_LowCyl);
        p->high_cyl    = env_get(blk, DE_HighCyl);
        p->num_buffers = env_get(blk, DE_NumBuffers);
        p->boot_pri    = (int32_t)env_get(blk, DE_BootPri);
        p->maxtransfer = env_get(blk, DE_MaxTransfer);
        p->mask        = env_get(blk, DE_Mask);
        p->dos_type    = env_get(blk, DE_DosType);
        p->bootable    = (be_get32(blk + PART_o_Flags) & 1u) ? 1 : 0;

        part_ptr = be_get32(blk + PART_o_Next);
    }
    return RDB_OK;
}
