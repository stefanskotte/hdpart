/* HDPart driver loading. Pure romtag-finder (host-testable) plus the OS-bound
   loader (guarded by HDPART_AMIGA, added in Task 2). Mirrors discover.c. */
#ifdef HDPART_AMIGA
#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/resident.h>
#include <exec/nodes.h>
#include <proto/exec.h>
#include <proto/dos.h>
#endif
#include "driver.h"
#include "endian.h"

/* struct Resident field offsets (exec/resident.h), as raw bytes: */
#define RT_MATCHWORD 0    /* UWORD */
#define RT_MATCHTAG  2    /* APTR  */
#define RT_TYPE      12   /* UBYTE */
#define RT_SIZE      26   /* sizeof(struct Resident) */
#define RTC_MATCHWORD 0x4AFCu

int drv_find_romtag(const unsigned char *base, uint32_t size,
                    uint32_t start_off, uint32_t *off_out, int *type_out)
{
    uint32_t off;
    if (size < RT_SIZE) return 0;
    /* romtags are word-aligned; start on an even offset */
    off = (start_off + 1u) & ~1u;
    for (; off + RT_SIZE <= size; off += 2) {
        uint32_t self;
        if (be_get16(base + off + RT_MATCHWORD) != RTC_MATCHWORD) continue;
        self = (uint32_t)(uintptr_t)(base + off);
        if (be_get32(base + off + RT_MATCHTAG) != self) continue;
        if (off_out)  *off_out  = off;
        if (type_out) *type_out = (int)base[off + RT_TYPE];
        return 1;
    }
    return 0;
}
