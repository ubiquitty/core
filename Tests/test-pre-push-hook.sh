#!/usr/bin/env bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

# Tests/test-pre-push-hook.sh — pin the audited whole-hook bypass.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

. "$ROOT/Tests/shell-assertions.sh"
. "$ROOT/Tests/git-fixture.sh"

FIXTURE="$TMP/repo"
ubq_make_fixture_repo "$FIXTURE"
mkdir -p "$FIXTURE/Scripts/hooks"
cp "$ROOT/Scripts/hooks/pre-push" "$FIXTURE/Scripts/hooks/pre-push"
cp "$ROOT/Scripts/run-local-hooks.sh" "$FIXTURE/Scripts/run-local-hooks.sh"

printf '#!/usr/bin/env bash\necho gates-ran > "${TMP_MARK:?}"\nexit 99\n' > "$FIXTURE/Scripts/pre-push-gates.sh"
chmod +x "$FIXTURE/Scripts/hooks/pre-push" "$FIXTURE/Scripts/pre-push-gates.sh"

cd "$FIXTURE" || exit 1
export TMP_MARK="$TMP/mark"
refs='refs/heads/topic 111 refs/heads/topic 000'

printf '%s\n' "$refs" | bash Scripts/hooks/pre-push >/dev/null 2>"$TMP/err"
check "without the bypass, a failing gate fails the hook" "1" "$?"
mark="<missing>"
if [ -f "$TMP/mark" ]; then
    mark="$(<"$TMP/mark")"
fi
check "without the bypass, the gate body runs" "gates-ran" "$mark"

rm -f "$TMP/mark"
printf '%s\n' "$refs" | UBIQUITTY_SKIP_PRE_PUSH_HOOK=1 bash Scripts/hooks/pre-push >/dev/null 2>"$TMP/err"
check "with the bypass, the hook exits successfully" "0" "$?"
mark="<missing>"
if [ -f "$TMP/mark" ]; then
    mark="$(<"$TMP/mark")"
fi
check "with the bypass, the gate body stays untouched" "<missing>" "$mark"

export TMP_FOREIGN="$TMP/foreign"
mkdir -p "$TMP_FOREIGN"
cat > "$FIXTURE/Scripts/pre-push-gates.sh" <<'GATE'
#!/usr/bin/env bash
set -euo pipefail
git -C "$TMP_FOREIGN" init -q
echo foreign > "$TMP_FOREIGN/sentinel.txt"
git -C "$TMP_FOREIGN" add sentinel.txt
GATE
echo retained > "$FIXTURE/retained.txt"
git add retained.txt
index_before="$(git hash-object .git/index)"
printf '%s\n' "$refs" | GIT_DIR="$FIXTURE/.git" GIT_INDEX_FILE="$FIXTURE/.git/index" \
    bash Scripts/hooks/pre-push >/dev/null 2>"$TMP/err"
check "gates can stage files in an isolated repository" "0" "$?"
check "hook-local paths cannot redirect the foreign index" "$index_before" "$(git hash-object .git/index)"
check "the foreign repository owns its staged file" "sentinel.txt" "$(git -C "$TMP_FOREIGN" ls-files)"

echo
report "test-pre-push-hook" "the hook isolates Git paths and honors the audited bypass"
