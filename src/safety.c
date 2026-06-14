/* HDPart device-liveness safety classifier. Pure helper (host-testable) +
   OS-bound gatherer guarded by HDPART_AMIGA. Mirrors discover.c. */
#ifdef HDPART_AMIGA
#include <exec/types.h>
#include <exec/execbase.h>
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
#endif /* HDPART_AMIGA */
