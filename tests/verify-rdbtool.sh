#!/bin/sh
# Emit an RDB image with the HDPart engine, then validate it with amitools rdbtool.
set -e
cd "$(dirname "$0")/.."
cc -std=c99 -Wall -Wextra -g -o /tmp/hdpart_tests tests/test_rdb.c src/rdb.c
/tmp/hdpart_tests --emit
echo "=== rdbtool free ==="
rdbtool /tmp/hdpart_test.img free
echo "=== rdbtool info ==="
rdbtool /tmp/hdpart_test.img info

# ── Embedded-FS cross-check ────────────────────────────────────────────────
# Emit a second image that includes an embedded PFS\3 filesystem (FSHD+LSEG).
# NOTE: rdbtool 0.4.0 (the brew-installed version) crashes with a Python 3
# bytes/str bug (FileSystem.py:47 "data += ls.get_data()") whenever it opens
# any image that has a populated FileSysHdr list. The crash happens inside
# rdbtool's open() call, before any subcommand runs, so there is no rdbtool
# subcommand that can show or verify the embedded FS with this build.
# Therefore we assert the FSHD block directly from the raw image bytes using
# a small python3 one-liner, which is always available on this platform.
# This is a strict cross-check: it verifies the FSHD magic, the expected
# DosType (0x50465303 = PFS\3), and that the SegList pointer is not FFFFFFFF.
echo ""
echo "=== emit FS image ==="
/tmp/hdpart_tests --emit-fs
echo "=== raw FSHD cross-check (rdbtool 0.4.0 unavailable for FS images) ==="
python3 -c "
import struct, sys
with open('/tmp/hdpart_test_fs.img', 'rb') as f:
    rdsk = f.read(512)
fshd_ptr = struct.unpack('>I', rdsk[32:36])[0]
print(f'RDSK FileSysHdr pointer: 0x{fshd_ptr:08X}')
if fshd_ptr == 0xFFFFFFFF:
    print('FAIL: FileSysHdr is FFFFFFFF (no embedded FS)')
    sys.exit(1)
with open('/tmp/hdpart_test_fs.img', 'rb') as f:
    f.seek(fshd_ptr * 512)
    fshd = f.read(512)
magic = fshd[0:4]
dos_type = struct.unpack('>I', fshd[32:36])[0]
lseg_ptr = struct.unpack('>I', fshd[72:76])[0]
print(f'FSHD magic : {magic}')
print(f'FSHD DosType: 0x{dos_type:08X}')
print(f'FSHD SegList: 0x{lseg_ptr:08X}')
if magic != b'FSHD':
    print('FAIL: block at FileSysHdr is not FSHD')
    sys.exit(1)
if dos_type != 0x50465303:
    print(f'FAIL: expected DosType 0x50465303 (PFS\\\\3), got 0x{dos_type:08X}')
    sys.exit(1)
if lseg_ptr == 0xFFFFFFFF:
    print('FAIL: FSHD SegList is FFFFFFFF (no LSEG data)')
    sys.exit(1)
print('PASS: embedded PFS\\\\3 FS verified in raw image bytes')
"
