#!/usr/bin/env bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

# Scripts/check-comments.sh — comment hygiene lint (the no-temporal-comments rule).
#
# BLOCKS on a comment attached to an #include / #import / Swift import line. The
# comment names what the header supplies, the header changes without it, and
# nothing checks the pairing — so it becomes a confident wrong answer. Say it
# once in the file-top comment instead, where it is prose rather than a claim
# about a specific symbol.
#
# BLOCKS on internal issue/PR/ticket-number references in CODE COMMENTS. A ticket
# number is temporal: it dates the code to a development moment and rots when the
# tracker moves. A comment states a present-tense invariant; the ticket lives in
# `git blame` and the PR. (Rule: Docs/coding-conventions.md §3.2.)
#
# RATCHET: the blocking modes (--staged, --since) scan only ADDED lines, so a
# legacy ref in a file you happen to touch never blocks you — only refs you write
# do. This mirrors check-acronyms.py. The full-tree backlog is reported ADVISORY
# (see the bare-invocation mode) and burned down separately; it never blocks CI.
#
# EXEMPT (not flagged):
#   - external-standard citations: UAX #29, UTS #51, RFC 1951, ECMA-48, ISO,
#     Unicode Annex, TR # — stable references, not history.
#   - terminfo-style capabilities (Co#256) and other letter-prefixed `name#NNN`.
#   - hex colors (#33ff66) and pixel sizes (#16px).
#   - single-digit enumerations (#1, #2) used as in-comment list markers.
#
# BLOCKS, in every mode, on change narration: a closed list of phrases that can
# only compare the code to an earlier version of itself (see temporal_filter).
# `used to`, `no longer` and `previously` are not on it; each also has a correct
# program-state sense, so restating them as invariants is a reading job.
#
# Deferral markers are not handled here. Scripts/check-slop.py owns them and
# blocks on them in every scanned file, source included: a marker recording work
# as postponed names no change and no owner, and the reader who needs it cannot
# act on it either way.
#
# Usage:
#   Scripts/check-comments.sh            # full tree — tickets and include comments ADVISORY
#   Scripts/check-comments.sh --staged   # staged ADDED lines (pre-commit BLOCK)
#   Scripts/check-comments.sh --since REF # ADDED lines vs REF (CI BLOCK)
#   Scripts/check-comments.sh file ...   # specific files, full content (BLOCK)
#
# Exit 0 = clean/advisory-only, 1 = a blocking finding, 2 = usage error.
#
# Wired into Scripts/hooks/pre-commit (--staged, BLOCKS) and Scripts/check-style.py
# (--since origin/main, BLOCKS the PR diff). Portable to bash 3.2 (macOS /bin/bash):
# no mapfile, no arrays.

set -uo pipefail
source "$(dirname "$0")/gate-report-lib.sh"
# The subject is the tree git reports for the directory the gate runs in, which is
# not this script's own when it runs as a library's copy.
ROOT="$(git rev-parse --show-toplevel 2>/dev/null || (cd "$(dirname "$0")/.." && pwd))"
cd "$ROOT" || exit 1

case "${1:-}" in
    --help|-h) sed -n '/^#!/d; /^[^#]/q; s/^# \{0,1\}//; p' "$0"; exit 0 ;;
esac

# --- shared detectors --------------------------------------------------------
# Both read "path\tlineno\tcontent" records on stdin (one per candidate line)
# and print "  path:lineno: content" for each match. Splitting the mode of
# extraction (whole file vs added diff lines) from the rule keeps the two in
# lockstep across pre-commit, CI, and ad-hoc runs.
ticket_filter() {
    perl -ne '
        my ($path, $lno, $line) = split(/\t/, $_, 3);
        next unless defined $line;
        # C-family comment (//, /*, *), or a shell FULL-LINE comment (^#) in a
        # .sh file. A ticket-shaped token inside a shell code string (a quoted
        # fail/grep argument) is not a comment, so only a leading # counts.
        next unless $line =~ m{(?://|/\*|^\s*\*)} || ($path =~ /\.sh$/ && $line =~ m{^\s*#});
        next unless $line =~ m{\#[0-9]{2,}\b};                      # ticket-ish number
        next if $line =~ m{\b(?:UAX|UTS|RFC|ISO|ECMA|Annex|Unicode|TR)\b}; # external standard
        next if $line =~ m{[A-Za-z]\#[0-9]};                        # terminfo cap (Co#256), hex color
        next if $line =~ m{\#[0-9]+\s*px};                          # pixel size
        # A six-digit run after # is an RGB colour, not an issue number. This
        # tracker numbers issues in four digits, and a comment quoting a colour
        # the terminal reported names a value rather than a ticket.
        next if $line =~ m{\#[0-9a-fA-F]{6}\b} && $line !~ m{\#[0-9]{1,4}\b};
        chomp $line; $line =~ s/^\s+//;
        print "  $path:$lno: $line\n";
    '
}
# An include line carries no comment. The comment is always a claim about what
# the header supplies — "Cell, Pen, maxRows" — and the header is free to change
# without it. Nothing checks the pairing, so the annotation drifts into a lie
# that reads as documentation, and the reader who trusts it looks in the wrong
# file. Where an include is genuinely surprising, the file-top comment is the
# place to say so once, in prose that survives the header changing.
include_comment_filter() {
    perl -ne '
        my ($path, $lno, $line) = split(/\t/, $_, 3);
        next unless defined $line;
        next unless $line =~ m{^\s*(?:\#\s*(?:include|import)\s+(?:[<"]|[A-Z_][A-Z0-9_]*)|import\s+\w)};
        next unless $line =~ m{(?:^|[>"\w\s])(?://|/\*)};
        chomp $line; $line =~ s/^\s+//;
        print "  $path:$lno: $line\n";
    '
}
# Change narration: a comment comparing the code to an earlier version of
# itself. It describes one commit and is false after the next ten, and the
# reader it addresses — someone who saw the previous version — does not exist.
#
# Three phrases are not checked, and naming them is the point: `used to`,
# `previously` and `no longer` each carry a second, correct sense that reads
# identically. A geometry `used to` spawn sessions is a purpose; a set
# `previously installed` is program state; panes `no longer` in the layout are a
# runtime fact, and so is a row that `now holds` what another row had. Across
# the tree those senses outnumber the narrating ones, so a rule spanning them
# rejects more correct comments than it catches, which is the gate people learn
# to skip. They are a reading job.
#
# What is left has no second reading: a phrase that can only mean "this differs
# from what was here before". "always did" and "as it always has" are such
# phrases; a comment that holds one is describing an edit, not the program.
#
# A full-line `#` comment is a comment, so the rule holds the shell scripts and
# git hooks too: those are the gates that enforce it.
#
# `this fix|change|commit|patch` is checked in C-family comments only: in a gate
# script the phrase names the change under inspection ("would any test notice if
# this change were wrong?"), not an earlier version. The rest have one reading
# in either language.
#
# Quoted spans are blanked first, so a rule quoting the phrases it forbids is
# mentioning them rather than narrating.
temporal_filter() {
    perl -ne '
        my ($path, $lno, $line) = split(/\t/, $_, 3);
        next unless defined $line;
        my $cfamily = $line =~ m{(?://|/\*|^\s*\*)};
        my $hash = $line =~ m{^\s*#};
        next unless $cfamily || $hash;
        my $text = $line;
        $text =~ s{"[^"\n]*"}{}g;
        my $always = $text =~ m{\b(?:formerly|has been (?:changed|updated|moved|renamed)|as of this|going forward|we now|now (?:returns|uses|runs|lives)|before this (?:fix|change)|the old code|used to be a|always did|(?:as|the way) (?:it|they) always (?:has|have))\b}i;
        my $codeonly = $cfamily && $text =~ m{\bthis (?:fix|change|commit|patch)\b}i;
        next unless $always || $codeonly;
        chomp $line; $line =~ s/^\s+//;
        print "  $path:$lno: $line\n";
    '
}

# Whole-file records: every line of each tracked source file, path\tlineno\tcontent.
RECORDS_FILE="$(mktemp -t ubq-check-comments.XXXXXX)"
trap 'rm -f "$RECORDS_FILE"' EXIT

records_full_tree() {
    # One perl for the whole set, not one per file. An interpreter start and a
    # program parse per file is where the per-file form spends its time, and
    # the output is byte-identical both ways.
    #
    # `close ARGV if eof` is what keeps that correct — perl's $. counts lines
    # across the whole argument list unless the handle is closed at each file's
    # end, so without it every record after the first file carries a line number
    # offset by everything before it.
    #
    # The hooks are listed by path. Every git hook here is extensionless, so a
    # suffix filter alone would exclude the scripts that enforce this rule on
    # everyone else.
    { git ls-files --cached --others --exclude-standard '*.h' '*.cpp' '*.cc' '*.mm' '*.m' '*.swift' '*.sh'
      git ls-files --cached --others --exclude-standard 'Scripts/hooks' 'Scripts/hooks/*' 'Scripts/hooks-dispatch'; } \
        | sort -u \
        | grep -vE '(^|/)ThirdParty/' \
        | tr '\n' '\0' \
        | xargs -0 perl -ne 'print "$ARGV\t$.\t$_"; close ARGV if eof;'
}
records_files() {
    for f in "$@"; do
        [ -f "$f" ] || continue
        perl -ne 'print "$ARGV\t$.\t$_"' "$f"
    done
}
# Added-line records from a unified -U0 diff on stdin: track the file from the
# `+++ b/PATH` header and the new-side line number from each `@@ … +N` hunk, then
# emit each added (`+`, not `+++`) line as path\tlineno\tcontent. Non-source and
# ThirdParty paths are dropped so the ratchet matches the full-tree file set.
records_from_diff() {
    perl -ne '
        if (m{^\+\+\+\ b/(.+)$}) {
            $f = $1;
            $ok = ($f =~ /\.(h|cpp|cc|mm|m|swift|sh)$/ && $f !~ m{(^|/)ThirdParty/});
            next;
        }
        if (m{^@@ .*?\+(\d+)}) { $ln = $1; next; }
        next unless defined $f && $ok;
        if (/^\+/ && !/^\+\+\+/) {
            my $content = substr($_, 1);         # strip the leading +
            my $n = $ln++;                        # this added line, then advance
            print "$f\t$n\t$content";
        }
    '
}

MODE="${1:-}"
BLOCK=0        # 1 = ticket refs fail the run; 0 = advisory-only

case "$MODE" in
    --staged)
        git diff --cached -U0 --diff-filter=ACM | records_from_diff > "$RECORDS_FILE"
        BLOCK=1
        ;;
    --since)
        REF="${2:-}"
        if [ -z "$REF" ]; then
            echo "check-comments: --since needs a git ref" >&2; exit 2
        fi
        if ! git rev-parse --verify --quiet "$REF" >/dev/null; then
            # No base to diff against (shallow checkout, no remote): nothing new
            # to ratchet — the pre-commit --staged pass already gated each commit.
            echo "check-comments: --since $REF not found; skipping (nothing to ratchet)"
            exit 0
        fi
        # This ratchet reads HEAD, so a fix sitting in the working tree is not
        # seen and the finding repeats unchanged. Judging what would land is
        # right; leaving the reader to discover it by doubting a correct edit is
        # not.
        dirtyCount="$(git status --porcelain 2>/dev/null | wc -l | tr -d ' ')"
        if [[ "${dirtyCount:-0}" != "0" ]]; then
            echo "check-comments: judging the lines HEAD adds over $REF; ${dirtyCount} path(s) differ in the working tree and are not read — commit to re-check." >&2
        fi
        git diff -U0 "$REF...HEAD" | records_from_diff > "$RECORDS_FILE"
        BLOCK=1
        ;;
    "")
        records_full_tree > "$RECORDS_FILE"
        BLOCK=0
        ;;
    *)
        records_files "$@" > "$RECORDS_FILE"
        BLOCK=1
        ;;
esac

# A file, not a variable. The full-tree records are large, and holding them
# in the shell means re-serialising all of it through printf once per filter —
# three passes that copy the whole corpus before any of them reads a line.
if [ ! -s "$RECORDS_FILE" ]; then
    echo "check-comments: no comment lines to scan"
    exit 0
fi

violations=0

# --- ticket/PR refs ----------------------------------------------------------
ticket_out=$(ticket_filter < "$RECORDS_FILE")
if [ -n "$ticket_out" ]; then
    if [ "$BLOCK" -eq 1 ]; then
        echo "check-comments: internal ticket/PR refs in comments — temporal, state the invariant instead:"
        echo "$ticket_out"
        violations=1
    else
        echo "check-comments: ADVISORY — internal ticket/PR refs in comments (pre-existing backlog; does NOT block):"
        echo "$ticket_out"
    fi
fi

# --- comments on include lines -----------------------------------------------
include_out=$(include_comment_filter < "$RECORDS_FILE")
if [ -n "$include_out" ]; then
    echo
    if [ "$BLOCK" -eq 1 ]; then
        echo "check-comments: a comment on an include line — it claims what the header supplies and nothing keeps it true:"
        echo "$include_out"
        violations=1
    else
        echo "check-comments: ADVISORY — comments on include lines (pre-existing backlog; does NOT block):"
        echo "$include_out"
    fi
fi

# --- change narration (BLOCKS) -----------------------------------------------
narration_out=$(temporal_filter < "$RECORDS_FILE")
if [ -n "$narration_out" ]; then
    echo
    echo "check-comments: change narration — a comment comparing the code to an earlier version of itself:"
    echo "$narration_out"
    echo
    echo "  State the invariant, or the failure mode in present terms. A regression"
    echo "  guard reads better for it: \"a band/run union that produces one wide rect"
    echo "  includes blank gaps, and the gaps leak stale pixels\" says what breaks,"
    echo "  where \"used to produce\" says only that someone once changed it."
    violations=$((violations + 1))
fi

echo
if [ "$violations" -ne 0 ]; then
    gate_failed_count "check-comments" "$violations" "internal ticket ref(s) / include-line comment(s) in the added lines, listed above"
    exit 1
fi
echo "check-comments: ok ($(grep -c . "$RECORDS_FILE") comment line(s) scanned)"
exit 0
