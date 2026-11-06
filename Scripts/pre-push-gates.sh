#!/usr/bin/env bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0
#
# Scripts/pre-push-gates.sh — the gates ubiquitty-core answers to before a push,
# run on this machine. A workflow that is disabled or queued is indistinguishable
# from one that passed: nothing goes red, so the tree looks green while nothing
# has executed.
#
# The families are Scripts/pre-push-gates-core.sh's, the tiers and the runner
# Scripts/pre-push-gates-lib.sh's. The frontends' arms, the wasm build and the
# snapshot regressions belong to the repositories that hold those frontends and
# are not in this list.
#
#   TIER=lint    every gate that needs no build                       no toolchain
#   TIER=quick   style + SSOT + PGO drift + host build + the tests the push reaches
#   TIER=ctest   quick + the ctest suite
#   TIER=linux   quick + Core and the host on real Linux
#   TIER=full    ctest + linux

# shellcheck source=Scripts/pre-push-gates-lib.sh
source "$(cd "$(dirname "$0")" && pwd)/pre-push-gates-lib.sh"
# shellcheck source=Scripts/pre-push-gates-core.sh
source "$UBQ_CORE_ROOT/Scripts/pre-push-gates-core.sh"

echo "pre-push gates (tier: $TIER) — the local stand-in for ci.yml" >&2
core_tier

ubq_pre_push_finish
