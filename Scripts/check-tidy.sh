#!/usr/bin/env bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

# check-tidy.sh — the tree's clang-tidy rules, as a gate and as a fixer.
#
# A hand-typed `clang-tidy -checks=…` selects its own rules, so two audits of one
# file reach different lists and neither is the project's. The list lives in
# .clang-tidy at the repo root; this runs that list.
#
#   Scripts/check-tidy.sh                  gate the whole tree
#   Scripts/check-tidy.sh Source/Core      gate a subtree
#   Scripts/check-tidy.sh --since REF      gate what changed against REF
#   Scripts/check-tidy.sh --fix            apply every fixit, then report
#   Scripts/check-tidy.sh --only CHECK     one enabled rule, for a burn-down
#   Scripts/check-tidy.sh --fix Source/VT  apply within a subtree
#   Scripts/check-tidy.sh --list           print the enabled checks and exit
#
# --since exists because the whole tree is slow to tidy, and a gate that
# expensive gets wired nowhere. A branch's own translation units are quick.
#
# The trap is that a changed header contains no translation unit. Filter the
# file list to what the diff touched and a header-only change compiles nothing,
# finds nothing, and prints the same words as a clean run. So a changed header
# pulls in the translation units that include it, found textually — this reaches
# direct includers, not transitive ones, and the count it could not place is
# printed rather than assumed to be zero.
#
# `clang-tidy --fix` run over several TUs at once corrupts headers. A diagnostic
# in a header is reported by every translation unit that includes it, so one
# header edit is emitted once per includer: run those concurrently and the same
# `[[nodiscard]]` is inserted twice, the same namespace concatenated twice.
# run-clang-tidy exports replacements and clang-apply-replacements merges
# identical ones before writing.
#
# The compile DATABASE decides what is checked, and no single one covers the
# tree: build/macos holds the macOS and portable TUs, build/linux the GTK ones.
# A run against one is silent about the other's files rather than failing, so the
# database used and the number of unreached files are part of the verdict. A gate
# that skips half the tree prints what a clean tree prints.

set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

if [ ! -f "$ROOT/.clang-tidy" ]; then
    echo "check-tidy.sh: REFUSED — missing $ROOT/.clang-tidy; restore the project's lint configuration" >&2
    exit 2
fi

# shellcheck source=Scripts/gate-report-lib.sh
source "$ROOT/Scripts/gate-report-lib.sh"
# Where this checkout builds; the compile databases below live under it.
# shellcheck source=Scripts/build-root.sh
. "$ROOT/Scripts/build-root.sh"

# The enabled checks, by ENTRY SHAPE rather than by category prefix. A prefix list
# is accurate until .clang-tidy enables a category it does not name -- bugprone,
# performance, readability -- and then it undercounts in silence, so a verdict
# reading "1 of 18 rules" claims a denominator the file disagrees with. `-*` and
# any other negation start with a dash and are not enabled checks.
enabledChecks() {
    sed -n '/^Checks:/,/^$/p' "$ROOT/.clang-tidy" \
        | sed 's/#.*//' \
        | grep -oE '^[[:space:]]+[a-z][a-z0-9]*-[a-z0-9-]+' \
        | sed 's/^[[:space:]]*//' \
        | sort -u
}

GATE="check-tidy"
FIX=0
SCOPE=""

SINCE=""
ONLY=""

while [ "$#" -gt 0 ]; do
    case "$1" in
        --fix)     FIX=1 ;;
        --since)   SINCE="${2:-}"; shift
                   [ -n "$SINCE" ] || { echo "check-tidy.sh: --since needs a git ref" >&2; exit 2; } ;;
        --only)    ONLY="${2:-}"; shift
                   [ -n "$ONLY" ] || { echo "check-tidy.sh: --only needs a check name" >&2; exit 2; }
                   # From the project's list, never beside it. A burn-down needs one
                   # rule at a time to stay reviewable, but a name this file does not
                   # enable would let a caller run a check the tree never agreed to
                   # and read the result as the project's verdict.
                   if ! sed -n '/^Checks:/,/^$/p' "$ROOT/.clang-tidy" | grep -q -- "$ONLY"; then
                       echo "check-tidy.sh: --only $ONLY is not enabled in .clang-tidy (see --list)" >&2
                       exit 2
                   fi ;;
        --list)    enabledChecks; exit 0 ;;
        --rule-count) enabledChecks | grep -c .; exit 0 ;;
        -h|--help) sed -n '5,38p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        -*)        echo "check-tidy.sh: unknown flag: $1" >&2; exit 2 ;;
        *)         SCOPE="$1" ;;
    esac
    shift
done

# Homebrew keeps LLVM off PATH so it does not shadow Apple's clang. The tools
# are still the ones to use: `run-clang-tidy` ships with clang-tidy, not with
# Xcode, and Xcode ships neither.
# Ask the cheap question before doing the expensive work.
#
# Every compile database in the tree is merged, and the toolchain probed around
# it, before this script reaches the point where it discovers the branch changed
# no compilable file — three hundred lines after a git diff could have said so.
# On a branch touching only documentation or shell that is time spent to report
# nothing, and every checkout pays it on every push.
#
# The predicate here is weaker than the one it short-circuits, on purpose: it
# asks whether the diff holds any file with a compilable extension. A file that
# passes it and turns out to be unreachable from any database is still caught by
# the full selection below, which runs unchanged. What it cannot do is skip a
# file the real selection would have judged — a superset of the extensions is a
# superset of the files.
if [ -n "$SINCE" ] && git rev-parse --verify --quiet "$SINCE" >/dev/null; then
    # Drain the names: quiet grep can SIGPIPE git under pipefail after a match.
    if ! git diff --name-only "$SINCE...HEAD" 2>/dev/null \
            | grep -E '\.(c|cc|cpp|cxx|m|mm|h|hh|hpp|hxx)$' >/dev/null; then
        gate_ok "$GATE" "no compilable file changed since $SINCE"
        exit 0
    fi
fi

LLVM_BIN="$(brew --prefix llvm 2>/dev/null || echo /usr/local)/bin"
TIDY="$LLVM_BIN/clang-tidy"
RUNNER="$LLVM_BIN/run-clang-tidy"
APPLY="$LLVM_BIN/clang-apply-replacements"
command -v "$TIDY" >/dev/null 2>&1 || TIDY="$(command -v clang-tidy || true)"

# What an unrunnable gate does depends on who asked. Run by hand, "it is not
# installed" is the answer and a failure is right. Wired into check-style, the
# same failure makes the aggregate unrunnable on a tree nobody has built yet,
# and the fix people reach for is to stop running the aggregate. So --since
# reports the reason and stands down — loudly, because a gate that skips
# quietly is one whose green means nothing.
standDown() {
    if [ -n "$SINCE" ]; then
        echo "check-tidy: SKIPPED — $1" >&2
        exit 0
    fi
    gate_failed "$GATE" "$1"
    exit 1
}

if [ -z "$TIDY" ] || [ ! -x "$TIDY" ]; then
    standDown "clang-tidy is not installed (brew install llvm)"
fi

# Every database, merged, rather than the first one found. No single
# configuration compiles the tree: build/macos holds the macOS and portable TUs,
# build/linux the GTK ones, BuildHostTests/ctest the only ones that compile
# Tests/. Taking the first match would read Source/ and report a clean verdict
# over the half it can see, with Tests/ never checked at all.
#
# First writer of a path wins, so a TU compiled by two frontends is judged under
# the flags of whichever appears first here rather than twice under different
# ones. macOS leads because it is the configuration with the most files.
WORK_DB="$(mktemp -d -t check-tidy-db.XXXXXX)"
DB_SOURCES=""
DB_ABSENT=""
#
# The GTK database is at build/linux-container/linux, not build/linux: the
# container gets its own build directory because a CMake cache records the
# absolute path it was configured at, and one tree bind-mounted into two
# environments would otherwise have two caches fighting over one. Nobody builds
# the GTK frontend natively on a macOS host, so build/linux does not exist here,
# and looking there alone leaves the Linux sources unchecked — a database this
# gate can read, one directory deeper.
#
# build/web is the wasm configuration, and it carries more than the four Web
# sources: it compiles the whole portable Core, so it is the only database
# holding many Core translation units.
# build/web is LAST because first-writer-wins: it compiles the whole portable
# Core under emscripten Release flags, so placing it earlier re-judges
# translation units another configuration already covers, and a diagnostic that
# fires under one set of flags and not the other silently stops being reported.
# Last, it contributes only the units nothing else compiles.
# DISCOVERED, not listed. A hand-written candidate list is a hypothesis about
# which builds exist, and it drifts: a build directory present on disk and
# absent from the list leaves its translation units unlinted, and the unreached
# note calls them "absent from every database" when they sit in one the gate
# did not open.
#
# The known roots come first so the merge order stays deterministic: first writer
# of a path wins, and macOS is the configuration with the most files. Anything
# else found is appended rather than ignored.
#
# The known roots are also reported when ABSENT, because discovery can only see
# what a checkout has built. A database exists where somebody ran the script that
# writes it and nowhere else, so a scan is as wide as its build history and the
# verdict reads the same at any width. Enumerating Core's compilers by hand has
# reached three different answers; the wasm arm compiles 165 of Core's 166
# translation units, more than any other single database, and a tree without it
# is scanned by whatever the rest happen to cover.
# The script that writes each known database. Both the absent path and the
# unparseable one name it, because a reader who has to act on either needs the
# command rather than the directory.
producerFor() {
    # Matched on the tail rather than the whole path: these arrive under
    # $UBQ_BUILD_ROOT, whose leading directories differ per checkout, and a case
    # spelled against the checkout answers "an unrecorded script" for every one
    # of them — advice that names no script at all.
    case "$1" in
        */macos) echo "Scripts/build-macos.sh" ;;
        */linux) echo "Scripts/run-linux.sh" ;; # also the container tree, which ends the same
        */core) echo "Scripts/build-core-snapshot.sh" ;;
        */BuildHostTests/ctest) echo "Scripts/build-tests.sh" ;;
        */web) echo "Scripts/build-web.sh" ;;
        *) echo "an unrecorded script" ;;
    esac
}

# The build output lives under $UBQ_BUILD_ROOT, so a list spelled against the
# checkout finds no database and this gate stands down on every run.
for candidate in "$UBQ_BUILD_ROOT/macos" "$UBQ_BUILD_ROOT/linux" "$UBQ_BUILD_ROOT/linux-container/linux" "$UBQ_BUILD_ROOT/core" "$UBQ_BUILD_ROOT/BuildHostTests/ctest" "$UBQ_BUILD_ROOT/web"; do
    if [ -f "$candidate/compile_commands.json" ]; then
        DB_SOURCES="$DB_SOURCES $candidate"
    else
        DB_ABSENT="$DB_ABSENT $candidate|$(producerFor "$candidate")"
    fi
done
while IFS= read -r discovered; do
    case " $DB_SOURCES " in
        *" $discovered "*) ;;
        *) DB_SOURCES="$DB_SOURCES $discovered" ;;
    esac
done < <(find "$UBQ_BUILD_ROOT" -maxdepth 3 -name compile_commands.json 2>/dev/null | sed 's|/compile_commands\.json$||' | sort)
if [ -z "$DB_SOURCES" ]; then
    standDown "no compile database (run Scripts/build-macos.sh or build-tests.sh first)"
fi
# `|| MERGE_STATUS=$?` rather than a bare call and `$?` on the next line: under
# `set -e` a non-zero exit here kills the script before anything reads the
# status, so the run that most needs the notes below is the one that prints
# nothing at all and exits 2.
MERGE_STATUS=0
python3 - "$WORK_DB" $DB_SOURCES <<'PY' || MERGE_STATUS=$?
import json, os, re, sys

# A precompiled header is built by the compiler that will consume it, and
# Homebrew's clang-tidy is not that compiler: it rejects Apple clang's PCH with
# "uses an older format that is no longer supported" and abandons the whole
# translation unit. Every Tests/ TU dies there, so the directory reported zero
# findings — and zero findings from a TU that never parsed is indistinguishable
# from a clean one. Dropping the flag costs parse time and nothing else.
PCH = re.compile(r'\s+-(?:include-pch|Xclang\s+-include-pch)\s+\S+|\s+-Winvalid-pch\b')

# A configure killed part-way leaves a truncated or zero-byte database, and
# json.load answers that with a traceback naming the standard library. The
# directory that would not parse is the one fact the reader needs, and it was
# the one thing the traceback did not carry. Skipping rather than dying also
# keeps the other databases' coverage: no single one compiles every translation
# unit, so a run over five of six still reaches files only the sixth carries.
out, seen, merged, unparseable = sys.argv[1], set(), [], []
for directory in sys.argv[2:]:
    path = os.path.join(directory, "compile_commands.json")
    try:
        with open(path) as handle:
            entries = json.load(handle)
    except (json.JSONDecodeError, OSError, UnicodeDecodeError) as error:
        unparseable.append("%s\t%s" % (directory, error))
        continue
    for entry in entries:
        if entry["file"] in seen:
            continue
        seen.add(entry["file"])
        if "command" in entry:
            entry["command"] = PCH.sub("", entry["command"])
        if "arguments" in entry:
            args, skip = [], False
            for argument in entry["arguments"]:
                if skip:
                    skip = False
                    continue
                if argument in ("-include-pch",):
                    skip = True
                    continue
                args.append(argument)
            entry["arguments"] = args
        merged.append(entry)
json.dump(merged, open(os.path.join(out, "compile_commands.json"), "w"))
if unparseable:
    with open(os.path.join(out, "unparseable.txt"), "w") as handle:
        handle.write("\n".join(unparseable) + "\n")
# Every database unreadable is not the same verdict as some of them being
# unreadable, and it is not "no database" either: the caller built something,
# and what it built cannot be read.
sys.exit(2 if not merged else 0)
PY
if [ -f "$WORK_DB/unparseable.txt" ]; then
    SKIPPED_DBS=""
    while IFS="$(printf '\t')" read -r directory reason; do
        [ -n "$directory" ] || continue
        echo "  note: $directory/compile_commands.json did not parse and was skipped — $reason" >&2
        echo "        rebuild it with $(producerFor "$directory"), or remove $directory." >&2
        SKIPPED_DBS="$SKIPPED_DBS $directory"
    done < "$WORK_DB/unparseable.txt"
    KEPT_DBS=""
    for directory in $DB_SOURCES; do
        case " $SKIPPED_DBS " in
            *" $directory "*) ;;
            *) KEPT_DBS="$KEPT_DBS $directory" ;;
        esac
    done
    # The label is what the verdict claims it scanned. Leaving a skipped database
    # in it would make the run report coverage it did not have, which is the
    # failure this gate exists to catch one level down.
    DB_SOURCES="$KEPT_DBS"
fi
if [ "$MERGE_STATUS" -ne 0 ]; then
    standDown "every compile database found was unreadable (see the notes above)"
fi
DB="$WORK_DB"
DB_LABEL="$(echo "$DB_SOURCES" | sed 's/^ //;s/ /, /g')"

# Printed beside every verdict, including the early ones. "No compilable file
# changed" is exactly when a reader needs the list: it can mean the change
# touched nothing, or that it touched only what an absent database compiles, and
# the sentence reads the same either way.
reportAbsentDatabases()
{
    [ -n "$DB_ABSENT" ] || return 0
    echo "  Not scanned here — no database, so nothing they alone compile was read:"
    for absent in $DB_ABSENT; do
        printf '    %-30s run %s\n' "${absent%%|*}" "${absent#*|}"
    done
}

# The --since selection: which translation units the branch's diff can reach.
# Empty means every one the database lists.
SELECT=""
UNPLACED_HEADERS=""
if [ -n "$SINCE" ]; then
    if ! git rev-parse --verify --quiet "$SINCE" >/dev/null; then
        # No base to diff against, as in a shallow clone. Saying so beats
        # checking the whole tree under a flag that asked for a subset, and
        # beats reporting a clean subset of nothing.
        echo "check-tidy: --since $SINCE not found; skipping (no base to diff against)"
        exit 0
    fi
    SELECT="$WORK_DB/select.txt"
    UNPLACED_HEADERS=$(python3 - "$SINCE" "$SELECT" <<'PY'
import os, re, subprocess, sys

ref, out = sys.argv[1], sys.argv[2]
sourceSuffixes = (".c", ".cc", ".cpp", ".m", ".mm")

def git(*arguments):
    return subprocess.run(["git", *arguments], capture_output=True, text=True).stdout.split("\n")

changed = [p for p in git("diff", "--name-only", f"{ref}...HEAD")
           if p and "ThirdParty" not in p]
units = {p for p in changed if p.endswith(sourceSuffixes)}
headers = [p for p in changed if p.endswith((".h", ".hpp", ".inc"))]

# A header is checked through whoever compiles it. Matching the basename inside
# an include directive over-selects when two directories share a file name, and
# over-selecting costs time rather than correctness — the wrong direction to err
# in is the cheap-and-silent one.
unplaced = 0
for header in headers:
    # POSIX classes, not \s: git grep's engine takes an extended regex, where \s
    # is not a metacharacter. It matched nothing and reported no includers —
    # a wrong answer in the direction that checks less and still says ok.
    pattern = (r'#[[:space:]]*(include|import)[[:space:]]*[<"][^">]*'
               + re.escape(os.path.basename(header)) + r'[">]')
    includers = [p for p in git("grep", "-lE", pattern, "--",
                                *(f"*{suffix}" for suffix in sourceSuffixes))
                 if p and "ThirdParty" not in p]
    if includers:
        units.update(includers)
    else:
        unplaced += 1

# The denominator. Selecting direct includers is not the same as covering a
# header's consumers, and the gap is not a rounding error: Base/Types.h is
# included directly by 3 files and reached by 519, so a verdict over 2 of them
# reads exactly like a verdict over the tree. Printing what was compiled beside
# what could have been turns "ok" into a claim a reader can size.
#
# A number, not a threshold. A ratio that failed a header change would fail
# nearly every header change, and a gate that always fires is one people route
# around -- the number sizes the claim, the findings decide the verdict.
reaching = 0
if headers:
    tracked = [p for p in git("ls-files") if p and "ThirdParty" not in p]
    compilable = sourceSuffixes + (".h", ".hpp", ".inc")
    bySuffix = {}
    for path in tracked:
        parts = path.split("/")
        for cut in range(len(parts)):
            bySuffix.setdefault("/".join(parts[cut:]), path)
    includedBy = {}
    includePattern = re.compile(r'^[ \t]*#[ \t]*(?:include|import)[ \t]*[<"]([^">]+)[">]', re.M)
    for path in tracked:
        if not path.endswith(compilable):
            continue
        try:
            text = open(path, errors="replace").read()
        except OSError:
            continue
        for target in includePattern.findall(text):
            resolved = bySuffix.get(target)
            if resolved:
                includedBy.setdefault(resolved, set()).add(path)
    # Seeded with the changed TUs as well as the changed headers, so this counts
    # the same population the selection is drawn from. Closing over headers alone
    # made the ratio's two halves different subjects: a branch touching a header
    # and an unrelated .cpp put that .cpp in the numerator and outside the
    # denominator, and the result was never absurd enough to notice.
    frontier = list(headers) + list(units)
    seen = set(headers) | set(units)
    while frontier:
        for consumer in includedBy.get(frontier.pop(), ()):
            if consumer not in seen:
                seen.add(consumer)
                frontier.append(consumer)
    reaching = sum(1 for p in seen if p.endswith(sourceSuffixes))

with open(out, "w") as handle:
    handle.write("\n".join(sorted(units)))
print(f"{unplaced} {reaching}")
PY
)
    REACHING="${UNPLACED_HEADERS#* }"
    UNPLACED_HEADERS="${UNPLACED_HEADERS%% *}"
fi

# .swift entries sit in the same database and clang-tidy cannot consume them: it
# reports "no input files" as a clang-diagnostic-error, which lands in the
# results looking like a compile failure in the tree.
# A read loop rather than `mapfile`, which is bash 4 and macOS ships 3.2.
FILES=()
while IFS= read -r translationUnit; do
    [ -n "$translationUnit" ] && FILES+=("$translationUnit")
done < <(python3 - "$DB" "$SCOPE" "$SELECT" <<'PY'
import json, os, subprocess, sys
db, scope, select = sys.argv[1], sys.argv[2], sys.argv[3]
root = os.getcwd()
selected = None
if select:
    with open(select) as handle:
        selected = {line for line in handle.read().split("\n") if line}
# Tracked files only. A compile database lists whatever the build compiled,
# which includes sources a generator writes into a build directory — and a
# finding in one of those is a finding nobody can act on: the file says it is
# regenerated at build time, and the next build rewrites the fix away.
# Excluding the build directories by name instead would key the rule on their
# spelling, and there are eight of them under names that do not share a prefix.
tracked = set(subprocess.run(["git", "ls-files"],
                             capture_output=True, text=True).stdout.split("\n"))
for entry in json.load(open(os.path.join(db, "compile_commands.json"))):
    path = entry["file"]
    if not path.endswith((".c", ".cc", ".cpp", ".m", ".mm")):
        continue
    relative = os.path.relpath(path, root)
    if relative.startswith(".."):
        continue                      # compiled from outside the tree
    if "ThirdParty" in relative or relative not in tracked:
        continue
    if scope and not relative.startswith(scope):
        continue
    if selected is not None and relative not in selected:
        continue
    print(relative)
PY
)

if [ "${#FILES[@]}" -eq 0 ]; then
    if [ -n "$SINCE" ]; then
        # A branch that touches no compilable file is clean here, and a gate that
        # failed on it would be one every documentation change learned to skip.
        if [ "$UNPLACED_HEADERS" != "0" ]; then
            echo "check-tidy: $UNPLACED_HEADERS changed header(s) have no includer in any database; nothing to check." >&2
        fi
        gate_ok "$GATE" "no compilable file changed since $SINCE"
        reportAbsentDatabases
        exit 0
    fi
    gate_failed "$GATE" "no translation unit matched${SCOPE:+ under $SCOPE}"
    exit 1
fi

# Tracked sources the chosen database does not list. A clean verdict over a
# subset and a clean verdict over the tree print the same words, so the count
# goes in the output.
UNREACHED=$(python3 - "$DB" "$SCOPE" <<'PY'
import json, os, subprocess, sys
db, scope = sys.argv[1], sys.argv[2]
root = os.getcwd()
covered = set()
for entry in json.load(open(os.path.join(db, "compile_commands.json"))):
    covered.add(os.path.relpath(entry["file"], root))
tracked = subprocess.run(["git", "ls-files", "Source", "Tests"],
                         capture_output=True, text=True).stdout.split()
missing = [f for f in tracked
           if f.endswith((".c", ".cc", ".cpp", ".m", ".mm"))
           and "ThirdParty" not in f
           and (not scope or f.startswith(scope))
           and f not in covered]
# The count alone does not say what went unscanned, and the answer is usually
# actionable: a cluster is either a frontend nobody has built in this worktree,
# or a set no CMake target compiles at all. Tests/Host is the second kind --
# run-tests.sh compiles it from a stanza, which produces no database ever.
import collections
byDirectory = collections.Counter(os.path.dirname(f) for f in missing)
print(len(missing))
for directory, count in byDirectory.most_common(4):
    print(f"{count}\t{directory}")
PY
)
UNREACHED_SHAPE="$(printf '%s\n' "$UNREACHED" | tail -n +2)"
UNREACHED="$(printf '%s\n' "$UNREACHED" | head -1)"

WORK="$(mktemp -d -t check-tidy.XXXXXX)"
# One trap: a second on EXIT REPLACES the first, so registering them separately
# leaked whichever was registered earlier.
trap 'rm -rf "$WORK" "$WORK_DB"' EXIT

# Apple clang accepts noescape on the spans imported by Swift. Upstream clang
# can analyze their C++ while ignoring that lifetime annotation, but -Werror
# would stop it. Keep the diagnostic enabled in .clang-tidy and demote it only
# for analysis; changed warnings still fail and production flags are untouched.
ANALYSIS_ARGUMENTS=(-extra-arg=-Wno-error=ignored-attributes)

if [ "$FIX" = "1" ]; then
    if [ ! -x "$RUNNER" ] || [ ! -x "$APPLY" ]; then
        gate_failed "$GATE" "--fix needs run-clang-tidy and clang-apply-replacements (brew install llvm)"
        exit 1
    fi
    echo "check-tidy: applying fixes over ${#FILES[@]} TU(s) via $DB_LABEL …"
    # A DIRECTORY, not a file. Given a single output file run-clang-tidy needs
    # PyYAML to merge into it and raises without it; a directory is what
    # clang-apply-replacements consumes anyway, so the dependency does not arise.
    # The raise must not be swallowed: it exits before writing anything, so a
    # discarded error applies nothing and still reports success.
    FIXES="$WORK/fixes"
    mkdir -p "$FIXES"
    if ! "$RUNNER" -p "$DB" -quiet "${ANALYSIS_ARGUMENTS[@]}" ${ONLY:+-checks=-*,$ONLY} -export-fixes "$FIXES" "${FILES[@]}" > "$WORK/run.log" 2>&1; then
        if grep -q "RuntimeError\|Traceback" "$WORK/run.log"; then
            tail -5 "$WORK/run.log" >&2
            gate_failed "$GATE" "run-clang-tidy could not export fixes"
            exit 1
        fi
    fi
    # A check can diagnose without offering a fixit — modernize-avoid-c-arrays
    # emits none at all. Reporting "applied" over an empty export is then
    # indistinguishable from a tree that had nothing to fix.
    EXPORTED=$(find "$FIXES" -name '*.yaml' -size +0 2>/dev/null | wc -l | tr -d ' ')
    if [ "$EXPORTED" = "0" ]; then
        echo "check-tidy: no fixits to apply (a check can diagnose without offering one)."
        exit 0
    fi
    "$APPLY" -format -style=file "$FIXES" 2>/dev/null || "$APPLY" "$FIXES"
    echo "check-tidy: fixes from $EXPORTED translation unit(s) applied; re-run without --fix to see what remains."
    exit 0
fi

TIDY_STATUS=0
printf '%s\n' "${FILES[@]}" | xargs -P "$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)" -n 1 \
    "$TIDY" -p "$DB" --quiet "${ANALYSIS_ARGUMENTS[@]}" ${ONLY:+-checks=-*,$ONLY} \
    > "$WORK/out.txt" 2> "$WORK/errors.txt" || TIDY_STATUS=$?

# A frontend error anywhere in a TU makes its analysis incomplete, including
# an unchanged header or a tool that printed an error with a zero exit status.
# These failures cannot pass through the changed-line warning ratchet below.
python3 - "$WORK/out.txt" "$WORK/errors.txt" > "$WORK/failures.txt" <<'PY'
import re, sys

pattern = re.compile(r"^(?:.*:\d+(?::\d+)?:\s+|(?:clang-tidy|clang):\s+)?(?:fatal )?error:|^LLVM ERROR:")
failures = set()
for path in sys.argv[1:]:
    with open(path, errors="replace") as handle:
        failures.update(line.rstrip("\n") for line in handle if pattern.search(line))
for failure in sorted(failures):
    print(failure)
PY
if [ "$TIDY_STATUS" -ne 0 ] || [ -s "$WORK/failures.txt" ]; then
    cat "$WORK/failures.txt" >&2
    if [ ! -s "$WORK/failures.txt" ]; then
        cat "$WORK/errors.txt" >&2
    fi
    gate_failed "$GATE" "analysis incomplete (frontend errors or analyzer runner exit $TIDY_STATUS)"
    exit 1
fi

# One site is one (file, line, column, check) — not one diagnostic. A header
# reached by 200 TUs reports its one defect 200 times, and a gate that counts
# emissions reports a number that tracks the include graph rather than the tree.
python3 - "$WORK/out.txt" > "$WORK/report.txt" <<'PY'
import os, re, sys
sites = {}
pattern = re.compile(r"^(/[^:]+):(\d+):(\d+): warning: (.*?) \[([a-z0-9-]+)\]$")
root = os.getcwd()
for line in open(sys.argv[1], errors="replace"):
    match = pattern.match(line.rstrip("\n"))
    if not match:
        continue
    path, lineNumber, column, message, check = match.groups()
    relative = os.path.relpath(path, root)
    if relative.startswith("..") or "ThirdParty" in relative or ".tables.inc" in relative:
        continue
    sites[(relative, lineNumber, column, check)] = message
for (relative, lineNumber, _, check), message in sorted(sites.items()):
    print(f"{relative}:{lineNumber}\t{check}\t{message}")
PY

# The ratchet. Selecting the branch's translation units decides what is
# compiled, not what is reported, and the two differ by every pre-existing
# finding in the headers those units include. Touch one Core file and the
# tree's whole backlog arrives attributed to your branch.
#
# So a finding counts only where the diff put a line. This is the same rule
# check-comments and check-acronyms apply, and it is what lets the gate be wired
# while a backlog exists: what you wrote is yours, what you compiled is not.
if [ -n "$SINCE" ]; then
    git diff -U0 "$SINCE...HEAD" > "$WORK/diff.txt"
    python3 - "$WORK/report.txt" "$WORK/diff.txt" > "$WORK/ratcheted.txt" <<'PY'
import re, sys

report, diff = sys.argv[1], sys.argv[2]
added, path = {}, None
for line in open(diff, errors="replace"):
    header = re.match(r"^\+\+\+ b/(.+)$", line.rstrip("\n"))
    if header:
        path = header.group(1)
        continue
    hunk = re.match(r"^@@ .*?\+(\d+)(?:,(\d+))?", line)
    if hunk and path:
        start, count = int(hunk.group(1)), int(hunk.group(2) or 1)
        added.setdefault(path, set()).update(range(start, start + count))

for line in open(report, errors="replace"):
    location = line.split("\t", 1)[0]
    file, _, lineNumber = location.rpartition(":")
    if lineNumber.isdigit() and int(lineNumber) in added.get(file, ()):
        sys.stdout.write(line)
PY
    mv "$WORK/ratcheted.txt" "$WORK/report.txt"
fi

COUNT=$(wc -l < "$WORK/report.txt" | tr -d ' ')

# What the database could not see is reported on both paths. Emitting it only on
# success meant a failing run showed a count and never said the count was over a
# subset — and every run that fails is a run someone is reading closely.
reportUnreached() {
    # In --since mode the tree-wide count answers a question nobody asked: what
    # went unchecked is the changed headers no translation unit includes, and
    # printing the other number instead would read as coverage of the branch.
    if [ -n "$SINCE" ]; then
        [ "${UNPLACED_HEADERS:-0}" = "0" ] && return 0
        echo "  note: $UNPLACED_HEADERS changed header(s) have no includer in any database and were not checked." >&2
        return 0
    fi
    [ "$UNREACHED" = "0" ] && return 0
    echo "  note: $UNREACHED tracked source file(s) are absent from every database and were not checked." >&2
    if [ -n "${UNREACHED_SHAPE:-}" ]; then
        printf '%s\n' "$UNREACHED_SHAPE" | awk -F'\t' 'NF==2 {printf "    %-42s %s file(s)\n", $2, $1}' >&2
    fi
    echo "  A cluster under Tests/ is compiled by a run-tests.sh stanza, which produces no" >&2
    echo "  database at any time. One under Source/ is a frontend not configured here, and" >&2
    echo "  cmake writes the database when it configures rather than when it compiles, so" >&2
    echo "  Scripts/build-*.sh widens the scan the moment its cmake step returns -- the" >&2
    echo "  build itself need not finish." >&2
}

if [ "$COUNT" != "0" ]; then
    while IFS=$'\t' read -r location check message; do
        gate_finding "$location" "$check" "$message"
    done < <(head -40 "$WORK/report.txt")
    if [ "$COUNT" -gt 40 ]; then
        echo "  … and $((COUNT - 40)) more; Scripts/check-tidy.sh --fix applies every one that has a fixit." >&2
        # Which rules, and how many each. A bare total says how much is wrong
        # and nothing about what kind, so the reader cannot tell a mechanical
        # sweep from a design question without running the whole thing again.
        echo "  by check:" >&2
        cut -f2 "$WORK/report.txt" | sort | uniq -c | sort -rn | sed 's/^/    /' >&2
    fi
    reportUnreached
    gate_failed_count "$GATE" "$COUNT" "clang-tidy site(s)${ONLY:+ under $ONLY alone}; run Scripts/check-tidy.sh --fix"
    exit 1
fi

reportUnreached
# `grep -c` exits 1 on no match, and under `set -e` a failing substitution ends
# the script before its verdict -- a run with no verdict line, which reads as
# nothing having gone wrong. A .clang-tidy that lost its Checks block would do it.
RULE_TOTAL=$(enabledChecks | grep -c . || true)
RULE_TOTAL="${RULE_TOTAL:-0}"
RULE_SCOPE=""
[ -n "$ONLY" ] && RULE_SCOPE=" under 1 of $RULE_TOTAL rules ($ONLY)"
# `-ge`, not `-gt`. With both halves over one population the numerator cannot
# exceed the denominator, so an equal count is the ordinary case rather than a
# reason to fall back to a form that states no denominator at all.
if [ -n "$SINCE" ] && [ "${REACHING:-0}" -ge "${#FILES[@]}" ] && [ "${REACHING:-0}" -gt 0 ]; then
    gate_ok "$GATE" "${#FILES[@]} of $REACHING translation unit(s) the change reaches, clean via $DB_LABEL$RULE_SCOPE"
else
    gate_ok "$GATE" "${#FILES[@]} translation unit(s) clean via $DB_LABEL${SINCE:+ (changed since $SINCE)}$RULE_SCOPE"
fi
reportAbsentDatabases
