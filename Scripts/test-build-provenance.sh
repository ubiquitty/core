#!/usr/bin/env bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

# test-build-provenance.sh — self-contained test for the build-directory
# freshness guard. Builds a throwaway git repo, stamps a build directory, then
# moves the worktree the way a stash, an unstash, and a commit each do, and
# asserts whether the guard contradicts the attribution. Deterministic; no
# network, no real build.

set -euo pipefail

# This fixture owns its library checkouts; containing build roots must not
# redirect provenance checks to the real source trees.
unset UBQ_CORE_ROOT UBQ_KIT_ROOT

SCRIPTS="$(cd "$(dirname "$0")" && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# shellcheck source=Scripts/build-provenance.sh
source "$SCRIPTS/build-provenance.sh"

# Under a hook, GIT_DIR names the PUSHING worktree and outranks the directory a
# command runs in — `git -C` included — so an un-isolated `git init` here
# re-initialises the real repository and exits 0, reporting success for having
# done the one thing this test must never do.
# shellcheck source=Tests/git-fixture.sh
source "$(cd "$SCRIPTS/.." && pwd)/Tests/git-fixture.sh"
ubq_make_fixture_repo "$WORK"

cd "$WORK"

mkdir -p Source/Demo
printf 'int base() { return 0; }\n' > Source/Demo/Demo.cpp
# build/ is ignored here as it is in the tree, so the stamp the guard writes
# stays out of the delta it measures.
printf 'build/\n' > .gitignore
git add -A && git commit -qm base

BUILD="$WORK/build/demo"
mkdir -p "$BUILD"
printf 'build demo.o: CXX_COMPILER %s/Source/Demo/Demo.cpp\n' "$WORK" > "$BUILD/build.ninja"
printf 'build posix.o: CXX_COMPILER %s/Source/Platform/POSIX/Demo.cpp\n' "$WORK" >> "$BUILD/build.ninja"

failures=0
# expect <case> <expected: warn|silent>
expect() {
    local caseName="$1" expected="$2" output verdict
    output=$(build_provenance_warning "$WORK" "$BUILD" "rebuild --clean" 2>&1)
    case "$output" in
        *PROVENANCE*) verdict=warn ;;
        *)            verdict=silent ;;
    esac
    if [ "$verdict" = "$expected" ]; then
        echo "ok   $caseName ($verdict)"
    else
        echo "FAIL $caseName: expected $expected, got $verdict" >&2
        failures=$((failures + 1))
    fi
}

expectRequirement() {
    local caseName="$1" expected="$2" output status=0
    output=$(build_provenance_require "$WORK" "$BUILD" "driver" "rebuild" 2>&1) || status=$?
    if [ "$status" -eq "$expected" ]; then
        echo "ok   $caseName (exit $status)"
    else
        echo "FAIL $caseName: expected exit $expected, got $status" >&2
        echo "$output" >&2
        failures=$((failures + 1))
    fi
}

# An unstamped directory has made no claim to contradict.
expect "unstamped directory is silent" silent

build_provenance_stamp "$WORK" "$BUILD"
expect "stamped, unchanged tree" silent
expectRequirement "clean built sources are accepted" 0

# Uncommitted work appears: the objects predate it, HEAD is unmoved.
printf 'int added() { return 1; }\n' >> Source/Demo/Demo.cpp
expect "worktree gains uncommitted changes" warn

# Stashing restores the exact tree the stamp was taken from.
git stash -q
expect "stash restores the stamped tree" silent

# Restoring the stash is the state the guard exists to catch: a build directory
# holding objects from before the change, at an unmoved HEAD.
git stash pop -q
expect "stash pop diverges again" warn

# Committing the change moves HEAD away from the stamped revision.
git add -A && git commit -qm change
expect "commit moves HEAD from the stamped revision" warn

# A fresh build re-establishes the attribution.
build_provenance_stamp "$WORK" "$BUILD"
expect "restamp after rebuild" silent

# The staged-only case moves the delta digest without touching HEAD.
printf 'int staged() { return 2; }\n' >> Source/Demo/Demo.cpp
git add -A
expect "staged-but-uncommitted changes" warn

build_provenance_stamp "$WORK" "$BUILD"
expectRequirement "freshly built staged sources are accepted" 0
git commit -qm staged
expectRequirement "committing built bytes preserves freshness" 0

printf 'int dirty() { return 3; }\n' >> Source/Demo/Demo.cpp
expectRequirement "postbuild source edit is rejected" 2
build_provenance_stamp "$WORK" "$BUILD"
expectRequirement "freshly built unstaged sources are accepted" 0
git add Source/Demo/Demo.cpp
expectRequirement "staging built bytes preserves freshness" 0
git commit -qm dirty
expectRequirement "committing the built source preserves freshness" 0

mkdir -p Docs Source/Platform/macOS
printf 'notes\n' > Docs/notes.md
printf 'let frontend = 0\n' > Source/Platform/macOS/Frontend.swift
expectRequirement "unrelated worktree changes preserve freshness" 0
git add Docs Source/Platform/macOS
git commit -qm unrelated
expectRequirement "unrelated commits preserve freshness" 0

printf '#pragma once\n' > Source/Demo/Demo.h
expectRequirement "untracked compiled header is rejected" 2
build_provenance_stamp "$WORK" "$BUILD"
expectRequirement "freshly built untracked header is accepted" 0
git add Source/Demo/Demo.h
expectRequirement "staging a built header preserves freshness" 0
git commit -qm header
expectRequirement "committing a built header preserves freshness" 0
printf 'struct Header { int value; };\n' >> Source/Demo/Demo.h
expectRequirement "postbuild header edit is rejected" 2
build_provenance_stamp "$WORK" "$BUILD"
expectRequirement "rebuilt header is accepted" 0

git add Source/Demo/Demo.h
git commit -qm header-change
git rm -q Source/Demo/Demo.h
expectRequirement "removed built header is rejected" 2
build_provenance_stamp "$WORK" "$BUILD"
expectRequirement "rebuilt deletion is accepted" 0
git commit -qm remove-header
expectRequirement "committing a built deletion preserves freshness" 0

printf '# source with a spaced name\n' > 'Source/Demo/Spaced Name.h'
printf '# source with a quoted name\n' > 'Source/Demo/Quoted"Name.h'
printf '# source with a newline\n' > $'Source/Demo/Newline\nName.h'
build_provenance_stamp "$WORK" "$BUILD"
expectRequirement "special source names are accepted after build" 0
git add Source/Demo
expectRequirement "staging special source names preserves freshness" 0
printf '# changed\n' >> 'Source/Demo/Spaced Name.h'
expectRequirement "special source name changes are rejected" 2
build_provenance_stamp "$WORK" "$BUILD"
git add Source/Demo
git commit -qm special-names
expectRequirement "committing special source names preserves freshness" 0
touch Source/Demo/Demo.cpp
expectRequirement "mtime-only changes preserve freshness" 0

printf 'object bytes\n' > "$BUILD/demo.o"
expectRequirement "ignored build output preserves freshness" 0
git mv 'Source/Demo/Spaced Name.h' 'Source/Demo/Renamed.h'
expectRequirement "renaming unchanged input bytes is rejected" 2
build_provenance_stamp "$WORK" "$BUILD"
expectRequirement "rebuilt rename is accepted" 0

printf 'project(Demo)\n' > CMakeLists.txt
expectRequirement "CMake source changes are rejected" 2
build_provenance_stamp "$WORK" "$BUILD"
printf 'set(flag ON)\n' >> CMakeLists.txt
expectRequirement "postbuild CMake source edit is rejected" 2

mkdir -p Scripts
printf 'set(flag ON)\n' > Scripts/build-input.cmake
printf 'build build.ninja: RERUN_CMAKE %s/Scripts/build-input.cmake\n' "$WORK" >> "$BUILD/build.ninja"
build_provenance_stamp "$WORK" "$BUILD"
printf 'set(other ON)\n' >> Scripts/build-input.cmake
expectRequirement "referenced configuration input edit is rejected" 2
build_provenance_stamp "$WORK" "$BUILD"
printf '# unrelated tooling\n' > Scripts/notes.sh
expectRequirement "unrelated tooling edit preserves freshness" 0

mkdir -p CMake
printf 'set(module ON)\n' > CMake/Module.cmake
printf 'set(root ON)\n' > BuildOptions.cmake
printf 'build build.ninja: RERUN_CMAKE %s/CMake/Module.cmake %s/BuildOptions.cmake\n' "$WORK" "$WORK" >> "$BUILD/build.ninja"
build_provenance_stamp "$WORK" "$BUILD"
printf 'set(module OFF)\n' >> CMake/Module.cmake
expectRequirement "referenced CMake module edit is rejected" 2
build_provenance_stamp "$WORK" "$BUILD"
printf 'set(root OFF)\n' >> BuildOptions.cmake
expectRequirement "referenced root CMake input edit is rejected" 2
build_provenance_stamp "$WORK" "$BUILD"
printf 'CMAKE_HOME_DIRECTORY:INTERNAL=%s/other-checkout\n' "$WORK" > "$BUILD/CMakeCache.txt"
printf 'build build.ninja: RERUN_CMAKE %s/other-checkout/CMake/Module.cmake\n' "$WORK" >> "$BUILD/build.ninja"
build_provenance_stamp "$WORK" "$BUILD"
printf 'set(remapped ON)\n' >> CMake/Module.cmake
expectRequirement "CMake inputs in a sibling checkout graph are remapped" 2

printf 'CMAKE_BUILD_TYPE:STRING=Debug\n' > "$BUILD/CMakeCache.txt"
build_provenance_stamp "$WORK" "$BUILD"
printf 'CMAKE_BUILD_TYPE:STRING=Release\n' > "$BUILD/CMakeCache.txt"
expectRequirement "postbuild CMake options edit is rejected" 2
build_provenance_stamp "$WORK" "$BUILD"
printf 'flags = -DCHANGED\n' >> "$BUILD/build.ninja"
expectRequirement "postbuild command graph edit is rejected" 2

printf 'build demo.o: CXX_COMPILER %s/Source/Demo/Demo.cpp\n' "$WORK" > "$BUILD/build.ninja"
build_provenance_stamp "$WORK" "$BUILD"
expectRequirement "graph without a platform source is accepted" 0
printf 'int later() { return 4; }\n' >> Source/Demo/Demo.cpp
expectRequirement "graph without a platform source still rejects edits" 2
build_provenance_stamp "$WORK" "$BUILD"

mv "$BUILD/build.ninja" "$BUILD/saved.ninja"
expectRequirement "missing build graph is rejected" 2
printf '[{"file":"%s/Source/Demo/Demo.cpp"}]\n' "$WORK" > "$BUILD/compile_commands.json"
build_provenance_stamp "$WORK" "$BUILD"
expectRequirement "non-Ninja compilation database is accepted" 0
printf 'int last() { return 5; }\n' >> Source/Demo/Demo.cpp
expectRequirement "non-Ninja compilation database rejects edits" 2
build_provenance_stamp "$WORK" "$BUILD"
mv Source/Demo/Demo.cpp "$BUILD/saved.cpp"
mkdir Source/Demo/Demo.cpp
expectRequirement "unreadable source kind is rejected" 2
rmdir Source/Demo/Demo.cpp
mv "$BUILD/saved.cpp" Source/Demo/Demo.cpp
expectRequirement "restored input bytes preserve freshness" 0

printf 'truncated\n' > "$BUILD/.ubiquitty-build-provenance"
expectRequirement "malformed stamp is rejected" 2

# build_provenance_current answers the same question in this shell, without
# exiting, for a caller that words its own refusal: reaching the next case is
# the proof that it returned.
expectCurrent() {
    local caseName="$1" expected="$2" status=0
    build_provenance_current "$WORK" "$BUILD" "probe" 2>/dev/null || status=$?
    if [ "$status" -eq "$expected" ]; then
        echo "ok   $caseName (status $status)"
    else
        echo "FAIL $caseName: expected status $expected, got $status" >&2
        failures=$((failures + 1))
    fi
}
expectCurrent "malformed stamp is not current" 1
build_provenance_stamp "$WORK" "$BUILD"
expectCurrent "fresh stamp is current" 0
printf 'int current() { return 6; }\n' >> Source/Demo/Demo.cpp
expectCurrent "postbuild edit is not current" 1

# A repository that carries a library sees it as one pinned commit, so an edit
# inside the library's checkout has to move the fingerprint on its own.
git checkout -q -- Source/Demo/Demo.cpp
LIBRARY_WORK="$(mktemp -d)"
trap 'rm -rf "$WORK" "$LIBRARY_WORK"' EXIT
ubq_make_fixture_repo "$LIBRARY_WORK"
mkdir -p "$LIBRARY_WORK/Source/Demo"
printf 'int library() { return 0; }\n' > "$LIBRARY_WORK/Source/Demo/Library.cpp"
git -C "$LIBRARY_WORK" add -A && git -C "$LIBRARY_WORK" commit -qm library
mkdir -p Libraries
git -c protocol.file.allow=always submodule add -q "$LIBRARY_WORK" Libraries/ubiquitty-core
git commit -qm "carry the library"
printf 'build library.o: CXX_COMPILER %s/Libraries/ubiquitty-core/Source/Demo/Library.cpp\n' "$WORK" >> "$BUILD/build.ninja"
build_provenance_stamp "$WORK" "$BUILD"
expectCurrent "a library's pinned bytes are current" 0
printf 'int edited() { return 1; }\n' >> Libraries/ubiquitty-core/Source/Demo/Library.cpp
expectCurrent "an edit inside a library's checkout is not current" 1

if [ "$failures" -ne 0 ]; then
    echo "test-build-provenance: $failures failure(s)" >&2
    exit 1
fi
echo "test-build-provenance: all cases pass"
