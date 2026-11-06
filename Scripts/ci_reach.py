#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Computes CI reach against a fully successful platform-battery checkpoint.

A failed or cancelled battery did not execute all of its stages. It cannot
become the diff baseline for a later push. An absent baseline or API failure
runs the full batteries; the immediate parent is not a safe substitute.
Scheduled sweeps stand down for a completed verdict on this exact tip,
including a known failure, without calling that failure complete coverage.
A sweep that stands down also skips preflight's own battery, and the CI image
it would build first: the run that gave this tip its verdict ran them. It
concludes as that run did, so a tip that failed a gate stays red, and the next
sweep reads the same verdict from it instead of a pass.
"""

import json
import os
import re
import subprocess
import sys
import time
import urllib.parse
import urllib.request


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
    reaching = [path for path in paths if not SKIPPABLE.search(path)]
    if not reaching:
        return all_reach(False)
    return {"builds": True,
            "linux": not all(APPLE_ONLY.search(path) for path in reaching),
            "apple": not all(LINUX_ONLY.search(path) for path in reaching),
            "cmake": any(CMAKE.search(path) for path in reaching),
            "code": not all(path.startswith("Tests/Snapshots/golden/") for path in reaching)}


def indexed_jobs(jobs):
    # An ambiguous duplicate job name is not evidence of a complete battery.
    selected = [job for job in jobs if job.get("name") in JOBS]
    if len({job["name"] for job in selected}) != len(selected):
        return {}
    return {job["name"]: job for job in selected}


def is_full_checkpoint(jobs):
    indexed = indexed_jobs(jobs)
    if any(indexed.get(name, {}).get("conclusion") != "success" for name in JOBS):
        return False
    return all(any(step.get("name") == marker and step.get("conclusion") == "success"
                   for step in indexed[name].get("steps", []))
               for name, marker in FULL_MARKERS.items())


FAILED = "failed gate; later stages may be unexecuted"
PASSED = "all required batteries succeeded or were outside change reach"


def tip_verdict(jobs):
    indexed = indexed_jobs(jobs)
    if any(indexed.get(name, {}).get("conclusion") == "failure" for name in JOBS):
        return FAILED
    if (indexed.get("CI preflight", {}).get("conclusion") == "success"
            and all(indexed.get(name, {}).get("conclusion") in ("success", "skipped")
                    for name in FULL_MARKERS)):
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


def git_output(*arguments):
    return subprocess.check_output(["git", *arguments], text=True, stderr=subprocess.DEVNULL)


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


def main():
    reach, reason = decide(os.environ)
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
    sys.exit(main())
