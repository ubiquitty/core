#!/usr/bin/env bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
source "$ROOT/Scripts/pre-push-cache.sh"
source "$ROOT/Tests/git-fixture.sh"

fail() {
    echo "test-pre-push-cache: FAILED — $1" >&2
    exit 1
}

fixture="$(mktemp -d)"
cache="$(mktemp -d)"
trap 'rm -rf "$fixture" "$cache"' EXIT
ubq_make_fixture_repo "$fixture"
printf 'one\n' > "$fixture/tracked.txt"
git -C "$fixture" add tracked.txt
git -C "$fixture" commit -qm initial
git -C "$fixture" update-ref refs/remotes/origin/main HEAD

# The switch a push uses to bypass the cache is documented for the caller's
# environment, and this test runs inside one: a gate run with the cache off
# would otherwise hand this test a cache that never initialises.
unset UBIQUITTY_PREPUSH_CACHE
export UBIQUITTY_PREPUSH_CACHE_DIR="$cache"
ubq_gate_cache_init "$fixture" quick
[ "$UBQ_GATE_CACHE_ENABLED" = 1 ] || fail "cache did not initialise"

first="$(ubq_gate_cache_key sample-gate command --flag)"
[ -n "$first" ] || fail "identical snapshot produced no key"
ubq_gate_cache_store "$first"
ubq_gate_cache_has "$first" || fail "successful gate was not reusable"

same="$(ubq_gate_cache_key sample-gate command --flag)"
[ "$same" = "$first" ] || fail "identical inputs changed the cache key"
different_command="$(ubq_gate_cache_key sample-gate command --other)"
[ "$different_command" != "$first" ] || fail "command arguments do not invalidate the cache"

original_path="$PATH"
PATH="$PATH:/ubiquitty-cache-test"
ubq_gate_cache_init "$fixture" quick
different_environment="$(ubq_gate_cache_key sample-gate command --flag)"
[ "$different_environment" != "$first" ] || fail "environment changes do not invalidate the cache"
PATH="$original_path"
ubq_gate_cache_init "$fixture" quick

printf 'two\n' > "$fixture/tracked.txt"
modified="$(ubq_gate_cache_key sample-gate command --flag)"
[ "$modified" != "$first" ] || fail "tracked edits do not invalidate the cache"
ubq_gate_cache_has "$modified" && fail "modified snapshot reused a passing verdict"

printf 'one\n' > "$fixture/tracked.txt"
printf 'untracked\n' > "$fixture/new.txt"
untracked="$(ubq_gate_cache_key sample-gate command --flag)"
[ "$untracked" != "$first" ] || fail "untracked files do not invalidate the cache"

# A gate that exits 0 after printing `<gate>: SKIPPED — <why>` checked nothing.
# A cached pass for it would outlive the installation of the tool it named, so it
# is reported as skipped and stores no verdict. The clean gate is the control: it
# proves `run` stores under the key computed here, and that SKIPPED in the middle
# of a line is prose rather than a stand-down.
source "$ROOT/Scripts/pre-push-runner.sh"
stubs="$(mktemp -d)"
trap 'rm -rf "$fixture" "$cache" "$stubs"' EXIT
printf '#!/usr/bin/env bash\necho "check-clean --self-test: ok (a SKIPPED fixture is classified)"\n' > "$stubs/clean-gate"
printf '#!/usr/bin/env bash\necho "reading inputs"\necho "check-stub: SKIPPED — stubtool is not installed"\necho "format-stub.sh: SKIPPED (no other-tool)"\n' > "$stubs/stood-down-gate"
chmod +x "$stubs/clean-gate" "$stubs/stood-down-gate"

clean_key="$(ubq_gate_cache_key "clean gate" "$stubs/clean-gate")"
stood_down_key="$(ubq_gate_cache_key "stood-down gate" "$stubs/stood-down-gate")"
run "clean gate" "$stubs/clean-gate" >/dev/null
run "stood-down gate" "$stubs/stood-down-gate" > "$stubs/outcome"
summary="$(ubq_gate_report_skipped 2>&1)"

ubq_gate_cache_has "$clean_key" || fail "a clean pass was not cached, so the stand-down case below proves nothing"
[ "${PASSED[*]}" = "clean gate" ] || fail "PASSED holds '${PASSED[*]}', not the clean gate alone"
ubq_gate_cache_has "$stood_down_key" && fail "a gate that stood down was cached as a pass"
case "$summary" in
    *"1 SKIPPED"*"  - stood-down gate (stubtool is not installed | no other-tool)"*) ;;
    *) fail "the summary does not list the stood-down gate with its reasons; it printed: $summary" ;;
esac
grep -q 'stood-down gate *SKIPPED (stubtool is not installed | no other-tool)' "$stubs/outcome" \
    || fail "the gate's own line does not say it was skipped; it printed: $(cat "$stubs/outcome")"

# A gate that exits 0 after printing `<gate>: WARNING — <why>` passed, and said
# what a later push is refused for. The pass is reported with the warning
# beside it and is not cached, so the line is printed on every push until it is
# acted on; cached, it would print once. A warning quoted mid-line is prose.
printf '#!/usr/bin/env bash\necho "check-stub: ok (a WARNING fixture is classified)"\necho "check-stub: WARNING — 180000 counters are 60%% of the budget"\n' > "$stubs/warned-gate"
chmod +x "$stubs/warned-gate"
warned_key="$(ubq_gate_cache_key "warned gate" "$stubs/warned-gate")"
run "warned gate" "$stubs/warned-gate" > "$stubs/warned-outcome"
warnings="$(ubq_gate_report_warned 2>&1)"

ubq_gate_cache_has "$warned_key" && fail "a gate that passed with a warning was cached, so the warning would print once"
case " ${PASSED[*]} " in
    *" warned gate "*) ;;
    *) fail "a gate that passed with a warning is not among the passes: '${PASSED[*]}'" ;;
esac
grep -q 'warned gate *ok, WARNING (180000 counters are 60% of the budget)' "$stubs/warned-outcome" \
    || fail "the gate's own line does not carry its warning; it printed: $(cat "$stubs/warned-outcome")"
case "$warnings" in
    *"1 passed with a WARNING"*"  - warned gate (180000 counters are 60% of the budget)"*) ;;
    *) fail "the summary does not list the warned gate with its reason; it printed: $warnings" ;;
esac

export UBIQUITTY_PREPUSH_CACHE=off
ubq_gate_cache_init "$fixture" quick
[ "$UBQ_GATE_CACHE_ENABLED" = 0 ] || fail "explicit cache disable was ignored"

echo "test-pre-push-cache: PASSED — successes reuse only an identical source, command, and environment; a stand-down is never cached, and a pass with a warning is printed with it and not cached"
