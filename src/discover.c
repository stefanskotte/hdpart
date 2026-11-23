/* HDPart device discovery. This file mixes pure helpers (host-testable) with
   OS-bound scanning (added in the next task, guarded by __amigaos__-style use). */
#ifdef HDPART_AMIGA
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/execbase.h>
#include <exec/nodes.h>
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

int disc_is_device_name(const char *s)
{
    int len = 0, i;
    const char suf[8] = ".device";   /* 7 chars + NUL */
    if (!s) return 0;
    while (s[len]) {
        if (s[len] < 0x20 || s[len] > 0x7E) return 0;  /* printable only */
        if (++len > 60) return 0;                      /* implausibly long */
    }
    if (len < 8) return 0;                             /* need >=1 char + ".device" */
    for (i = 0; i < 7; i++) {
        char c = s[len - 7 + i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != suf[i]) return 0;
    }
    return 1;
}

int disc_is_partitionable(const char *driver, uint32_t total_blocks)
{
    const char *fd = "trackdisk.device";
    int i;
    if (total_blocks == 0) return 0;        /* no media / directory drive */
    for (i = 0; driver[i] && fd[i] && driver[i] == fd[i]; i++) ;
    if (driver[i] == 0 && fd[i] == 0) return 0;  /* exact "trackdisk.device" */
    return 1;
}

int disc_parse_read_capacity10(const uint8_t resp[8],
                               uint32_t *block_bytes, uint32_t *total_blocks)
{
    uint32_t last_lba =
        ((uint32_t)resp[0] << 24) | ((uint32_t)resp[1] << 16) |
        ((uint32_t)resp[2] <<  8) |  (uint32_t)resp[3];
    uint32_t blk =
        ((uint32_t)resp[4] << 24) | ((uint32_t)resp[5] << 16) |
        ((uint32_t)resp[6] <<  8) |  (uint32_t)resp[7];
    if (last_lba == 0xFFFFFFFFu) return 0;   /* needs READ CAPACITY(16) */
    if (blk == 0) blk = 512;
    if (block_bytes)  *block_bytes  = blk;
    if (total_blocks) *total_blocks = last_lba + 1u;
    return 1;
}

void disc_synth_chs(uint32_t total_blocks,
                    uint32_t *cyl, uint32_t *heads, uint32_t *sectors)
{
    uint32_t h = 16, s = 63, cyl_blocks, c;
    if (total_blocks == 0) {
        if (cyl)     *cyl     = 0;
        if (heads)   *heads   = 0;
        if (sectors) *sectors = 0;
        return;
    }
    cyl_blocks = h * s;                       /* 1008 */
    if (total_blocks < cyl_blocks) { h = 1; s = 1; cyl_blocks = 1; }
    c = total_blocks / cyl_blocks;
    if (c == 0) c = 1;
    if (cyl)     *cyl     = c;
    if (heads)   *heads   = h;
    if (sectors) *sectors = s;
}

/* ---- SCSI unit numbering (plain C; shared by host + target) ---- */
#define DISC_WIDE_MARK 8u        /* HD_WIDESCSI: last digit 8 marks a Phase V wide unit */

uint32_t disc_unit_encode(uint32_t id, uint32_t lun)
{
    if (id > 7u || lun > 7u)
        return lun * 10000u + id * 10u + DISC_WIDE_MARK;
    return id + lun * 10u;
}

void disc_unit_decode(uint32_t unit, uint32_t *id, uint32_t *lun)
{
    uint32_t i, l;
    if (unit % 10u == DISC_WIDE_MARK) {
        i = (unit / 10u) % 1000u;
        l = (unit / 10000u) % 1000u;
    } else {
        i = unit % 10u;
        l = unit / 10u;
    }
    if (id)  *id  = i;
    if (lun) *lun = l;
}

static int disc_put_uint(char *out, int p, int outsz, uint32_t v)
{
    char tmp[11];
    int n = 0;
    do { tmp[n++] = (char)('0' + v % 10u); v /= 10u; } while (v && n < 10);
    while (n > 0 && p < outsz - 1) out[p++] = tmp[--n];
    return p;
}

int disc_unit_label(uint32_t unit, char *out, int outsz)
{
    uint32_t id, lun;
    int p = 0;
    if (!out || outsz <= 0) return 0;
    disc_unit_decode(unit, &id, &lun);
    p = disc_put_uint(out, p, outsz, id);
    if (lun) {
        if (p < outsz - 1) out[p++] = '.';
        p = disc_put_uint(out, p, outsz, lun);
    }
    out[p] = 0;
    return p;
}

static int disc_name_ieq(const char *a, const char *b)
{
    int n;
    for (n = 0; a[n] && b[n]; n++) {
        char ca = a[n], cb = b[n];
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);
        if (ca != cb) return 0;
    }
    return a[n] == 0 && b[n] == 0;
}

/* Drivers that decode Phase V wide unit numbers. a4770/scsi770 are the 53C770
   drivers from a4091-software (16 targets when wide is on; a target beyond what the
   board allows simply fails to open). cybppc.device is Phase 5's own CyberStorm PPC
   driver, where the scheme comes from. */
static const char *const kWideDrivers[] = {
    "a4770.device", "scsi770.device", "cybppc.device", 0
};

int disc_driver_wide(const char *name)
{
    int i;
    if (!name) return 0;
    for (i = 0; kWideDrivers[i]; i++)
        if (disc_name_ieq(name, kWideDrivers[i])) return 1;
    return 0;
}

int disc_inquiry_absent(uint8_t byte0)
{
    return (byte0 & 0xE0u) != 0 || (byte0 & 0x1Fu) == 0x1Fu;
}

int disc_ident_same(const DiscIdent *a, const DiscIdent *b)
{
    int i;
    if (!a || !b) return 0;
    if (a->has_inq != b->has_inq) return 0;
    if (a->has_inq)
        for (i = 0; i < 36; i++) if (a->inq[i] != b->inq[i]) return 0;
    return a->total_blocks == b->total_blocks && a->sum == b->sum;
}

/* ---- user-loaded driver registry (plain C; shared by host + target) ---- */
#define DISC_EXTRA_MAX 8
static char g_extra[DISC_EXTRA_MAX][DISC_DRIVER_LEN];
static int  g_extra_n = 0;

void disc_add_extra_driver(const char *name)
{
    int i, n;
    if (!name || !name[0]) return;
    for (i = 0; i < g_extra_n; i++) {            /* dedup (exact match) */
        for (n = 0; name[n] && g_extra[i][n] == name[n]; n++) ;
        if (name[n] == 0 && g_extra[i][n] == 0) return;
    }
    if (g_extra_n >= DISC_EXTRA_MAX) return;      /* silently ignore overflow */
    for (n = 0; n < DISC_DRIVER_LEN - 1 && name[n]; n++)
        g_extra[g_extra_n][n] = name[n];
    g_extra[g_extra_n][n] = 0;
    g_extra_n++;
}

int disc_extra_count(void) { return g_extra_n; }

#ifdef HDPART_AMIGA

/* Curated list of common block-device driver names (probed by probe_driver_units).
   uaehf.device covers FS-UAE/WinUAE hardfiles; scsi.device is the A600/A1200
   built-in IDE and most controllers; the rest are common 3rd-party controllers.
   a4091/a4092/a4770.device and scsi710/scsi770.device are the a4091-software
   family (A4091, A4092, A4770, A4000T). disc_candidate_drivers() lists only the
   ones actually resident in memory. */
static const char *const kProbeDrivers[] = {
    "scsi.device", "2nd.scsi.device", "uaehf.device", "lide.device",
    "ide.device", "oktagon.device", "gvpscsi.device", "omniscsi.device",
    "a4091.device", "a4092.device", "a4770.device", "scsi710.device",
    "scsi770.device", "cybppc.device", "a3000scsi.device", "z3scsi.device", 0
};

static int disc_name_eq(const char *a, const char *b)
{
    int n; for (n = 0; a[n] && a[n] == b[n]; n++) ;
    return a[n] == 0 && b[n] == 0;
}

/* True if a device of this name is currently in exec's device list. */
static int driver_is_resident(const char *name)
{
    struct Node *nd;
    int found;
    Forbid();
    nd = FindName(&SysBase->DeviceList, (CONST_STRPTR)name);
    found = (nd != 0);
    Permit();
    return found;
}

/* Curated disk drivers that are RESIDENT in exec's device list, plus the
   user-loaded extras (resident after InitResident). Drivers that are neither
   curated-and-resident nor loaded simply do not appear — the user loads them
   from disk via the GUI's Driver... button. */
int disc_candidate_drivers(const char *out[], int max)
{
    int n = 0, i, j;
    for (i = 0; kProbeDrivers[i] && n < max; i++)
        if (driver_is_resident(kProbeDrivers[i])) out[n++] = kProbeDrivers[i];
    for (i = 0; i < g_extra_n && n < max; i++) {       /* extras, deduped vs above */
        int dup = 0;
        for (j = 0; j < n; j++)
            if (disc_name_eq(g_extra[i], out[j])) { dup = 1; break; }
        if (!dup) out[n++] = g_extra[i];
    }
    return n;
}

/* Add (driver,unit) if not present; return index or -1 if full. */
static int add_unique(DiscDisk out[], int *count, int max,
                      const char *driver, uint32_t unit)
{
    int idx = disc_find(out, *count, driver, unit);
    int n;
    if (idx >= 0) return idx;
    if (*count >= max) return -1;
    idx = (*count)++;
    out[idx].unit         = unit;
    out[idx].size_mb      = 0;
    out[idx].status       = DST_UNKNOWN;
    out[idx].partitionable = 0;
    out[idx].used_pct     = -1;
    out[idx].layout.nseg  = 0; out[idx].layout.rdb_end = 0;
    out[idx].label[0]     = 0;
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

    if (!dev_unit_ready(h)) {
        if (d->status == DST_UNKNOWN) d->status = DST_NOMEDIA;
        dev_close(h);
        return;
    }

    if (dev_geometry(h, &info) == 0 && info.has_media && info.total_blocks > 0) {
        d->size_mb = disc_blocks_to_mb(info.total_blocks, info.block_bytes);
        d->partitionable = disc_is_partitionable(d->driver, info.total_blocks);
        if (d->label[0] == 0) {
            char model[40];
            if (dev_inquiry_model(h, model) == 0 && model[0]) {
                int n; for (n = 0; n < DISC_LABEL_LEN - 1 && model[n]; n++)
                    d->label[n] = model[n];
                d->label[n] = 0;
            }
        }
        /* Discovery only needs "has an RDB, and how full is it?", not the embedded
           handler, so this avoids reading a ~120-block filesystem on every unit.
           The full table + FS load happens later on unit-select. */
        { uint32_t pct = 0;
          if (rdb_used_layout(dev_block_io, h, &pct, &d->layout) == RDB_OK) {
              d->used_pct = (int)pct;
              if (d->status != DST_MOUNTED) d->status = DST_RDB;
          } else {
              d->used_pct = -1;
              if (d->status != DST_MOUNTED) d->status = DST_BLANK;
          }
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
        /* Many DLT_DEVICE entries are handlers (RAW:, PRT:, SER:, ...) whose
           dol_Startup is NOT a FileSysStartupMsg. Dereferencing it as one would
           read garbage or crash, so validate every pointer with TypeOfMem (0 =
           not in any memory region) and require a real ".device" name. */
        if (TypeOfMem((APTR)BADDR(startup)) == 0) continue;
        fssm = (struct FileSysStartupMsg *)BADDR(startup);
        if (fssm->fssm_Device == 0) continue;
        if (TypeOfMem((APTR)BADDR(fssm->fssm_Device)) == 0) continue;

        disc_bcpl_to_c((const unsigned char *)BADDR(fssm->fssm_Device),
                       driver, sizeof(driver));
        if (!disc_is_device_name(driver)) continue;   /* skip non-disk handlers */
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

/* What one unit looks like from outside (see DiscIdent). 0 if the unit opened. */
static int read_ident(const char *driver, ULONG unit, DiscIdent *id)
{
    static UBYTE blk[512];
    DeviceHandle *h = dev_open(driver, unit);
    DeviceInfo info;
    ULONG b;
    int i;
    for (i = 0; i < (int)sizeof(*id); i++) ((UBYTE *)id)[i] = 0;
    if (!h) return -1;
    id->has_inq = (dev_inquiry(h, id->inq) == 0);
    if (dev_unit_ready(h) && dev_geometry(h, &info) == 0 &&
        info.has_media && info.total_blocks > 0) {
        id->total_blocks = info.total_blocks;
        for (b = 0; b < 16 && b < info.total_blocks; b++) {
            if (dev_read_block(h, b, blk) != 0) { id->sum ^= 0xA5A5A5A5u; break; }
            for (i = 0; i < 512; i++) id->sum += (uint32_t)blk[i] << (8 * (i & 3));
        }
    }
    dev_close(h);
    return 0;
}

/* RDBFF_LASTLUN set in this unit's RDB? */
static int unit_last_lun(const char *driver, ULONG unit)
{
    DeviceHandle *h = dev_open(driver, unit);
    DeviceInfo info;
    uint32_t flags = 0;
    if (!h) return 0;
    if (dev_unit_ready(h) && dev_geometry(h, &info) == 0 && info.has_media &&
        info.total_blocks > 0 && rdb_disk_flags(dev_block_io, h, &flags) != RDB_OK)
        flags = 0;
    dev_close(h);
    return (flags & RDB_RDBFF_LASTLUN) != 0;
}

/* Scan one driver the way a4091/mounter does (see discover_probe_driver in the
   header), adding each unit found. A LUN above 0 must also say a device is there
   and must not mirror LUN 0, since HDPart writes to what it lists. */
static void probe_driver_units(DiscDisk out[], int *count, int max, const char *driver)
{
    uint32_t ids = disc_driver_wide(driver) ? DISC_WIDE_IDS : DISC_NARROW_IDS;
    uint32_t id, lun;
    for (id = 0; id < ids; id++) {
        DiscIdent lun0;
        int have_lun0 = 0;
        for (lun = 0; lun < DISC_MAX_LUN; lun++) {
            ULONG unit = disc_unit_encode(id, lun);
            DeviceHandle *h = dev_open(driver, unit);
            if (!h) break;                          /* no unit: next ID */
            if (lun > 0) {
                static UBYTE inq[36];
                DiscIdent me;
                int absent = (dev_inquiry(h, inq) == 0 && disc_inquiry_absent(inq[0]));
                dev_close(h);
                if (absent) break;
                if (!have_lun0) {
                    if (read_ident(driver, disc_unit_encode(id, 0), &lun0) != 0) break;
                    have_lun0 = 1;
                }
                if (read_ident(driver, unit, &me) != 0 || disc_ident_same(&lun0, &me))
                    break;                          /* a ghost of LUN 0 */
            } else {
                dev_close(h);
            }
            add_unique(out, count, max, driver, unit);
            if (unit_last_lun(driver, unit)) break;
        }
    }
}

/* (2) Probe the curated drivers and the user-loaded ones. */
static void scan_probe(DiscDisk out[], int *count, int max)
{
    int di;
    for (di = 0; kProbeDrivers[di]; di++)
        probe_driver_units(out, count, max, kProbeDrivers[di]);
    for (di = 0; di < g_extra_n; di++)
        probe_driver_units(out, count, max, g_extra[di]);
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

int discover_probe_driver(DiscDisk out[], int *count, int max, const char *driver)
{
    int before;
    int i;
    if (!driver || !driver[0] || !count) return 0;
    before = *count;
    probe_driver_units(out, count, max, driver);
    /* Classify only the entries we just added. */
    for (i = before; i < *count; i++)
        probe_one(&out[i]);
    return *count - before;
}

#endif /* HDPART_AMIGA */
