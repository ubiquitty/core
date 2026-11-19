#!/usr/bin/env bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0
#
# default-compiler.sh — which compiler a build takes when its caller names none.
#
# Sourced from Scripts/build-root.sh, so every script that builds gets it, and
# so does a cmake one of those scripts launches: CMake reads CC and CXX from the
# environment the first time it configures a directory.
#
# On Linux `cc` and `c++` are usually GCC. The tree compiles clean under both,
# but they do not build the same terminal: the training profile in Support/pgo
# is LLVM's and GCC cannot read it, and on the parse and fill lanes of
# Tests/Performance/bench_ingest_corpora.cpp a GCC build runs between a few
# percent and several times slower than a Clang one. A build that silently took
# GCC is a slower program than the one that ships, measured as if it were it.
#
# So an unset CC and CXX become clang and clang++ where both are installed and
# meet the floor in Scripts/cmake/ToolchainFloor.cmake. A caller that sets
# either keeps both exactly as it set them, which is how a GCC arm asks for
# GCC. Other systems are left alone: their `c++` is already the compiler their
# builds are measured with.
#
# A build directory keeps the compiler it was first configured with, whatever
# this exports later; a directory configured under GCC stays GCC until it is
# removed.

ubq_default_compiler() {
    [ "$(uname -s)" = Linux ] || return 0
    [ -z "${CC:-}" ] && [ -z "${CXX:-}" ] || return 0
    command -v clang >/dev/null 2>&1 && command -v clang++ >/dev/null 2>&1 || return 0
    local major
    major="$(clang++ -dumpversion 2>/dev/null | cut -d. -f1)"
    case "$major" in '' | *[!0-9]*) return 0 ;; esac
    # The Clang floor: ToolchainFloor.cmake's UBIQUITTY_CLANG_FLOOR.
    [ "$major" -ge 19 ] || return 0
    export CC=clang CXX=clang++
}
ubq_default_compiler
