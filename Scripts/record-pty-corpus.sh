#!/bin/bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0
#
# record-pty-corpus.sh — re-record the real-program entries of the Sync corpus.
#
# Tests/Corpus/sync/ holds PTY streams cut into the reads a host makes off them,
# with the instant of each read and whether a would-block followed it. Most
# entries are synthesised (Tests/Corpus/sync/generate.py, deterministic); the
# three whose shape synthesis would misrepresent are recorded from the real
# program by Tests/Corpus/sync/record.py:
#
#   vim-margins  vim with no initialization file and a horizontal split, each
#                window scrolled line by line — the DECSTBM margined scroll an
#                editor issues.
#   less         paging, line scrolling, a search, end and start of file.
#   top          a once-a-second full-screen refresh, filtered to one scratch
#                process; it stands in for htop where htop is not installed.
#
# Each runs on a 24x80 PTY in a scratch directory with an empty HOME and a fixed
# environment, typed at by a fixed key script, so the stream names nobody and
# describes no machine. Each entry is trimmed to under 200 KB. A recording is a
# recording: two runs differ in timing, so re-record only to change an entry,
# and re-run the corpus test and Docs/sync-measurements.md after.
#
# Usage:
#   ./Scripts/record-pty-corpus.sh              record every real-program entry
#   ./Scripts/record-pty-corpus.sh less         record one
#   ./Scripts/record-pty-corpus.sh --synthetic  regenerate the synthetic entries
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CORPUS="$ROOT/Tests/Corpus/sync"
if [ "${1:-}" = "--synthetic" ]; then
    exec python3 "$CORPUS/generate.py"
fi
exec python3 "$CORPUS/record.py" "$@"
