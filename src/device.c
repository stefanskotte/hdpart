/* HDPart device I/O: a thin wrapper over exec block-device access. Uses only
   V37 exec/dos APIs. No libc. */
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/io.h>
#include <devices/trackdisk.h>
#include <devices/scsidisk.h>
#include <proto/exec.h>
#include "device.h"

struct DeviceHandle {
    struct MsgPort *port;
    struct IOExtTD *req;
    int             opened;   /* OpenDevice succeeded */
};

DeviceHandle *dev_open(const char *driver, ULONG unit)
{
    DeviceHandle *h = (DeviceHandle *)AllocVec(sizeof(*h), MEMF_CLEAR);
    if (!h) return 0;

    h->port = CreateMsgPort();
    if (!h->port) { dev_close(h); return 0; }

    h->req = (struct IOExtTD *)CreateIORequest(h->port, sizeof(struct IOExtTD));
    if (!h->req) { dev_close(h); return 0; }

    if (OpenDevice((CONST_STRPTR)driver, unit,
                   (struct IORequest *)h->req, 0) != 0) {
        dev_close(h);
        return 0;
    }
    h->opened = 1;
    return h;
}

void dev_close(DeviceHandle *h)
{
    if (!h) return;
    if (h->opened) CloseDevice((struct IORequest *)h->req);
    if (h->req)    DeleteIORequest((struct IORequest *)h->req);
    if (h->port)   DeleteMsgPort(h->port);
    FreeVec(h);
}

int dev_geometry(DeviceHandle *h, DeviceInfo *out)
{
    struct DriveGeometry dg;
    LONG err;
    int i;
    /* zero dg so undefined fields read as 0 */
    for (i = 0; i < (int)sizeof(dg); i++) ((UBYTE *)&dg)[i] = 0;

    h->req->iotd_Req.io_Command = TD_GETGEOMETRY;
    h->req->iotd_Req.io_Data    = &dg;
    h->req->iotd_Req.io_Length  = sizeof(dg);
    h->req->iotd_Req.io_Offset  = 0;
    h->req->iotd_Req.io_Actual  = 0;
    err = DoIO((struct IORequest *)h->req);

    out->block_bytes  = dg.dg_SectorSize ? dg.dg_SectorSize : 512;
    out->cylinders    = dg.dg_Cylinders;
    out->heads        = dg.dg_Heads;
    out->sectors      = dg.dg_TrackSectors;
    out->total_blocks = dg.dg_TotalSectors;
    out->has_media    = (err == 0);
    out->model[0]     = 0;
    return (int)err;
}

static int dev_rw(DeviceHandle *h, ULONG block, UBYTE *buf, UWORD cmd)
{
    h->req->iotd_Req.io_Command = cmd;
    h->req->iotd_Req.io_Data    = buf;
    h->req->iotd_Req.io_Length  = 512;
    h->req->iotd_Req.io_Offset  = block * 512UL;   /* 32-bit: RDB is low */
    h->req->iotd_Req.io_Actual  = 0;
    return (int)DoIO((struct IORequest *)h->req);
}

int dev_read_block(DeviceHandle *h, ULONG block, UBYTE *buf512)
{
    return dev_rw(h, block, buf512, CMD_READ);
}

int dev_write_block(DeviceHandle *h, ULONG block, UBYTE *buf512)
{
    return dev_rw(h, block, buf512, CMD_WRITE);
}

int dev_block_io(void *ctx, uint32_t block, uint8_t *buf, int write)
{
    DeviceHandle *h = (DeviceHandle *)ctx;
    return write ? dev_write_block(h, block, (UBYTE *)buf)
                 : dev_read_block (h, block, (UBYTE *)buf);
}

/* dev_inquiry_model is added in Task 2.2.1 */
