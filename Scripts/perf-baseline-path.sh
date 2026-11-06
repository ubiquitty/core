#!/usr/bin/env bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

# Sourced by the measurement gates. Machine-specific records have an explicit
# external owner; running a gate must not create baseline data in the checkout.
ubiquitty_perf_baseline_directory() {
    if [ -z "${UBIQUITTY_PERF_BASELINE_DIR:-}" ]; then
        echo "Set UBIQUITTY_PERF_BASELINE_DIR to an external machine-specific baseline directory." >&2
        return 2
    fi
    printf '%s\n' "$UBIQUITTY_PERF_BASELINE_DIR"
}
