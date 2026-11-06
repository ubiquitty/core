#!/usr/bin/env bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

#
# check-mutants.sh — would any test notice if this change were wrong?
#
#   ./Scripts/check-mutants.sh --since origin/main    lines this branch changed
#   ./Scripts/check-mutants.sh --staged               lines about to be committed
#   ./Scripts/check-mutants.sh --file F --lines N,M   one specific place
#
# Coverage answers whether a line RAN. It does not answer whether an assertion
# would notice the line being wrong, and those are different questions: a line
# can be executed by every test in the suite and guarded by none of them.
#
# This answers the second one. It changes a line in a way that must alter
# behaviour — a boundary, an equality, a condition, a returned answer — rebuilds,
# and runs the tests. A mutant the suite still passes is a SURVIVOR: the line is
# unguarded, and a real defect there would ship the same way.
#
# Why the DIFF and not the tree. Mutating Core exhaustively takes long enough to
# be a tool nobody runs. Mutating the lines a change touches is quick and
# answers the question actually in front of a reviewer. It is the same ratchet
# every other gate here uses: the backlog is tolerated, the change is not.
#
# Why it is advisory by default. A survivor is a question, not a verdict. Some
# lines are genuinely not worth a test — a diagnostic string, a defensive branch
# that cannot be reached from any input the suite can construct. Reporting those
# as failures teaches the bypass. --block makes it fatal for a caller that wants
# that, and CI can decide separately.
#

set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# Build output lives outside the checkout; see Scripts/build-root.sh.
# shellcheck source=Scripts/build-root.sh
. "$ROOT/Scripts/build-root.sh"
cd "$ROOT" || exit 1

BUILD="${UBQ_MUTANT_BUILD:-$UBQ_BUILD_ROOT/BuildHostTests/ctest}"
MODE=""
REF=""
FILE=""
LINES=""
BLOCK=0
# A cap, because one careless commit should not start a thousand builds. The
# summary says when it truncates: a silent cap reads as "all clear".
LIMIT="${UBQ_MUTANT_LIMIT:-40}"

while [ "$#" -gt 0 ]; do
    case "$1" in
        --since)  MODE=since; REF="${2:-}"; shift 2 ;;
        --staged) MODE=staged; shift ;;
        --file)   MODE="file"; FILE="${2:-}"; shift 2 ;;
        --lines)  LINES="${2:-}"; shift 2 ;;
        --limit)  LIMIT="${2:-}"; shift 2 ;;
        --block)  BLOCK=1; shift ;;
        --help|-h)
            sed -n '/^#!/d; /^[^#]/q; s/^# \{0,1\}//; p' "$0"; exit 0 ;;
        *) echo "check-mutants: unknown argument: $1" >&2; exit 2 ;;
    esac
done

[ -n "$MODE" ] || { echo "check-mutants: need --since, --staged or --file" >&2; exit 2; }
[ -d "$BUILD" ] || { echo "check-mutants: no build directory at $BUILD" >&2; exit 2; }

# Only sources a mutant can be compiled into. A test's own lines are excluded:
# mutating an assertion proves the assertion is carrying weight, which is not the
# question, and it is the question the rest of this script exists to ask about
# the code under test.
#
# The input is `path:line`, so the extension is not at the end of the line. An
# anchor placed straight after it matches nothing, for every change, and the
# caller is told the diff touched no source.
eligible() {
    grep -E '^Source/.*\.(cpp|h|hpp):[0-9]+$' | grep -vE '(^|/)ThirdParty/'
}

# path:line for every line this change touched, from git's own hunk headers.
changed_lines() {
    case "$MODE" in
        staged) git diff --cached -U0 --diff-filter=ACM ;;
        since)  git diff -U0 "$(git merge-base "$REF" HEAD)"...HEAD ;;
    esac | awk '
        /^\+\+\+ b\// { file = substr($0, 7); next }
        /^@@/ {
            split($3, a, ",")
            start = substr(a[1], 2) + 0
            count = (a[2] == "" ? 1 : a[2] + 0)
            for (i = 0; i < count; i++) print file ":" (start + i)
        }'
}

# A file must be restorable before a single byte of it is mutated. This tool
# writes a defect in place and relies on putting it back; if the file was already
# modified there is no safe "back" to put it, and a leftover mutant is
# indistinguishable from source to everything downstream — including a person
# reading the file.
#
# What has to be clean is the WORKING TREE against the index, not the index
# against HEAD. The file is copied and put back, so what a failed restore has to
# be distinguishable from is an unstaged edit — and in --staged mode the staged
# changes are the very thing being asked about, so refusing them refuses the
# mode.
#
# Asked per file rather than over Source/. A modified file this run will not
# touch is not a hazard: nothing is written to it, so nothing has to be put back,
# and refusing over it blocks a legitimate run — mutating one clean file while
# unrelated work sits staged is an ordinary thing to want.
require_restorable() { # <path>
    if ! git diff --quiet -- "$1"; then
        echo "check-mutants: $1 has unstaged changes; refusing to mutate it" >&2
        echo "  A mutant is applied in place and restored afterwards. Starting from a" >&2
        echo "  modified file means a failed restore cannot be told from your own edit." >&2
        echo "  Stage or revert them; staged changes are what --staged reads." >&2
        exit 2
    fi
    if [ "$MODE" != staged ] && ! git diff --cached --quiet -- "$1"; then
        echo "check-mutants: $1 has staged changes not covered by this mode" >&2
        echo "  Run with --staged to mutate exactly those, or commit them first." >&2
        exit 2
    fi
}

WORK="$(mktemp -d)"
# The mutated file must come back, whatever ends the run — a signal, a killed
# terminal, an error. A tool that leaves a defect in a working copy when
# interrupted is worse than no tool: the next reader has no way to know the line
# in front of them is not the source.
UBQ_MUTATED_FILE=""
restore_and_exit() {
    if [ -n "$UBQ_MUTATED_FILE" ] && [ -f "$WORK/original.bak" ]; then
        cp "$WORK/original.bak" "$UBQ_MUTATED_FILE"
        echo "check-mutants: interrupted — restored $UBQ_MUTATED_FILE" >&2
    fi
    rm -rf "$WORK"
    exit 130
}
trap restore_and_exit INT TERM HUP
trap 'if [ -n "$UBQ_MUTATED_FILE" ] && [ -f "$WORK/original.bak" ]; then cp "$WORK/original.bak" "$UBQ_MUTATED_FILE"; fi; rm -rf "$WORK"' EXIT

# ---- collect the mutants ----------------------------------------------------
: > "$WORK/mutants.json"
if [ "$MODE" = file ]; then
    [ -n "$LINES" ] || LINES=all
    require_restorable "$FILE"
    python3 "$ROOT/Scripts/mutate.py" --file "$FILE" --lines "$LINES" > "$WORK/mutants.json"
else
    changed_lines | eligible > "$WORK/changed" || true
    if [ ! -s "$WORK/changed" ]; then
        echo "check-mutants: no eligible source lines changed"
        exit 0
    fi
    # One mutate.py per file, with all of that file's changed lines at once.
    #
    # Split at the LAST colon and compare the path as a string. A path is data
    # here, not a pattern: `grep "^$f:"` reads it as a regular expression, so a
    # filename carrying a metacharacter selects the wrong lines rather than
    # failing — `Source/a[1].h` matches the entry for `Source/a1.h` and the
    # mutants are then applied to another file's line numbers. Splitting at the
    # last colon rather than the first also keeps a path containing one intact.
    : > "$WORK/parts"
    awk '{ i = match($0, /:[0-9]+$/); if (i) print substr($0, 1, i - 1) }' \
        "$WORK/changed" | sort -u > "$WORK/files"
    # Every file checked before any is touched, and read by redirection rather
    # than through a pipe: a `while` on the right of a pipe runs in a subshell,
    # where `exit` from require_restorable would end the subshell and let the
    # run carry on mutating.
    while IFS= read -r f; do
        [ -f "$f" ] || continue
        require_restorable "$f"
    done < "$WORK/files"
    while IFS= read -r f; do
        [ -f "$f" ] || continue
        nums=$(awk -v want="$f" '{
            i = match($0, /:[0-9]+$/)
            if (i && substr($0, 1, i - 1) == want) print substr($0, i + 1)
        }' "$WORK/changed" | paste -sd, -)
        [ -n "$nums" ] || continue
        python3 "$ROOT/Scripts/mutate.py" --file "$f" --lines "$nums" >> "$WORK/parts"
    done < "$WORK/files"
    python3 -c "
import json, sys
out = []
text = open('$WORK/parts', encoding='utf-8').read()
# each mutate.py run emitted its own JSON array; concatenate them
depth = 0; start = None
for i, c in enumerate(text):
    if c == '[':
        if depth == 0: start = i
        depth += 1
    elif c == ']':
        depth -= 1
        if depth == 0:
            out.extend(json.loads(text[start:i+1]))
json.dump(out, sys.stdout)
" > "$WORK/mutants.json"
fi

TOTAL=$(python3 -c "import json;print(len(json.load(open('$WORK/mutants.json'))))")
if [ "$TOTAL" -eq 0 ]; then
    echo "check-mutants: no mutable sites in the changed lines"
    exit 0
fi

TRUNCATED=0
if [ "$TOTAL" -gt "$LIMIT" ]; then
    TRUNCATED=$((TOTAL - LIMIT))
    TOTAL="$LIMIT"
fi

# The suite must pass before any mutant is applied. A tree with a failing test
# scores every mutant as killed, because the run fails either way — the tool
# would report perfect coverage of a change it never actually measured. This is
# the same shape as the tests it exists to find, so it gets the same scepticism.
echo "check-mutants: checking the suite passes unmutated ..."
if ! ninja -C "$BUILD" >/dev/null 2>&1; then
    echo "check-mutants: the tree does not build; nothing to measure" >&2
    exit 2
fi
if ! ctest --test-dir "$BUILD" -j"${UBQ_MUTANT_JOBS:-4}" ${UBQ_MUTANT_CTEST_ARGS:-} > "$WORK/baseline.log" 2>&1; then
    echo "check-mutants: the suite is already failing, so a mutant cannot be scored:" >&2
    grep -E '^\s+[0-9]+ - ' "$WORK/baseline.log" | head -5 >&2
    echo "  Fix that first, or exclude it with ctest -E and UBQ_MUTANT_CTEST_ARGS." >&2
    exit 2
fi

echo "check-mutants: $TOTAL mutant(s) to try, one build and test run each"
[ "$TRUNCATED" -eq 0 ] || echo "  (capped: $TRUNCATED more not tried — raise with --limit)"

# ---- try each one -----------------------------------------------------------
KILLED=0
SURVIVED=0
NOBUILD=0
UNREACHED=0
: > "$WORK/survivors"
: > "$WORK/unreached"

# ----------------------------------------------------------------------
# Which tests can possibly notice a mutant in this file.
#
# A mutant is only killable by a test that executes the mutated file, so every
# other test is a run that could not have said anything.
#
# The relinking is not the cost. One changed Core TU does re-archive and relink
# every consumer, but that is cheap; the suite run is the whole bill, and
# selection removes it.
#
# Indirect calls are covered correctly and this is why the index is coverage and
# not the include graph — llvm-cov records the lines a test EXECUTED, so a test
# that reaches the file three frames down is in the set, and a test that merely
# includes its header is not.
#
INDEX="${UBQ_COVERING_INDEX:-$ROOT/Tests/coverage/covering-tests.tsv}"
SELECT=1
if [ ! -f "$INDEX" ]; then
    SELECT=0
    echo "check-mutants: no covering-test index, so every mutant runs the whole suite."
    echo "  ./Scripts/run-coverage.sh builds one and makes this dramatically faster."
fi
INDEX_COMMIT=""
[ "$SELECT" = "1" ] && INDEX_COMMIT="$(sed -n '1s/^# \([^,]*\).*/\1/p' "$INDEX")"

covering_tests() { # <source path relative to root> -> entries, or the empty set
    local path="$1"
    awk -F'\t' -v want="$path" '$1 == want { print $2; found = 1 }
                                END { exit !found }' "$INDEX" 2>/dev/null || true
}

# An index built before this file existed knows nothing about it, which is not
# the same as knowing no test reaches it. The first must widen to the full
# suite; the second is a finding.
file_predates_index() { # <path> -> 0 if the file existed when the index was built
    [ -n "$INDEX_COMMIT" ] || return 1
    git -C "$ROOT" cat-file -e "$INDEX_COMMIT:$1" 2>/dev/null
}

# Whether the index can still support "no test reaches this line" at all.
#
# A file that existed when the index was built and is named by no row was
# uncovered at that moment. A test added since can cover it now, and the index
# cannot see that test: it distinguishes "no test reaches it" from "no test reached it yet"
# only while no test has arrived. Source/Core/Screen/Addressing.h is the shape —
# present at the header sha, named by no row, and exercised by a test file added
# afterwards — and the verdict it drew was that no test in the suite executes
# its bounds check.
#
# Counted in test files rather than in commits: a documentation burst moves the
# commit count and covers nothing, and a single commit can add a batch. A header
# sha that is not in this clone answers zero from the count and 1 from the
# rev-parse, and the claim is withheld either way — which is right, because an
# index built on a pull request carries that run's merge commit, and no branch
# contains it.
index_can_claim_unreached() {
    [ -n "$INDEX_COMMIT" ] || return 1
    git -C "$ROOT" rev-parse --verify --quiet "$INDEX_COMMIT^{commit}" >/dev/null 2>&1 || return 1
    [ "$INDEX_TESTS_ADDED" -eq 0 ]
}

INDEX_TESTS_ADDED=0
if [ -n "$INDEX_COMMIT" ] && git -C "$ROOT" rev-parse --verify --quiet "$INDEX_COMMIT^{commit}" >/dev/null 2>&1; then
    INDEX_TESTS_ADDED=$(git -C "$ROOT" diff --name-only --diff-filter=A "$INDEX_COMMIT..HEAD" -- Tests 2>/dev/null \
        | grep -cE '/(test|fuzz)_[A-Za-z0-9_]*\.(cpp|sh)$' || true)
fi
if [ "$SELECT" = "1" ] && ! index_can_claim_unreached; then
    echo "check-mutants: the index names $INDEX_TESTS_ADDED test file(s) fewer than the tree has, so a"
    echo "  file it does not name is unknown rather than unreached — those run the whole suite."
    echo "  Adopt a fresher index from a green Coverage floor run's covering-tests-index artifact."
fi

i=0
while [ "$i" -lt "$TOTAL" ]; do
    src=$(python3 -c "
import json;m=json.load(open('$WORK/mutants.json'))[$i];print(m['file'])")
    desc=$(python3 -c "
import json;m=json.load(open('$WORK/mutants.json'))[$i]
print(f\"{m['file']}:{m['line']} {m['operator']}  {m['was']} -> {m['now']}\")")

    # Resolve the covering set before touching the file: a mutant no test can
    # reach is scored without being built at all, which is both the correct
    # verdict and the cheapest one.
    ENTRIES=""
    TARGETS=""
    if [ "$SELECT" = "1" ]; then
        ENTRIES="$(covering_tests "$src")"
        if [ -z "$ENTRIES" ]; then
            if index_can_claim_unreached && file_predates_index "$src"; then
                UNREACHED=$((UNREACHED + 1))
                printf '  unreached      %s\n' "$desc"
                printf '  %s\n' "$desc" >> "$WORK/unreached"
                i=$((i + 1))
                continue
            fi
            # Newer than the index. Nothing is known about it, so assume nothing.
            ENTRIES=""
        else
            # A sharded entry (test_foo.3) is one of several ctest entries over
            # the test_foo binary, and it is the binary that has to be built.
            TARGETS="$(printf '%s\n' $ENTRIES | sed 's/\.[0-9]*$//' | sort -u | tr '\n' ' ')"
        fi
    fi

    cp "$src" "$WORK/original.bak"
    UBQ_MUTATED_FILE="$src"
    python3 - "$WORK/mutants.json" "$i" "$src" <<'PY'
import json, sys
sys.path.insert(0, 'Scripts')
from mutate import apply_mutant
mutants = json.load(open(sys.argv[1], encoding='utf-8'))
m = mutants[int(sys.argv[2])]
text = open(sys.argv[3], encoding='utf-8').read()
open(sys.argv[3], 'w', encoding='utf-8').write(apply_mutant(text, m))
PY

    # The mutant must be in the file before anything is built from it. This
    # tool's central risk is that a change never reaches the binary, and a
    # verdict from a run that compiled the original is worse than no verdict:
    # it reports "your tests caught it" about a test that was never challenged.
    if ! python3 - "$WORK/mutants.json" "$i" "$src" <<'VERIFY'
import json, sys
mutants = json.load(open(sys.argv[1], encoding='utf-8'))
m = mutants[int(sys.argv[2])]
line = open(sys.argv[3], encoding='utf-8').read().split('\n')[m['line'] - 1]
start = m['column']
sys.exit(0 if line[start:start + len(m['now'])] == m['now'] else 1)
VERIFY
    then
        echo "check-mutants: the mutant did not reach $src — aborting rather than scoring it" >&2
        cp "$WORK/original.bak" "$src"
        exit 2
    fi

    # Build with the mutant in place, then put the file back before running the
    # tests. The binary keeps the defect; the source does not need to.
    #
    # This is what bounds the window a mutant can be seen in. Restoring after
    # the tests leaves the file wrong for the whole suite run — minutes — and a
    # trap cannot shorten that, because bash defers a trap until the running
    # child returns and ctest is that child. Restoring here makes the window the
    # length of one compile, and an interrupt during the tests harmless.
    ubqBuilt=0
    # shellcheck disable=SC2086  # TARGETS is a word list, and empty means "all"
    ninja -C "$BUILD" $TARGETS >/dev/null 2>&1 && ubqBuilt=1
    cp "$WORK/original.bak" "$src"
    UBQ_MUTATED_FILE=""

    # ctest's ubq_build fixture is `cmake --build` over the whole tree and every
    # entry requires it, so it runs before the tests — after the restore above,
    # from the original source, over the binary the line above just mutated.
    # The mutant is compiled and then compiled away, and every one scores as a
    # survivor: the gate reported nothing killed for as long as it has existed.
    #
    # Excluded rather than reordered. The restore has to stay before the tests —
    # it bounds the window the file is wrong to one compile, and bash defers a
    # trap until ctest returns — and the fixture has nothing to do here anyway,
    # because the ninja above built exactly the targets this run needs.
    MUTANT_CTEST_ARGS="-FS ubq_build ${UBQ_MUTANT_CTEST_ARGS:-}"

    # Match the covering entries exactly. Anchoring both ends keeps test_foo
    # from also selecting test_foobar, and escaping the dot keeps a shard
    # entry from matching entries that differ in that position.
    FILTER=()
    if [ -n "$ENTRIES" ]; then
        alternation="$(printf '%s\n' $ENTRIES | sed 's/\./\\./g' | paste -sd'|' -)"
        FILTER=(-R "^($alternation)\$")

        # A filter that selects nothing must not be allowed to look like a pass.
        # ctest reports success over an empty selection, so without this the
        # gate would run zero tests, see exit 0, and call every mutant a
        # survivor — the strongest possible claim, made on no evidence.
        selected=$(ctest --test-dir "$BUILD" -N ${FILTER[@]+"${FILTER[@]}"} 2>/dev/null |
            sed -n 's/^Total Tests: //p')
        if [ "${selected:-0}" -eq 0 ]; then
            echo "check-mutants: the covering set for $src selects no ctest entry." >&2
            echo "  The index names: $ENTRIES" >&2
            echo "  It is out of date with this build. Re-run ./Scripts/run-coverage.sh." >&2
            cp "$WORK/original.bak" "$src"
            exit 2
        fi
    fi

    if [ "$ubqBuilt" -eq 0 ]; then
        # A mutant that will not compile proves nothing about the tests.
        NOBUILD=$((NOBUILD + 1))
        printf '  did not build  %s\n' "$desc"
    elif ctest --test-dir "$BUILD" -j"${UBQ_MUTANT_JOBS:-4}" ${FILTER[@]+"${FILTER[@]}"} $MUTANT_CTEST_ARGS >/dev/null 2>&1; then
        SURVIVED=$((SURVIVED + 1))
        printf '  SURVIVED       %s\n' "$desc"
        printf '  %s\n' "$desc" >> "$WORK/survivors"
    else
        KILLED=$((KILLED + 1))
        printf '  killed         %s\n' "$desc"
    fi

    i=$((i + 1))
done

ninja -C "$BUILD" >/dev/null 2>&1 || true

echo
SUMMARY="$KILLED killed, $SURVIVED survived, $NOBUILD did not compile"
[ "$UNREACHED" -gt 0 ] && SUMMARY="$SUMMARY, $UNREACHED unreached"
echo "check-mutants: $SUMMARY"

# An unreached mutant is a stronger result than a survivor and has to be
# reported separately. A survivor says some test ran the line and asserted
# nothing about it; unreached says no test in the suite executes the line at
# all. Folding the two together would let the second hide inside the first.
if [ "$UNREACHED" -gt 0 ]; then
    echo
    echo "No test in the suite executes these lines, so no mutant of them could be killed:"
    cat "$WORK/unreached"
fi

if [ "$SURVIVED" -eq 0 ] && [ "$UNREACHED" -eq 0 ]; then
    if [ "$KILLED" -eq 0 ]; then
        # Nothing survived because nothing was tried. Saying "guarded" here
        # would report the strongest possible result for a run that measured
        # nothing, which is the failure mode this whole tool exists to catch.
        echo "check-mutants: no mutant compiled, so nothing was measured"
        exit 0
    fi
    echo "check-mutants: every changed line is guarded ($KILLED mutant(s) killed)"
    exit 0
fi

if [ "$SURVIVED" -eq 0 ]; then
    cat <<'EXPLAIN'

Nothing survived, but some changed lines are not reached by any test, so the
suite had no opportunity to have an opinion on them. A test that executes the
file is the prerequisite for a test that checks it.
EXPLAIN
    [ "$BLOCK" -eq 1 ] && exit 1
    exit 0
fi

cat <<'EXPLAIN'

A survivor is a line the suite executes and does not check. The change compiles,
behaves differently, and every test still passes — so a real defect there would
ship the same way.

Some survivors are fine, and the two common shapes are worth recognising before
writing a test for one:

  EQUIVALENT — the mutant compiles to different code and cannot behave
  differently. `while (i < n && cell(i))` mutated to `<=` reads one past the
  end, but a bounds-checked accessor returns null there and the loop stops
  anyway. No test can kill it because there is nothing to observe.

  UNREACHABLE — a defensive branch no input the suite can construct will take.

The rest want an assertion that fails when the line is wrong, which is usually
one CHECK away from a test that already covers the file.
EXPLAIN

[ "$BLOCK" -eq 1 ] && exit 1
exit 0
