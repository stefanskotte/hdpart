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
    uint32_t cyl_blocks;   /* its blocks per cylinder */
    uint32_t dos_type;     /* the DosType it was mounted for */
} MountedPart;

/* Pure: does the target partition (driver,unit,low_cyl..high_cyl) overlap any
   entry in parts[]? Match = case-insensitive driver AND same unit AND cylinder
   ranges overlap ([low..high] inclusive). On the first match, copies that
   entry's name into out_name[8] (if non-NULL) and returns 1; else returns 0. */
int safety_part_conflict(const char *driver, uint32_t unit,
                         uint32_t low_cyl, uint32_t high_cyl,
                         const MountedPart parts[], int n_parts,
                         char out_name[8]);

/* Pure, for the Quick setup restart path only: what stands on this partition of a marked
   table? SAFETY_RESUME_MATCH: one mount, of this name, on this driver and unit, with exactly
   these bounds, cylinder size and DosType. SAFETY_RESUME_NONE: nothing on those blocks and
   nothing of that name. SAFETY_RESUME_MISMATCH: anything else, so nothing may be formatted. */
enum { SAFETY_RESUME_MISMATCH = -1, SAFETY_RESUME_NONE = 0, SAFETY_RESUME_MATCH = 1 };
int safety_resume_decide(const char *driver, uint32_t unit, const char *name,
                         uint32_t low_cyl, uint32_t high_cyl, uint32_t cyl_blocks, uint32_t dos_type,
                         const MountedPart parts[], int n_parts);

/* Pure: is this scsi.device module's code an internal IDE driver? Every IDE build loads
   its hardware base into A4 at init with lea abs.l,a4, so that instruction says which
   port it drives. SCSI builds may use another instruction (A3000 2.x: movea.l), so
   their bases are matched as a long in any instruction. 1 only for exactly one IDE lea
   and neither SCSI base anywhere. len is in bytes. See docs/reference/quick-setup-tests.md. */
int safety_driver_is_ide(const uint8_t *code, uint32_t len);

#ifdef HDPART_AMIGA
/* OS: 1 only when the scsi.device in exec's device list is proven to be a ROM-resident
   internal IDE driver. 0 for anything else, including doubt: no scsi.device, an
   A3000's or an NCR driver, a driver loaded from disk, or BeginIO/AbortIO patched.
   Reads memory only and touches no hardware. */
int safety_is_internal_ide(void);

/* OS wrapper: resolve the boot device + the mount list from the running system,
   then call safety_decide. */
DevLiveness safety_classify(const char *driver, uint32_t unit,
                            char out_names[][8], int max_names, int *n_names);

/* OS: scan live DOS devices; if the partition at (driver,unit,low_cyl..high_cyl)
   is currently mounted, return 1 and copy its live DOS name into out_name[8].
   Returns 0 if not mounted. */
int safety_partition_mounted(const char *driver, uint32_t unit,
                             uint32_t low_cyl, uint32_t high_cyl, char out_name[8]);

/* OS: safety_resume_decide against the live DOS devices. Too many to list is a mismatch. */
int safety_resume_mount(const char *driver, uint32_t unit, const char *name,
                        uint32_t low_cyl, uint32_t high_cyl, uint32_t cyl_blocks, uint32_t dos_type);
#endif

#endif /* HDPART_SAFETY_H */
