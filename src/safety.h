#ifndef HDPART_SAFETY_H
#define HDPART_SAFETY_H
#include <stdint.h>

/* How "live" the physical device (driver,unit) is to the running system. */
typedef enum { DEV_CLEAR = 0, DEV_MOUNTED = 1, DEV_BOOT = 2 } DevLiveness;

/* One mounted DOS device's backing store. */
typedef struct {
    char     driver[40];   /* exec device name, e.g. "scsi.device" */
    uint32_t unit;
    char     name[8];      /* DOS device name, e.g. "DH0" (no colon) */
} MountEntry;

/* Pure classification. Compares the target (driver,unit) against the boot
   device (if known) and the list of currently-mounted DOS devices.
   - boot_known: 0 if the boot device could not be resolved.
   - out_names/max_names: filled with DOS names of volumes on the same physical
     (driver,unit) as the target, for the warning text. *n_names set to count.
   Returns DEV_BOOT / DEV_MOUNTED / DEV_CLEAR. Fail-safe: when boot is unknown
   and the target has no matching mount, returns DEV_MOUNTED (never CLEAR),
   because we cannot prove the target is not the boot device. */
DevLiveness safety_decide(const char *driver, uint32_t unit,
                          int boot_known, const char *boot_driver, uint32_t boot_unit,
                          const MountEntry mounts[], int n_mounts,
                          char out_names[][8], int max_names, int *n_names);

#ifdef HDPART_AMIGA
/* OS wrapper: resolve the boot device + the mount list from the running system,
   then call safety_decide. */
DevLiveness safety_classify(const char *driver, uint32_t unit,
                            char out_names[][8], int max_names, int *n_names);
#endif

#endif /* HDPART_SAFETY_H */
