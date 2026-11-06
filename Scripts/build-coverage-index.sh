#!/bin/bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

#
# build-coverage-index.sh — which tests reach which source file.
#
# The merged coverage profile answers "was this line reached". It cannot answer
# "by which test", because merging is what throws that away. This reads the
# per-test profiles instead and writes the second answer down:
#
#   Source/Core/Input/PointerShape.cpp<TAB>test_mouse test_pointer_shape
#
# what it is for. A mutant in a file can only be killed by a test that executes
# that file. Everything else is a compile, a link and a run that could not have
# noticed. With 231 test binaries statically linking Core, "everything else" is
# the entire cost of a mutation run.
#
# FILE GRANULARITY, not line. A line-level index would narrow the set further
# and would be wrong more often: it is keyed by numbers that move under every
# edit, while a file keeps its identity. The file level already turns the whole
# suite into a handful of entries, and it stays true across the edits a
# mutation gate exists to examine.
#
# Usage:
#   ./Scripts/build-coverage-index.sh              build the index
#   ./Scripts/build-coverage-index.sh --check      report staleness, build nothing
#

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
INDEX="$ROOT/Tests/coverage/covering-tests.tsv"
if [ "${1:-}" = "--check" ]; then
    exec python3 "$ROOT/Scripts/coverage_index_freshness.py" --root "$ROOT" --index "$INDEX"
fi

# Build output lives outside the checkout; see Scripts/build-root.sh.
# shellcheck source=Scripts/build-root.sh
. "$ROOT/Scripts/build-root.sh"
OUT="$UBQ_BUILD_ROOT/BuildHostTests/coverage"
BUILD="$OUT/build"
RAW="$BUILD/coverage-raw"
JOBS="${JOBS:-$( ( (sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null) || echo 8 ) )}"
JOBS=$(( JOBS / 2 > 0 ? JOBS / 2 : 1 ))

PROFDATA="$(xcrun --find llvm-profdata 2>/dev/null || echo llvm-profdata)"
LLVMCOV="$(xcrun --find llvm-cov 2>/dev/null || echo llvm-cov)"

if [ ! -d "$RAW" ]; then
    echo "build-coverage-index: no per-test profiles in $RAW" >&2
    echo "  Run ./Scripts/run-coverage.sh first — it produces them." >&2
    exit 1
fi

shopt -s nullglob
profiles=( "$RAW"/*.profraw )
if [ ${#profiles[@]} -eq 0 ]; then
    echo "build-coverage-index: $RAW exists but holds no .profraw" >&2
    exit 1
fi

# Group the profiles by the ctest entry that wrote them. A test that forks left
# one file per process and all of them belong to that entry.
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

for p in "${profiles[@]}"; do
    base="$(basename "$p")"
    entry="${base%%--*}"
    if [ "$entry" = "$base" ]; then
        # No separator: written by something that did not go through ctest's
        # per-test environment. Attributing it to a test would be a guess.
        continue
    fi
    printf '%s\n' "$p" >> "$WORK/entry.$entry"
done

entries=( "$WORK"/entry.* )
if [ ${#entries[@]} -eq 0 ]; then
    echo "build-coverage-index: none of the ${#profiles[@]} profiles carry a test name." >&2
    echo "  These are PID-only names from a build before per-test attribution." >&2
    echo "  Re-run ./Scripts/run-coverage.sh to regenerate them." >&2
    exit 1
fi

echo "build-coverage-index: ${#entries[@]} test(s), ${#profiles[@]} profile(s)"

# One test's file set: merge its profiles, ask llvm-cov which files it touched.
# --summary-only keeps this to per-file totals; the full export is per-line and
# an order of magnitude more work for an answer this does not use.
#
# A shell entry's binaries live outside $BUILD/Tests: run-tests.sh compiles
# them under $BUILD/shell/<stanza>/ and fills the two shared Host roots. Only
# binaries carrying a coverage map are passed — run-tests.sh sets the knob
# explicitly both ways, so a clean binary from a normal run filters itself out.
covmap_objects_under() {
    local root="$1" candidate
    [ -d "$root" ] || return 0
    while IFS= read -r candidate; do
        if command -v otool >/dev/null 2>&1; then
            otool -l "$candidate" 2>/dev/null | grep -q __llvm_covmap && printf '%s\n' "$candidate"
        else
            llvm-objdump -h "$candidate" 2>/dev/null | grep -q covmap && printf '%s\n' "$candidate"
        fi
    done < <(find "$root" -type f -perm -111 ! -name '*.a' ! -name '*.o' ! -name '*.sh' ! -name '*.py' 2>/dev/null)
}
one_test() {
    local list="$1" entry name binary
    entry="$(basename "$list")"
    name="${entry#entry.}"
    local -a objects=()
    case "$name" in
    shell_*)
        local candidate
        while IFS= read -r candidate; do objects+=(-object "$candidate"); done < <(
            covmap_objects_under "$BUILD/shell/${name#shell_}"
            covmap_objects_under "$UBQ_BUILD_ROOT/host-tests"
            covmap_objects_under "$UBQ_BUILD_ROOT/host-quic"
        )
        [ ${#objects[@]} -gt 0 ] || return 0
        ;;
    *)
        # A shard entry (test_foo.3) runs the test_foo binary.
        binary="$BUILD/Tests/${name%%.*}"
        [ -x "$binary" ] || return 0
        objects=(-object "$binary")
        ;;
    esac

    local -a files=()
    while IFS= read -r f; do files+=("$f"); done < "$list"
    "$PROFDATA" merge -sparse -o "$WORK/$name.profdata" "${files[@]}" 2>/dev/null || return 0
    "$LLVMCOV" export "${objects[@]}" -instr-profile "$WORK/$name.profdata" \
        --summary-only -format=text 2>/dev/null \
    | python3 -c '
import json, sys
try:
    data = json.load(sys.stdin)
except ValueError:
    sys.exit(0)
name = sys.argv[1]
for export in data.get("data", []):
    for f in export.get("files", []):
        if f.get("summary", {}).get("lines", {}).get("covered", 0) > 0:
            print(f["filename"] + "\t" + name)
' "$name"
}
export -f one_test covmap_objects_under
export BUILD WORK PROFDATA LLVMCOV ROOT

printf '%s\n' "${entries[@]}" \
    | xargs -P "$JOBS" -I{} bash -c 'one_test "$@"' _ {} \
    > "$WORK/pairs.tsv"

if [ ! -s "$WORK/pairs.tsv" ]; then
    echo "build-coverage-index: every test exported an empty file set." >&2
    echo "  The profiles exist but llvm-cov read no covered lines from them —" >&2
    echo "  check that the build under $BUILD was configured with -DUBIQUITTY_COVERAGE=ON." >&2
    exit 1
fi

# Absolute paths from llvm-cov, repo-relative in the index: the consumer holds a
# path from a diff, and a diff never speaks in absolute paths.
cat > "$WORK/fold.py" <<'PY'
import collections, sys

root = sys.argv[1].rstrip('/') + '/'
by_file = collections.defaultdict(set)
with open(sys.argv[2], encoding='utf-8') as pairs:
    for line in pairs:
        path, _, test = line.rstrip('\n').partition('\t')
        if not test:
            continue
        if path.startswith(root):
            path = path[len(root):]
        # Tests/ belongs in the index as much as Source/ does. A test file is
        # executed by the binary it compiles into, and a shared helper by every
        # binary that links it — llvm-cov already reports both, so the pairs are
        # here and dropping them is what makes a test-only change unanswerable.
        # Anything else llvm-cov names is a toolchain or SDK path outside the
        # tree, which no diff can hand back.
        if not path.startswith(('Source/', 'Tests/')):
            continue
        by_file[path].add(test)
for path in sorted(by_file):
    print(path + '\t' + ' '.join(sorted(by_file[path])))
PY

# Written only when the mapping changed, and touched otherwise. The header
# carries the commit, so rewriting unconditionally dirties a TRACKED file on
# every coverage run even when no test's reach moved — which shows up as a
# rebase that refuses and a diff whose only line is a hash nothing reads. Every
# consumer skips `#`, so the commit is provenance for a human: it means the tree
# at which this mapping was established, which is the more useful of the two
# readings anyway.
#
# The touch is not decoration. coverage-gaps.py judges staleness by this file's
# mtime against the sources, so leaving it alone when the content matches would
# latch that refusal on forever — a regeneration that changes nothing has to
# still say it ran.
{
    echo "# $(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown), ${#entries[@]} tests"
    python3 "$WORK/fold.py" "$ROOT" "$WORK/pairs.tsv"
} > "$WORK/index.new"

# The greps are redirected as well as the diff: diff -q stops at the first
# difference and closes both readers, so a grep killed mid-write is the expected
# path here and its "write error: Broken pipe" is not a finding.
if [ -f "$INDEX" ] && diff -q <(grep -v '^#' "$INDEX" 2>/dev/null) <(grep -v '^#' "$WORK/index.new" 2>/dev/null) >/dev/null 2>&1; then
    touch "$INDEX"
else
    mv "$WORK/index.new" "$INDEX"
fi

covered=$(grep -vc '^#' "$INDEX" || true)
echo "build-coverage-index: $covered source file(s) -> $INDEX"
