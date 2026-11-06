#!/usr/bin/env bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

# Scripts/test-build-version.sh — the version rule, against throwaway
# repositories. No build, seconds to run.
#
# Scripts/cmake/BuildVersion.cmake derives a build's version from the commit.
# Each case here is a tree it has to answer for: at a named version, past one,
# modified, in a linked worktree, on a detached head, in a shallow clone, with
# no repository, with no git, and inside a container that is told the version.
# The refusals are cases too: a tag or a stated version that is not a version
# stops the configure and names what it read.
set -uo pipefail

# git exports these into a hook's environment and they override the repository
# a git command discovers from its directory, `git -C <tree>` included. A run
# started from a hook would aim the fixture's git at the repository pushing.
unset GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE GIT_OBJECT_DIRECTORY \
    GIT_ALTERNATE_OBJECT_DIRECTORIES GIT_COMMON_DIR GIT_NAMESPACE

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
. "$ROOT/Tests/shell-assertions.sh"
. "$ROOT/Tests/git-fixture.sh"

# No configuration but the fixture's own: a signing rule, a hooks path or a tag
# option from the machine's configuration would decide what a case sees.
export GIT_CONFIG_GLOBAL=/dev/null GIT_CONFIG_SYSTEM=/dev/null GIT_CONFIG_NOSYSTEM=1
unset UBIQUITTY_BUILD_VERSION UBIQUITTY_BUILD_IDENTITY

RULE="$ROOT/Scripts/cmake/BuildVersion.cmake"
CMAKE="$(command -v cmake)"

# field <tree> <field> — one answer, or the refusal's text.
field() {
    "$CMAKE" -DUBIQUITTY_VERSION_SOURCE_DIR="$1" -DUBIQUITTY_VERSION_PRINT="$2" -P "$RULE" 2>&1
}

# answers <tree> — "version|named|commits|bundle|identity".
answers() {
    printf '%s|%s|%s|%s|%s' "$(field "$1" version)" "$(field "$1" named)" "$(field "$1" commits)" \
        "$(field "$1" bundle)" "$(field "$1" identity)"
}

commit() { # <tree> <message>
    git -C "$1" commit --quiet --allow-empty -m "$2"
}

hash() { # <tree>
    git -C "$1" rev-parse --short=8 HEAD
}

echo "build_version:"

# A tree with no repository: the VERSION file, and the identity says why.
ARCHIVE="$TMP/archive"
mkdir -p "$ARCHIVE"
echo "0.2.0" > "$ARCHIVE/VERSION"
check "a tree with no repository reads VERSION" "0.2.0|0.2.0|0|20000000|no git" "$(answers "$ARCHIVE")"

# The same tree, where git is not installed at all.
EMPTY_PATH="$TMP/empty-path"
mkdir -p "$EMPTY_PATH"
check "a machine with no git reads VERSION" "0.2.0|no git" \
    "$(PATH="$EMPTY_PATH" field "$ARCHIVE" version)|$(PATH="$EMPTY_PATH" field "$ARCHIVE" identity)"

MAIN="$TMP/main"
ubq_make_fixture_repo "$MAIN" || exit 1
echo "0.2.0" > "$MAIN/VERSION"
echo "tracked" > "$MAIN/file"
git -C "$MAIN" add VERSION file
git -C "$MAIN" commit --quiet -m "first"

# A repository no named version reaches.
check "no reachable tag reads VERSION and says so" "0.2.0|0.2.0|0|20000000|$(hash "$MAIN"), no version tag" "$(answers "$MAIN")"

# At the named version.
git -C "$MAIN" tag -a v0.2.0 -m "0.2.0"
TAGGED="$(hash "$MAIN")"
check "at the tag the count is not printed" "0.2.0|0.2.0|0|20000000|$TAGGED" "$(answers "$MAIN")"

# Three commits past it.
commit "$MAIN" one
commit "$MAIN" two
commit "$MAIN" three
THIRD="$(hash "$MAIN")"
check "three commits past the tag" "0.2.0.3|0.2.0|3|20000003|$THIRD" "$(answers "$MAIN")"

# A hook's environment names another repository; the tree named is still the
# one that answers.
check "an inherited GIT_DIR does not redirect the question" "0.2.0.3|$THIRD" \
    "$(GIT_DIR="$TMP/lane-elsewhere/.git" GIT_WORK_TREE="$TMP" field "$MAIN" version)|$(GIT_DIR="$TMP/lane-elsewhere/.git" field "$MAIN" identity)"

# The number is the commit's: asked twice, and from a fresh clone, it is the same.
check "asked again, the answer is the same" "$(answers "$MAIN")" "$(answers "$MAIN")"
git clone --quiet "$MAIN" "$TMP/clone"
check "a clone of the commit agrees" "$(answers "$MAIN")" "$(answers "$TMP/clone")"

# A modified tree keeps its number; only the identity says so. An untracked
# file is not a modification: a build leaves those.
echo "untracked" > "$MAIN/left-by-a-build"
check "an untracked file marks nothing" "0.2.0.3|$THIRD" "$(field "$MAIN" version)|$(field "$MAIN" identity)"
echo "edited" > "$MAIN/file"
check "a modified tree keeps the number and says so" "0.2.0.3|20000003|$THIRD, modified" \
    "$(field "$MAIN" version)|$(field "$MAIN" bundle)|$(field "$MAIN" identity)"
git -C "$MAIN" checkout --quiet -- file
rm -f "$MAIN/left-by-a-build"

# A linked worktree on a branch, two commits further.
git -C "$MAIN" worktree add --quiet -b lane "$TMP/lane" HEAD
commit "$TMP/lane" four
commit "$TMP/lane" five
check "a linked worktree on a branch" "0.2.0.5|0.2.0|5|20000005|$(hash "$TMP/lane")" "$(answers "$TMP/lane")"

# A detached head, in a linked worktree as a landing builds one.
git -C "$MAIN" worktree add --quiet --detach "$TMP/detached" "$THIRD"
check "a detached head" "0.2.0.3|$THIRD" "$(field "$TMP/detached" version)|$(field "$TMP/detached" identity)"

# A later named version. The commit that carries it sets VERSION first.
echo "0.2.1" > "$MAIN/VERSION"
git -C "$MAIN" commit --quiet -am "name 0.2.1"
check "a raised VERSION does not move an untagged build" "0.2.0.4" "$(field "$MAIN" version)"
git -C "$MAIN" tag -a v0.2.1 -m "0.2.1"
check "the next named version" "0.2.1|0.2.1|0|20100000|$(hash "$MAIN")" "$(answers "$MAIN")"
commit "$MAIN" six
check "the bundle integer rises across the tag" "0.2.1.1|20100001" "$(field "$MAIN" version)|$(field "$MAIN" bundle)"

# A branch that merges main reaches main's newest tag through the merge.
git -C "$TMP/lane" merge --quiet --no-ff -m "merge main" "$(git -C "$MAIN" rev-parse HEAD)"
check "a branch that merged main is on main's version" "0.2.1" "$(field "$TMP/lane" named)"

# A lightweight tag names a version as an annotated one does: a release
# checkout may hold either.
LIGHT="$TMP/light"
ubq_make_fixture_repo "$LIGHT" || exit 1
echo "1.4.2" > "$LIGHT/VERSION"
git -C "$LIGHT" add VERSION
git -C "$LIGHT" commit --quiet -m "first"
git -C "$LIGHT" tag v1.4.2
commit "$LIGHT" one
check "a lightweight tag is read" "1.4.2.1|1040200001" "$(field "$LIGHT" version)|$(field "$LIGHT" bundle)"

# A shallow clone that cannot reach a tag falls back, and one that can says it
# is shallow, because a cut history may hide commits the count would include.
git clone --quiet --depth 1 --no-tags "file://$MAIN" "$TMP/shallow-untagged"
check "a shallow clone past its tags reads VERSION" "0.2.1|$(hash "$TMP/shallow-untagged"), no version tag" \
    "$(field "$TMP/shallow-untagged" version)|$(field "$TMP/shallow-untagged" identity)"
git clone --quiet --depth 2 "file://$MAIN" "$TMP/shallow-tagged"
check "a shallow clone that reaches a tag says it is shallow" "0.2.1.1|$(hash "$TMP/shallow-tagged"), shallow clone" \
    "$(field "$TMP/shallow-tagged" version)|$(field "$TMP/shallow-tagged" identity)"

# An archive unpacked inside another repository must not read that
# repository's tags.
mkdir -p "$MAIN/vendored"
echo "3.0.0" > "$MAIN/vendored/VERSION"
check "a tree inside another repository answers for itself" "3.0.0|no git" \
    "$(field "$MAIN/vendored" version)|$(field "$MAIN/vendored" identity)"
rm -rf "$MAIN/vendored"

# Refusals. Each names what it read.
refusal() { # <description> <tag> <wanted text>
    local fixture="$TMP/refusal-$RANDOM"
    ubq_make_fixture_repo "$fixture" || exit 1
    echo "0.2.0" > "$fixture/VERSION"
    git -C "$fixture" add VERSION
    git -C "$fixture" commit --quiet -m "first"
    git -C "$fixture" tag "$2"
    commit "$fixture" one
    local output status
    output="$(field "$fixture" version)"
    status=$?
    check "$1: the configure stops" "1" "$status"
    contains "$1: the message names it" "$3" "$output"
}
refusal "a two-field tag" "v0.2" "The tag 'v0.2' is '0.2', which is not a version"
refusal "a tag with a leading zero" "v0.02.0" "which is not a version"
refusal "a tag with a suffix" "v0.2.0-rc1" "which is not a version"
refusal "a tag with a counted version" "v0.2.0.7" "is not a named version"
refusal "a tag with a field past its bound" "v0.100.0" "minor field is above 99"
refusal "a tag carrying shell text" 'v0.2.0;echo' "which is not a version"

# The tagged commit carries its own number in VERSION.
MISMATCH="$TMP/mismatch"
ubq_make_fixture_repo "$MISMATCH" || exit 1
echo "0.2.0" > "$MISMATCH/VERSION"
git -C "$MISMATCH" add VERSION
git -C "$MISMATCH" commit --quiet -m "first"
git -C "$MISMATCH" tag -a v0.3.0 -m "0.3.0"
contains "a tag whose commit holds another VERSION is refused" "tagged v0.3.0 and its VERSION file holds '0.2.0'" \
    "$(field "$MISMATCH" version)"

# A malformed VERSION file.
BAD_FILE="$TMP/bad-file"
mkdir -p "$BAD_FILE"
echo "0.2" > "$BAD_FILE/VERSION"
contains "a malformed VERSION is refused" "The VERSION file is '0.2', which is not a version" "$(field "$BAD_FILE" version)"
echo "0.2.0.4" > "$BAD_FILE/VERSION"
contains "a counted VERSION is refused" "it holds a named version" "$(field "$BAD_FILE" version)"

# A build told its version: a container whose mount hides the repository.
check "a stated version replaces the derivation" "0.2.0.41|0.2.0|41|20000041|abcdef01, modified" \
    "$(UBIQUITTY_BUILD_VERSION=0.2.0.41 UBIQUITTY_BUILD_IDENTITY='abcdef01, modified' answers "$ARCHIVE")"
check "a stated version with no identity says where it came from" "stated by the build environment" \
    "$(UBIQUITTY_BUILD_VERSION=0.2.0.41 field "$ARCHIVE" identity)"
contains "a stated version that is not one is refused" "UBIQUITTY_BUILD_VERSION is '0.2.0.100000'" \
    "$(UBIQUITTY_BUILD_VERSION=0.2.0.100000 field "$ARCHIVE" version)"
contains "a stated version with markup is refused" "which is not a version" \
    "$(UBIQUITTY_BUILD_VERSION='0.2.0</string>' field "$ARCHIVE" version)"
contains "a stated identity with markup is refused" "UBIQUITTY_BUILD_IDENTITY is not a build identity" \
    "$(UBIQUITTY_BUILD_VERSION=0.2.0 UBIQUITTY_BUILD_IDENTITY='a<b>&"c' field "$ARCHIVE" version)"
contains "a stated identity past its bound is refused" "UBIQUITTY_BUILD_IDENTITY is not a build identity" \
    "$(UBIQUITTY_BUILD_VERSION=0.2.0 UBIQUITTY_BUILD_IDENTITY="$(printf 'a%.0s' $(seq 1 65))" field "$ARCHIVE" version)"

# The generated header is rewritten only when its text changes.
HEADER="$TMP/generated/UbiquittyBuildVersion.h"
mkdir -p "$TMP/generated"
writeHeader() { # [extra cmake arguments...]
    "$CMAKE" -DUBIQUITTY_VERSION_SOURCE_DIR="$MAIN" -DUBIQUITTY_VERSION_HEADER="$HEADER" "$@" -P "$RULE" 2>&1
}
writeHeader > /dev/null
contains "the header carries the version" 'buildVersion[] = "0.2.1.1";' "$(cat "$HEADER")"
contains "the header carries the identity" "buildIdentity[] = \"$(hash "$MAIN")\";" "$(cat "$HEADER")"
touch -t 200001010000 "$HEADER"
# GNU stat first: its -f reports the file system, so the BSD spelling succeeds
# there with the free-block count in its output, and that count moves.
modified() { stat -c %Y "$1" 2>/dev/null || stat -f %m "$1"; }
STAMPED="$(modified "$HEADER")"
writeHeader > /dev/null
check "an unchanged version leaves the header untouched" "$STAMPED" "$(modified "$HEADER")"
commit "$MAIN" seven
writeHeader > /dev/null
contains "a new commit rewrites it" 'buildVersion[] = "0.2.1.2";' "$(cat "$HEADER")"

# A build directory configured for another version stops and leaves the stamp
# that makes the next build configure first.
STAMP="$TMP/generated/reconfigure.stamp"
CONFIGURED="$TMP/generated/configured-version.txt"
echo "0.2.1.1" > "$CONFIGURED"
output="$(writeHeader -DUBIQUITTY_VERSION_CONFIGURED_FILE="$CONFIGURED" -DUBIQUITTY_VERSION_RECONFIGURE_STAMP="$STAMP")"
contains "a stale configure is refused" "configured as 0.2.1.1 and the tree is now 0.2.1.2" "$output"
check "and the reconfigure stamp is written" "0.2.1.2" "$(cat "$STAMP" 2>/dev/null)"
echo "0.2.1.2" > "$CONFIGURED"
check "a current configure passes" "" "$(writeHeader -DUBIQUITTY_VERSION_CONFIGURED_FILE="$CONFIGURED" -DUBIQUITTY_VERSION_RECONFIGURE_STAMP="$STAMP")"

# The wrapper script prints the same answer.
check "build-version.sh prints the rule's answer" "$(field "$MAIN" version)" "$(UBQ_VERSION_TREE="$MAIN" "$ROOT/Scripts/build-version.sh")"
check "build-version.sh refuses an unknown field" "2" "$("$ROOT/Scripts/build-version.sh" nonsense >/dev/null 2>&1; echo $?)"

report "build_version"
