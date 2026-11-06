#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""stamp-bench-baseline.py — write a captured run out as the committed baseline.

A baseline holds absolute, machine-specific numbers, so the numbers alone do not
say what they mean. Two things have to travel with them: which machine produced
them, and why they moved. `run-bench.sh` emits neither, so a plain copy loses
both at every refresh, and a file that records a lane at many times its
previous value with nothing beside it cannot be read as a win rather than a
broken capture.

Hardware carries forward from the file being replaced — the same machine, or the
capture is wrong for a larger reason. Provenance is written fresh: the previous
capture's note describes the previous capture, and re-dating it to this one is
the failure this script exists to prevent. What the run can observe it records;
the cause is passed in, because no measurement infers it.
"""

import argparse
import hashlib
import json
import os
import platform
from datetime import datetime, timezone

from pgo_localize import default_profile


def profile_identity(root: str) -> str:
    """Describes the PGO profile the measured binary was built against.

    A baseline built with a drifted profile and one built with a current profile
    are different measurements of the same source, and a single lane can differ
    between them by a large factor, so the profile is part of what a baseline
    number means.

    - Parameter root: The repository root.
    - Returns: A one-line description of the profile, or that there was none.
    """
    root = os.path.abspath(root)
    profile = os.environ.get("UBQ_PGO_PROFILE") or default_profile(root, platform.system(), platform.machine())
    if not os.path.isabs(profile):
        profile = os.path.join(root, profile)
    if os.environ.get("UBIQUITTY_PGO") == "OFF" or not os.path.exists(profile):
        return "none (UBIQUITTY_PGO=OFF or no selected profile)"
    with open(profile, "rb") as handle:
        digest = hashlib.sha1(handle.read()).hexdigest()[:12]
    trained = "unrecorded"
    sidecar = profile.removesuffix(".profdata") + ".trained-at"
    if os.path.exists(sidecar):
        with open(sidecar, encoding="utf-8") as handle:
            trained = handle.read().strip()[:12] or "unrecorded"
    return f"{digest}, trained at {trained}"


def load_at_capture() -> str:
    """The machine's load when the baseline is written.

    That is the load the run saw when `bench-gate.sh` drives this, since it
    stamps immediately after collecting. Stamping a capture separately reads a
    later machine, and the value has to be corrected by hand.

    - Returns: The three load averages, or that the platform does not report them.
    """
    try:
        one, five, fifteen = os.getloadavg()
    except OSError:
        return "unavailable on this platform"
    return f"{one:.2f} / {five:.2f} / {fifteen:.2f} (1/5/15 min)"


def main() -> int:
    """Merges a captured run into the committed baseline with its provenance.

    - Returns: Zero once the baseline is written.
    """
    parser = argparse.ArgumentParser()
    parser.add_argument("--current", required=True, help="the run just captured")
    parser.add_argument("--baseline", required=True, help="the committed baseline to replace")
    parser.add_argument("--root", required=True, help="repository root")
    parser.add_argument("--reason", required=True, help="why these numbers moved")
    arguments = parser.parse_args()

    with open(arguments.current, encoding="utf-8") as handle:
        fresh = json.load(handle)

    if os.path.exists(arguments.baseline):
        with open(arguments.baseline, encoding="utf-8") as handle:
            previous = json.load(handle)
        if "hardware" in previous:
            fresh["hardware"] = previous["hardware"]

    fresh["provenance"] = {
        "measuredOn": datetime.now(timezone.utc).strftime("%Y-%m-%d"),
        "loadAtCapture": load_at_capture(),
        "pgoProfile": profile_identity(arguments.root),
        "reason": arguments.reason,
    }

    ordered = {}
    for key in ("platform", "commit", "build", "hardware", "provenance"):
        if key in fresh:
            ordered[key] = fresh[key]
    for key, value in fresh.items():
        if key not in ordered:
            ordered[key] = value

    with open(arguments.baseline, "w", encoding="utf-8") as handle:
        json.dump(ordered, handle, indent=2)
        handle.write("\n")
    print(f"stamp-bench-baseline: {len(fresh.get('benchmarks', []))} lane(s), "
          f"profile {fresh['provenance']['pgoProfile']}, load {fresh['provenance']['loadAtCapture']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
