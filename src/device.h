#ifndef HDPART_DEVICE_H
#define HDPART_DEVICE_H

#include <exec/types.h>
#include <stdint.h>

/* Geometry + identity for one drive (filled by dev_geometry / dev_inquiry_model). */
typedef struct {
    ULONG block_bytes;     /* bytes per sector (usually 512) */
    ULONG cylinders;
    ULONG heads;
    ULONG sectors;         /* sectors per track */
    ULONG total_blocks;    /* total sectors on the drive */
    int   has_media;       /* nonzero if geometry query succeeded */
    char  model[40];       /* "VENDOR PRODUCT" from INQUIRY, or "" */
} DeviceInfo;

/* Opaque open-device handle. */
typedef struct DeviceHandle DeviceHandle;

/* Open driver/unit for block I/O. Returns NULL on any failure (no media counts
   as success for OpenDevice on most drivers; use dev_geometry to detect media). */
DeviceHandle *dev_open(const char *driver, ULONG unit);
void          dev_close(DeviceHandle *h);

/* Query geometry via TD_GETGEOMETRY. Returns 0 on success, else the io_Error. */
int dev_geometry(DeviceHandle *h, DeviceInfo *out);

/* Read/write one 512-byte block. Returns 0 on success, else io_Error. */
int dev_read_block (DeviceHandle *h, ULONG block, UBYTE *buf512);
int dev_write_block(DeviceHandle *h, ULONG block, UBYTE *buf512);

/* Best-effort SCSI INQUIRY model string. Returns 0 on success (model filled),
   nonzero if the device does not support HD_SCSICMD (model set to ""). */
int dev_inquiry_model(DeviceHandle *h, char model[40]);

/* Media-presence pre-check via TD_CHANGESTATE.
   Returns 1 if media is present OR the device doesn't support change-state
   (fail-safe: assume present so fixed disks are never skipped).
   Returns 0 ONLY when change-state succeeds AND reports no disk (io_Actual != 0). */
int dev_unit_ready(DeviceHandle *h);

/* BlockIO adapter for the RDB engine (src/rdb.h). ctx must be a DeviceHandle*.
   Matches the BlockIO typedef: returns 0 on success, nonzero on I/O error. */
int dev_block_io(void *ctx, uint32_t block, uint8_t *buf, int write);

#endif /* HDPART_DEVICE_H */
