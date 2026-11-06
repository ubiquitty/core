#!/usr/bin/env bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0
#
# ccache-env.sh — the compiler-cache settings this tree's builds need, carried
# in the checkout rather than left to each machine's ccache.conf.
#
# Sourced from Scripts/build-root.sh, so every script that builds gets them, and
# so does a cmake that one of those scripts launches — the launcher
# Scripts/cmake/UbiquittyPreProject.cmake sets adds CCACHE_SLOPPINESS through
# `cmake -E env`, which passes the inherited environment through rather than
# replacing it.
#
# A machine runs many worktrees of this repository at once, and each builds the
# same sources. Whether they share cache entries or each keep a copy of every
# object is what decides how long a build takes when several run together:
#
#   CCACHE_NOHASHDIR   Keeps the build directory out of the cache key. It is in
#                      the key by default, and RelWithDebInfo compiles with -g,
#                      which is when that keying applies — so without this each
#                      worktree keeps its own copy of every object: a
#                      byte-identical source compiled in a second directory
#                      misses without it, and hits with it.
#   CCACHE_BASEDIR     Rewrites absolute paths under it before hashing, which is
#                      what lets two worktrees at different paths produce one
#                      key. The worktrees of this repository live under several
#                      roots — a home directory and a temporary one — so the
#                      only prefix covering them is the root.
#
# The cost, stated because it is the reason to override this: an object reused
# across worktrees carries the compilation directory of whichever built it
# first, so a debugger resolves its source paths against that worktree. Set
# CCACHE_NOHASHDIR= (empty) in the environment to buy exact debug paths back at
# the price of the sharing.
#
# Capacity is not set here. It is a property of the machine's disk rather than
# of this tree, and a cache too small for the shared working set evicts what it
# is about to be asked for — degrading toward no cache rather than failing, so
# it is worth checking rather than assuming.

: "${CCACHE_BASEDIR:=/}"
: "${CCACHE_NOHASHDIR:=1}"
export CCACHE_BASEDIR CCACHE_NOHASHDIR
