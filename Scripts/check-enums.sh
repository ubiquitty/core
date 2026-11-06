#!/usr/bin/env bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

# Scripts/check-enums.sh — WebKit bool/enum naming gate (companion to the
# acronym gate check-acronyms.py). Surfaces three WebKit-style smells:
#
#   - k-prefixed constants (kSomething)  → prefer an unprefixed `enum class` for
#     a related set, or a plain `constexpr` for a lone scalar. The k-prefix is a
#     Chromium convention, not WebKit. Only DEFINITIONS (`kXxx = …`) are flagged,
#     never the (many) uses, so the report stays one hint per constant.
#   - boolean function parameters (`(bool x`, `, bool y`)  → a descriptive enum,
#     even for two cases, so call sites read as intent not `true`/`false`.
#   - boolean NAMES that do not read as an assertion  → the predicate LEADS
#     (isVisible/hasFocus/shouldWrap/didLoad/canScroll) or it is a boolean
#     adjective (valid/empty/active). A subject-led `xIsY` (clientIsAgent) is not
#     the WebKit form — lead with is/has (isAgentClient).
#
# --staged / --since REF BLOCK newly-added violations (the ratchet, added lines
# only — mirrors check-comments.sh). The full-tree scan is ADVISORY — the
# existing backlog, chipped by the boy-scout rule, not a tree-wide block.
# Annotate a case the rule should not flag: // bool-ok, or /* bool-ok */ where a line
# comment cannot go — inside a multi-line macro a // swallows the backslash
# that continues it, so the block form is the only one available there.
#
# Usage (same surface as check-acronyms.py):
#   Scripts/check-enums.sh              # all tracked source — ADVISORY
#   Scripts/check-enums.sh --staged     # staged ADDED lines (pre-commit BLOCK)
#   Scripts/check-enums.sh --since REF  # ADDED lines vs REF (CI BLOCK)
#   Scripts/check-enums.sh file ...     # specific files
#   Scripts/check-enums.sh --self-test  # the rules against a planted fixture
#
# Exit 0 = clean, 1 = hints found, 2 = usage error.
#
# Wired into Scripts/hooks/pre-commit (--staged) and Scripts/check-style.py
# (--since origin/main, blocks the PR diff).

set -euo pipefail

# The subject is the tree git reports for the directory the gate runs in, which is
# not this script's own when it runs as a library's copy.
ROOT="$(git rev-parse --show-toplevel 2>/dev/null || (cd "$(dirname "$0")/.." && pwd))"

get_files() {
    case "${1:-}" in
        --staged)
            git -C "$ROOT" diff --cached --name-only --diff-filter=ACM \
                | grep -E '\.(h|cp|mm|cpp|m|swift)$' | grep -vE '(^|/)ThirdParty/' || true
            ;;
        "")
            git -C "$ROOT" ls-files \
                | grep -E '\.(h|cp|mm|cpp|m|swift)$' | grep -vE '(^|/)ThirdParty/' || true
            ;;
        *)
            printf '%s\n' "$@" | grep -E '\.(h|cp|mm|cpp|m|swift)$' | grep -vE '(^|/)ThirdParty/' || true
            ;;
    esac
}

PERL_PROG='
use strict;
# Apple/vendor k-prefixed framework symbols we cannot rename.
my $APPLE = qr/^k(?:CF|CA|CG|CT|CV|CM|CN|Sec|UT|IO|Audio|MIDI|LS|MD|TIS|AX|VK)/;
# A boolean name reads as an assertion when it LEADS with a predicate (is/has/
# should/…) or is a boolean adjective (valid/empty/active). Shared by the C++ and
# Swift bool-naming rules so both stay in lockstep.
my $PREDICATE_LEAD = qr/^(?:is|are|was|were|has|have|had|should|would|will|did|do|does|can|could|may|might|must|need|needs|want|wants|use|uses|used|allow|allows|allowed|enable|enables|enabled|disable|disables|disabled|require|requires|support|supports|contain|contains|include|includes|exclude|excludes|match|matches|prefer|prefers|expect|expects|accept|accepts|handle|handles|own|owns|know|knows|wrap|wraps|draw|draws|show|shows|hide|hides|skip|skips|force|forces|omit|omits|keep|keeps|treat|treats|send|sends|make|makes|got|saw|found|done|ok|valid|invalid|empty|nonempty|emptied|ready|active|inactive|visible|hidden|enabled|disabled|present|absent|dirty|clean|pending|busy|idle|open|opened|closed|running|paused|stopped|dead|alive|connected|selected|focused|checked|pressed|passed|failed|approx|first|last|only|same|equal)(?:[A-Z0-9_]|$)/;

# Every file in one run, taken as (path, display path) pairs. One pair is what
# --staged and --since pass and behaves exactly as a single file did: its hit
# count decides the exit status. The whole-tree scan passes all of them, because
# a process per file meant this program was compiled 717 times to read 717 files,
# and that compile was most of what the scan cost.
#
# The count of files with hits comes back through a file named in the
# environment rather than through the argument list, so the two callers that
# pass one pair need no change at all.
my $countFile = $ENV{UBQ_ENUMS_COUNT_FILE};
my $filesWithHits = 0;
while (@ARGV) {
my ($path, $rel) = (shift, shift);
open(my $fh, "<", $path) or die "cannot open $path: $!";
my $n = 0;
my $hits = 0;
while (<$fh>) {
    $n++;
    next if /^\s*\/\//;            # full-line comment
    next if /^\s*\*[^\/]/;         # block-comment body
    chomp(my $line = $_);
    $line =~ s|//.*$||;            # strip inline comment tail
    # A string literal is data, not a declaration: a terminfo entry spells its
    # capabilities `kUP6=\E[1;6A` and a `k` followed by a capital and `=` inside
    # quotes is that entry, not a k-prefixed constant.
    $line =~ s/"(?:[^"\\]|\\.)*"/""/g;
    next if $line =~ /^\s*#\s*(?:include|ifdef|ifndef|endif|pragma|if\b|else|elif)/;

    my @matches;
    # $rel is the real filename (the display path); $path may be a temp file in
    # --staged mode, so the language is keyed off $rel, not $path.
    my $isSwift = ($rel =~ /\.swift$/);

    # k-prefixed constant DEFINITIONS (k + Capital, defined with `=`). Uses are
    # left alone so each constant is flagged once, at its declaration. Skipped for
    # Swift — the k-prefix is a Carbon/ObjC artifact, N/A to Swift-native code.
    while ($line =~ /\b(k[A-Z][A-Za-z0-9_]*)\s*=/g) {
        my $name = $1;
        next if $isSwift || $name =~ $APPLE;
        push @matches, "k-prefixed constant `$name` -> prefer an unprefixed enum class (related set) or constexpr (scalar); WebKit avoids the k-prefix";
        last;
    }

    # g-prefixed global variable DEFINITIONS (Hungarian-notation globals). A
    # mutable global is reassigned and read throughout the file with no type in
    # front of it (`gFoo = value;`, `if (gFoo == x)`, `return *gFoo;`) but
    # declared exactly once with a TYPE token leading it at statement start
    # (`Type gFoo = value;` / `Type* gFoo;`) — so the declaration match anchors
    # at the trimmed line start, requires a type-shaped token (letters/`::`/
    # template brackets/pointer-or-reference) before the name, and a single `=`
    # (not `==`) or a bare `;` after it. A handful of C++ keywords
    # (return/if/while/for/else/switch) are letters too and would otherwise look
    # like a type; excluded explicitly since they lead a USE, never a
    # declaration. Skipped for Swift — file-scope `var`/`let` globals are a
    # different, already-covered naming surface (the Swift bool-property rule
    # below), not this g-prefix one.
    if (!$isSwift
        && $line =~ /^\s*(?:static\s+|inline\s+|volatile\s+|const\s+|extern\s+)*
                      (?!return\b|if\b|while\b|for\b|else\b|switch\b|case\b)
                      [A-Za-z_][\w:<>,\s]*[\s*&]+
                      (g[A-Z][A-Za-z0-9_]*)
                      \s*(?:=(?!=)|;)/x) {
        my $name = $1;
        push @matches, "g-prefixed global `$name` -> spell out the ownership/scope instead of Hungarian notation; a global belongs in the module-map process-global register regardless of name";
    }

    # boolean parameters: `(bool name` or `, bool name`. Spell the choice as an
    # enum so call sites read as intent, not a bare true/false. A regex cannot
    # tell a behaviour FLAG (the trap) from a function that legitimately carries
    # boolean DATA, so three non-trap shapes are skipped to cut noise without
    # hiding real traps:
    #   - an inline `// bool-ok` / `// NOLINT(bool)` suppression on the line,
    #   - a function that operates on boolean data (its name contains "bool":
    #     tomlBool, boolean, boolOr, …),
    #   - a lambda parameter (a local predicate, not an API surface).
    # Everything else is still hinted — annotate a data param with
    # `// bool-ok: <reason>` to silence it.
    if ($line =~ /[(,]\s*bool\s+\w+/) {
        my $suppressed   = ($_    =~ m{(?://|/\*)\s*(?:bool-ok|allow-bool|NOLINT\(bool)});
        my $bool_data_fn = ($line =~ /[A-Za-z_]*[Bb]ool[A-Za-z_]*\s*\(/);
        my $lambda_param = ($line =~ /\][^(]*\(\s*[^)]*\bbool\s+\w/);
        unless ($suppressed || $bool_data_fn || $lambda_param) {
            push @matches, "boolean parameter -> prefer a descriptive enum, even for two cases (no-boolean-arguments)";
        }
    }

    # Boolean NAMING (WebKit): a bool reads as an assertion — the predicate LEADS
    # (isVisible / hasFocus / shouldWrap / didLoad / canScroll), or it is a boolean
    # adjective (valid / empty / active). A subject-led name (clientIsAgent,
    # foregroundIsShell) is not the WebKit form: the is/has must LEAD (isAgentClient).
    # Flags a bool decl / member / return / param whose name does not lead with a
    # predicate; `// bool-ok` silences a line whose annotation carries the reason,
    # `operator` is skipped.
    while ($line =~ /\bbool\s+(?:const\s+)?(?:m_|s_|g_)?([a-z]\w*)(\s*\(?)/g) {
        my ($nm, $after) = ($1, $2);
        # A bool-returning FUNCTION (name followed by "(") is not checked:
        # both a predicate query (isVisible()) and an action-returns-success verb
        # (tryLock(), reserveWindowForSpawn()) are valid, and a regex cannot tell
        # them apart — this rule governs bool variables / members, not functions.
        next if $after =~ /\(/;
        last if $nm eq "operator";
        next if $nm =~ $PREDICATE_LEAD;
        next if ($_ =~ m{(?://|/\*)\s*(?:bool-ok|allow-bool|NOLINT\(bool)});
        push @matches, "boolean `$nm` -> lead with a predicate (is/has/should/did/can/…) or a boolean adjective — WebKit boolean naming (a subject-led `xIsY` should read `isY`)";
        last;
    }

    # --- Swift bool/enum rules (-cxx-interop Swift-native code) --------------
    # The C rules above key off `bool`/`(bool`/`k...=` C syntax that never matches
    # Swift; this matches the Swift NAMING shape so the predicate-led-name gate
    # covers Swift too (Swift API Design Guidelines). The no-boolean-ARGUMENTS
    # gate is not applied to Swift: a Swift argument label names the
    # value at every call site (findMatches(query, isRegex: true), and Apple ships
    # animated: Bool), so a labeled Bool parameter is idiomatic, not a trap.
    if ($isSwift) {
        # A stored/computed Bool property: `var/let name: Bool` reads as an
        # assertion — the predicate must LEAD (isVisible, hasFocus). An array
        # `[Bool]` / dictionary value is DATA, not a flag, so the `:` must be
        # immediately followed by `Bool` (no `[`).
        while ($line =~ /\b(?:var|let)\s+([a-z]\w*)\s*:\s*Bool\b/g) {
            my $nm = $1;
            next if $nm =~ $PREDICATE_LEAD;
            next if ($_ =~ m{//\s*(?:bool-ok|allow-bool)});
            push @matches, "Swift boolean `$nm` -> lead with a predicate (is/has/should/…) — Swift API Design Guidelines: boolean members read as assertions";
            last;
        }
    }

    for my $rule (@matches) {
        print "$rel:$n: enum-hint: $rule\n  > $line\n";
        $hits++;
    }
}
close($fh);
$filesWithHits++ if $hits > 0;
}
if ($countFile) {
    open(my $out, ">", $countFile) or die "cannot write $countFile: $!";
    print $out "$filesWithHits\n";
    close($out);
}
exit($filesWithHits > 0 ? 1 : 0);
'


# The names a file already declared, so the ratchet owns what this change ADDED.
#
# A bool is its NAME, not the text of the line declaring it. A line edited for a
# reason that leaves the name alone -- a parameter type respelled by a rename, a
# field carried into a moved file -- adds no bool, and a ratchet that reads the
# changed LINE as a new declaration demands a rename of something that was
# already there. A module rename touches every `<field>Set` companion's line
# without moving its spelling.
#
# Two ways a file's history is not at its own path. A rename git can pair, given
# both paths in one diff. A path REUSED for different content it cannot: when
# one header is deleted and another moved onto its name, git reports one
# modified file with a whole-file diff and the pre-image knows none of the
# fields.
#
# So the pre-image is the file's own history at its path, PLUS every file this
# change removed. A name the change deleted from one place and added in another
# was moved, not written. Loose in the safe direction: this can only exempt, so a
# name it over-collects costs a report the ratchet would otherwise have made,
# while a name it misses costs nothing at all.
preexistingNames() { # <--staged | REF> <file> -> the pre-change text on stdout
    local mode="$1" file="$2" origin="" gone=""
    if [[ "$mode" == "--staged" ]]; then
        origin="$(git -C "$ROOT" diff --cached --name-status -M --diff-filter=R \
                  | awk -v t="$file" '$1 ~ /^R/ && $3 == t { print $2; exit }')"
        [[ -n "$origin" ]] || origin="$file"
        git -C "$ROOT" show "HEAD:$origin" 2>/dev/null || true
        gone="$(git -C "$ROOT" diff --cached --name-status -M --diff-filter=DR \
                | awk '{ print $2 }')"
        while IFS= read -r removed; do
            [[ -z "$removed" ]] && continue
            git -C "$ROOT" show "HEAD:$removed" 2>/dev/null || true
        done <<< "$gone"
    else
        origin="$(git -C "$ROOT" diff --name-status -M --diff-filter=R "$mode...HEAD" \
                  | awk -v t="$file" '$1 ~ /^R/ && $3 == t { print $2; exit }')"
        [[ -n "$origin" ]] || origin="$file"
        git -C "$ROOT" show "$mode:$origin" 2>/dev/null || true
        gone="$(git -C "$ROOT" diff --name-status -M --diff-filter=DR "$mode...HEAD" \
                | awk '{ print $2 }')"
        while IFS= read -r removed; do
            [[ -z "$removed" ]] && continue
            git -C "$ROOT" show "$mode:$removed" 2>/dev/null || true
        done <<< "$gone"
    fi
}

# Drop a finding whose name the file already carried. Findings arrive on stdin.
#
# A finding is TWO lines: the verdict, then the source line quoted under `  > `.
# Dropping only the first leaves the quoted line orphaned, which prints as a
# violation with no verdict and still fails the gate -- so the decision made on a
# verdict line carries to the quoted lines under it.
dropPreexisting() { # <path to the pre-change text>
    local before="$1" finding name pending="" quoted dropping=0
    flush() { [[ -n "$pending" ]] && printf '%s\n' "$pending"; pending=""; }
    while IFS= read -r finding; do
        [[ -z "$finding" ]] && continue
        if [[ "$finding" == "  > "* ]]; then
            # A verdict dropped by name takes its quoted lines with it, or the
            # quote prints alone: a violation with no reason, still failing.
            [[ "$dropping" == 1 ]] && continue
            # A verdict held back is one whose text names no identifier -- the
            # bool-PARAMETER rule reports a signature, not a name -- so the
            # quoted line decides: a line the pre-image already carried was not
            # added by this change.
            if [[ -n "$pending" ]]; then
                quoted="${finding#  > }"
                if grep -qF -- "$quoted" "$before"; then
                    pending=""
                    continue
                fi
                flush
            fi
            printf '%s\n' "$finding"
            continue
        fi
        flush
        dropping=0
        # The FIRST backticked span names the offender. The sentence after it
        # quotes `xIsY` and `isY` as examples, and a greedy match takes those.
        name="$(sed -n 's/^[^`]*`\([A-Za-z_][A-Za-z0-9_]*\)`.*/\1/p' <<< "$finding")"
        if [[ -z "$name" ]]; then
            pending="$finding"
            continue
        fi
        if grep -qE "(^|[^A-Za-z0-9_])${name}([^A-Za-z0-9_]|\$)" "$before"; then
            dropping=1
            continue
        fi
        printf '%s\n' "$finding"
    done
    flush
}

# --- staged: BLOCKING ratchet on the ADDED lines only ----------------------
# The added lines of a diff, each placed at the line number it occupies in the
# NEW file, with blank lines padding the gaps.
#
# The scanner counts lines as it reads, so a file holding only the added lines
# reports the position within that extract and calls it `file:line`. That field
# is a machine-readable contract — an editor jumps to it, a script annotates at
# it — and pointing it at a copyright header is worse than omitting it. Padding
# costs nothing a scan can see: a blank line matches no rule, and the extract is
# never longer than the file it came from.
# A gate that judges HEAD says so when the tree it is being run from differs.
#
# The --since and --staged ratchets read committed content, so a fix in the
# working tree is invisible to them and the finding repeats unchanged. That is
# the correct thing to judge — it is what would land — but silence about it
# costs the reader a cycle of doubting an edit that was right. Naming the
# revision turns "why is my fix ignored" into "of course, it reads HEAD".
noteTheRevisionJudged() {  # $1 = a human name for what is being judged
    local dirty
    dirty="$(git -C "$ROOT" status --porcelain -- '*.h' '*.cp' '*.cpp' '*.mm' '*.m' '*.swift' 2>/dev/null | wc -l | tr -d ' ')"
    if [[ "${dirty:-0}" != "0" ]]; then
        printf 'check-enums: judging %s; %s source file(s) differ in the working tree and are NOT read — commit to re-check.\n' \
            "$1" "$dirty" >&2
    fi
}

addedLinesAtTheirNumbers() {  # reads a `git diff -U0` on stdin
    awk '
        /^\+\+\+/ { next }
        /^@@/ {
            # @@ -old,count +new,count @@ — the new-file start is what a reader
            # of the result will open.
            match($0, /\+[0-9]+/)
            next_line = substr($0, RSTART + 1, RLENGTH - 1) + 0
            next
        }
        /^\+/ {
            while (emitted < next_line - 1) { print ""; emitted++ }
            print substr($0, 2)
            emitted++
            next_line++
        }
    '
}

# A newly-added bool-naming / bool-param / k-prefix violation BLOCKS the commit;
# the existing backlog stays advisory (the full scan below) and is chipped away
# by the boy-scout rule, not a tree-wide block. Annotate a case the rule should
# not flag with `// bool-ok: <reason>`.
# The rules against a fixture holding one of each: a k-prefixed definition and
# a subject-led boolean, both of which must be reported, and a string literal
# spelling terminfo capabilities (`kUP6=`), which must not — the literal is
# data, and a rule that read it as a declaration flagged an entry that cannot
# be renamed. A rule that stops matching prints nothing, which is what a clean
# tree prints, so the planted hits are what make a quiet run evidence.
if [[ "${1:-}" == "--self-test" ]]; then
    FIXTURE="$(mktemp -t check-enums-fixture.XXXXXX.cpp)"
    cat > "$FIXTURE" <<'FIXTURE_EOF'
constexpr int kPlanted = 1;
bool pastNames = true;
const char source[] = "\tkUP6=\\E[1;6A, kDN=\\E[1;2B,\n";
FIXTURE_EOF
    FOUT="$(perl - "$FIXTURE" "fixture.cpp" <<< "$PERL_PROG" 2>/dev/null || true)"
    rm -f "$FIXTURE"
    ok=0
    grep -q 'k-prefixed constant `kPlanted`' <<< "$FOUT" || { echo "self-test: the k-prefix rule missed a planted definition" >&2; ok=1; }
    grep -q 'boolean `pastNames`' <<< "$FOUT" || { echo "self-test: the boolean rule missed a planted subject-led name" >&2; ok=1; }
    grep -q 'kUP6\|kDN' <<< "$FOUT" && { echo "self-test: a capability name inside a string literal was read as a constant" >&2; ok=1; }

    # The reported line is the line in the FILE, not the offset within the
    # extract of added lines. A diff whose one added line sits at 42 must be
    # reported at 42; counting the extract would say 1, and `file:1` is a
    # location a reader can act on and be wrong about.
    NUMBERED="$(printf '%s\n' \
        '--- a/fixture.cpp' \
        '+++ b/fixture.cpp' \
        '@@ -41,0 +42 @@' \
        '+bool pastNames = true;' \
        | addedLinesAtTheirNumbers)"
    NTMP="$(mktemp)"; printf '%s\n' "$NUMBERED" > "$NTMP"
    NOUT="$(perl - "$NTMP" "fixture.cpp" <<< "$PERL_PROG" 2>/dev/null || true)"
    rm -f "$NTMP"
    grep -q '^fixture\.cpp:42: ' <<< "$NOUT" || {
        echo "self-test: an added line at 42 was not reported at 42 (got: ${NOUT%%$'\n'*})" >&2; ok=1; }

    [[ "$ok" == 0 ]] && echo "check-enums: self-test ok (a planted constant and boolean are reported at their real line, a string literal is not)"
    exit "$ok"
fi

if [[ "${1:-}" == "--staged" ]]; then
    # Per staged source file (C-family + Swift), scan only its ADDED lines with
    # the REAL filename as $rel, so the Swift-vs-C rule set selects by extension.
    # || true throughout: with nothing staged the greps exit 1, which under
    # set -e -o pipefail would abort the whole script as a SILENT exit-1 "block".
    FILES="$(git -C "$ROOT" diff --cached --name-only --diff-filter=ACM -- \
                 '*.h' '*.cp' '*.cpp' '*.mm' '*.m' '*.swift' \
             | grep -vE '(^|/)ThirdParty/' || true)"
    [[ -z "$FILES" ]] && exit 0
    OUT=""
    while IFS= read -r f; do
        [[ -z "$f" ]] && continue
        ADDED="$(git -C "$ROOT" diff --cached -U0 --diff-filter=ACM -- "$f" \
                 | addedLinesAtTheirNumbers || true)"
        # grep, not `${ADDED//[[:space:]]/}`: bash's pattern substitution is
        # quadratic in the string, and on the padded added-lines text of a large
        # file it spins with no child process to show for it -- a pre-commit
        # hook that never returns.
        grep -q '[^[:space:]]' <<< "$ADDED" || continue
        TMP="$(mktemp)"; printf '%s\n' "$ADDED" > "$TMP"
        # $path = TMP (opened); $rel = the real file (drives the Swift/C split).
        FOUT="$(perl - "$TMP" "$f" <<< "$PERL_PROG" 2>/dev/null || true)"
        rm -f "$TMP"
        BEFORE="$(mktemp)"; preexistingNames --staged "$f" > "$BEFORE"
        FOUT="$(dropPreexisting "$BEFORE" <<< "$FOUT")"
        rm -f "$BEFORE"
        [[ -n "$FOUT" ]] && OUT="${OUT}${FOUT}"$'\n'
    done <<< "$FILES"
    if [[ -n "$OUT" ]]; then
        printf '%s' "$OUT"
        printf '\ncheck-enums: newly-added bool/enum violation(s) above — fix before commit.\n'
        printf '  bool param -> a descriptive enum; bool name -> is/has/should/… prefix or a boolean adjective; no k-prefix.\n'
        printf '  Swift: `var/let x: Bool` -> predicate-led name. (Labeled Bool ARGUMENTS are fine in Swift — the label names them.)\n'
        printf '  To keep a bool here, annotate the line with // bool-ok: <reason>.\n'
        exit 1
    fi
    exit 0
fi

# --- since REF: BLOCKING ratchet on lines added vs REF (CI) ----------------
# Same ratchet as --staged, over `git diff REF...HEAD` instead of the index —
# the PR-diff surface for CI (mirrors check-comments.sh --since). Falls back
# to a skip when REF is absent (shallow checkout, no remote): the pre-commit
# --staged pass already gated each commit.
if [[ "${1:-}" == "--since" ]]; then
    REF="${2:-}"
    if [[ -z "$REF" ]]; then
        echo "check-enums: --since needs a git ref" >&2
        exit 2
    fi
    if ! git -C "$ROOT" rev-parse --verify --quiet "$REF" >/dev/null; then
        echo "check-enums: --since $REF not found; skipping (nothing to ratchet)"
        exit 0
    fi
    noteTheRevisionJudged "the lines HEAD adds over $REF"
    FILES="$(git -C "$ROOT" diff --name-only --diff-filter=ACM "$REF...HEAD" -- \
                 '*.h' '*.cp' '*.cpp' '*.mm' '*.m' '*.swift' \
             | grep -vE '(^|/)ThirdParty/' || true)"
    [[ -z "$FILES" ]] && exit 0
    OUT=""
    while IFS= read -r f; do
        [[ -z "$f" ]] && continue
        ADDED="$(git -C "$ROOT" diff -U0 --diff-filter=ACM "$REF...HEAD" -- "$f" \
                 | addedLinesAtTheirNumbers || true)"
        # grep, not `${ADDED//[[:space:]]/}`: bash's pattern substitution is
        # quadratic in the string, and on the padded added-lines text of a large
        # file it spins with no child process to show for it -- a pre-commit
        # hook that never returns.
        grep -q '[^[:space:]]' <<< "$ADDED" || continue
        TMP="$(mktemp)"; printf '%s\n' "$ADDED" > "$TMP"
        FOUT="$(perl - "$TMP" "$f" <<< "$PERL_PROG" 2>/dev/null || true)"
        rm -f "$TMP"
        BEFORE="$(mktemp)"; preexistingNames "$REF" "$f" > "$BEFORE"
        FOUT="$(dropPreexisting "$BEFORE" <<< "$FOUT")"
        rm -f "$BEFORE"
        [[ -n "$FOUT" ]] && OUT="${OUT}${FOUT}"$'\n'
    done <<< "$FILES"
    if [[ -n "$OUT" ]]; then
        printf '%s' "$OUT"
        printf '\ncheck-enums: newly-added bool/enum violation(s) above — fix before merge.\n'
        printf '  bool param -> a descriptive enum; bool name -> is/has/should/… prefix or a boolean adjective; no k-prefix.\n'
        printf '  Swift: `var/let x: Bool` -> predicate-led name. (Labeled Bool ARGUMENTS are fine in Swift — the label names them.)\n'
        printf '  To keep a bool here, annotate the line with // bool-ok: <reason>.\n'
        exit 1
    fi
    exit 0
fi

# --- full-tree / explicit files: ADVISORY hints ----------------------------
TOTAL=0

# One perl for the whole list. The pairs are built first because the program
# reads them as (path, display path), and the display path is what a finding
# names -- in --staged the two differ, and here they are the same file.
PAIRS=()
while IFS= read -r rel; do
    [[ -z "$rel" ]] && continue
    abs="$rel"
    [[ "$abs" != /* ]] && abs="$ROOT/$abs"
    [[ -f "$abs" ]] || continue
    PAIRS+=("$abs" "$rel")
done < <(
    if [[ $# -gt 0 ]]; then get_files "$@"
    else get_files
    fi
)

if [[ ${#PAIRS[@]} -gt 0 ]]; then
    COUNT_FILE="$(mktemp)"
    UBQ_ENUMS_COUNT_FILE="$COUNT_FILE" perl - "${PAIRS[@]}" <<< "$PERL_PROG" 2>/dev/null && true
    code=$?
    [[ $code -gt 1 ]] && { echo "check-enums: perl error scanning the tree" >&2; rm -f "$COUNT_FILE"; exit 2; }
    TOTAL=$(cat "$COUNT_FILE" 2>/dev/null || echo 0)
    rm -f "$COUNT_FILE"
    [[ -n "$TOTAL" ]] || TOTAL=0
fi

if [[ $TOTAL -gt 0 ]]; then
    printf '\ncheck-enums: %d file(s) with enum hints (advisory — new violations block via --staged).\n' "$TOTAL"
    printf '  k-prefixed constants -> unprefixed enum class / constexpr (WebKit).\n'
    printf '  bool parameters -> descriptive enum; bool names -> is/has/should/… (or a boolean adjective).\n'
    # Which question this answered. The whole-tree scan reports the standing
    # backlog, most of it older than whatever change is in hand; the gating set
    # is what --since reports. Without this line the two are told apart only by
    # their length, and the first instinct on reading hundreds of findings is to
    # start fixing code the change never touched.
    printf '  This is the whole-tree backlog, not the gating set: run --since <ref> for the findings that block a merge.\n'
    exit 1
fi

exit 0
