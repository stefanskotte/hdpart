#include "rdb.h"
#include "endian.h"
#include <string.h>
/* implementation grows over the following tasks */

#define LSEG_PAYLOAD 492u   /* raw hunk bytes carried per LSEG block */

#ifdef HDPART_AMIGA
#include <exec/memory.h>
#include <proto/exec.h>
/* Amiga FreeMem needs the size; we store it in a 4-byte size header.
   Public API (declared in rdb.h) so fsload.c can use the same convention. */
void *rdb_seg_alloc(uint32_t n) {
    uint32_t *p = (uint32_t *)AllocMem(n + 4, MEMF_PUBLIC | MEMF_CLEAR);
    if (!p) return 0;
    p[0] = n + 4;
    return p + 1;
}
void rdb_seg_free(void *p) {
    uint32_t *h;
    if (!p) return;
    h = (uint32_t *)p - 1;
    FreeMem(h, h[0]);
}
#else
#include <stdlib.h>
void *rdb_seg_alloc(uint32_t n) { return calloc(1, n ? n : 1); }
void rdb_seg_free(void *p) { free(p); }
#endif

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
    m->hi_cyl      = (cyl > 0) ? cyl - 1 : 0;
    m->rdb_blocks_lo = 0;
    {
        uint32_t rc = RDB_RESERVED_CYLS;              /* classic: cyl 0..1 */
        if (m->cyl_blocks != 0) {
            /* grow reserve so it holds >= RDB_RESERVED_MIN_BLOCKS metadata blocks */
            uint32_t need = (RDB_RESERVED_MIN_BLOCKS + m->cyl_blocks - 1u) / m->cyl_blocks; /* ceil cyls */
            if (need > rc) rc = need;
            /* never reserve so much that the disk has no partitionable room:
               cap at a quarter of the disk (but never below RDB_RESERVED_CYLS). */
            if (cyl > 8u) { uint32_t cap = cyl / 4u; if (rc > cap) rc = cap; }
            if (rc < RDB_RESERVED_CYLS) rc = RDB_RESERVED_CYLS;
        }
        m->lo_cyl       = rc;
        m->rdb_blocks_hi = rc * m->cyl_blocks - 1u;
    }
    m->num_parts   = 0;
}

uint32_t rdb_lseg_block_count(uint32_t seg_len)
{
    return (seg_len + LSEG_PAYLOAD - 1u) / LSEG_PAYLOAD;
}

void rdb_model_free(RdbModel *m)
{
    int i;
    if (!m) return;
    for (i = 0; i < m->num_fs; i++) {
        if (m->fs[i].seg_data) { rdb_seg_free(m->fs[i].seg_data); m->fs[i].seg_data = 0; }
    }
    m->num_fs = 0;
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
    p->flags = 0;
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

int rdb_resize_cyl(RdbModel *m, int index, uint32_t low, uint32_t high)
{
    RdbPartition saved, *p;
    int v;
    if (index < 0 || index >= m->num_parts) return RDB_ERR_RANGE;
    if (low > high)                          return RDB_ERR_RANGE;
    p = &m->parts[index];
    saved = *p;
    p->low_cyl  = low;
    p->high_cyl = high;
    v = rdb_validate(m);
    if (v != RDB_OK) { *p = saved; return v; }
    return RDB_OK;
}

uint32_t rdb_gap_end_after(const RdbModel *m, int index)
{
    uint32_t end = m->hi_cyl + 1;            /* exclusive */
    uint32_t hi;
    int j;
    if (index < 0 || index >= m->num_parts) return end;
    hi = m->parts[index].high_cyl;
    for (j = 0; j < m->num_parts; j++) {
        if (j == index) continue;
        if (m->parts[j].low_cyl > hi && m->parts[j].low_cyl < end)
            end = m->parts[j].low_cyl;
    }
    return end;
}

uint32_t rdb_gap_start_before(const RdbModel *m, int index)
{
    uint32_t start = m->lo_cyl;              /* inclusive */
    uint32_t lo;
    int j;
    if (index < 0 || index >= m->num_parts) return start;
    lo = m->parts[index].low_cyl;
    for (j = 0; j < m->num_parts; j++) {
        if (j == index) continue;
        if (m->parts[j].high_cyl < lo && m->parts[j].high_cyl + 1 > start)
            start = m->parts[j].high_cyl + 1;
    }
    return start;
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
    p->flags = 0;
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

/* Lowest unused "DH<n>" into out[<=12]. */
static void next_dh_name(const RdbModel *m, char *out)
{
    int n;
    for (n = 0; n < 1000; n++) {
        int p = 0; uint32_t v = (uint32_t)n; char tmp[10]; int ti = 0;
        out[p++] = 'D'; out[p++] = 'H';
        if (v == 0) tmp[ti++] = '0';
        while (v) { tmp[ti++] = (char)('0' + v % 10); v /= 10; }
        while (ti > 0) out[p++] = tmp[--ti];
        out[p] = 0;
        if (!name_taken(m, out, -1)) return;
    }
}

int rdb_split_range(RdbModel *m, uint32_t lo, uint32_t hi, int count,
                    uint32_t dos_type)
{
    uint32_t span;
    int i, before = m->num_parts;
    if (count < 1)                          return RDB_ERR_RANGE;
    if (before + count > RDB_MAX_PARTS)      return RDB_ERR_TOO_MANY;
    if (hi < lo || lo < m->lo_cyl || hi > m->hi_cyl) return RDB_ERR_RANGE;
    span = hi - lo + 1;
    if ((uint32_t)count > span) return RDB_ERR_NO_SPACE;   /* need >= 1 cyl each */

    for (i = 0; i < count; i++) {
        /* Proportional split: slice i is [span*i/count, span*(i+1)/count); the
           last slice's end lands exactly on hi. Contiguous, no gaps/overlap.
           span<=65535, count<=RDB_MAX_PARTS, so span*(i+1) stays 32-bit. */
        uint32_t s = lo + (span * (uint32_t)i)       / (uint32_t)count;
        uint32_t e = lo + (span * (uint32_t)(i + 1)) / (uint32_t)count - 1;
        char name[12];
        next_dh_name(m, name);
        if (rdb_add_partition_cyl(m, name, s, e, dos_type) < 0) {
            m->num_parts = before;          /* roll back any added slices */
            return RDB_ERR_NO_SPACE;
        }
    }
    return RDB_OK;
}

int rdb_split_equal(RdbModel *m, int count, uint32_t dos_type)
{
    if (count < 1 || count > RDB_MAX_PARTS) return RDB_ERR_RANGE;
    m->num_parts = 0;                        /* whole disk: clear, then fill */
    return rdb_split_range(m, m->lo_cyl, m->hi_cyl, count, dos_type);
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
    p->flags       = 0;
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
#define ID_FSHD 0x46534844u
#define ID_LSEG 0x4C534547u
#define FSHD_SUMMEDLONGS 64u
/* LSEG SummedLongs is computed per block in write_lseg_block (5 header longs +
   data longs); a full block works out to 128. */
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

/* FSHD field offsets within the 512-byte block. */
#define FSHD_o_Next       16
#define FSHD_o_Flags      20
#define FSHD_o_DosType    32
#define FSHD_o_Version    36
#define FSHD_o_PatchFlags 40
#define FSHD_o_Type       44   /* dn_Type */
#define FSHD_o_Task       48
#define FSHD_o_Lock       52
#define FSHD_o_Handler    56
#define FSHD_o_StackSize  60
#define FSHD_o_Priority   64
#define FSHD_o_Startup    68
#define FSHD_o_SegList    72   /* first LSEG block# */
#define FSHD_o_GlobalVec  76
#define LSEG_o_Next       16
#define LSEG_o_LoadData   20

static void env_put(uint8_t *blk, int idx, uint32_t v)
{
    be_put32(blk + PART_o_Environment + idx * 4, v);
}
static uint32_t env_get(const uint8_t *blk, int idx)
{
    return be_get32(blk + PART_o_Environment + idx * 4);
}
/* Return env_get(blk, idx) when idx <= table_size, else dflt.
 * Implements the mounter's "copy only (de_TableSize+1) longs" rule: a
 * field beyond the declared table length was not written by the formatter
 * and must not be read (the memory holds whatever was in the reserved area). */
static uint32_t env_get_bounded(const uint8_t *blk, int idx,
                                uint32_t table_size, uint32_t dflt)
{
    return ((uint32_t)idx <= table_size) ? env_get(blk, idx) : dflt;
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
    be_put32(blk + PART_o_Flags,
             (p->flags & ~RDB_PBFF_BOOTABLE) |
             (p->bootable ? RDB_PBFF_BOOTABLE : 0u)); /* preserve all non-bootable bits */
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

static void write_fshd_block(uint8_t *blk, const RdbFileSys *fs,
                             uint32_t next_blk, uint32_t first_lseg)
{
    memset(blk, 0, 512);
    be_put32(blk + 0,  ID_FSHD);
    be_put32(blk + 4,  FSHD_SUMMEDLONGS);
    be_put32(blk + 12, 7u);                 /* HostID */
    be_put32(blk + FSHD_o_Next,      next_blk);
    be_put32(blk + FSHD_o_Flags,     0u);
    be_put32(blk + FSHD_o_DosType,   fs->dos_type);
    be_put32(blk + FSHD_o_Version,   fs->version);
    be_put32(blk + FSHD_o_PatchFlags,fs->patch_flags);
    be_put32(blk + FSHD_o_Type,      fs->dn_type);
    be_put32(blk + FSHD_o_Task,      fs->dn_task);
    be_put32(blk + FSHD_o_Lock,      fs->dn_lock);
    be_put32(blk + FSHD_o_Handler,   fs->dn_handler);
    be_put32(blk + FSHD_o_StackSize, fs->dn_stack);
    be_put32(blk + FSHD_o_Priority,  fs->dn_pri);
    be_put32(blk + FSHD_o_Startup,   fs->dn_startup);
    be_put32(blk + FSHD_o_SegList,   first_lseg);
    be_put32(blk + FSHD_o_GlobalVec, fs->dn_globalvec);
    rdb_set_checksum(blk, FSHD_SUMMEDLONGS, 8);
}

/* Real data bytes carried by one LSEG block, from its stored lsb_SummedLongs:
   (SummedLongs-5) longs, clamped to the 492-byte physical payload. */
static uint32_t lseg_payload_bytes(const uint8_t *lb)
{
    uint32_t summed = be_get32(lb + 4);
    uint32_t bytes  = (summed >= 5u) ? (summed - 5u) * 4u : 0u;
    return (bytes > LSEG_PAYLOAD) ? LSEG_PAYLOAD : bytes;
}

static void write_lseg_block(uint8_t *blk, const uint8_t *data, uint32_t len,
                             uint32_t next_blk)
{
    /* lsb_SummedLongs encodes this block's real size: 5 header longs plus the
       data longs actually carried.  A strict Commodore-style loader sizes each
       block's payload as (SummedLongs-5) longs, so the LAST (short) block must
       advertise a smaller SummedLongs than a full 128 — otherwise the trailing
       zero pad is ingested and the seglist is no longer a valid load module.
       Full blocks (len == 492) come out to the usual 128. */
    uint32_t data_longs, summed;
    memset(blk, 0, 512);
    if (len > LSEG_PAYLOAD) len = LSEG_PAYLOAD;
    data_longs = (len + 3u) / 4u;            /* round up to a longword */
    summed     = 5u + data_longs;            /* 5 hdr longs + data longs */
    be_put32(blk + 0,  ID_LSEG);
    be_put32(blk + 4,  summed);
    be_put32(blk + 12, 7u);
    be_put32(blk + LSEG_o_Next, next_blk);
    memcpy(blk + LSEG_o_LoadData, data, len);
    rdb_set_checksum(blk, summed, 8);
}

int rdb_serialize(const RdbModel *m, BlockIO io, void *ctx)
{
    uint8_t blk[RDB_BLOCK_BYTES];
    uint32_t part_block_first, i;
    uint32_t high_rdsk;
    int v = rdb_validate(m);
    if (v != RDB_OK) return v;

    /* Capacity check + linear allocation for FSHD/LSEG, after PART blocks.
       Run this BEFORE any I/O so that a too-big model writes nothing. */
    {
        uint32_t next = 1u + (uint32_t)m->num_parts;   /* first free block */
        uint32_t reserved_hi = m->rdb_blocks_hi;       /* inclusive */
        uint32_t total = next;
        for (i = 0; i < (uint32_t)m->num_fs; i++)
            total += 1u + rdb_lseg_block_count(m->fs[i].seg_len); /* FSHD + LSEGs */
        if (m->num_fs > 0 && total - 1u > reserved_hi)
            return RDB_ERR_NO_RDB_SPACE;
    }

    /* Compute HighRDSKBlock (last used block) before writing block 0, since
       block 0 carries this field. */
    high_rdsk = (uint32_t)m->num_parts;   /* default (parts only, or 0) */
    if (m->num_fs > 0) {
        uint32_t t = 1u + (uint32_t)m->num_parts;
        for (i = 0; i < (uint32_t)m->num_fs; i++)
            t += 1u + rdb_lseg_block_count(m->fs[i].seg_len);
        high_rdsk = t - 1u;
    }

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
    be_put32(blk + RDB_o_FileSysHdr,
             m->num_fs > 0 ? (1u + (uint32_t)m->num_parts) : NULLPTR);
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
    be_put32(blk + RDB_o_HighRDSKBlock, high_rdsk);
    rdb_set_checksum(blk, RDB_SUMMEDLONGS, RDB_o_ChkSum);
    if (io(ctx, 0, blk, 1)) return RDB_ERR_IO;

    /* Write PART chain at blocks 1..num_parts */
    for (i = 0; i < (uint32_t)m->num_parts; i++) {
        uint32_t next = (i + 1 < (uint32_t)m->num_parts) ? (i + 2) : NULLPTR;
        write_partition_block(blk, m, &m->parts[i], next);
        if (io(ctx, 1 + i, blk, 1)) return RDB_ERR_IO;
    }

    /* Write FSHD/LSEG chains after the PART blocks. */
    {
        uint32_t cur = 1u + (uint32_t)m->num_parts;   /* current FSHD block */
        for (i = 0; i < (uint32_t)m->num_fs; i++) {
            const RdbFileSys *fs = &m->fs[i];
            uint32_t nseg = rdb_lseg_block_count(fs->seg_len);
            uint32_t first_lseg = nseg ? cur + 1u : NULLPTR;
            uint32_t fshd_blk = cur;
            uint32_t next_fshd;
            uint32_t s, off = 0u;
            cur += 1u + nseg;                          /* advance past FSHD+LSEGs */
            next_fshd = (i + 1u < (uint32_t)m->num_fs) ? cur : NULLPTR;
            write_fshd_block(blk, fs, next_fshd, first_lseg);
            if (io(ctx, fshd_blk, blk, 1)) return RDB_ERR_IO;
            for (s = 0; s < nseg; s++) {
                uint32_t lseg_blk = fshd_blk + 1u + s;
                uint32_t next_lseg = (s + 1u < nseg) ? lseg_blk + 1u : NULLPTR;
                uint32_t chunk = fs->seg_len - off;
                if (chunk > LSEG_PAYLOAD) chunk = LSEG_PAYLOAD;
                write_lseg_block(blk, fs->seg_data + off, chunk, next_lseg);
                if (io(ctx, lseg_blk, blk, 1)) return RDB_ERR_IO;
                off += chunk;
            }
        }
    }
    return RDB_OK;
}

/* Printable 3 chars + version byte, e.g. "PFS\3"; hex fallback. out >= 16. */
static void dostype_name(char *out, uint32_t t)
{
    unsigned char c0=(t>>24)&0xff, c1=(t>>16)&0xff, c2=(t>>8)&0xff, v=t&0xff;
    if (c0>=32&&c0<127&&c1>=32&&c1<127&&c2>=32&&c2<127) {
        out[0]=(char)c0; out[1]=(char)c1; out[2]=(char)c2; out[3]='\\';
        out[4]=(char)('0'+(v<=9?v:0)); out[5]=0;
    } else {
        static const char *h="0123456789ABCDEF"; int i;
        out[0]='0'; out[1]='x';
        for (i=0;i<8;i++) out[2+i]=h[(t>>((7-i)*4))&0xf];
        out[10]=0;
    }
}

/* Grow a heap seg buffer geometrically to hold at least `need` bytes, preserving
   the first `used` bytes. Returns the (possibly new) buffer, or 0 on alloc
   failure (the old buffer is then freed by the caller). No realloc API exists,
   so this allocs-new + copies + frees-old; geometric growth keeps it to a few
   allocations for a typical ~120-block handler. */
static uint8_t *seg_grow(uint8_t *buf, uint32_t used, uint32_t *cap, uint32_t need)
{
    uint32_t nc = *cap ? *cap : 4096u;
    uint8_t *nb;
    if (need <= *cap) return buf;
    while (nc < need) nc <<= 1;
    nb = (uint8_t *)rdb_seg_alloc(nc);
    if (!nb) return 0;
    if (buf) { memcpy(nb, buf, used); rdb_seg_free(buf); }
    *cap = nc;
    return nb;
}

/* Read the FSHD chain starting at block `fshd_blk` into m->fs[]. Returns RDB_OK
   or RDB_ERR_IO. Silently stops at RDB_MAX_FS or a bad/zero pointer. */
static int read_fshd_chain(RdbModel *m, BlockIO io, void *ctx, uint32_t fshd_blk)
{
    uint8_t blk[512];
    int guard = 0;
    while (fshd_blk != NULLPTR && fshd_blk != 0 && m->num_fs < RDB_MAX_FS
           && ++guard < 64) {
        RdbFileSys *fs = &m->fs[m->num_fs];
        uint32_t seg_blk;
        if (io(ctx, fshd_blk, blk, 0)) return RDB_ERR_IO;
        if (be_get32(blk + 0) != ID_FSHD ||
            !rdb_checksum_ok(blk, FSHD_SUMMEDLONGS)) break;
        memset(fs, 0, sizeof *fs);
        fs->dos_type     = be_get32(blk + FSHD_o_DosType);
        fs->version      = be_get32(blk + FSHD_o_Version);
        fs->patch_flags  = be_get32(blk + FSHD_o_PatchFlags);
        fs->dn_type      = be_get32(blk + FSHD_o_Type);
        fs->dn_task      = be_get32(blk + FSHD_o_Task);
        fs->dn_lock      = be_get32(blk + FSHD_o_Lock);
        fs->dn_handler   = be_get32(blk + FSHD_o_Handler);
        fs->dn_stack     = be_get32(blk + FSHD_o_StackSize);
        fs->dn_pri       = be_get32(blk + FSHD_o_Priority);
        fs->dn_startup   = be_get32(blk + FSHD_o_Startup);
        fs->dn_globalvec = be_get32(blk + FSHD_o_GlobalVec);
        fs->source       = RDB_FS_EMBEDDED;
        dostype_name(fs->name, fs->dos_type);   /* readable default name */

        /* Single pass: read each LSEG block ONCE, validating (bad ID/checksum =>
           reject this FS) and copying its payload into a geometrically-grown
           buffer. Each block's real payload is (SummedLongs-5) longs — honour the
           stored value (clamped to 492) so the recovered seg_len is exact. Reading
           the chain once (vs the old two-pass) halves the disk I/O for an embedded
           handler (~120 blocks for PFS3). */
        seg_blk = be_get32(blk + FSHD_o_SegList);
        { uint32_t b = seg_blk; int g = 0; int lseg_ok = 1;
          uint8_t *buf = 0; uint32_t cap = 0, off = 0;
          while (b != NULLPTR && b != 0 && ++g < 4096) {
            uint8_t lb[512];
            uint32_t n;
            if (io(ctx, b, lb, 0)) { rdb_seg_free(buf); return RDB_ERR_IO; }
            if (be_get32(lb + 0) != ID_LSEG ||
                !rdb_checksum_ok(lb, be_get32(lb + 4))) { lseg_ok = 0; break; }
            n = lseg_payload_bytes(lb);
            if (n) {
                uint8_t *nb = seg_grow(buf, off, &cap, off + n);
                if (!nb) { rdb_seg_free(buf); return RDB_ERR_IO; }
                buf = nb;
                memcpy(buf + off, lb + LSEG_o_LoadData, n);
                off += n;
            }
            b = be_get32(lb + LSEG_o_Next);
          }
          if (!lseg_ok) { rdb_seg_free(buf);
                          fshd_blk = be_get32(blk + FSHD_o_Next); continue; }
          fs->seg_data = buf;          /* may be 0 for an empty (no-LSEG) handler */
          fs->seg_len  = off; }
        m->num_fs++;
        fshd_blk = be_get32(blk + FSHD_o_Next);
    }
    return RDB_OK;
}

/* Scan blocks 0..RDB_LOCATION_LIMIT-1 for a valid RDSK block. On success
   returns 1 with the block left in blk[]; returns 0 (no RDB) or -1 (IO error). */
static int find_rdsk_block(BlockIO io, void *ctx, uint8_t *blk)
{
    uint32_t b;
    for (b = 0; b < RDB_LOCATION_LIMIT; b++) {
        if (io(ctx, b, blk, 0)) return -1;
        if (be_get32(blk + RDB_o_ID) == ID_RDSK &&
            rdb_checksum_ok(blk, be_get32(blk + RDB_o_SummedLongs)))
            return 1;
    }
    return 0;
}

int rdb_present(BlockIO io, void *ctx)
{
    uint8_t blk[RDB_BLOCK_BYTES];
    int r = find_rdsk_block(io, ctx, blk);
    return (r == 1) ? RDB_OK : (r < 0 ? RDB_ERR_IO : RDB_ERR_NO_RDB);
}

int rdb_parse(RdbModel *m, BlockIO io, void *ctx)
{
    uint8_t blk[RDB_BLOCK_BYTES];
    uint32_t part_ptr, fshd_ptr;
    int found;

    memset(m, 0, sizeof(*m));

    found = find_rdsk_block(io, ctx, blk);
    if (found < 0) return RDB_ERR_IO;
    if (found == 0) return RDB_ERR_NO_RDB;

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
    fshd_ptr         = be_get32(blk + RDB_o_FileSysHdr);  /* capture before PART walk reuses blk */

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

        { uint32_t ts = env_get(blk, DE_TableSize);
          p->low_cyl     = env_get_bounded(blk, DE_LowCyl,      ts, 0u);
          p->high_cyl    = env_get_bounded(blk, DE_HighCyl,     ts, 0u);
          p->num_buffers = env_get_bounded(blk, DE_NumBuffers,  ts, 0u);
          p->boot_pri    = (int32_t)env_get_bounded(blk, DE_BootPri,
                                                    ts, 0u);
          p->maxtransfer = env_get_bounded(blk, DE_MaxTransfer, ts,
                                           RDB_DEFAULT_MAXTRANSFER);
          p->mask        = env_get_bounded(blk, DE_Mask,        ts,
                                           RDB_DEFAULT_MASK);
          p->dos_type    = env_get_bounded(blk, DE_DosType,     ts,
                                           RDB_DOSTYPE_FFS_INTL); }
        p->flags       = be_get32(blk + PART_o_Flags);
        p->bootable    = (p->flags & RDB_PBFF_BOOTABLE) ? 1 : 0;

        part_ptr = be_get32(blk + PART_o_Next);
    }

    {
        int rc = read_fshd_chain(m, io, ctx, fshd_ptr);
        if (rc != RDB_OK) return rc;
    }
    return RDB_OK;
}
