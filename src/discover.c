/* HDPart device discovery. This file mixes pure helpers (host-testable) with
   OS-bound scanning (added in the next task, guarded by __amigaos__-style use). */
#include "discover.h"

void disc_bcpl_to_c(const unsigned char *bcpl, char *out, int outsz)
{
    int len = bcpl ? bcpl[0] : 0;
    int i;
    if (outsz <= 0) return;
    if (len > outsz - 1) len = outsz - 1;
    for (i = 0; i < len; i++) out[i] = (char)bcpl[1 + i];
    out[len] = 0;
}

int disc_find(const DiscDisk list[], int count, const char *driver, ULONG unit)
{
    int i, j;
    for (i = 0; i < count; i++) {
        if (list[i].unit != unit) continue;
        for (j = 0; driver[j] && list[i].driver[j] == driver[j]; j++) ;
        if (driver[j] == 0 && list[i].driver[j] == 0) return i;
    }
    return -1;
}

ULONG disc_blocks_to_mb(ULONG total_blocks, ULONG block_bytes)
{
    ULONG blocks_per_mb;
    if (block_bytes == 0) return 0;
    blocks_per_mb = (1024UL * 1024UL) / block_bytes;   /* 2048 for 512 */
    if (blocks_per_mb == 0) return 0;
    return total_blocks / blocks_per_mb;
}
