#!/usr/bin/env bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

# check-rename-coverage.sh — what a semantic rename would actually see.
#
#   ./Scripts/check-rename-coverage.sh            # report
#   ./Scripts/check-rename-coverage.sh --require  # exit 1 if anything is dark
#
# A semantic rename (clang-refactor local-rename, clangd's textDocument/rename)
# rewrites exactly the translation units in the compile databases it is given,
# and exits 0 on everything else. So the databases are the tool's idea of "the
# code", and a file outside them is renamed by nobody and reported by nothing.
#
# Two ways that goes wrong:
#
# One FRONTEND is not the code. build/macos alone holds under half the TUs, and
# a rename driven from it leaves the rest untouched and succeeds. Every frontend
# needs its own configure, and this unions whatever it finds rather than
# trusting one.
#
# A DATABASE can name a FILESYSTEM that is not here. The Linux database is
# written inside a container against a bind mount, so it names `/src/Source/...`
# — well-formed, 172 entries, and not one path openable from the host. A tool
# pointed at it finds no files and exits 0, which reads exactly like a rename
# with nothing to do. Every entry is stat'd here, and unopenable ones are
# reported as their own category rather than silently missing from the union.
#
# Databases are DISCOVERED, never named. Two checkouts measuring this from
# differently-named build trees reach different counts; a path spelled in a
# script is a path another checkout will not have.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT" || exit 1

REQUIRE=0
[ "${1:-}" = "--require" ] && REQUIRE=1

# `mapfile` is bash 4; macOS ships 3.2, where it is not a syntax error but a
# missing command — the array stays unset and the script dies on the next line.
DATABASES=""
while IFS= read -r found; do
    DATABASES="$DATABASES $found"
done < <(find . -maxdepth 4 -name compile_commands.json -not -path "./Source/ThirdParty/*" 2>/dev/null | sort)

if [ -z "$DATABASES" ]; then
    echo "check-rename-coverage: no compile database anywhere." >&2
    echo "  Configure with -DCMAKE_EXPORT_COMPILE_COMMANDS=ON — a rename driven" >&2
    echo "  without one sees nothing and reports success." >&2
    exit 2
fi

python3 - "$REQUIRE" $DATABASES <<'PYEOF'
import json, os, subprocess, sys
from collections import Counter

require = sys.argv[1] == "1"
databases = sys.argv[2:]

covered, unopenable = set(), set()
print(f"{'database':<42}{'entries':>9}{'here':>8}{'absent':>8}")
print("-" * 67)
for path in databases:
    try:
        entries = json.load(open(path))
    except Exception as exc:
        print(f"{path:<42}  unreadable: {exc}")
        continue
    here = absent = 0
    for entry in entries:
        name = os.path.realpath(os.path.join(entry.get("directory", ""), entry["file"]))
        if os.path.exists(name):
            covered.add(name); here += 1
        else:
            unopenable.add(entry["file"]); absent += 1
    print(f"{path:<42}{len(entries):>9}{here:>8}{absent:>8}")

found = subprocess.run(
    ["find", "Source", "Tests", "-name", "*.cpp", "-o", "-name", "*.mm"],
    capture_output=True, text=True).stdout.split()
every = {os.path.realpath(p) for p in found if "ThirdParty" not in p}
dark = every - covered

print(f"\ncovered {len(every & covered)} of {len(every)} translation units")

if unopenable:
    print(f"\n{len(unopenable)} entries name paths that do not exist on this host.")
    print("A tool pointed at that database finds no files and exits 0 — the same")
    print("output as a rename with nothing to do. Sample:")
    for name in sorted(unopenable)[:3]:
        print(f"    {name}")

if dark:
    buckets = Counter()
    for name in dark:
        rel = os.path.relpath(name, os.getcwd())
        parts = rel.split(os.sep)
        buckets[os.sep.join(parts[:2]) if len(parts) > 1 else parts[0]] += 1
    print(f"\n{len(dark)} translation units are in NO database — a rename would")
    print("skip every one of them and report success:")
    for where, count in buckets.most_common(10):
        print(f"    {where:<28}{count:>5}")

if require and (dark or unopenable):
    print(f"check-rename-coverage: FAILED — {len(dark) + len(unopenable)} translation unit(s) "
          f"in no compilation database ({len(dark)} dark, {len(unopenable)} unopenable); "
          f"configure the missing frontends, or name each excluded unit before renaming anything",
          file=sys.stderr)
    sys.exit(1)
print("\ncheck-rename-coverage: every translation unit is in a database" if not (dark or unopenable) else "")
PYEOF
