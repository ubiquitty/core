#!/usr/bin/env bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

# Build and run terminal_velocity, the escape-code demo and drain benchmark.
# Run it inside Ubiquitty at full size. Keys: 1-9 jump to a part, n/p or
# arrow right/left cycle, a resumes the timed loop, q quits. The animation
# clock is wall time, so every terminal shows the same content at whatever
# frame rate its parser can drain; the HUD's FPS and MB/s are the measurement.
#
#   run.sh                 the full nine-part loop, uncapped
#   run.sh --fps 60        frame-capped
#   run.sh --part 3        a single part (1-9)
#   run.sh --bench         headless generator-throughput table
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# Build output lives outside the checkout; see Scripts/build-root.sh.
# shellcheck source=Scripts/build-root.sh
. "$ROOT/Scripts/build-root.sh"
OUT="$UBQ_BUILD_ROOT/demo"
mkdir -p "$OUT"
"${CC:-cc}" -std=c99 -O2 -Wall -Wextra -o "$OUT/terminal_velocity" \
    "$ROOT/Tools/demo/terminal_velocity.c" -lm
exec "$OUT/terminal_velocity" "$@"
