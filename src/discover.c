/* HDPart device discovery. This file mixes pure helpers (host-testable) with
   OS-bound scanning (added in the next task, guarded by __amigaos__-style use). */
#ifdef HDPART_AMIGA
#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dosextens.h>
#include <dos/filehandler.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include "device.h"
#include "rdb.h"
#endif
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

int disc_find(const DiscDisk list[], int count, const char *driver, uint32_t unit)
{
    int i, j;
    for (i = 0; i < count; i++) {
        if (list[i].unit != unit) continue;
        for (j = 0; driver[j] && list[i].driver[j] == driver[j]; j++) ;
        if (driver[j] == 0 && list[i].driver[j] == 0) return i;
    }
    return -1;
}

uint32_t disc_blocks_to_mb(uint32_t total_blocks, uint32_t block_bytes)
{
    uint32_t blocks_per_mb;
    if (block_bytes == 0) return 0;
    blocks_per_mb = (1024UL * 1024UL) / block_bytes;   /* 2048 for 512 */
    if (blocks_per_mb == 0) return 0;
    return total_blocks / blocks_per_mb;
}

#ifdef HDPART_AMIGA

/* Curated list of common block-device drivers to probe (units 0..PROBE_UNITS-1).
   uaehf.device covers FS-UAE/WinUAE hardfiles; scsi.device is the A600/A1200
   built-in IDE and most controllers; the rest are common 3rd-party controllers. */
static const char *const kProbeDrivers[] = {
    "scsi.device", "2nd.scsi.device", "uaehf.device", "lide.device",
    "ide.device", "oktagon.device", "gvpscsi.device", "omniscsi.device",
    "a4091.device", "a3000scsi.device", "z3scsi.device", 0
};
#define PROBE_UNITS 8

/* Add (driver,unit) if not present; return index or -1 if full. */
static int add_unique(DiscDisk out[], int *count, int max,
                      const char *driver, uint32_t unit)
{
    int idx = disc_find(out, *count, driver, unit);
    int n;
    if (idx >= 0) return idx;
    if (*count >= max) return -1;
    idx = (*count)++;
    out[idx].unit    = unit;
    out[idx].size_mb = 0;
    out[idx].status  = DST_UNKNOWN;
    out[idx].label[0]= 0;
    for (n = 0; n < DISC_DRIVER_LEN - 1 && driver[n]; n++)
        out[idx].driver[n] = driver[n];
    out[idx].driver[n] = 0;
    return idx;
}

/* Classify one disk by opening it, querying geometry and (if media) RDB. */
static void probe_one(DiscDisk *d)
{
    DeviceHandle *h = dev_open(d->driver, d->unit);
    DeviceInfo info;
    if (!h) { if (d->status == DST_UNKNOWN) d->status = DST_NOMEDIA; return; }

    if (dev_geometry(h, &info) == 0 && info.has_media && info.total_blocks > 0) {
        RdbModel m;
        d->size_mb = disc_blocks_to_mb(info.total_blocks, info.block_bytes);
        if (d->label[0] == 0) {
            char model[40];
            if (dev_inquiry_model(h, model) == 0 && model[0]) {
                int n; for (n = 0; n < DISC_LABEL_LEN - 1 && model[n]; n++)
                    d->label[n] = model[n];
                d->label[n] = 0;
            }
        }
        if (rdb_parse(&m, dev_block_io, h) == RDB_OK) {
            if (d->status != DST_MOUNTED) d->status = DST_RDB;
        } else {
            if (d->status != DST_MOUNTED) d->status = DST_BLANK;
        }
    } else {
        if (d->status == DST_UNKNOWN) d->status = DST_NOMEDIA;
    }
    dev_close(h);
}

/* (1) Scan mounted AmigaDOS devices for driver/unit pairs. */
static void scan_dos_devices(DiscDisk out[], int *count, int max)
{
    /* NextDosEntry advances from the entry you pass it, so the iterator MUST
       be reassigned each pass (`e = NextDosEntry(e, ...)`). Passing the locked
       list header every time returns the first device forever -> infinite loop.
       The guard is belt-and-suspenders so a discovery bug can never hard-hang. */
    struct DosList *e = LockDosList(LDF_DEVICES | LDF_READ);
    int guard = 0;
    if (!e) return;
    while ((e = NextDosEntry(e, LDF_DEVICES | LDF_READ)) != 0 && ++guard < 256) {
        struct FileSysStartupMsg *fssm;
        BPTR startup = (BPTR)e->dol_misc.dol_handler.dol_Startup;
        char driver[DISC_DRIVER_LEN];
        char dosname[DISC_LABEL_LEN];
        int idx;
        if (e->dol_Type != DLT_DEVICE || startup == 0) continue;
        fssm = (struct FileSysStartupMsg *)BADDR(startup);
        if (!fssm || fssm->fssm_Device == 0) continue;

        disc_bcpl_to_c((const unsigned char *)BADDR(fssm->fssm_Device),
                       driver, sizeof(driver));
        idx = add_unique(out, count, max, driver, fssm->fssm_Unit);
        if (idx < 0) continue;
        out[idx].status = DST_MOUNTED;
        if (out[idx].label[0] == 0) {
            int n;
            disc_bcpl_to_c((const unsigned char *)BADDR(e->dol_Name),
                           dosname, sizeof(dosname));
            for (n = 0; n < DISC_LABEL_LEN - 1 && dosname[n]; n++)
                out[idx].label[n] = dosname[n];
            out[idx].label[n] = 0;
        }
    }
    UnLockDosList(LDF_DEVICES | LDF_READ);
}

/* (2) Probe the curated driver/unit matrix for disks not already listed. */
static void scan_probe(DiscDisk out[], int *count, int max)
{
    int di;
    for (di = 0; kProbeDrivers[di]; di++) {
        ULONG u;
        for (u = 0; u < PROBE_UNITS; u++) {
            DeviceHandle *h = dev_open(kProbeDrivers[di], u);
            if (!h) continue;          /* driver/unit not present */
            dev_close(h);
            add_unique(out, count, max, kProbeDrivers[di], u);
        }
    }
}

int discover_disks(DiscDisk out[], int max)
{
    int count = 0;
    int i;
    scan_dos_devices(out, &count, max);
    scan_probe(out, &count, max);
    for (i = 0; i < count; i++)
        probe_one(&out[i]);
    return count;
}

#endif /* HDPART_AMIGA */
