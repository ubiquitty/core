#!/usr/bin/env bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

# Build + run pty_probe: ground-truth measurement of the macOS PTY transport
# with no parser attached — quantum size distribution, EAGAIN behavior, and
# the read-side throughput ceiling under a chosen wait strategy.
#
# Usage:
#   ./run.sh                        # 200 MB, 64 KB reads, poll on EAGAIN
#   ./run.sh ceiling                # spin 200 ns on EAGAIN (drain-style riding)
#   ./run.sh stall <us> <every>     # synthetic parse stall curve point
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="$ROOT/BuildHostTests"
mkdir -p "$OUT"
CC="${CC:-cc}"
"$CC" -O2 -o "$OUT/pty_probe" "$ROOT/Tools/pty-probe/pty_probe.c"

case "${1:-default}" in
    ceiling) "$OUT/pty_probe" 200000000 65536 0 65536 65536 200 ;;
    stall) "$OUT/pty_probe" 200000000 65536 "${2:-3}" "${3:-1024}" 65536 200 ;;
    *) "$OUT/pty_probe" 200000000 65536 0 ;;
esac
