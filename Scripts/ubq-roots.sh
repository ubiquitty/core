#!/usr/bin/env bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

# ubq-roots.sh — which tree a script judges, and where the other repositories
# are, for shell callers. Scripts/ubq_roots.py derives the identical answers for
# python callers.
#
# Sourced, not run, from inside the checkout it judges:
#
#     . "$(dirname "$0")/ubq-roots.sh"
#     "$UBQ_CORE_ROOT/Scripts/check-style.py"
#
# Sets, for the sourcing script (a child process resolves its own, the same way,
# so none is exported: a planted tree a self-test builds must not inherit them):
#
#   UBQ_SUBJECT_ROOT   the working tree git reports for the current directory
#   UBQ_CORE_ROOT      the ubiquitty-core checkout
#   UBQ_KIT_ROOT       the ubiquitty-kit checkout
#   UBQ_REPOSITORIES   the repositories whose files the subject holds
#
# UBQ_CORE_ROOT and UBQ_KIT_ROOT keep a value already set, and one a caller
# exported stays exported. Otherwise UBQ_KIT_ROOT is Libraries/ubiquitty-kit
# under the subject when that path exists. UBQ_CORE_ROOT is the Kit's own
# Libraries/ubiquitty-core when the subject carries the Kit, the Core that Kit
# pins, and Libraries/ubiquitty-core under the subject when the subject carries
# Core and no Kit. Each is the subject itself otherwise, so a repository that
# holds everything, and ubiquitty-core itself, read one tree. A submodule is
# chosen by existing and not by being populated, so a checkout that has not
# initialised it fails on the missing files instead of judging its own tree as
# the library's. Scripts/ubq_roots.py's core_submodule states the same rule.
#
# Scripts/repository.txt names the repository a checkout is: core, kit or app.
# A checkout without it holds all three.

UBQ_SUBJECT_ROOT="$(git rev-parse --show-toplevel 2>/dev/null || pwd)"
if [ -z "${UBQ_CORE_ROOT:-}" ]; then
    if [ -e "$UBQ_SUBJECT_ROOT/Libraries/ubiquitty-kit" ]; then
        UBQ_CORE_ROOT="$UBQ_SUBJECT_ROOT/Libraries/ubiquitty-kit/Libraries/ubiquitty-core"
    elif [ -e "$UBQ_SUBJECT_ROOT/Libraries/ubiquitty-core" ]; then
        UBQ_CORE_ROOT="$UBQ_SUBJECT_ROOT/Libraries/ubiquitty-core"
    else
        UBQ_CORE_ROOT="$UBQ_SUBJECT_ROOT"
    fi
fi
if [ -z "${UBQ_KIT_ROOT:-}" ]; then
    if [ -e "$UBQ_SUBJECT_ROOT/Libraries/ubiquitty-kit" ]; then
        UBQ_KIT_ROOT="$UBQ_SUBJECT_ROOT/Libraries/ubiquitty-kit"
    else
        UBQ_KIT_ROOT="$UBQ_SUBJECT_ROOT"
    fi
fi
if [ -f "$UBQ_SUBJECT_ROOT/Scripts/repository.txt" ]; then
    UBQ_REPOSITORIES="$(grep -v '^#' "$UBQ_SUBJECT_ROOT/Scripts/repository.txt" | tr -d '[:space:]')"
    case "$UBQ_REPOSITORIES" in
        core | kit | app) ;;
        *) echo "ubq-roots: Scripts/repository.txt names '$UBQ_REPOSITORIES', not core, kit or app" >&2
           return 1 2>/dev/null || exit 1 ;;
    esac
else
    UBQ_REPOSITORIES="core kit app"
fi
