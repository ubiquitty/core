#!/bin/bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0
#
# check-workflows.sh — lint .github/workflows/*.yml with a pinned actionlint.
#
# A workflow file that YAML accepts and GitHub rejects reaches main as a run
# with zero jobs and the message "This run likely failed because of a workflow
# file issue". No YAML parser sees it: the rule that breaks the file is
# GitHub's expression-context rule (a job's env: cannot read `runner`, which
# exists only once a runner does), not YAML's. actionlint checks expression
# contexts, action inputs against the pinned action's schema, and the shape of
# each run: step.
#
#   Scripts/check-workflows.sh              # lint every workflow file
#   Scripts/check-workflows.sh --self-test  # the rules against a planted file
#
# The binary is pinned by version and by the sha256 of its release archive,
# as the workflows pin their actions by commit. Resolution order:
#   1. $UBQ_ACTIONLINT — an explicit path, for a machine that keeps its own.
#      A path that is not executable is a fault, not a stand-down: an operator
#      who named one asked for that binary.
#   2. $UBQ_BUILD_ROOT/tools/actionlint-<version>/actionlint — the copy an
#      earlier run of this script downloaded and verified.
#   3. A download of the pinned release into that cache, verified before it
#      runs. The CI image carries the same version under /usr/local/bin, so a
#      battery inside it takes the first branch and fetches nothing.
# A copy found on PATH is not consulted: an unpinned version applies a
# different rule set than the one the tree was checked against.
#
# A machine that has no copy and cannot fetch one stands down, the way every
# gate here stands down for a tool it does not have — the absence is a fact
# about the machine, not about the tree. CI carries the binary in its image,
# so the lint runs there whatever a developer's machine has.
#
# The shellcheck and pyflakes integrations are off. actionlint runs them only
# where the binaries happen to be installed, so with them on the verdict
# would depend on the machine rather than on the tree.
#
# Two rules are ignored, and the reason for each lives here rather than in a
# configuration file the gate could drift from:
#   - `runner.workspace` is a real context property the workflows rely on for
#     their cache paths; actionlint's schema of the runner context omits it.
#   - `if: ${{ false }}` is how a job is parked with its wiring intact; the
#     rule's advice is to delete the condition, which would run the job.
# Both are spelled narrowly, and --self-test proves it: a different missing
# property and a different constant condition are still reported.
set -euo pipefail
SCRIPTS="$(cd "$(dirname "$0")" && pwd)"
# The subject is the tree git reports for the directory the gate runs in, which is
# not this script's own when it runs as a library's copy.
ROOT="$(git rev-parse --show-toplevel 2>/dev/null || (cd "$(dirname "$0")/.." && pwd))"
# shellcheck source=Scripts/build-root.sh
. "$SCRIPTS/build-root.sh"

ACTIONLINT_VERSION=1.7.12
case "$(uname -s)-$(uname -m)" in
    Darwin-arm64)
        ARCHIVE="actionlint_${ACTIONLINT_VERSION}_darwin_arm64.tar.gz"
        SHA256=aba9ced2dee8d27fecca3dc7feb1a7f9a52caefa1eb46f3271ea66b6e0e6953f ;;
    Darwin-x86_64)
        ARCHIVE="actionlint_${ACTIONLINT_VERSION}_darwin_amd64.tar.gz"
        SHA256=5b44c3bc2255115c9b69e30efc0fecdf498fdb63c5d58e17084fd5f16324c644 ;;
    Linux-x86_64)
        ARCHIVE="actionlint_${ACTIONLINT_VERSION}_linux_amd64.tar.gz"
        SHA256=8aca8db96f1b94770f1b0d72b6dddcb1ebb8123cb3712530b08cc387b349a3d8 ;;
    Linux-aarch64|Linux-arm64)
        ARCHIVE="actionlint_${ACTIONLINT_VERSION}_linux_arm64.tar.gz"
        SHA256=325e971b6ba9bfa504672e29be93c24981eeb1c07576d730e9f7c8805afff0c6 ;;
    *)
        ARCHIVE=""
        SHA256="" ;;
esac

# The two suppressions, in one place so the lint and the self-test cannot
# disagree about what is suppressed.
IGNORES=(
    -ignore 'property "workspace" is not defined in object type'
    -ignore 'constant expression "false" in condition'
)

standDown() {
    echo "check-workflows: SKIPPED — $1"
    exit 0
}

sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | cut -d' ' -f1
    else
        shasum -a 256 "$1" | cut -d' ' -f1
    fi
}

# Prints the binary's path, or nothing when this machine has no copy and
# cannot fetch one. A named-but-unusable UBQ_ACTIONLINT exits non-zero.
resolveActionlint() {
    if [ -n "${UBQ_ACTIONLINT:-}" ]; then
        if [ ! -x "$UBQ_ACTIONLINT" ]; then
            echo "check-workflows: UBQ_ACTIONLINT=$UBQ_ACTIONLINT is not executable" >&2
            exit 1
        fi
        printf '%s' "$UBQ_ACTIONLINT"
        return 0
    fi
    [ -n "$ARCHIVE" ] || return 0
    local cache="$UBQ_BUILD_ROOT/tools/actionlint-$ACTIONLINT_VERSION"
    local binary="$cache/actionlint"
    if [ -x "$binary" ]; then
        printf '%s' "$binary"
        return 0
    fi
    command -v curl >/dev/null 2>&1 || return 0
    mkdir -p "$cache"
    local archive="$cache/$ARCHIVE"
    local url="https://github.com/rhysd/actionlint/releases/download/v${ACTIONLINT_VERSION}/${ARCHIVE}"
    curl -fsSL --retry 2 --max-time 120 -o "$archive" "$url" 2>/dev/null || { rm -f "$archive"; return 0; }
    local digest
    digest="$(sha256_of "$archive")"
    if [ "$digest" != "$SHA256" ]; then
        rm -f "$archive"
        echo "check-workflows: $ARCHIVE has sha256 $digest, pinned $SHA256; refusing to run it" >&2
        exit 1
    fi
    tar -xzf "$archive" -C "$cache" actionlint
    rm -f "$archive"
    [ -x "$binary" ] || { echo "check-workflows: $ARCHIVE held no actionlint binary" >&2; exit 1; }
    printf '%s' "$binary"
}

ACTIONLINT="$(resolveActionlint)"
if [ -z "$ACTIONLINT" ]; then
    if [ -z "$ARCHIVE" ]; then
        standDown "no pinned actionlint for $(uname -s)-$(uname -m); set UBQ_ACTIONLINT to an ${ACTIONLINT_VERSION} binary"
    fi
    standDown "no actionlint ${ACTIONLINT_VERSION} here and it could not be fetched; the CI image carries it, and UBQ_ACTIONLINT names a local copy"
fi
found="$("$ACTIONLINT" -version | head -1)"
if [ "$found" != "$ACTIONLINT_VERSION" ]; then
    echo "check-workflows: $ACTIONLINT is actionlint $found, the gate pins $ACTIONLINT_VERSION" >&2
    exit 1
fi

# Every finding actionlint reports over one directory of workflow files, with
# the suppressions applied. Exits 0 whether or not it found anything, so the
# caller decides what a finding means.
lint() {
    "$ACTIONLINT" -shellcheck= -pyflakes= "${IGNORES[@]}" "$@" 2>&1 || true
}

if [ "${1:-}" = "--self-test" ]; then
    # Spelled with the X's rather than `-t <prefix>`: BSD mktemp appends the
    # random suffix to a bare prefix, and GNU mktemp refuses one ("too few X's
    # in template"). This gate runs on both.
    fixture="$(mktemp -d "${TMPDIR:-/tmp}/check-workflows-fixture.XXXXXX")"
    trap 'rm -rf "$fixture"' EXIT
    mkdir -p "$fixture/.github/workflows"

    # The defect this gate exists for: `runner` read by a job's env:, which is
    # evaluated before a runner exists. GitHub rejects the whole file for it
    # and reports a run with no jobs.
    cat > "$fixture/.github/workflows/planted.yml" <<'PLANTED'
name: planted
on: [push]
jobs:
  build:
    runs-on: ubuntu-24.04
    env:
      CACHE: ${{ runner.temp }}/cache
    steps:
      - run: echo "$CACHE"
PLANTED
    # A file with nothing wrong, so a rule that reports everything fails here
    # rather than passing as vigilance.
    cat > "$fixture/.github/workflows/clean.yml" <<'CLEAN'
name: clean
on: [push]
jobs:
  build:
    runs-on: ubuntu-24.04
    steps:
      - run: echo "${{ runner.temp }}"
CLEAN
    # The two suppressions, each beside the finding it must not swallow: a
    # different absent property and a different constant condition.
    cat > "$fixture/.github/workflows/suppressed.yml" <<'SUPPRESSED'
name: suppressed
on: [push]
jobs:
  parked:
    if: ${{ false }}
    runs-on: ubuntu-24.04
    steps:
      - run: echo "${{ runner.workspace }}"
  reported:
    if: ${{ true }}
    runs-on: ubuntu-24.04
    steps:
      - run: echo "${{ runner.nonesuch }}"
SUPPRESSED

    status=0
    planted="$(lint "$fixture/.github/workflows/planted.yml")"
    grep -q 'property "temp" is not defined\|runner' <<< "$planted" \
        || { echo "check-workflows --self-test: a job-level env: reading the runner context produced no finding" >&2; status=1; }
    clean="$(lint "$fixture/.github/workflows/clean.yml")"
    [ -z "$clean" ] \
        || { echo "check-workflows --self-test: a clean workflow was reported: $clean" >&2; status=1; }
    suppressed="$(lint "$fixture/.github/workflows/suppressed.yml")"
    grep -q '"workspace" is not defined' <<< "$suppressed" \
        && { echo "check-workflows --self-test: the runner.workspace suppression stopped working" >&2; status=1; }
    grep -q 'constant expression "false"' <<< "$suppressed" \
        && { echo "check-workflows --self-test: the parked-job suppression stopped working" >&2; status=1; }
    grep -q '"nonesuch" is not defined' <<< "$suppressed" \
        || { echo "check-workflows --self-test: the runner.workspace suppression swallowed another absent property" >&2; status=1; }
    grep -q 'constant expression "true"' <<< "$suppressed" \
        || { echo "check-workflows --self-test: the parked-job suppression swallowed another constant condition" >&2; status=1; }
    [ "$status" = 0 ] || exit 1
    echo "check-workflows --self-test: ok (4 case(s): the planted context defect is caught, a clean file is not reported, and each suppression is narrow enough to leave its neighbour standing)"
    exit 0
fi

shopt -s nullglob
files=("$ROOT"/.github/workflows/*.yml "$ROOT"/.github/workflows/*.yaml)
if [ ${#files[@]} -eq 0 ]; then
    echo "check-workflows: no workflow files under .github/workflows" >&2
    exit 1
fi
findings="$(lint "${files[@]}")"
if [ -n "$findings" ]; then
    printf '%s\n' "$findings" >&2
    echo "check-workflows: FAILED — ${#files[@]} workflow file(s), findings above" >&2
    exit 1
fi
echo "check-workflows: ok (${#files[@]} workflow file(s) pass actionlint $ACTIONLINT_VERSION)"
