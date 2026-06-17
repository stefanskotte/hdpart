#!/bin/sh
# Compile and run HDPart host unit tests with the system compiler.
set -e
cd "$(dirname "$0")/.."
cc -std=c99 -Wall -Wextra -g -o /tmp/hdpart_tests tests/test_rdb.c src/rdb.c
/tmp/hdpart_tests
cc -std=c99 -Wall -Wextra -g -o /tmp/hdpart_fshd tests/test_fshd.c src/rdb.c
/tmp/hdpart_fshd
cc -std=c99 -Wall -Wextra -g -o /tmp/hdpart_disc tests/test_discover.c src/discover.c
/tmp/hdpart_disc
cc -std=c99 -Wall -Wextra -g -o /tmp/hdpart_driver tests/test_driver.c src/driver.c
/tmp/hdpart_driver
cc -std=c99 -Wall -Wextra -g -o /tmp/hdpart_safety tests/test_safety.c src/safety.c
/tmp/hdpart_safety
cc -std=c99 -Wall -Wextra -g -o /tmp/hdpart_format tests/test_format.c src/format.c src/rdb.c
/tmp/hdpart_format
