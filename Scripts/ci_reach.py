#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Computes CI reach against a fully successful platform-battery checkpoint.

A failed or cancelled battery did not execute all of its stages. It cannot
become the diff baseline for a later push. An absent baseline or API failure
runs the full batteries; the immediate parent is not a safe substitute.
Scheduled sweeps stand down for a completed verdict on this exact tip,
including a known failure, without calling that failure complete coverage.
A battery runs as one job of its own name or as a matrix of jobs named
`<battery> (<group>)`; it is complete only when every one of its jobs is.
A sweep that stands down also skips preflight's own battery, and the CI image
it would build first: the run that gave this tip its verdict ran them. It
concludes as that run did, so a tip that failed a gate stays red, and the next
sweep reads the same verdict from it instead of a pass.
"""

import argparse
import json
import os
import re
import subprocess
import sys
import time
import urllib.parse
import urllib.request

from ubq_roots import source_roots


OUTPUTS = ("builds", "linux", "apple", "cmake", "code")
JOBS = ("CI preflight", "Linux battery", "Apple battery")
FULL_MARKERS = {"Linux battery": "Full Linux battery completed",
                "Apple battery": "Full Apple battery completed"}
SKIPPABLE = re.compile(r"^Docs/|\.md$|^Scripts/check-[a-z0-9-]+\.(py|sh)$|^\.github/ISSUE_TEMPLATE/|^Tests/coverage/covering-tests\.tsv$")
APPLE_ONLY = re.compile(r"^Source/Platform/(macOS|iOS)/.*\.(mm|m|swift|metal|xib|storyboard|strings|entitlements|sdef|plist\.in)$|^Source/Platform/(macOS|iOS)/.*\.xcassets/|^Source/UbiquittyRenderKit/|^Source/UbiquittyKit/Swift/|^Tests/Snapshots/(known-failing|exempt)-(macos|swift|ios)\.txt$|^Tests/Snapshots/golden/native/coretext/")
LINUX_ONLY = re.compile(r"^Source/Platform/(Linux|Wscons|Web|Wasm)/|^Tests/Web/|^Tests/Linux/|^Tests/Platform/test_linux_|^Tests/Snapshots/(known-failing|exempt)-(linux|web)\.txt$|^Tests/Snapshots/golden/native/freetype/")
CMAKE = re.compile(r"(^|/)CMakeLists\.txt$|\.cmake$|^CMake/|^Scripts/generate-xcode-workspace\.sh$|^Ubiquitty(IOS)?\.xcworkspace/")


def all_reach(value=True):
    return dict.fromkeys(OUTPUTS, value)


def changed_reach(paths):
    if not paths:
        return all_reach()
    if any(path == mount or path.startswith(mount + "/")
           for path in paths for mount in ("Libraries/ubiquitty-core", "Libraries/ubiquitty-kit")):
        # A consumer pin is not a proof of which library capabilities moved.
        # Do not reuse a partial App checkpoint for an unseen dependency tree.
        return all_reach()
    reaching = [path for path in paths if not SKIPPABLE.search(path)]
    if not reaching:
        return all_reach(False)
    return {"builds": True,
            "linux": not all(APPLE_ONLY.search(path) for path in reaching),
            "apple": not all(LINUX_ONLY.search(path) for path in reaching),
            "cmake": any(CMAKE.search(path) for path in reaching),
            "code": not all(path.startswith("Tests/Snapshots/golden/") for path in reaching)}


def battery_of(name):
    """The battery a job belongs to: its own name, or the one before a matrix group's suffix.

    Only a battery may be split, so a preflight job named with a group suffix
    belongs to nothing and cannot stand in for the preflight.
    """
    if name in JOBS:
        return name
    battery, separator, group = name.partition(" (")
    if separator and group.endswith(")") and len(group) > 1 and battery in FULL_MARKERS:
        return battery
    return None


def indexed_jobs(jobs):
    """Each battery's jobs, or nothing when two jobs share a name.

    An ambiguous duplicate job name is not evidence of a complete battery.
    """
    selected = [job for job in jobs if battery_of(job.get("name") or "")]
    if len({job["name"] for job in selected}) != len(selected):
        return {}
    indexed = {}
    for job in selected:
        indexed.setdefault(battery_of(job["name"]), []).append(job)
    return indexed


def concluded(indexed, name, conclusions):
    """Whether the battery ran at least one job and every one concluded as allowed."""
    group = indexed.get(name, [])
    return bool(group) and all(job.get("conclusion") in conclusions for job in group)


def is_full_checkpoint(jobs):
    indexed = indexed_jobs(jobs)
    if not all(concluded(indexed, name, ("success",)) for name in JOBS):
        return False
    return all(any(step.get("name") == marker and step.get("conclusion") == "success"
                   for step in job.get("steps", []))
               for name, marker in FULL_MARKERS.items() for job in indexed[name])


FAILED = "failed gate; later stages may be unexecuted"
PASSED = "all required batteries succeeded or were outside change reach"


def tip_verdict(jobs):
    indexed = indexed_jobs(jobs)
    if any(job.get("conclusion") == "failure" for group in indexed.values() for job in group):
        return FAILED
    if (concluded(indexed, "CI preflight", ("success",))
            and all(concluded(indexed, name, ("success", "skipped")) for name in FULL_MARKERS)):
        return PASSED
    return ""


class GitHub:
    def __init__(self, environment):
        self.base = environment["GITHUB_API_URL"] + "/repos/" + environment["GITHUB_REPOSITORY"] + "/"
        self.token = environment["GITHUB_TOKEN"]
        self.deadline = time.monotonic() + 60

    def get(self, path):
        remaining = self.deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError("CI history lookup exceeded its time budget")
        request = urllib.request.Request(self.base + path, headers={
            "Authorization": "Bearer " + self.token,
            "Accept": "application/vnd.github+json"})
        with urllib.request.urlopen(request, timeout=min(20, remaining)) as response:
            return json.load(response)

    def runs(self, branch):
        query = urllib.parse.urlencode({"branch": branch, "status": "completed", "per_page": 30})
        return [run for run in self.get("actions/workflows/ci.yml/runs?" + query)["workflow_runs"]
                if run.get("event") in ("push", "schedule", "workflow_dispatch")]

    def jobs(self, run):
        return self.get(f"actions/runs/{run['id']}/jobs?per_page=100")["jobs"]


def select_history(runs, head, get_jobs, sweep=False):
    for run in runs:
        if sweep and run["head_sha"] == head:
            verdict = tip_verdict(get_jobs(run))
            if verdict:
                return "", verdict
    for run in runs:
        if run["head_sha"] == head or run.get("conclusion") != "success":
            continue
        if is_full_checkpoint(get_jobs(run)):
            return run["head_sha"], ""
    return "", ""


def git_output(*arguments, cwd=None, environment=None):
    return subprocess.check_output(["git", *arguments], cwd=cwd, env=environment,
                                   text=True, stderr=subprocess.DEVNULL)


def decide(environment, github_factory=GitHub, git=git_output):
    event = environment.get("GITHUB_EVENT_NAME", "workflow_dispatch")
    if event == "workflow_dispatch":
        return all_reach(), "requested by hand: full batteries"
    base = environment.get("BASE_SHA", "")
    if event in ("push", "schedule"):
        try:
            github = github_factory(environment)
            base, verdict = select_history(github.runs(environment["GITHUB_REF_NAME"]),
                                          environment["GITHUB_SHA"], github.jobs, event == "schedule")
        except (OSError, ValueError, KeyError) as error:
            return all_reach(), f"history unavailable ({type(error).__name__}): full batteries"
        if verdict:
            # The run carries the verdict it stood down for: a sweep that ran
            # no gate and concluded green would read as a pass to the next one.
            standing = "failure" if verdict == FAILED else "success"
            return ({**all_reach(False), "preflight": False, "standing": standing},
                    f"sweep stands down: this tip has a {verdict}")
    if not base:
        return all_reach(), "no fully successful checkpoint: full batteries"
    try:
        git("cat-file", "-e", base + "^{commit}")
        paths = git("diff", "--name-only", base, "HEAD").splitlines()
    except subprocess.CalledProcessError:
        return all_reach(), "baseline unavailable in checkout: full batteries"
    reach = changed_reach(paths)
    names = ", ".join(key for key, value in reach.items() if value)
    return reach, f"against {base[:12]}: {names or 'preflight only'} ({len(paths)} changed paths)"


def main(argv=()):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root")
    parser.add_argument("--core-source-root")
    parser.add_argument("--kit-source-root")
    arguments = parser.parse_args(argv)
    try:
        roots = source_roots(arguments.source_root, arguments.core_source_root, arguments.kit_source_root)
    except ValueError as error:
        parser.error(str(error))
    environment = roots.environment(os.environ)
    reach, reason = decide(environment, git=lambda *args: git_output(*args, cwd=roots.subject, environment=environment))
    print("CI reach: " + reason)
    if "history unavailable" in reason:
        print("::warning title=CI reach::Could not establish the prior verdict; running full batteries")
    rendered = "".join(f"{key}={str(reach[key]).lower()}\n" for key in OUTPUTS)
    rendered += f"preflight={str(reach.get('preflight', True)).lower()}\n"
    rendered += f"standing={reach.get('standing', '')}\n"
    with open(os.environ["GITHUB_OUTPUT"], "a", encoding="utf-8") as output:
        output.write(rendered)
    if os.environ.get("GITHUB_STEP_SUMMARY"):
        with open(os.environ["GITHUB_STEP_SUMMARY"], "a", encoding="utf-8") as summary:
            summary.write("### CI reach\n\n" + reason + ".\n\n")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
