#ifndef HDPART_DRIVER_H
#define HDPART_DRIVER_H

#include <stdint.h>

#define DRV_NAME_LEN 40

/* Result codes for driver_load_file (negative = failure). */
enum {
    DRVL_OK         =  0,
    DRVL_ELOAD      = -1,  /* LoadSeg failed (missing / not a load file) */
    DRVL_ENOROMTAG  = -2,  /* no Resident romtag found */
    DRVL_ENOTDEVICE = -3,  /* a romtag exists but none is NT_DEVICE */
    DRVL_EINIT      = -4   /* InitResident ran but device not in list */
};

/* Exec node type for a device romtag (exec/nodes.h NT_DEVICE). Defined here so
   the pure helper and its host tests need no AmigaOS headers. */
#define DRV_NT_DEVICE 3

/* ---- pure helper (no OS calls; unit-tested on host) ----
   Scan [base, base+size) from byte offset start_off for the next valid Resident
   romtag: a big-endian 16-bit rt_MatchWord == 0x4AFC at an even offset whose
   big-endian 32-bit rt_MatchTag (at off+2) equals the low 32 bits of the
   romtag's own address (self-pointer). On a match, set *off_out to the offset,
   *type_out to rt_Type (the byte at off+12), and return 1. Return 0 if none.
   Stepping is 2 bytes (romtags are word-aligned). */
int drv_find_romtag(const unsigned char *base, uint32_t size,
                    uint32_t start_off, uint32_t *off_out, int *type_out);

/* ---- OS-bound entry point (implemented under HDPART_AMIGA in driver.c) ----
   Load a .device file so OpenDevice() can reach it. On success copies the exec
   device name (romtag rt_Name) into name_out (<= name_sz) and returns DRVL_OK.
   If a device of that name is already resident, returns DRVL_OK without
   re-loading. Otherwise returns one of DRVL_E*. */
int driver_load_file(const char *path, char *name_out, int name_sz);

#endif /* HDPART_DRIVER_H */
