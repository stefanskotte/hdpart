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
#ifndef RTC_MATCHWORD
#define RTC_MATCHWORD 0x4AFCu
#endif

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

#ifdef HDPART_AMIGA

/* Copy a C string into out (<= outsz), NUL-terminated. */
static void copy_name(char *out, int outsz, const char *src)
{
    int n = 0;
    if (outsz <= 0) return;
    while (n < outsz - 1 && src && src[n]) { out[n] = src[n]; n++; }
    out[n] = 0;
}

/* True if a device of this name is already in exec's device list. */
static int device_resident(const char *name)
{
    struct Node *n;
    int found = 0;
    Forbid();
    n = FindName(&SysBase->DeviceList, (CONST_STRPTR)name);
    found = (n != 0);
    Permit();
    return found;
}

int driver_load_file(const char *path, char *name_out, int name_sz)
{
    BPTR seglist, s;
    const struct Resident *dev = 0;   /* first NT_DEVICE romtag found */
    int any_romtag = 0;
    char name[DRV_NAME_LEN];

    seglist = LoadSeg((CONST_STRPTR)path);
    if (!seglist) return DRVL_ELOAD;

    /* Walk each segment's data for romtags. Segment layout (LoadSeg):
       [size longword][next BPTR][data...]; BADDR(s) points at the next BPTR,
       size is the longword before it, data starts 4 bytes after BADDR(s). */
    for (s = seglist; s; s = *(BPTR *)BADDR(s)) {
        ULONG *hdr = (ULONG *)BADDR(s);
        ULONG total = hdr[-1];
        const unsigned char *data = (const unsigned char *)(hdr + 1);
        uint32_t dlen = (total >= 8) ? (uint32_t)(total - 8) : 0;
        uint32_t off = 0;
        int type = 0;
        while (drv_find_romtag(data, dlen, off, &off, &type)) {
            any_romtag = 1;
            if (type == DRV_NT_DEVICE) {
                dev = (const struct Resident *)(data + off);
                break;
            }
            off += 2;   /* keep scanning this segment for an NT_DEVICE romtag */
        }
        if (dev) break;
    }

    if (!dev) {
        UnLoadSeg(seglist);
        return any_romtag ? DRVL_ENOTDEVICE : DRVL_ENOROMTAG;
    }

    copy_name(name, sizeof(name), (const char *)dev->rt_Name);

    /* A romtag with no usable name can't be opened by OpenDevice and is unsafe
       to InitResident (NULL/empty rt_Name corrupts the device list). Reject it
       like a non-device file rather than loading it. */
    if (name[0] == 0) {
        UnLoadSeg(seglist);
        return DRVL_ENOTDEVICE;
    }

    /* Already loaded (e.g. previously loaded by us, or by the controller ROM)?
       Don't re-init; just hand back the name and drop our redundant copy. */
    if (device_resident(name)) {
        UnLoadSeg(seglist);
        copy_name(name_out, name_sz, name);
        return DRVL_OK;
    }

    /* For an RTF_AUTOINIT device this MakeLibrary+AddDevice's it into exec's
       device list. We must NOT UnLoadSeg afterwards: the live device owns it. */
    InitResident((struct Resident *)dev, seglist);

    if (!device_resident(name))
        return DRVL_EINIT;     /* segment stays loaded; nothing safe to undo */

    copy_name(name_out, name_sz, name);
    return DRVL_OK;
}

#endif /* HDPART_AMIGA */
