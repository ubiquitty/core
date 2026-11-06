#!/usr/bin/env bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

# The gate runner the pre-push gates drive: each gate's verdict, the line
# it leaves on the terminal, and the cached pass behind a verdict that was one.
# Sourced after Scripts/pre-push-cache.sh, whose ubq_gate_cache_* calls it makes.
#
# A verdict lands in exactly one of PASSED, SKIPPED and FAILED. CACHED names the
# passes served from the cache, WARNED the passes whose gate printed a warning,
# and FAILED_LOG holds each failure's log path at the failure's index.
FAILED=()
FAILED_LOG=()
SKIPPED=()
PASSED=()
CACHED=()
WARNED=()

# A gate whose inputs are declared, keyed on those inputs alone.
#
#   run_scoped <name> <scope> <command...>
#
# <scope> is a space-separated list of git pathspecs. Everything the gate reads
# from the repository must be inside it, and Scripts/check-gate-scopes.py holds
# the declaration to that: a gate reaching an input its scope omits fails there
# rather than becoming a stale pass here.
#
# Without a declaration the only sound key is the whole tree, which makes every
# gate depend on every file, so a markdown edit reruns the emscripten link and
# the snapshot renders. The declaration is what lets the key be narrower.
run_scoped() {
    local name="$1"; local scope="$2"; shift 2
    UBQ_GATE_SCOPE="$scope" run "$name" "$@"
    local status=$?
    UBQ_GATE_SCOPE=""
    return $status
}

# Seconds since the epoch, fractional. bash 3.2 ships no EPOCHREALTIME and
# `date +%s` cannot resolve a sub-second gate, which is most of them.
ubq_gate_now() {
    python3 -c 'import time; print(time.time())' 2>/dev/null || echo 0
}

# How long a gate took, at one decimal, or empty when the clock was unavailable.
# Printed beside every verdict: a summary that says which gate was slow is what
# makes the next optimisation an argument about evidence.
ubq_gate_elapsed() { # <started>
    [ "${1:-0}" != 0 ] || { printf ''; return 0; }
    python3 -c "import sys; print(f' {float(sys.argv[2]) - float(sys.argv[1]):.1f}s')" \
        "$1" "$(ubq_gate_now)" 2>/dev/null || printf ''
}

# The gate's name while it works, and what it leaves behind once it is done.
# A push runs dozens of gates over several minutes, so a reader watching one
# needs to see which is working; a reader whose push succeeds needs none of it
# afterwards. On a terminal the progress line is erased by a pass. Where stdout
# is not a terminal there is nothing to erase, so the name is held back and
# written only by an outcome worth reading. Anything other than a pass names its
# gate: a verdict word with no gate beside it cannot be acted on.
ubq_gate_progress() { # <name>
    { [ -t 1 ] || [ -n "${UBQ_GATE_VERBOSE:-}" ]; } || return 0
    printf '  %-34s ' "$1"
}

ubq_gate_outcome() { # <name> <word> <started> <passed:0|1>
    if [ -n "${UBQ_GATE_VERBOSE:-}" ]; then
        printf '%s%s\n' "$2" "$(ubq_gate_elapsed "$3")"
        return 0
    fi
    if [ "$4" = 1 ]; then
        [ -t 1 ] && printf '\r\033[K'
        return 0
    fi
    [ -t 1 ] || printf '  %-34s ' "$1"
    printf '%s\n' "$2"
}

# Prints why a gate stood down and returns 1 when it did not. A stand-down is a
# line `<gate>: SKIPPED — <why>`, matched by check-style.py's STOOD_DOWN pattern
# so the two runners agree on what counts. An aggregate such as check-style.py
# relays one per sub-gate; they are joined by " | " because a reason may itself
# carry "; ".
ubq_gate_stood_down() { # <log>
    LC_ALL=C awk '
        /^[a-z][a-z0-9.-]*: SKIPPED/ {
            why = $0
            sub(/^[a-z][a-z0-9.-]*: SKIPPED[ \t]*/, "", why)
            sub(/^(—|-|:)[ \t]*/, "", why)
            if (why ~ /^\(.*\)$/) {
                why = substr(why, 2, length(why) - 2)
            }
            if (why == "") {
                why = $0
            }
            reasons = reasons (reasons == "" ? "" : " | ") why
        }
        END {
            if (reasons == "") {
                exit 1
            }
            print reasons
        }
    ' "$1"
}

# Prints the warnings a passing gate left and returns 1 when it left none. A
# warning is a line `<gate>: WARNING — <why>`: the gate passed, and it names
# what a later push is refused for. The log of a pass is deleted in `run`, so
# without this the line is printed into a file removed a moment later; and a
# warned pass is not cached, so the line is printed on every push until it is
# acted on rather than once.
ubq_gate_warned() { # <log>
    LC_ALL=C awk '
        /^[a-z][a-z0-9.-]*: WARNING/ {
            why = $0
            sub(/^[a-z][a-z0-9.-]*: WARNING[ \t]*/, "", why)
            sub(/^(—|-|:)[ \t]*/, "", why)
            if (why == "") {
                why = $0
            }
            reasons = reasons (reasons == "" ? "" : " | ") why
        }
        END {
            if (reasons == "") {
                exit 1
            }
            print reasons
        }
    ' "$1"
}

# The path a gate's script has in the checkout that holds it. A gate is named by
# its `./Scripts/` path in the repository that owns it; in another repository it
# sits under ubiquitty-core's or ubiquitty-kit's checkout. The subject's own copy
# wins, so a repository can run a gate of its own beside the shared ones. Any other
# argument is returned as it came, and a script no checkout holds stays as written,
# so the failure names the missing file.
ubq_gate_script() { # <argument>
    case "$1" in
        ./Scripts/*)
            local relative="${1#./}" base
            for base in "${UBQ_SUBJECT_ROOT:-$PWD}" "${UBQ_CORE_ROOT:-}" "${UBQ_KIT_ROOT:-}"; do
                [ -n "$base" ] && [ -e "$base/$relative" ] || continue
                if [ "$base" = "${UBQ_SUBJECT_ROOT:-$PWD}" ]; then
                    printf '%s' "$1"
                else
                    printf '%s' "$base/$relative"
                fi
                return 0
            done
            ;;
    esac
    printf '%s' "$1"
}

# run_for <repositories> <name> <command...> — a gate that answers for the
# repositories it names (space-separated: core, kit, app). It runs where the
# checkout holds one of them. Elsewhere it is counted, not run: a gate whose
# subject this checkout does not hold has nothing to judge, and passing it would
# be a pass about nothing. ubq_gate_report_elsewhere names them at the end.
ELSEWHERE=()
run_for() {
    local repositories="$1" name="$2"; shift 2
    local held wanted
    for wanted in $repositories; do
        for held in ${UBQ_REPOSITORIES:-core kit app}; do
            if [ "$wanted" = "$held" ]; then
                run "$name" "$@"
                return $?
            fi
        done
    done
    ELSEWHERE+=("$name ($repositories)")
    return 0
}

# ubq_runs_here <repositories> <name> — succeeds where the checkout holds one of the
# repositories. Elsewhere it names the gate as answering for them and fails, so the
# caller leaves it unrun, as `run_for` does for a gate it runs itself.
ubq_runs_here() {
    local repositories="$1" name="$2" wanted held
    for wanted in $repositories; do
        for held in ${UBQ_REPOSITORIES:-core kit app}; do
            [ "$wanted" = "$held" ] && return 0
        done
    done
    ELSEWHERE+=("$name ($repositories)")
    return 1
}

# run_whole <name> <command...> — a gate whose subject is the product as one tree:
# it runs only in a checkout holding every repository, and is named, not run,
# anywhere that holds fewer.
run_whole() {
    local name="$1"
    ubq_whole_here "$name" || return 0
    run "$@"
}

# ubq_whole_here <name> — succeeds in a checkout holding every repository. Elsewhere
# it names the gate as needing them together and fails, so the caller leaves it
# unrun, as `run_whole` does for a gate it runs itself.
ubq_whole_here() {
    local name="$1" repository held
    for repository in core kit app; do
        for held in ${UBQ_REPOSITORIES:-core kit app}; do
            [ "$repository" = "$held" ] && continue 2
        done
        ELSEWHERE+=("$name (needs core kit app held together)")
        return 1
    done
    return 0
}

# run <name> <command...> — a gate. A missing tool reports SKIPPED, never passes.
run() {
    local name="$1"; shift
    local arguments=() argument
    for argument in "$@"; do
        arguments+=("$(ubq_gate_script "$argument")")
    done
    set -- "${arguments[@]}"
    local log cache_key="" started
    log="$(mktemp)"
    started="$(ubq_gate_now)"
    ubq_gate_progress "$name"
    if [ -n "${UBQ_GATE_SCOPE:-}" ]; then
        cache_key="$(ubq_gate_cache_scoped_key "$UBQ_GATE_SCOPE" "$@")" || cache_key=""
    else
        cache_key="$(ubq_gate_cache_key "$name" "$@")" || cache_key=""
    fi
    if ubq_gate_cache_has "$cache_key"; then
        ubq_gate_outcome "$name" cached "$started" 1
        PASSED+=("$name")
        CACHED+=("$name")
        rm -f "$log"
        return 0
    fi
    if "$@" >"$log" 2>&1; then
        # A gate that stood down for a missing tool exits 0 with that part unchecked.
        # Caching it as a pass would keep it from running once the tool exists.
        local stood_down warned
        if stood_down="$(ubq_gate_stood_down "$log")"; then
            ubq_gate_outcome "$name" "SKIPPED ($stood_down)" "$started" 0
            SKIPPED+=("$name ($stood_down)")
        elif warned="$(ubq_gate_warned "$log")"; then
            ubq_gate_outcome "$name" "ok, WARNING ($warned)" "$started" 0
            PASSED+=("$name")
            WARNED+=("$name ($warned)")
        else
            ubq_gate_outcome "$name" ok "$started" 1
            PASSED+=("$name")
            ubq_gate_cache_store "$cache_key"
        fi
    else
        local status=$?
        # 127 is "command not found", which a gate returns for a toolchain this
        # machine lacks — and equally for a gate script that tried to run one of
        # its own arguments. A line continuation followed by a blank line does
        # exactly that: the arguments below it become commands, every one of them
        # 127, and the gate is excused for the reason it is broken.
        #
        # An option is never a program, so the missing name decides which it is.
        # The name is also reported: "tool not found" without saying which tool
        # is a skip nobody can act on or verify.
        local missing
        missing="$(sed -n 's/.*: \(.*\): command not found.*/\1/p' "$log" | head -1)"
        # A heredoc terminator that leaks into the command position also exits
        # 127, and a gate broken that way is excused for the reason it is
        # broken: a script with two PERL_EOF terminators runs the second as a
        # command, and the gate reports a skip.
        #
        # An installable tool is spelled in lower case. PERL_EOF, SQL_END and
        # their kin are heredoc terminators or shell variables that lost their
        # expansion, and none of them is something a developer can install — so a
        # name with no lower-case letter is a broken gate rather than a missing
        # one, and fails.
        if [ "$status" -eq 127 ] && [ -n "$missing" ] && [ "${missing#-}" = "$missing" ] \
                && printf '%s' "$missing" | grep -q '[a-z]'; then
            ubq_gate_outcome "$name" "SKIPPED (no $missing)" "$started" 0
            SKIPPED+=("$name")
        elif [ "$status" -eq 127 ] && [ -n "$missing" ] && [ "${missing#-}" = "$missing" ]; then
            ubq_gate_outcome "$name" "FAILED (ran '$missing' as a command — a broken gate, not a missing tool)" "$started" 0
            FAILED+=("$name")
            FAILED_LOG+=("$log")
            echo "--- $name ---" >&2
            tail -40 "$log" >&2
            echo "--- end $name (full log: $log) ---" >&2
            return 0
        elif [ "$status" -eq 127 ] && [ -z "$missing" ]; then
            ubq_gate_outcome "$name" "SKIPPED (tool not found)" "$started" 0
            SKIPPED+=("$name")
        else
            ubq_gate_outcome "$name" FAILED "$started" 0
            FAILED+=("$name")
            # The log is not removed: the line below names it, and the summary
            # names it again after everything else has scrolled past. Deleting
            # it here would point the reader at a path that no longer exists.
            FAILED_LOG+=("$log")
            echo "--- $name ---" >&2
            tail -40 "$log" >&2
            echo "--- end $name (full log: $log) ---" >&2
            return 0
        fi
    fi
    rm -f "$log"
}

# Lists the gates that left something unchecked, on stderr, and prints nothing
# when every gate ran in full. An aggregate that relayed a sub-gate's stand-down
# is listed although its other sub-gates ran.
ubq_gate_report_skipped() {
    [ "${#SKIPPED[@]}" -gt 0 ] || return 0
    echo "pre-push gates: ${#SKIPPED[@]} SKIPPED — what each names went unchecked:" >&2
    printf '  - %s\n' "${SKIPPED[@]}" >&2
}

# Lists the gates that passed with a warning, on stderr, and prints nothing when
# none did. The verdict is a pass; the list is what to act on before the push
# that is not one.
ubq_gate_report_warned() {
    [ "${#WARNED[@]}" -gt 0 ] || return 0
    echo "pre-push gates: ${#WARNED[@]} passed with a WARNING — act on each before a push is refused:" >&2
    printf '  - %s\n' "${WARNED[@]}" >&2
}

# Names the gates this checkout does not hold the subject of, so a list that is
# shorter here than in another repository says why rather than reading as
# coverage.
ubq_gate_report_elsewhere() {
    [ "${#ELSEWHERE[@]}" -gt 0 ] || return 0
    echo "pre-push gates: ${#ELSEWHERE[@]} gate(s) answer for a repository this checkout does not hold (${UBQ_REPOSITORIES:-all} here):" >&2
    printf '  - %s\n' "${ELSEWHERE[@]}" >&2
}
