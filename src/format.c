/* HDPart partition formatter. Pure env builder (host-testable) + OS-bound
   live-mount + ACTION_FORMAT guarded by HDPART_AMIGA. Mirrors discover.c. */
#ifdef HDPART_AMIGA
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/filehandler.h>
#include <libraries/expansion.h>
#include <resources/filesysres.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/expansion.h>
#endif
#include "format.h"

/* DE_ indexes relative to env[0]=DE_TABLESIZE. */
enum { FE_TABLESIZE=0, FE_SIZEBLOCK=1, FE_SECORG=2, FE_NUMHEADS=3, FE_SECSPERBLK=4,
       FE_BLKSPERTRACK=5, FE_RESERVEDBLKS=6, FE_PREFAC=7, FE_INTERLEAVE=8,
       FE_LOWCYL=9, FE_HIGHCYL=10, FE_NUMBUFFERS=11, FE_BUFMEMTYPE=12,
       FE_MAXTRANSFER=13, FE_MASK=14, FE_BOOTPRI=15, FE_DOSTYPE=16 };

int format_build_envec(const RdbModel *m, int part_index, uint32_t env[FMT_ENV_LONGS])
{
    const RdbPartition *p;
    if (!m || part_index < 0 || part_index >= m->num_parts) return -1;
    p = &m->parts[part_index];
    env[FE_TABLESIZE]    = 16;
    env[FE_SIZEBLOCK]    = m->block_bytes / 4u;   /* longwords per block */
    env[FE_SECORG]       = 0;
    env[FE_NUMHEADS]     = m->heads;
    env[FE_SECSPERBLK]   = 1;
    env[FE_BLKSPERTRACK] = m->sectors;
    env[FE_RESERVEDBLKS] = 2;
    env[FE_PREFAC]       = 0;
    env[FE_INTERLEAVE]   = 0;
    env[FE_LOWCYL]       = p->low_cyl;
    env[FE_HIGHCYL]      = p->high_cyl;
    env[FE_NUMBUFFERS]   = p->num_buffers ? p->num_buffers : 30u;
    env[FE_BUFMEMTYPE]   = 0;
    env[FE_MAXTRANSFER]  = p->maxtransfer;
    env[FE_MASK]         = p->mask;
    env[FE_BOOTPRI]      = (uint32_t)p->boot_pri;
    env[FE_DOSTYPE]      = p->dos_type;
    return 0;
}
