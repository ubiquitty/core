#!/usr/bin/env bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

# check-wasm-oracle.sh — the wasm arm catches a break the host misses, proven
# freshly rather than cited from history.
#
# The pre-push gate_wasm builds portable Core under emscripten on the
# claim that wasm32's toolchain rejects shapes a 64-bit host accepts, so the arm
# catches Core/Base breaks no host lane does. That claim is only worth a minute
# of every Core push while the two toolchains disagree, and any one source shape
# that separates them can stop separating them when either toolchain changes.
#
# This holds the claim to a fixture that disagrees by construction: a
# static_assert that a pointer is 64-bit, which a 64-bit host passes and wasm32
# fails. Compiled under both, the host must accept it and wasm must reject it. If
# a toolchain change ever made them agree, the arm would compile a broken Core
# and say ok; this goes red here first, naming the arm that stopped disagreeing.
#
# SKIPPED, not passed, when emscripten is absent — gate_wasm's own shape.

set -u

host_cxx="${CXX:-c++}"

if ! command -v emcc >/dev/null 2>&1; then
    echo "check-wasm-oracle: SKIPPED (no emscripten; emsdk provides emcc)"
    exit 0
fi

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
fixture="$work/wasm_only_break.cpp"

# The disagreement, by construction. A missing transitive include would be more
# faithful to the recorded incident but rots the moment a libc++ update changes
# what it hands over; a 64-bit pointer is a fixed property of the two ABIs.
cat >"$fixture" <<'CPP'
#include <cstddef>
static_assert(sizeof(void*) == 8, "a 64-bit host accepts this; wasm32 must reject it");
int main() { return 0; }
CPP

"$host_cxx" -std=c++23 -c "$fixture" -o "$work/host.o" 2>/dev/null
host_status=$?

emcc -std=c++23 -c "$fixture" -o "$work/wasm.o" 2>/dev/null
wasm_status=$?

# The gate's premise is host-accepts and wasm-rejects. Either half failing means
# the two toolchains no longer disagree about this fixture, so the arm can no
# longer be trusted to catch what the host misses.
if [ "$host_status" -ne 0 ]; then
    echo "check-wasm-oracle: FAILED — the host compiler ($host_cxx) rejected a fixture it must accept (exit $host_status); the disagreement the wasm arm relies on is not measurable here"
    exit 1
fi
if [ "$wasm_status" -eq 0 ]; then
    echo "check-wasm-oracle: FAILED — emscripten accepted a wasm-only break the host also accepts, so the wasm arm no longer disagrees with the host and gate_wasm would pass a broken Core"
    exit 1
fi

echo "check-wasm-oracle: ok (host accepts, wasm rejects — the arm still catches a break the host misses)"
exit 0
