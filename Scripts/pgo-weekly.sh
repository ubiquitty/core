#!/usr/bin/env bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

# pgo-weekly.sh — the weekly retrain check, run in a clone of its own.
#
#   Scripts/pgo-weekly.sh
#
# What Support/pgo/org.ubiquitty.pgo-retrain-check.plist runs on Sundays at
# 03:00. The steps live here so the launchd agent names one file and nothing
# else:
#
#   1. Refuse to start when the disk holding the clone, or $TMPDIR where a
#      retrain builds, has under 15 GiB free.
#   2. In the clone, `git fetch` and `git checkout --detach origin/main`.
#   3. Scripts/pgo-retrain-check.sh in report mode, at nice 10, always.
#   4. Scripts/pgo-retrain-check.sh --retrain, only when the report exits 1. It
#      trains at nice 10 itself and leaves a commit on pgo/retrain-<date>; it
#      never pushes.
#
# Every line goes to ~/Library/Logs/ubiquitty-pgo-weekly.log, each run headed
# with its date.
#
# A clone of its own rather than a worktree of a checkout somebody works in:
# the checkout moves HEAD, and the report then runs the scripts origin/main
# has, so the job always measures with the newest gate. The clone is made once
# by hand (Support/pgo/README.md, "Retraining").
#
#   UBQ_PGO_WEEKLY_CLONE          the clone (~/Projects/ubq-pgo-weekly)
#   UBQ_PGO_WEEKLY_LOG            the log (~/Library/Logs/ubiquitty-pgo-weekly.log)
#   UBQ_PGO_WEEKLY_MIN_FREE_GIB   the free space it needs to start (15)
#
# Exit 0 = within budget, or retrained onto a branch; 1 = the retrain failed;
# 2 = nothing could be measured, or the run could not start.
set -uo pipefail

# The whole body is one function, read before any of it runs: the checkout
# below rewrites this file in the clone, and bash reads a script as it goes.
pgo_weekly() {
    local clone log minimum
    clone="${UBQ_PGO_WEEKLY_CLONE:-$HOME/Projects/ubq-pgo-weekly}"
    log="${UBQ_PGO_WEEKLY_LOG:-$HOME/Library/Logs/ubiquitty-pgo-weekly.log}"
    minimum="${UBQ_PGO_WEEKLY_MIN_FREE_GIB:-15}"

    mkdir -p "$(dirname "$log")" || return 2
    exec >>"$log" 2>&1
    echo
    echo "=== pgo-weekly $(date '+%Y-%m-%d %H:%M:%S %z')"

    # launchd starts an agent with /usr/bin:/bin:/usr/sbin:/sbin, and the
    # training build wants the cmake and ninja a package manager installed.
    PATH="/opt/homebrew/bin:/usr/local/bin:$PATH"
    export PATH

    if [ ! -d "$clone/.git" ]; then
        echo "pgo-weekly: no clone at $clone; make it once with"
        echo "  git clone <the repository's URL> $clone"
        return 2
    fi

    local where free
    for where in "$clone" "${TMPDIR:-/tmp}"; do
        free="$(df -Pk "$where" | awk 'NR == 2 { print $4 }')"
        if ! [ "${free:-0}" -ge $((minimum * 1024 * 1024)) ] 2>/dev/null; then
            echo "pgo-weekly: REFUSED — $((${free:-0} / 1024 / 1024)) GiB free under $where, under the $minimum GiB a run needs"
            return 2
        fi
    done

    # A hook's environment names the checkout it ran in, and git obeys it over
    # the directory a command runs in.
    unset GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE GIT_COMMON_DIR

    cd "$clone" || return 2
    if ! git fetch --quiet origin || ! git checkout --quiet --detach origin/main; then
        echo "pgo-weekly: REFUSED — the clone could not be brought to origin/main; $clone needs a look"
        return 2
    fi
    echo "pgo-weekly: $clone at $(git rev-parse --short HEAD) (origin/main)"

    local status=0
    nice -n 10 ./Scripts/pgo-retrain-check.sh || status=$?
    case "$status" in
        0)
            echo "pgo-weekly: within budget; nothing to retrain"
            return 0
            ;;
        1) ;;
        *)
            echo "pgo-weekly: the report could not measure the drift (exit $status)"
            return 2
            ;;
    esac

    echo "pgo-weekly: a retrain is due; training"
    status=0
    ./Scripts/pgo-retrain-check.sh --retrain || status=$?
    if [ "$status" -ne 0 ]; then
        echo "pgo-weekly: the retrain failed (exit $status)"
        return 1
    fi
    echo "pgo-weekly: retrained; review the pgo/retrain-* branch in $clone and push it by hand"
    return 0
}

pgo_weekly "$@"; exit $?
