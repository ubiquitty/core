#!/usr/bin/env bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0
#
# Runs one CTest build tree through the shared structured Python reporter.

set -euo pipefail

if [ "$#" -lt 1 ]; then
    echo "usage: $0 <build-dir> [ctest arg ...]" >&2
    exit 2
fi

build="$1"
shift
arguments=("$@")
if [ "${UBQ_TEST_FAIL_FAST:-0}" = "1" ]; then
    arguments=(--stop-on-failure "${arguments[@]}")
fi

export UBQ_REPORT_HEARTBEAT_SECONDS="${UBQ_CTEST_HEARTBEAT_SECONDS:-30}"
exec python3 "${BASH_SOURCE[0]%/*}/ctest_report.py" \
    --build "$build" -- "${arguments[@]}"
