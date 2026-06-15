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

/* A live mounted partition's identity, for "is THIS partition mounted?" checks. */
typedef struct {
    char     driver[40];   /* exec device name */
    uint32_t unit;
    uint32_t low_cyl;
    uint32_t high_cyl;
    char     name[8];      /* live DOS device name */
} MountedPart;

/* Pure: does the target partition (driver,unit,low_cyl..high_cyl) overlap any
   entry in parts[]? Match = case-insensitive driver AND same unit AND cylinder
   ranges overlap ([low..high] inclusive). On the first match, copies that
   entry's name into out_name[8] (if non-NULL) and returns 1; else returns 0. */
int safety_part_conflict(const char *driver, uint32_t unit,
                         uint32_t low_cyl, uint32_t high_cyl,
                         const MountedPart parts[], int n_parts,
                         char out_name[8]);

#ifdef HDPART_AMIGA
/* OS wrapper: resolve the boot device + the mount list from the running system,
   then call safety_decide. */
DevLiveness safety_classify(const char *driver, uint32_t unit,
                            char out_names[][8], int max_names, int *n_names);

/* OS: scan live DOS devices; if the partition at (driver,unit,low_cyl..high_cyl)
   is currently mounted, return 1 and copy its live DOS name into out_name[8].
   Returns 0 if not mounted. */
int safety_partition_mounted(const char *driver, uint32_t unit,
                             uint32_t low_cyl, uint32_t high_cyl, char out_name[8]);
#endif

#endif /* HDPART_SAFETY_H */
