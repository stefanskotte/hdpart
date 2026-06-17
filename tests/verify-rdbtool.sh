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
#
# WHY we do NOT use rdbtool subcommands here:
#   amitools 0.4.0 (the brew-installed build) has a Python 3 bug in
#   amitools/fs/FileSystem.py:47 — it does `data += ls.get_data()` where
#   `data` starts as "" (str) but get_data() returns bytes.  This crashes with
#   a TypeError the instant rdbtool tries to open ANY image whose RDSK block
#   has a populated FileSysHdr list, i.e. before any subcommand executes.
#   The amitools *block parsers* (FSHeaderBlock.read / LoadSegBlock.read) are
#   unaffected; only the higher-level FileSystem glue is broken.
#
# WHAT we do instead:
#   Import amitools' own block-parser classes directly (bypassing FileSystem.py)
#   and feed them our image.  This gives us a genuine third-party validation of
#   every FSHD and LSEG block we emit — stronger than raw-byte offsets.
echo ""
echo "=== amitools block-parser cross-check (embedded FS) ==="
/tmp/hdpart_tests --emit-fs
python3 - <<'PY'
import sys, glob
# locate amitools site-packages (brew Cellar) without importing the buggy CLI
for p in glob.glob("/opt/homebrew/Cellar/amitools/*/libexec/lib/python*/site-packages"):
    sys.path.insert(0, p)
from amitools.fs.blkdev.RawBlockDevice import RawBlockDevice
from amitools.fs.block.rdb.RDBlock import RDBlock
from amitools.fs.block.rdb.FSHeaderBlock import FSHeaderBlock
from amitools.fs.block.rdb.LoadSegBlock import LoadSegBlock
dev = RawBlockDevice("/tmp/hdpart_test_fs.img"); dev.open()
rdsk = None
for b in range(16):
    r = RDBlock(dev, b)
    if r.read(): rdsk = r; break
assert rdsk, "no RDSK found"
assert rdsk.fs_list != 0xffffffff, "RDSK FileSysHdr is NULL (FS not embedded)"
fsh = FSHeaderBlock(dev, rdsk.fs_list)
assert fsh.read() and fsh.valid, "amitools rejected our FSHD block"
assert fsh.dos_type == 0x50465303, "FSHD dos_type mismatch: 0x%08X" % fsh.dos_type
seg = fsh.dev_node.seg_list_blk
data = b""; n = 0
while seg != 0xffffffff:
    ls = LoadSegBlock(dev, seg)
    assert ls.read(), "amitools rejected an LSEG block at %d" % seg
    data += bytes(ls.get_data()); seg = ls.next; n += 1
assert n >= 1, "no LSEG blocks"
assert data[:4] == b"\x00\x00\x03\xf3", "reconstructed payload missing HUNK header"
print("amitools parsers accept our FSHD (dos_type=0x%08X) + %d LSEG blocks, %d bytes" % (fsh.dos_type, n, len(data)))
dev.close()
PY
echo "amitools cross-check OK"
