/* HDPart device-liveness safety classifier. Pure helper (host-testable) +
   OS-bound gatherer guarded by HDPART_AMIGA. Mirrors discover.c. */
#ifdef HDPART_AMIGA
#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/resident.h>
#include <dos/dosextens.h>
#include <dos/filehandler.h>
#include <dos/dostags.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include "discover.h"
#endif
#include <string.h>
#include "safety.h"

/* Case-insensitive ASCII equality for device names. */
static int dev_eq(const char *a, const char *b)
{
    int i;
    for (i = 0; a[i] && b[i]; i++) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return 0;
    }
    return a[i] == 0 && b[i] == 0;
}

DevLiveness safety_decide(const char *driver, uint32_t unit,
                          int boot_known, const char *boot_driver, uint32_t boot_unit,
                          const MountEntry mounts[], int n_mounts,
                          char out_names[][8], int max_names, int *n_names)
{
    int i, nn = 0, matched = 0;
    int is_boot = boot_known && dev_eq(driver, boot_driver) && unit == boot_unit;

    for (i = 0; i < n_mounts; i++) {
        if (mounts[i].unit != unit || !dev_eq(mounts[i].driver, driver)) continue;
        matched = 1;
        if (nn < max_names) {
            int k;
            for (k = 0; k < 7 && mounts[i].name[k]; k++) out_names[nn][k] = mounts[i].name[k];
            out_names[nn][k] = 0;
            nn++;
        }
    }
    if (n_names) *n_names = nn;

    if (is_boot)  return DEV_BOOT;
    if (matched)  return DEV_MOUNTED;
    if (!boot_known) return DEV_MOUNTED;   /* fail-safe: cannot prove not-boot */
    return DEV_CLEAR;
}

int safety_part_conflict(const char *driver, uint32_t unit,
                         uint32_t low_cyl, uint32_t high_cyl,
                         const MountedPart parts[], int n_parts,
                         char out_name[8])
{
    int i, k;
    for (i = 0; i < n_parts; i++) {
        if (parts[i].unit != unit) continue;
        if (!dev_eq(parts[i].driver, driver)) continue;
        if (!(low_cyl <= parts[i].high_cyl && parts[i].low_cyl <= high_cyl)) continue;
        if (out_name) {
            for (k = 0; k < 7 && parts[i].name[k]; k++) out_name[k] = parts[i].name[k];
            out_name[k] = 0;
        }
        return 1;
    }
    return 0;
}

int safety_resume_decide(const char *driver, uint32_t unit, const char *name,
                         uint32_t low_cyl, uint32_t high_cyl, uint32_t cyl_blocks, uint32_t dos_type,
                         const MountedPart parts[], int n_parts)
{
    int i, found = 0;
    for (i = 0; i < n_parts; i++) {
        const MountedPart *p = &parts[i];
        int same_dev = p->unit == unit && dev_eq(p->driver, driver);
        int same_name = dev_eq(p->name, name);
        int overlap = same_dev && low_cyl <= p->high_cyl && p->low_cyl <= high_cyl;
        if (!overlap && !same_name) continue;
        if (!same_dev || !same_name || p->low_cyl != low_cyl || p->high_cyl != high_cyl ||
            p->cyl_blocks != cyl_blocks || p->dos_type != dos_type) return SAFETY_RESUME_MISMATCH;
        found++;
    }
    if (found > 1) return SAFETY_RESUME_MISMATCH;
    return found ? SAFETY_RESUME_MATCH : SAFETY_RESUME_NONE;
}

int safety_driver_is_ide(const uint8_t *code, uint32_t len)
{
    uint32_t i;
    int gayle = 0, a4000 = 0;
    if (!code || len < 6) return 0;
    for (i = 0; i + 6 <= len; i += 2) {
        uint32_t arg = ((uint32_t)code[i + 2] << 24) | ((uint32_t)code[i + 3] << 16) |
                       ((uint32_t)code[i + 4] << 8)  |  (uint32_t)code[i + 5];
        int lea_a4 = code[i] == 0x49 && code[i + 1] == 0xF9;   /* lea abs.l,a4 */
        if (arg == 0x00DD0000UL || arg == 0x00DD0040UL) return 0;   /* A3000 SDMAC, A4000T NCR */
        if (lea_a4 && arg == 0x00DA2000UL) gayle++;
        if (lea_a4 && arg == 0x00DD2020UL) a4000++;
    }
    return gayle + a4000 == 1;
}

#ifdef HDPART_AMIGA
/* Read the exec device name + unit backing a DLT_DEVICE DOS entry from its
   FileSysStartupMsg. Returns 1 on success. Guards BPTR derefs (see discover.c).
   out_driver must hold >=40 bytes. */
static int dev_backing(struct DosList *dl, char *out_driver, uint32_t *out_unit)
{
    struct FileSysStartupMsg *fssm;
    unsigned char *bstr;
    int len, i;
    if (!dl || !dl->dol_misc.dol_handler.dol_Startup) return 0;
    if (TypeOfMem((void *)BADDR(dl->dol_misc.dol_handler.dol_Startup)) == 0) return 0;
    fssm = (struct FileSysStartupMsg *)BADDR(dl->dol_misc.dol_handler.dol_Startup);
    if (!fssm->fssm_Device) return 0;
    if (TypeOfMem((void *)BADDR(fssm->fssm_Device)) == 0) return 0;
    bstr = (unsigned char *)BADDR(fssm->fssm_Device);
    len = bstr[0];
    if (len <= 0 || len > 39) return 0;
    for (i = 0; i < len; i++) out_driver[i] = (char)bstr[1 + i];
    out_driver[len] = 0;
    *out_unit = (uint32_t)fssm->fssm_Unit;
    if (!disc_is_device_name(out_driver)) return 0;
    return 1;
}

/* Copy a DOS device node's BSTR name (no colon) into out[<=8]. */
static void dev_dosname(struct DosList *dl, char *out)
{
    unsigned char *bstr; int len, i;
    out[0] = 0;
    if (!dl->dol_Name) return;
    if (TypeOfMem((void *)BADDR(dl->dol_Name)) == 0) return;
    bstr = (unsigned char *)BADDR(dl->dol_Name);
    len = bstr[0];
    if (len > 7) len = 7;
    for (i = 0; i < len; i++) out[i] = (char)bstr[1 + i];
    out[len] = 0;
}

DevLiveness safety_classify(const char *driver, uint32_t unit,
                            char out_names[][8], int max_names, int *n_names)
{
    static MountEntry mounts[32];           /* static: keep off the 4KB stack */
    int n_mounts = 0;
    int boot_known = 0;
    char boot_driver[40]; uint32_t boot_unit = 0;
    struct MsgPort *boot_port = 0;
    struct DevProc *dp;
    struct DosList *dl;

    boot_driver[0] = 0;

    /* Resolve the boot device's handler port via SYS:. */
    dp = GetDeviceProc((STRPTR)"SYS:", 0);
    if (dp) { boot_port = dp->dvp_Port; FreeDeviceProc(dp); }

    /* Walk the DOS device list: collect mounts + identify the boot device. */
    dl = LockDosList(LDF_DEVICES | LDF_READ);
    {
        struct DosList *e = dl;
        int guard = 0;
        while ((e = NextDosEntry(e, LDF_DEVICES | LDF_READ)) != 0 && ++guard < 256) {
            char drv[40]; uint32_t u;
            if (!dev_backing(e, drv, &u)) continue;
            if (n_mounts < 32) {
                int k;
                for (k = 0; k < 39 && drv[k]; k++) mounts[n_mounts].driver[k] = drv[k];
                mounts[n_mounts].driver[k] = 0;
                mounts[n_mounts].unit = u;
                dev_dosname(e, mounts[n_mounts].name);
                n_mounts++;
            }
            if (boot_port && e->dol_Task == boot_port) {
                int k;
                for (k = 0; k < 39 && drv[k]; k++) boot_driver[k] = drv[k];
                boot_driver[k] = 0;
                boot_unit = u; boot_known = 1;
            }
        }
    }
    UnLockDosList(LDF_DEVICES | LDF_READ);

    return safety_decide(driver, unit, boot_known, boot_driver, boot_unit,
                         mounts, n_mounts, out_names, max_names, n_names);
}

/* The live DOS devices backed by a device, up to SAFETY_MAX_LIVE. */
#define SAFETY_MAX_LIVE 32
static MountedPart g_live_parts[SAFETY_MAX_LIVE];       /* static: keep off the 4KB stack */

static int live_parts(void)
{
    MountedPart *parts = g_live_parts;
    int n = 0;
    struct DosList *dl;

    dl = LockDosList(LDF_DEVICES | LDF_READ);
    {
        struct DosList *e = dl;
        int guard = 0;
        while ((e = NextDosEntry(e, LDF_DEVICES | LDF_READ)) != 0 && ++guard < 256) {
            char drv[40]; uint32_t u;
            struct FileSysStartupMsg *fssm;
            struct DosEnvec *env;
            if (!dev_backing(e, drv, &u)) continue;
            if (!e->dol_misc.dol_handler.dol_Startup) continue;
            if (TypeOfMem((void *)BADDR(e->dol_misc.dol_handler.dol_Startup)) == 0) continue;
            fssm = (struct FileSysStartupMsg *)BADDR(e->dol_misc.dol_handler.dol_Startup);
            if (!fssm->fssm_Environ) continue;
            if (TypeOfMem((void *)BADDR(fssm->fssm_Environ)) == 0) continue;
            env = (struct DosEnvec *)BADDR(fssm->fssm_Environ);
            /* DE_UPPERCYL == 10; de_HighCyl is at offset 10 in DosEnvec.
               Note: the header defines DE_UPPERCYL (not DE_HIGHCYL) as the
               index macro for de_HighCyl. Guard: de_TableSize must be >= 10. */
            if (env->de_TableSize < DE_UPPERCYL) continue;
            if (n < SAFETY_MAX_LIVE) {
                int k;
                for (k = 0; k < 39 && drv[k]; k++) parts[n].driver[k] = drv[k];
                parts[n].driver[k] = 0;
                parts[n].unit = u;
                parts[n].low_cyl  = (uint32_t)env->de_LowCyl;
                parts[n].high_cyl = (uint32_t)env->de_HighCyl;
                parts[n].cyl_blocks = (uint32_t)env->de_Surfaces * (uint32_t)env->de_BlocksPerTrack;
                parts[n].dos_type = env->de_TableSize >= DE_DOSTYPE ? (uint32_t)env->de_DosType : 0;
                dev_dosname(e, parts[n].name);
                n++;
            }
        }
    }
    UnLockDosList(LDF_DEVICES | LDF_READ);
    return n;
}

int safety_partition_mounted(const char *driver, uint32_t unit,
                             uint32_t low_cyl, uint32_t high_cyl, char out_name[8])
{
    int n = live_parts();
    return safety_part_conflict(driver, unit, low_cyl, high_cyl, g_live_parts, n, out_name);
}

int safety_resume_mount(const char *driver, uint32_t unit, const char *name,
                        uint32_t low_cyl, uint32_t high_cyl, uint32_t cyl_blocks, uint32_t dos_type)
{
    int n = live_parts(), rc;
    struct DosList *e;
    int guard = 0;
    if (n >= SAFETY_MAX_LIVE) return SAFETY_RESUME_MISMATCH;
    rc = safety_resume_decide(driver, unit, name, low_cyl, high_cyl, cyl_blocks, dos_type, g_live_parts, n);
    if (rc != SAFETY_RESUME_NONE) return rc;
    /* Nothing listed by that name. A device of that name that could not be listed, because
       what backs it can't be read, is still something we know nothing about. */
    e = LockDosList(LDF_DEVICES | LDF_READ);
    while ((e = NextDosEntry(e, LDF_DEVICES | LDF_READ)) != 0 && ++guard < 256) {
        char live[8];
        live[0] = 0;
        dev_dosname(e, live);
        if (dev_eq(live, name)) { rc = SAFETY_RESUME_MISMATCH; break; }
    }
    UnLockDosList(LDF_DEVICES | LDF_READ);
    return rc;
}

/* The Kickstart ROM's addresses. A ROM soft-kicked with a mapping tool sits here too. */
#define ROM_LOW  0x00F80000UL
#define ROM_HIGH 0x01000000UL

/* Where a library vector jumps to, or 0 if it is not a plain absolute jump. */
static uint32_t vector_target(struct Library *lib, int lvo)
{
    const UBYTE *v = (const UBYTE *)lib + lvo;
    if (v[0] != 0x4E || v[1] != 0xF9) return 0;
    return ((uint32_t)v[2] << 24) | ((uint32_t)v[3] << 16) | ((uint32_t)v[4] << 8) | v[5];
}

int safety_is_internal_ide(void)
{
    struct Library *dev;
    struct Resident *found = 0;
    uint32_t begin_io, abort_io;

    Forbid();
    dev = (struct Library *)FindName(&SysBase->DeviceList, (CONST_STRPTR)"scsi.device");
    begin_io = dev ? vector_target(dev, -30) : 0;
    abort_io = dev ? vector_target(dev, -36) : 0;
    /* The driver running must be the ROM module these calls land in. A module from
       disk, or a patch on the calls, leaves them outside it and is refused. */
    if (begin_io >= ROM_LOW && begin_io < ROM_HIGH && SysBase->ResModules) {
        ULONG *p = (ULONG *)SysBase->ResModules;
        int guard = 0;
        while (*p && ++guard < 512) {
            struct Resident *rt;
            uint32_t at, end;
            if (*p & 0x80000000UL) {
                if (*p & 1) break;                /* an odd link: address error on a 68000 */
                p = (ULONG *)(*p & 0x7FFFFFFFUL);
                continue;
            }
            rt = (struct Resident *)*p++;
            at = (uint32_t)rt;
            if (at < ROM_LOW || at >= ROM_HIGH || (at & 1)) continue;
            if (rt->rt_MatchWord != RTC_MATCHWORD || rt->rt_MatchTag != rt) continue;
            end = (uint32_t)rt->rt_EndSkip;
            if (end <= at || end > ROM_HIGH) continue;
            if (begin_io < at || begin_io >= end || abort_io < at || abort_io >= end) continue;
            if (!rt->rt_Name || !dev_eq((const char *)rt->rt_Name, "scsi.device")) continue;
            found = rt;
            break;
        }
    }
    Permit();

    /* ROM does not move, so this needs no Forbid. */
    return found ? safety_driver_is_ide((const uint8_t *)found,
                                        (uint32_t)found->rt_EndSkip - (uint32_t)found) : 0;
}
#endif /* HDPART_AMIGA */
