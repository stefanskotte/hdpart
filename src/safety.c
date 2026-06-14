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
