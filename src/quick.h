/* HDPart Quick setup: layout arithmetic and setup-pending check (host-testable). */
#ifndef HDPART_QUICK_H
#define HDPART_QUICK_H
#include <stdint.h>
#include "rdb.h"

enum { QS_WORK = 1, QS_GAMES = 2, QS_DEMOS = 4 };
#define QS_ERR_TOO_SMALL (-20)

/* Clear m's partitions and fill them for `mask`: System (bootable DH0) plus one
   partition per ticked name. m must be rdb_init_model-ed with the card's geometry.
   Sets m->hi_cyl so nothing ends past 4 GiB; m->cylinders is left alone.
   Returns RDB_OK, QS_ERR_TOO_SMALL (under 128 MB usable) or an RDB_ERR_*. */
int quick_layout(RdbModel *m, uint32_t mask);

/* Volume name for partition `part_index` (0 = "System", then the ticked names in
   order Work, Games, Demos). NULL when out of range. */
const char *quick_volume_name(uint32_t mask, int part_index);

/* Does cylinders x heads x sectors agree with the drive's total, to within total/64 either
   way? Some drivers report figures that don't: scsi.device 40.20 on an A4000 said a third of
   a card, and a figure too high could put a partition past the end. 0 when they disagree. */
int quick_geometry_agrees(uint32_t cylinders, uint32_t heads, uint32_t sectors, uint32_t total);

/* Cylinders that fit the drive: figures can agree with its total and still overstate it,
   and a partition must not end past the end. Returns the count unchanged if it already fits
   or if heads or sectors is zero. */
uint32_t quick_cap_cylinders(uint32_t cylinders, uint32_t heads, uint32_t sectors, uint32_t total);

/* 1 only when m->setup_pending is set and the table still matches it. */
int quick_pending_valid(const RdbModel *m);

#endif
