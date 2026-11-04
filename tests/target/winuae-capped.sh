#!/bin/sh
# Starts WinUAE by hand with a size limit on its log, for one-off experiments.
# Never send the emulator's output straight to a file: an orphaned one wrote 255 GiB.
#
#   tests/target/winuae-capped.sh LOG winuae-arguments...
#
# When the log reaches WINUAE_LOG_CAP bytes (default 20 MiB) head exits, and the
# emulator gets SIGPIPE on its next write. Stop it yourself when done:
#   kill $(lsof -nP -tiTCP:2345 -sTCP:LISTEN)
[ $# -ge 1 ] || { echo "usage: $0 LOG winuae-arguments..." >&2; exit 2; }
log=$1; shift
"${WINUAE_PATH:-$HOME/Devel/winuae-logical/build/winuae}" "$@" 2>&1 | head -c "${WINUAE_LOG_CAP:-20971520}" > "$log"
