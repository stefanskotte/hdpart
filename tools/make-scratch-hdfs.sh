#!/bin/sh
# Create scratch hardfile fixtures for HDPart device-I/O testing.
#   scratch_blank.hdf : zeroed, no RDB  (HDPart should report "blank")
#   scratch_rdb.hdf   : a small RDB with two partitions written by rdbtool
#                       (HDPart should report "RDB"; partitions must match)
# Geometry: 64 cyl * 4 heads * 32 sec = 8192 blocks = 4 MB (matches the engine's
# rdbtool cross-check fixture so results are easy to compare).
#
# NOTE on rdbtool (amitools 0.4.0) syntax:
#   - "create chs=64,4,32" creates the image file with correct block count but the
#     geometry in the RDB block is set by "init", not "create".
#   - "init" (subclasses OpenCommand) re-detects geometry from file size unless you
#     pass chs= explicitly.  So BOTH create and init must receive chs=64,4,32.
#   - dostype=DOS3 (FFS Intl, 0x444F5303) must be stated explicitly per add command
#     (the default in 0.4.0 is 'ffs+intl', which is equivalent, but explicit is safer).
#   - size=50% allocates ~half the logical cylinders per partition; cyl 63 is left
#     free (an artefact of integer rounding), which is fine for a test fixture.
#   - The -f flag lets rdbtool overwrite an existing file (idempotent re-runs).
set -e
RDBTOOL=/opt/homebrew/bin/rdbtool
cd "$(dirname "$0")/.."
mkdir -p amiga_scratch
BLANK=amiga_scratch/scratch_blank.hdf
RDBF=amiga_scratch/scratch_rdb.hdf
BLOCKS=8192   # 64 cyl * 4 heads * 32 sec = 4 MB

# 4 MB zeroed image (no RDB) — recreate cleanly each run
dd if=/dev/zero of="$BLANK" bs=512 count=$BLOCKS >/dev/null 2>&1

# 4 MB image with an RDB + two partitions (DH4 and DH5, ~50% each), DOS3 (FFS Intl).
# create makes the file; -f overwrites an existing one (idempotent).
# init must also receive chs= so the geometry written to the RDB block is correct.
"$RDBTOOL" -f "$RDBF" create chs=64,4,32 \
  + init chs=64,4,32 \
  + add name=DH4 dostype=DOS3 size=50% \
  + add name=DH5 dostype=DOS3 size=50%

echo "=== scratch_rdb.hdf RDB ==="
"$RDBTOOL" "$RDBF" info
echo "Created amiga_scratch/scratch_blank.hdf and amiga_scratch/scratch_rdb.hdf"
