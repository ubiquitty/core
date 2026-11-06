#!/usr/bin/env bash
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

#
# Tests/Agent/ubq_smoke_cleanup.sh — a ubq_smoke the runner's limit ends leaves
# no daemon behind.
#
# Each daemon the smoke starts double-forks into a session of its own, so the
# runner's process-group kill never reaches it; only the smoke's own trap can
# stop it, and only if the runner signals before it kills. This starts the
# smoke under the runner's own bound (Scripts/run-bounded.sh) with a limit far
# shorter than the smoke, so the limit ends it mid-run, then looks for any
# ubiquitty-host whose home is under that run's temporary directory.
#
# Env: as Tests/Agent/ubq_smoke.sh (UBQ_BIN, UBIQUITTY_HOST_BIN,
# UBQ_CONSENT_PERSON_BIN).

set -uo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
# shellcheck source=Scripts/run-bounded.sh
. "$ROOT/Scripts/run-bounded.sh"

fail() { echo "ubq-smoke-cleanup: FAIL: $*" >&2; exit 1; }

PARENT="$(mktemp -d "/tmp/ubq-smoke-cleanup.XXXXXX")"
trap 'rm -rf "$PARENT"' EXIT

# The ubiquitty-host processes whose environment names a home under the
# parent, one PID a line. Linux reads /proc; macOS asks ps for the environment.
daemons_under() {
    python3 - "$1" <<'PY'
import os, subprocess, sys
parent = os.path.realpath(sys.argv[1])
found = []
def homes(environment):
    for entry in environment:
        name, _, value = entry.partition("=")
        if name in ("HOME", "UBIQUITTY_HOME") and value:
            yield os.path.realpath(value)
if os.path.isdir("/proc"):
    for pid in filter(str.isdigit, os.listdir("/proc")):
        try:
            command = open(f"/proc/{pid}/cmdline", "rb").read().split(b"\0")[0].decode(errors="replace")
            environment = open(f"/proc/{pid}/environ", "rb").read().decode(errors="replace").split("\0")
        except OSError:
            continue
        if os.path.basename(command) == "ubiquitty-host" and any(home.startswith(parent + "/") for home in homes(environment)):
            found.append(pid)
else:
    listing = subprocess.run(["ps", "-axww", "-E", "-o", "pid=,command="], capture_output=True, text=True).stdout
    for line in listing.splitlines():
        pid, _, rest = line.strip().partition(" ")
        words = rest.split()
        if words and os.path.basename(words[0]) == "ubiquitty-host" and any(home.startswith(parent + "/") for home in homes(words[1:])):
            found.append(pid)
print("\n".join(found))
PY
}

# Long enough that the smoke has started its daemon and is mid-run, far short
# of the smoke itself.
UBQ_SMOKE_TMP="$PARENT" ubq_run_bounded "${UBQ_SMOKE_CLEANUP_LIMIT_SECONDS:-30}" bash "${UBQ_SMOKE_SCRIPT:-$ROOT/Tests/Agent/ubq_smoke.sh}" >"$PARENT/smoke.log" 2>&1
rc=$?
[ "$rc" -eq 124 ] || fail "the smoke was not ended by the limit (exit $rc): $(tail -3 "$PARENT/smoke.log")"
grep -q '^ubq-smoke: ok:' "$PARENT/smoke.log" || fail "the smoke started no daemon before the limit: $(tail -3 "$PARENT/smoke.log")"
LEFT="$(daemons_under "$PARENT")"
if [ -n "$LEFT" ]; then
    # Stopped by exact PID, so this test leaves nothing behind either.
    for pid in $LEFT; do kill "$pid" 2>/dev/null; done
    fail "a smoke the limit ended left daemons under its home: $(echo $LEFT)"
fi
[ -z "$(find "$PARENT" -maxdepth 1 -name 'ubq-smoke.*' -print -quit)" ] || fail "the smoke the limit ended left its temporary directory behind"
echo "ubq-smoke-cleanup: ok: a smoke ended by the runner's limit stops every daemon it started and removes its directory"
