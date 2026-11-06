#!/usr/bin/env bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

# build-jobs.sh — how many compilers this machine can run at once, for shell
# callers. The reasoning lives in Scripts/build_jobs.py, which the Python
# callers import; this is the shell door onto the same answer.

# The job count for this machine, as a number on stdout.
ubq_build_jobs() { # [megabytes-per-job]
    python3 "${BASH_SOURCE[0]%/*}/build_jobs.py" "$@"
}
