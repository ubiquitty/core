#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Names the revision of a dependent repository that a cross-repository CI job builds.

A pull request to Core is built into the Kit and the app before it merges, and
one to the Kit into the app. Each such job builds the dependent repository's
main, unless the pull request names a pull request of that repository to pair
with, in a line of its description:

    Pairs-With: ubiquitty/kit#123

A Core change that needs a matching Kit change is then built against that Kit
change instead of failing against Kit main until both land. The line is read
from the pull request as it is when the job runs, not from the event that
started the run, so editing it and re-running the job pairs the run anew.

    python3 Scripts/ci_pairing.py --want ubiquitty/kit [--default-ref main]

Writes `repository`, `ref` and `pull-request` to $GITHUB_OUTPUT. Without a
pairing line the repository is the one wanted, `ref` is --default-ref (empty
when the caller keeps the revision it already has, such as a pin), and
`pull-request` is empty. A paired pull request contributes its head repository,
which may be a fork, and its head commit. A paired pull request that was closed
without merging is refused: building against a change nobody will land proves
nothing about what will. One that was merged pairs with main, which holds it.

The description is untrusted text. Only a line of exactly this shape is read,
the repository must be the one wanted, and the numbers and names it yields are
checked before anything is written. With no pull request (a push to main) the
dependent repository's main is built.
"""

import argparse
import json
import os
import re
import sys
import urllib.error
import urllib.request

PAIRING = re.compile(r"^Pairs-With:[ \t]*([A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+)#([1-9][0-9]{0,8})[ \t\r]*$", re.MULTILINE)
REPOSITORY = re.compile(r"^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$")
COMMIT = re.compile(r"^[0-9a-f]{40}$")


class PairingError(Exception):
    """A pairing line that names something this job cannot build."""


def pairings(body, wanted):
    """The pull request numbers the description pairs with in `wanted`, in order."""
    return [int(number) for repository, number in PAIRING.findall(body or "")
            if repository.lower() == wanted.lower()]


def choose(body, wanted, default_ref, read_pull_request):
    """The repository, ref and paired number the job builds.

    - Parameters:
      - body: The pull request's description, or None when there is no pull request.
      - wanted: The dependent repository, `owner/name`.
      - default_ref: The ref built without a pairing line; empty keeps the caller's.
      - read_pull_request: Returns the API object of `wanted`'s pull request number.
    """
    numbers = pairings(body, wanted)
    if len(set(numbers)) > 1:
        raise PairingError(f"the description pairs with {len(set(numbers))} pull requests of {wanted}; name one")
    if not numbers:
        return {"repository": wanted, "ref": default_ref, "pull-request": ""}
    number = numbers[0]
    paired = read_pull_request(number)
    if paired.get("merged_at"):
        return {"repository": wanted, "ref": "main", "pull-request": str(number)}
    if paired.get("state") != "open":
        raise PairingError(f"{wanted}#{number} was closed without merging; pair with an open pull request or none")
    head = paired.get("head") or {}
    repository = (head.get("repo") or {}).get("full_name") or ""
    commit = head.get("sha") or ""
    if not REPOSITORY.match(repository):
        raise PairingError(f"{wanted}#{number} has no head repository to build (a deleted fork?)")
    if not COMMIT.match(commit):
        raise PairingError(f"{wanted}#{number} names no head commit")
    return {"repository": repository, "ref": commit, "pull-request": str(number)}


def github_reader(environment):
    """Reads pull requests through the API the job runs against."""
    base = environment.get("GITHUB_API_URL", "https://api.github.com")
    token = environment.get("GITHUB_TOKEN", "")

    def read(path):
        headers = {"Accept": "application/vnd.github+json"}
        if token:
            headers["Authorization"] = "Bearer " + token
        request = urllib.request.Request(f"{base}/repos/{path}", headers=headers)
        with urllib.request.urlopen(request, timeout=20) as response:
            return json.load(response)

    return read


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--want", required=True, help="the dependent repository, owner/name")
    parser.add_argument("--default-ref", default="main", help="the ref built without a pairing line; empty keeps the caller's")
    arguments = parser.parse_args(argv)
    if not REPOSITORY.match(arguments.want):
        parser.error(f"--want must be owner/name, not {arguments.want!r}")
    environment = os.environ
    read = github_reader(environment)
    number = environment.get("PULL_REQUEST", "")
    body = None
    try:
        if number:
            if not re.fullmatch(r"[1-9][0-9]{0,8}", number):
                raise PairingError(f"PULL_REQUEST is not a pull request number: {number!r}")
            body = read(f"{environment['GITHUB_REPOSITORY']}/pulls/{number}").get("body")
        chosen = choose(body, arguments.want, arguments.default_ref,
                        lambda paired: read(f"{arguments.want}/pulls/{paired}"))
    except PairingError as error:
        print(f"::error title=ci-pairing::{error}", file=sys.stderr)
        return 1
    except (OSError, KeyError, ValueError, urllib.error.URLError) as error:
        print(f"::error title=ci-pairing::could not read the pairing: {type(error).__name__}: {error}", file=sys.stderr)
        return 1
    shown = chosen["ref"] or "the revision it pins"
    paired = f" (paired with {arguments.want}#{chosen['pull-request']})" if chosen["pull-request"] else ""
    print(f"ci-pairing: building {chosen['repository']} at {shown}{paired}")
    output = environment.get("GITHUB_OUTPUT")
    if output:
        with open(output, "a", encoding="utf-8") as stream:
            for key, value in chosen.items():
                stream.write(f"{key}={value}\n")
    summary = environment.get("GITHUB_STEP_SUMMARY")
    if summary:
        with open(summary, "a", encoding="utf-8") as stream:
            stream.write(f"Built against {chosen['repository']} at `{shown}`{paired}.\n\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
