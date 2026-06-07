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
