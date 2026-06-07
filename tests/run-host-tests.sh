#!/bin/sh
# Compile and run HDPart host unit tests with the system compiler.
set -e
cd "$(dirname "$0")/.."
cc -std=c99 -Wall -Wextra -g -o /tmp/hdpart_tests tests/test_rdb.c src/rdb.c
/tmp/hdpart_tests
AMIGA_SDK="/Users/sfs/.vscode/extensions/bartmanabyss.amiga-debug-1.8.2/bin/darwin/opt/m68k-amiga-elf/sys-include"
cc -std=c99 -Wall -Wextra -g -idirafter "$AMIGA_SDK" -o /tmp/hdpart_disc tests/test_discover.c src/discover.c
/tmp/hdpart_disc
