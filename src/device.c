/* HDPart device I/O: a thin wrapper over exec block-device access. Uses only
   V37 exec/dos APIs. No libc. */
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/io.h>
#include <exec/errors.h>
#include <devices/trackdisk.h>
#include <devices/scsidisk.h>
#include <proto/exec.h>
#include "device.h"
#include "discover.h"   /* disc_parse_read_capacity10 / disc_synth_chs (pure) */

struct DeviceHandle {
    struct MsgPort *port;
    struct IOExtTD *req;
    int             opened;   /* OpenDevice succeeded */
    ULONG           block_bytes; /* device sector size from geometry; default 512 */
};

/* OpenDevice initializes a device-specific IORequest whose size varies by
   device: IOExtTD (trackdisk) is 56 bytes, but IOExtPar (parallel) is 62 and
   IOExtSer (serial) is 82. dev_open can be asked to open an ARBITRARY
   user-loaded .device, so an IOExtTD-sized buffer is too small for non-disk
   devices: OpenDevice then writes its defaults (baud, term arrays, ...) past
   the end of our allocation and corrupts the heap (delayed crash at the next
   allocation; Guru 8100xxxx). Allocate a request large enough for any standard
   device. We still drive it as an IOExtTD for trackdisk commands; OpenDevice
   only writes what its own device needs, so the slack is harmless. */
#define DEV_IOREQ_SIZE 256

DeviceHandle *dev_open(const char *driver, ULONG unit)
{
    DeviceHandle *h = (DeviceHandle *)AllocVec(sizeof(*h), MEMF_CLEAR);
    if (!h) return 0;

    h->port = CreateMsgPort();
    if (!h->port) { dev_close(h); return 0; }

    h->req = (struct IOExtTD *)CreateIORequest(h->port, DEV_IOREQ_SIZE);
    if (!h->req) { dev_close(h); return 0; }

    if (OpenDevice((CONST_STRPTR)driver, unit,
                   (struct IORequest *)h->req, 0) != 0) {
        dev_close(h);
        return 0;
    }
    h->opened = 1;
    h->block_bytes = 512; /* safe default before dev_geometry runs */
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

/* SCSI READ CAPACITY(10): 10-byte CDB (opcode 0x25), 8-byte big-endian response.
   Returns 0 and fills *block_bytes / *total_blocks on success; a DoIO error code
   (or 1) on failure. Used as the geometry fallback for controllers whose driver
   does not implement TD_GETGEOMETRY (notably the A3000 ROM scsi.device 37.x). */
static int scsi_read_capacity10(DeviceHandle *h,
                                ULONG *block_bytes, ULONG *total_blocks)
{
    static UBYTE cdb[10];
    static UBYTE data[8];
    static UBYTE sense[32];
    struct SCSICmd sc;
    LONG err;
    uint32_t bb = 512, tot = 0;
    int i;

    for (i = 0; i < 10; i++) cdb[i]  = 0;
    for (i = 0; i < 8;  i++) data[i] = 0;
    cdb[0] = 0x25;   /* READ CAPACITY(10) */

    sc.scsi_Data       = (UWORD *)data;
    sc.scsi_Length     = sizeof(data);
    sc.scsi_Actual     = 0;
    sc.scsi_Command    = cdb;
    sc.scsi_CmdLength  = sizeof(cdb);
    sc.scsi_CmdActual  = 0;
    sc.scsi_Flags      = SCSIF_READ | SCSIF_AUTOSENSE;
    sc.scsi_Status     = 0;
    sc.scsi_SenseData  = sense;
    sc.scsi_SenseLength= sizeof(sense);
    sc.scsi_SenseActual= 0;

    h->req->iotd_Req.io_Command = HD_SCSICMD;
    h->req->iotd_Req.io_Data    = &sc;
    h->req->iotd_Req.io_Length  = sizeof(sc);
    h->req->iotd_Req.io_Actual  = 0;
    err = DoIO((struct IORequest *)h->req);
    if (err != 0) return (int)err;            /* no SCSI passthrough / failed */
    if (!disc_parse_read_capacity10(data, &bb, &tot)) return 1;

    *block_bytes  = bb;
    *total_blocks = tot;
    return 0;
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
    h->block_bytes    = out->block_bytes; /* thread into handle for dev_rw */
    out->cylinders    = dg.dg_Cylinders;
    out->heads        = dg.dg_Heads;
    out->sectors      = dg.dg_TrackSectors;
    out->total_blocks = dg.dg_TotalSectors;
    out->has_media    = (err == 0);
    out->model[0]     = 0;

    /* Fallback: some controllers (the A3000 ROM scsi.device 37.x) do not
       implement TD_GETGEOMETRY — it errors, or reports a zero block count — so
       the drive would otherwise be invisible. Ask the drive directly with SCSI
       READ CAPACITY and synthesize a self-consistent CHS geometry from it. */
    if (err != 0 || out->total_blocks == 0) {
        ULONG bb = 512, tot = 0;
        if (scsi_read_capacity10(h, &bb, &tot) == 0 && tot > 0) {
            uint32_t cyl, heads, sectors;
            disc_synth_chs(tot, &cyl, &heads, &sectors);
            out->block_bytes  = bb;
            h->block_bytes    = bb;
            out->total_blocks = tot;
            out->cylinders    = cyl;
            out->heads        = heads;
            out->sectors      = sectors;
            out->has_media    = 1;
            return 0;
        }
    }
    return (int)err;
}

static int dev_rw(DeviceHandle *h, ULONG block, UBYTE *buf, UWORD cmd)
{
    /* All block buffers in this codebase are exactly 512 bytes.  A device
       reporting block_bytes > 512 (e.g. a CD-ROM at 2048) would overflow the
       caller's buffer, so we refuse rather than corrupt memory.  For the
       supported 512-byte CF/SD/IDE media this guard is never triggered. */
    if (h->block_bytes == 0) h->block_bytes = 512;
    if (h->block_bytes > 512) return IOERR_BADLENGTH;
    h->req->iotd_Req.io_Command = cmd;
    h->req->iotd_Req.io_Data    = buf;
    h->req->iotd_Req.io_Length  = h->block_bytes;
    h->req->iotd_Req.io_Offset  = (ULONG)block * h->block_bytes; /* 32-bit: RDB is low */
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

int dev_unit_ready(DeviceHandle *h)
{
    LONG err;
    if (!h || !h->req) return 1;                 /* fail-safe: assume present */
    h->req->iotd_Req.io_Command = TD_CHANGESTATE;
    h->req->iotd_Req.io_Actual  = 0;
    err = DoIO((struct IORequest *)h->req);
    if (err != 0) return 1;                      /* unsupported/error -> assume present */
    return h->req->iotd_Req.io_Actual ? 0 : 1;  /* io_Actual != 0 => no disk */
}

/* Standard SCSI INQUIRY (6-byte CDB), 36-byte response.
   Response: bytes 8..15 = vendor id, 16..31 = product id (space padded). */
int dev_inquiry_model(DeviceHandle *h, char model[40])
{
    static UBYTE cdb[6];
    static UBYTE data[36];
    static UBYTE sense[32];
    struct SCSICmd sc;
    LONG err;
    int i, n;

    model[0] = 0;
    for (i = 0; i < 6;  i++) cdb[i]  = 0;
    for (i = 0; i < 36; i++) data[i] = 0;
    cdb[0] = 0x12;   /* INQUIRY */
    cdb[4] = 36;     /* allocation length */

    sc.scsi_Data       = (UWORD *)data;
    sc.scsi_Length     = 36;
    sc.scsi_Actual     = 0;
    sc.scsi_Command    = cdb;
    sc.scsi_CmdLength  = 6;
    sc.scsi_CmdActual  = 0;
    sc.scsi_Flags      = SCSIF_READ | SCSIF_AUTOSENSE;
    sc.scsi_Status     = 0;
    sc.scsi_SenseData  = sense;
    sc.scsi_SenseLength= sizeof(sense);
    sc.scsi_SenseActual= 0;

    h->req->iotd_Req.io_Command = HD_SCSICMD;
    h->req->iotd_Req.io_Data    = &sc;
    h->req->iotd_Req.io_Length  = sizeof(sc);
    h->req->iotd_Req.io_Actual  = 0;
    err = DoIO((struct IORequest *)h->req);
    if (err != 0) return (int)err;     /* device has no SCSI passthrough */

    /* Compose "VENDOR PRODUCT", trimming trailing spaces of each field. */
    n = 0;
    for (i = 8; i < 16 && n < 39; i++) {
        if (data[i] >= 32 && data[i] < 127) model[n++] = (char)data[i];
    }
    while (n > 0 && model[n-1] == ' ') n--;
    if (n < 39) model[n++] = ' ';
    for (i = 16; i < 32 && n < 39; i++) {
        if (data[i] >= 32 && data[i] < 127) model[n++] = (char)data[i];
    }
    while (n > 0 && model[n-1] == ' ') n--;
    model[n] = 0;
    return 0;
}
