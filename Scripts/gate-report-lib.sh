#!/usr/bin/env bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0
#
# gate-report-lib.sh — one shape for every gate's verdict line.
#
# The failure this exists for. A gate that prints "check-x: FAILED —" and then
# lists the offenders on the following lines is unreadable to anything that
# looks for the word FAILED: the line it finds ends in a colon and names no
# offender, no count, and nothing to act on. That is how a reader — human or
# tool — concludes a build is broken without learning what broke, and it is how
# nine failing tests get read as nine defects when they are one link error.
#
# So the rule is that the line CARRYING the word is the line carrying the
# verdict: which gate, what kind of thing, how many, and the first few by name.
# Detail may still follow. It may not be the only copy.
#
# `Scripts/gate_report.py` is the same format for Python gates, and
# `Tests/test-gate-report.sh` asserts the two agree byte for byte — a format
# spelled twice drifts the first time one side is edited alone.

# How many offenders the verdict line names before it summarises the rest. Three
# fits a terminal width and is enough to recognise the pattern; the count is
# what makes the remainder actionable.
GATE_REPORT_NAMED_LIMIT=3

# gate_failed <gate> <what> [offender ...]
#
# Writes the verdict to stderr and returns 1, so a caller can `gate_failed ... ||
# exit 1` or use the return directly. With offenders the line carries their
# number and the first few; without, it carries `what` alone, which is right for
# a gate whose failure is singular ("the corpus never ran").
gate_failed() {
    local gate="$1" what="$2"
    shift 2
    local count=$#
    if [ "$count" -eq 0 ]; then
        printf '%s: FAILED — %s\n' "$gate" "$what" >&2
        gate_annotate error "" "$gate" "$what"
        gate_summary_row "$gate" "$what" "${GATE_REPRODUCE:-$gate}"
        return 1
    fi
    local named="" shown=0
    while [ $# -gt 0 ] && [ "$shown" -lt "$GATE_REPORT_NAMED_LIMIT" ]; do
        [ -n "$named" ] && named="$named, "
        named="$named$1"
        shown=$((shown + 1))
        shift
    done
    local remaining=$((count - shown))
    if [ "$remaining" -gt 0 ]; then
        printf '%s: FAILED — %d %s: %s (+%d more)\n' "$gate" "$count" "$what" "$named" "$remaining" >&2
        gate_annotate error "" "$gate" "$count $what: $named (+$remaining more)"
        gate_summary_row "$gate" "$count $what: $named (+$remaining more)" "${GATE_REPRODUCE:-$gate}"
    else
        printf '%s: FAILED — %d %s: %s\n' "$gate" "$count" "$what" "$named" >&2
        gate_annotate error "" "$gate" "$count $what: $named"
        gate_summary_row "$gate" "$count $what: $named" "${GATE_REPRODUCE:-$gate}"
    fi
    return 1
}

# gate_failed_count <gate> <count> <what>
#
# The verdict where the offenders were counted but not collected — a gate that
# tallies while it walks rather than accumulating a list. The count still rides
# on the line, which is the part that makes it actionable.
gate_failed_count() {
    printf '%s: FAILED — %s %s\n' "$1" "$2" "$3" >&2
    gate_annotate error "" "$1" "$2 $3"
    gate_summary_row "$1" "$2 $3" "${GATE_REPRODUCE:-$1}"
    return 1
}

# gate_finding <location> <gate> <message>
#
# One offender, in the shape every compiler emits and therefore the shape every
# editor, CI annotator and language model already parses without being told:
#
#     Source/Core/Foo.cpp:42: check-style: a boolean parameter takes an enum
#
# `location` is `path:line` where a line is known and `path` where it is not.
# The gate's name rides on every line rather than only on the header, so a
# finding stays attributable when it is read on its own — which is how findings
# are read once the output is more than a screen long.
gate_finding() {
    printf '%s: %s: %s\n' "$1" "$2" "$3" >&2
    gate_annotate error "$1" "$2" "$3"
}

# gate_ok <gate> <what>
#
# The passing verdict, on stdout. Carries what was examined rather than a bare
# "ok": a gate that says only "ok" cannot be told apart from one that examined
# nothing, which is the other half of the same problem.
gate_ok() {
    printf '%s: ok (%s)\n' "$1" "$2"
}

# Whether this run is the verdict a CI job reports. Read from UBQ_CI_ANNOTATE,
# set once at the workflow level, rather than from GITHUB_ACTIONS: gates also
# run inside jobs measuring something else, and a run that is not a job's
# verdict should spend neither its annotation budget nor its summary. A local
# run sets nothing and sees no markers, which would otherwise be literal text.
ubq_ci_annotating() { [ -n "${UBQ_CI_ANNOTATE:-}" ]; }

# A workflow command is one line, and an unescaped newline truncates it there.
ubq_annotation_escape() {
    local text="$1"
    text="${text//\%/%25}"
    text="${text//$'\r'/%0D}"
    text="${text//$'\n'/%0A}"
    printf '%s' "$text"
}

# How many annotations one process emits before it stops counting them out.
# GitHub renders ten per level per step and discards the rest, so a gate with
# four hundred findings would spend the whole budget before the gate after it
# emits one. The remainder stays in the log.
GATE_ANNOTATION_BUDGET=10
GATE_ANNOTATIONS_EMITTED=0
GATE_ANNOTATIONS_SUPPRESSED=0

# gate_annotate <level> <location> <gate> <message>
#
# One annotation, so a failing gate reaches the run summary and the annotations
# API without the reader fetching the log at all. `location` is `path:line`,
# `path`, or empty for a verdict naming no file.
#
# The location rides in the MESSAGE as well as in the file= and line=
# properties: the runner renders the command into the log as `##[error]` plus
# the message with the properties stripped, so a message without them is
# unreadable in exactly the log a reader greps.
gate_annotate() {
    ubq_ci_annotating || return 0
    local level="$1" location="$2" gate="$3" message="$4" file="" line="" rest
    if [ "$GATE_ANNOTATIONS_EMITTED" -ge "$GATE_ANNOTATION_BUDGET" ]; then
        GATE_ANNOTATIONS_SUPPRESSED=$((GATE_ANNOTATIONS_SUPPRESSED + 1))
        return 0
    fi
    if [ -n "$location" ]; then
        file="${location%%:*}"
        rest="${location#*:}"
        case "$rest" in
            "$location"|'' | *[!0-9]*) line="" ;;
            *) line="$rest" ;;
        esac
        [ -f "$file" ] || file=""
    fi
    message="$(ubq_annotation_escape "${location:+$location: }$gate: $message")"
    if [ -n "$file" ] && [ -n "$line" ]; then
        printf '::%s file=%s,line=%s,title=%s::%s\n' "$level" "$file" "$line" "$gate" "$message" >&2
    elif [ -n "$file" ]; then
        printf '::%s file=%s,title=%s::%s\n' "$level" "$file" "$gate" "$message" >&2
    else
        printf '::%s title=%s::%s\n' "$level" "$gate" "$message" >&2
    fi
    GATE_ANNOTATIONS_EMITTED=$((GATE_ANNOTATIONS_EMITTED + 1))
}

# gate_summary_row <gate> <finding> <reproduce>
#
# One row in the job summary GitHub renders above the log. The summary is the
# only surface a reader sees before deciding whether to fetch anything, so it
# carries the three facts that decide it: which gate, what it found, and the
# command that runs that finding alone.
gate_summary_row() {
    ubq_ci_annotating || return 0
    [ -n "${GITHUB_STEP_SUMMARY:-}" ] || return 0
    if [ ! -s "$GITHUB_STEP_SUMMARY" ]; then
        printf '### Failing gates\n\n| gate | finding | reproduce |\n|---|---|---|\n' \
            >> "$GITHUB_STEP_SUMMARY"
    fi
    printf '| `%s` | %s | `%s` |\n' "$1" "$2" "$3" >> "$GITHUB_STEP_SUMMARY"
}
