#!/usr/bin/env bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

# Exercises the committed-tree exporter without builds or network access.
# Rejected exports must preserve their destination and all source Git state.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$(cd "$HERE/.." && pwd)"
. "$HERE/git-fixture.sh"
ubq_isolate_git_environment
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
GIT=(git -c commit.gpgsign=false -c core.hooksPath=/dev/null)
fail() { echo "FAIL: $1" >&2; exit 1; }

fixture="$WORK/source"
ubq_make_fixture_repo "$fixture"
for directory in Source Tests Scripts Resources Tools Samples Support cmake Docs LICENSES .github; do
    mkdir -p "$fixture/$directory"
    echo fixture > "$fixture/$directory/fixture.txt"
done
for file in CMakeLists.txt README.md AGENTS.md LICENSE LICENSES/Apache-2.0.txt NOTICE TRADEMARKS.md third-party-notices.md license-scope.json \
        .gitignore .gitleaks.toml .clang-format .clang-tidy .swift-format .clangd .editorconfig; do
    echo fixture > "$fixture/$file"
done
echo 0.1.0 > "$fixture/VERSION"
cp "$SRC/Scripts/snapshot-clean.sh" "$fixture/Scripts/"
cp "$HERE/git-fixture.sh" "$fixture/Tests/"
cat > "$fixture/Scripts/check-licenses.py" <<'STUB'
#!/usr/bin/env python3
import os, sys
assert sys.argv[1:] == ["--release"]
raise SystemExit(int(os.environ.get("TEST_LICENSE_FAILURE", "0")))
STUB
cat > "$fixture/Scripts/check-personal-paths.py" <<'STUB'
#!/usr/bin/env python3
import os, sys
assert sys.argv[1:] == ["--staged"]
raise SystemExit(int(os.environ.get("TEST_PERSONAL_PATH_FAILURE", "0")))
STUB
chmod +x "$fixture/Scripts/"*
"${GIT[@]}" -C "$fixture" add -A
"${GIT[@]}" -C "$fixture" commit -qm fixture
head="$(git -C "$fixture" rev-parse HEAD)"
refs="$(git -C "$fixture" show-ref)"

mkdir "$WORK/bin"
cat > "$WORK/bin/gitleaks" <<'STUB'
#!/usr/bin/env bash
exit "${TEST_SECRET_FAILURE:-0}"
STUB
cat > "$WORK/bin/cmake" <<'STUB'
#!/usr/bin/env bash
printf '%s\n' "$*" >> "$SMOKE_CALLS"
STUB
chmod +x "$WORK/bin/"*
export PATH="$WORK/bin:$PATH"
export UBIQUITTY_SNAPSHOT_SKIP_BUILD=1
export SMOKE_CALLS="$WORK/smoke-calls"
unset UBQ_BUILD_JOBS UBIQUITTY_QUIC_PREFIX
script="$fixture/Scripts/snapshot-clean.sh"

reject() { # <expected diagnostic> <command...>
    local diagnostic="$1" output
    shift
    if output="$("$@" 2>&1)"; then
        fail "unexpected success: $*"
    fi
    if ! printf '%s\n' "$output" | grep -Fq -- "$diagnostic"; then
        printf '%s\n' "$output" >&2
        fail "missing diagnostic: $diagnostic"
    fi
}

output="$("$script")"
printf '%s\n' "$output" | grep -q 'Dry run complete' || fail "dry run not reported"
[ "$(git -C "$fixture" rev-parse HEAD)" = "$head" ] || fail "dry run moved HEAD"
[ "$(git -C "$fixture" show-ref)" = "$refs" ] || fail "dry run changed refs"
echo "ok: dry run preserves Git history"

echo uncommitted > "$fixture/README.md"
echo untracked > "$fixture/Source/untracked.txt"
"$script" --output "$WORK/export with spaces" > "$WORK/export.log"
[ "$(cat "$WORK/export with spaces/README.md")" = fixture ] || fail "export used working tree bytes"
[ -f "$WORK/export with spaces/AGENTS.md" ] || fail "public engineering instructions omitted"
cmp "$fixture/license-scope.json" "$WORK/export with spaces/license-scope.json" \
    || fail "file-license metadata omitted or changed"
[ ! -e "$WORK/export with spaces/Source/untracked.txt" ] || fail "untracked file exported"
[ ! -e "$WORK/export with spaces/.git" ] || fail "Git metadata exported"
echo "ok: export includes only committed source files"

mkdir "$WORK/existing"
echo sentinel > "$WORK/existing/sentinel"
echo sentinel > "$WORK/existing-file"
ln -s "$WORK/existing" "$WORK/existing-link"
ln -s "$WORK/absent" "$WORK/broken-link"
for target in existing existing-file existing-link broken-link; do
    reject 'output already exists' "$script" --output "$WORK/$target"
done
[ "$(cat "$WORK/existing/sentinel")" = sentinel ] || fail "existing directory changed"
[ "$(cat "$WORK/existing-file")" = sentinel ] || fail "existing file changed"
[ -L "$WORK/existing-link" ] && [ -L "$WORK/broken-link" ] || fail "symlink changed"
reject 'output parent must exist' "$script" --output "$WORK/missing/child"
for option in --commit --push --unknown; do
    reject 'unsupported option' "$script" "$option"
done
reject 'needs a new directory' "$script" --output
reject 'needs a directory, not an option' "$script" --output --push
reject 'specified twice' "$script" --output "$WORK/a" --output "$WORK/b"
echo "ok: existing destinations and publishing options are refused"

for failure in TEST_LICENSE_FAILURE TEST_SECRET_FAILURE TEST_PERSONAL_PATH_FAILURE; do
    reject 'ERROR:' env "$failure=1" "$script" --output "$WORK/rejected"
    [ ! -e "$WORK/rejected" ] || fail "failed check left an export"
done
echo "ok: validation failures leave destination absent"

# Every malformed tree is a small independent repository, so a failed case
# cannot modify the source fixture or a real checkout through borrowed objects.
variant() { # <name>
    git clone -q --local "$fixture" "$WORK/$1"
    git -C "$WORK/$1" config user.email test@example.com
    git -C "$WORK/$1" config user.name Test
}
commitVariant() { # <name>
    "${GIT[@]}" -C "$WORK/$1" add -A
    "${GIT[@]}" -C "$WORK/$1" commit -qm variant
}
variant missing
rm "$WORK/missing/NOTICE"
commitVariant missing
reject 'manifest paths absent' "$WORK/missing/Scripts/snapshot-clean.sh" --output "$WORK/missing-export"
[ ! -e "$WORK/missing-export" ] || fail "missing manifest path produced export"
variant empty
: > "$WORK/empty/NOTICE"
commitVariant empty
reject 'required file missing/empty' "$WORK/empty/Scripts/snapshot-clean.sh"
echo "ok: incomplete trees cannot be exported"

variant links
for badLink in '[missing](absent.md)' '[guide]: absent.md' '<a href="absent.md">guide</a>' '[escape](../../absent.md)'; do
    printf '%s\n' "$badLink" > "$WORK/links/Docs/guide.md"
    commitVariant links
    reject 'BROKEN LINK' "$WORK/links/Scripts/snapshot-clean.sh" --output "$WORK/link-export"
    [ ! -e "$WORK/link-export" ] || fail "broken link produced export"
done
cat > "$WORK/links/Docs/guide.md" <<'LINKS'
[readme](../README.md#fixture)
[web](https://example.com)
`[example](absent.md)`
<!-- [example](absent.md) -->
```md
[example](absent.md)
```
LINKS
commitVariant links
"$WORK/links/Scripts/snapshot-clean.sh" > "$WORK/links.log"
echo "ok: rendered links are validated and literal examples are ignored"

foreign="$WORK/foreign"
ubq_make_fixture_repo "$foreign"
echo sentinel > "$foreign/sentinel"
"${GIT[@]}" -C "$foreign" add -A
"${GIT[@]}" -C "$foreign" commit -qm sentinel
foreignHead="$(git -C "$foreign" rev-parse HEAD)"
foreignIndex="$(git -C "$foreign" write-tree)"
env GIT_DIR="$foreign/.git" GIT_WORK_TREE="$foreign" GIT_INDEX_FILE="$foreign/.git/index" \
    "$script" --output "$WORK/isolated-export" > "$WORK/isolation.log"
[ "$(git -C "$foreign" rev-parse HEAD)" = "$foreignHead" ] || fail "foreign HEAD changed"
[ "$(git -C "$foreign" write-tree)" = "$foreignIndex" ] || fail "foreign index changed"
[ "$(cat "$WORK/isolated-export/README.md")" = fixture ] || fail "foreign files exported"
[ "$(git -C "$fixture" rev-parse HEAD)" = "$head" ] || fail "source HEAD changed"
[ "$(git -C "$fixture" show-ref)" = "$refs" ] || fail "source refs changed"
[ "$(cat "$fixture/README.md")" = uncommitted ] || fail "uncommitted source lost"
echo "ok: inherited Git environment cannot redirect the export"

env UBIQUITTY_SNAPSHOT_SKIP_BUILD=0 "$script" > "$WORK/build.log"
[ "$(grep -c -- '--parallel 1' "$SMOKE_CALLS")" = 2 ] || fail "build smoke is not single-job"
echo "ok: both smoke builds default to one job"
echo "PASS: snapshot-clean export validation and destination preservation"
