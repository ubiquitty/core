#!/usr/bin/env bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

# Proves tree-scoped gates discover untracked violations. --self-test runs
# isolated controls without planting anything in the caller's checkout.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
exec python3 "$ROOT/Scripts/gate_subjects.py" "$@"
