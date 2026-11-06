#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""merge-pr.py — rebase-merge a pull request and cancel its branch's remaining runs.

A merged branch's remaining jobs answer a question nobody is asking: the branch
landed, and whatever they report cannot change that. Left alone they keep
queueing and spending Actions minutes. `concurrency` does not stop them, because
the merge starts its run on `main`, a different ref. This cancels them from the
client as part of the merge. `.github/workflows/cancel-closed-pr.yml` is the
backstop for a merge made in the web interface, where there is no client to do
it.

    Tools/merge-pr.py <number>            merge that pull request, then cancel
    Tools/merge-pr.py                     the pull request for the current branch
    Tools/merge-pr.py --cancel-only       cancel the branch's runs, merge nothing
    Tools/merge-pr.py <number> --dry-run  print what would happen

Requires `gh` authenticated for the repository, which is how every other tool
here reaches GitHub.
"""

import argparse
import json
import subprocess
import sys


def gh(arguments, check=True):
    """Runs `gh` and returns its stdout.

    - Parameters:
      - arguments: The argument list after `gh`.
      - check: Whether a non-zero exit raises.
    - Returns: Standard output with trailing whitespace removed.
    """
    finished = subprocess.run(["gh", *arguments], capture_output=True, text=True, check=False)
    if check and finished.returncode != 0:
        message = (finished.stderr or finished.stdout).strip()
        raise SystemExit(f"merge-pr: gh {' '.join(arguments)} failed: {message}")
    return finished.stdout.strip()


def pullRequest(number):
    """The pull request's number, head branch and state.

    - Parameter number: The pull request number, or None for the current branch.
    - Returns: A dict with `number`, `headRefName` and `state`.
    """
    arguments = ["pr", "view", "--json", "number,headRefName,state"]
    if number is not None:
        arguments.insert(2, str(number))
    return json.loads(gh(arguments))


def unfinishedRuns(branch):
    """Every run on `branch` that still holds or waits for a runner.

    - Parameter branch: The branch name, as the pull request spells it.
    - Returns: A list of (id, workflow name) pairs, newest first.
    """
    listing = gh(["run", "list", "--branch", branch, "--limit", "50",
                  "--json", "databaseId,status,workflowName"])
    runs = json.loads(listing) if listing else []
    return [(run["databaseId"], run["workflowName"])
            for run in runs if run["status"] in ("queued", "in_progress", "waiting", "requested")]


def cancelRuns(branch, dryRun):
    """Cancels the unfinished runs on `branch` and says what it did.

    A run that finishes between the listing and the call is reported rather
    than raising: the point is to stop the run, and one that finished on its
    own is the outcome this wanted.

    - Parameters:
      - branch: The branch whose runs to cancel.
      - dryRun: Whether to print the calls instead of making them.
    - Returns: The number of runs cancelled.
    """
    runs = unfinishedRuns(branch)
    if not runs:
        print(f"merge-pr: no unfinished run on {branch}")
        return 0
    cancelled = 0
    for identifier, workflow in runs:
        if dryRun:
            print(f"merge-pr: would cancel {identifier} ({workflow})")
            continue
        finished = subprocess.run(["gh", "run", "cancel", str(identifier)],
                                  capture_output=True, text=True, check=False)
        if finished.returncode == 0:
            print(f"merge-pr: cancelled {identifier} ({workflow})")
            cancelled += 1
        else:
            print(f"merge-pr: {identifier} ({workflow}) finished before it could be cancelled")
    return cancelled


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("number", nargs="?", type=int,
                        help="the pull request number; omitted, the current branch's")
    parser.add_argument("--cancel-only", action="store_true",
                        help="cancel the branch's runs without merging")
    parser.add_argument("--dry-run", action="store_true",
                        help="print what would happen and change nothing")
    arguments = parser.parse_args()

    request = pullRequest(arguments.number)
    branch = request["headRefName"]
    number = request["number"]

    if arguments.cancel_only:
        cancelRuns(branch, arguments.dry_run)
        return 0

    if request["state"] != "OPEN":
        print(f"merge-pr: #{number} is {request['state']}, so only its runs are cancelled")
        cancelRuns(branch, arguments.dry_run)
        return 0

    if arguments.dry_run:
        print(f"merge-pr: would rebase-merge #{number} ({branch}) and delete the branch")
        cancelRuns(branch, True)
        return 0

    # Rebase, because this repository's history is linear and a merge commit
    # would be the one thing here that is not.
    #
    # A non-zero exit here does not mean the merge failed. `gh` deletes the
    # local branch last, and that step fails when a worktree has it checked
    # out — so the status reports a landed merge as a failure, which would have
    # a caller push again or go looking for a conflict. The pull request itself
    # is asked what happened rather than the exit status inferred from.
    merged = subprocess.run(["gh", "pr", "merge", str(number), "--rebase", "--delete-branch"],
                            capture_output=True, text=True, check=False)
    if merged.returncode != 0:
        state = json.loads(gh(["pr", "view", str(number), "--json", "state"]))["state"]
        if state != "MERGED":
            message = (merged.stderr or merged.stdout).strip()
            raise SystemExit(f"merge-pr: #{number} is {state} and gh said: {message}")
        print(f"merge-pr: merged #{number} ({branch}); "
              f"gh also reported: {(merged.stderr or '').strip().splitlines()[-1]}")
    else:
        print(f"merge-pr: merged #{number} ({branch})")
    # After the merge, so the listing cannot race a run the merge itself
    # started on the branch.
    cancelRuns(branch, False)
    return 0


if __name__ == "__main__":
    sys.exit(main())
