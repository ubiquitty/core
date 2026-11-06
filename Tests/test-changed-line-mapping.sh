#!/usr/bin/env bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

#
# test-changed-line-mapping.sh — each changed file gets its own line numbers.
#
# check-mutants.sh collects "path:line" pairs from git and then asks, per file,
# which of those lines belong to it. Answering that with a regular expression
# built from the path is wrong in a way that does not announce itself: a
# filename carrying a metacharacter selects ANOTHER file's entries, and the
# mutants are applied to line numbers that belong to a different file. The gate
# still reports killed and survived, from mutations of the wrong lines.
#
# So this drives the real script over a repository holding `a1.h` and `a[1].h`
# and asserts each one is mutated at its own line. The bracket is the case that
# breaks a basic regular expression; `+` is literal in one, which is why the
# tree's eight Objective-C category paths never exposed this.
#

set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$(dirname "$0")/shell-assertions.sh"
. "$(dirname "$0")/git-fixture.sh"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT


# Stubs: nothing is built and nothing is run, so what this observes is purely
# which lines the gate decided to mutate.
mkdir -p "$WORK/bin" "$WORK/build"
printf '#!/usr/bin/env bash\nexit 0\n' > "$WORK/bin/ninja"
cat > "$WORK/bin/ctest" <<'STUB'
#!/usr/bin/env bash
for arg in "$@"; do
    [ "$arg" = "-N" ] && { echo "Total Tests: 1"; exit 0; }
done
exit 0
STUB
chmod +x "$WORK/bin/ninja" "$WORK/bin/ctest"

REPO="$WORK/repo"
mkdir -p "$REPO/Scripts" "$REPO/Source"
cp "$ROOT/Scripts/check-mutants.sh" "$ROOT/Scripts/mutate.py" "$REPO/Scripts/"
ubq_make_fixture_repo "$REPO" || exit 1
git -C "$REPO" commit --quiet --allow-empty -m "empty"

# Two files whose names differ only by the metacharacter. Both carry a mutable
# operator on lines 3 and 4, so whichever line a file is wrongly handed still
# yields a mutant — which is what makes a mix-up observable rather than silent.
printf 'int f()\n{\n    return 1 > 0;\n    return 2 > 1;\n}\n' > "$REPO/Source/a1.h"
printf 'int g()\n{\n    return 3 > 2;\n    return 4 < 3;\n}\n' > "$REPO/Source/a[1].h"
git -C "$REPO" add -A
git -C "$REPO" commit --quiet -m "two files"

# Change line 3 of one and line 4 of the other, leaving the other line mutable
# and unchanged in both. That is what makes a mix-up observable: with the
# mapping broken, each file is handed the other's line number and mutates a line
# it does not own. If the crossed line held no mutable operator, a broken
# mapping would produce no extra mutant and this test would pass either way.
printf 'int f()\n{\n    return 1 >= 0;\n    return 2 > 1;\n}\n' > "$REPO/Source/a1.h"
printf 'int g()\n{\n    return 3 > 2;\n    return 4 <= 3;\n}\n' > "$REPO/Source/a[1].h"
git -C "$REPO" add -A

echo "test-changed-line-mapping: a filename is data, not a pattern"

OUT="$(cd "$REPO" && PATH="$WORK/bin:$PATH" \
    UBQ_MUTANT_BUILD="$WORK/build" \
    UBQ_COVERING_INDEX="$WORK/absent.tsv" \
    ./Scripts/check-mutants.sh --staged 2>&1)"

# Every mutant the gate reports names its file and line. Both files must appear,
# each at the line that actually changed in it.
BRACKET="$(printf '%s\n' "$OUT" | grep -c 'Source/a\[1\]\.h:4' || true)"
PLAIN="$(printf '%s\n' "$OUT" | grep -c 'Source/a1\.h:3' || true)"

check "the bracketed file is mutated at its own line" "1" "$([ "$BRACKET" -ge 1 ] && echo 1 || echo 0)"
check "the plain file is mutated at its own line" "1" "$([ "$PLAIN" -ge 1 ] && echo 1 || echo 0)"

# The corruption, in both directions: each file mutated at the line the other
# one changed. Both crossed lines carry a mutable operator, so a broken mapping
# produces these and a correct one cannot.
CROSSED_PLAIN="$(printf '%s\n' "$OUT" | grep -c 'Source/a1\.h:4' || true)"
CROSSED_BRACKET="$(printf '%s\n' "$OUT" | grep -c 'Source/a\[1\]\.h:3' || true)"
check "the plain file does not take the other's line" "0" "$CROSSED_PLAIN"
check "the bracketed file does not take the other's line" "0" "$CROSSED_BRACKET"

if [ "$FAILURES" -ne 0 ]; then
    echo
    echo "--- what the gate reported ---"
    printf '%s\n' "$OUT" | grep -E 'killed|SURVIVED|unreached|did not build' | head -8
fi

report "test-changed-line-mapping" "each changed file is mutated at its own lines"
