#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""check-census-claims.py — a count of the tree, written in prose, still holds.

A comment that counts something the tree contains is a claim with an expiry
nobody sets. A Swift module gate said "the eighteen files compile
together" and the module had reached thirty-nine; a reader who timed the gate
saw more than double the stated three seconds, and nothing in the comment said
whether the gate had grown or changed subject. The number was right when it
was written, which is the whole difficulty: nothing re-reads it, and the code it
describes moves underneath it.

Two counts are not this defect and are not asked about. A budget lives in code
and the gate holding it already fails in both directions, which is the shape a
number that decides something should have; this check exists to push the others
toward it. Small numbers are rhetoric: "a rule that fires on one file" counts nothing.
The threshold below is where a number stops reading as a turn of phrase and
starts reading as a census.

A claim can be settled three ways, and the gate wants one of them:

  DERIVED    `eighteen files [= Source/Core/Session/*.h]`
             The pathspec is re-globbed here and the count must match. This is
             the only form that cannot rot, because the tree answers it.

  HISTORICAL `the module was eighteen files when this was written`
             Explicitly past. A reader knows not to trust it as current, which
             is all a historical number has to do.

  DATED      `measured 2026-08-03: 260 cases in 4.5s`
             A timing is not re-derivable from the tree at all, so the date is
             what makes it readable — it says how old the evidence is.

Anything else is an undated census, and those are counted against a budget that
may only fall.

    python3 Scripts/check-census-claims.py
    python3 Scripts/check-census-claims.py --list
    python3 Scripts/check-census-claims.py --self-test
"""

import pathlib
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ubq_roots

REPO = ubq_roots.subject_root()

# Undated censuses still in prose, as the ledger sets them: budget
# `census-undated` of Scripts/gate-ledger.<repository>.txt. Two-sided: one
# arriving fails, and one resolved has to write the smaller number down. A list
# of exempt files would be a standing exemption for every name on it; a number
# cannot exempt anything.

WORDS = {
    "five": 5, "six": 6, "seven": 7, "eight": 8, "nine": 9, "ten": 10,
    "eleven": 11, "twelve": 12, "thirteen": 13, "fourteen": 14, "fifteen": 15,
    "sixteen": 16, "seventeen": 17, "eighteen": 18, "nineteen": 19,
    "twenty": 20, "thirty": 30, "forty": 40, "fifty": 50,
}

# Below this a number is a turn of phrase, not a census. "one file", "two cases"
# and "three gates" count nothing about the tree; they describe a shape.
CENSUS_FLOOR = 5

NOUNS = (r"files?|gates?|scripts?|cases?|tests?|rules?|sections?|targets?|"
         r"entries|callers?|sites?|declarations?|headers?|modules?|patterns?")

CENSUS = re.compile(
    r"\b(?P<count>%s|[0-9]{1,5})\s+(?P<noun>%s)\b" % ("|".join(WORDS), NOUNS),
    re.IGNORECASE)

# `[= <pathspec>]` immediately after the noun: the tree answers the claim.
DERIVATION = re.compile(r"^\s*\[=\s*(?P<pathspec>[^\]]+)\]")

# Past tense in the same sentence marks the number as history rather than a
# reading of the tree now.
HISTORICAL = re.compile(
    r"\b(was|were|had|used to|when this was written|at the time|since grown|"
    r"has since|then|originally)\b", re.IGNORECASE)

DATED = re.compile(r"\b20[0-9]{2}-[0-9]{2}-[0-9]{2}\b")

COMMENT = re.compile(r"(?:^|\s)(?:#|//)\s?(?P<text>.*)$")

SUFFIXES = (".sh", ".py")


def tracked():
    """Every gate script this check reads.

    - Returns: repository-relative paths under Scripts/.
    """
    # The index plus what is untracked, narrowed to what is on disk: a census of
    # the tree that read the index would count a script deleted in this change
    # and miss one added by it, and be wrong in both directions at once.
    listing = subprocess.run(
        ["git", "-C", REPO, "ls-files", "--cached", "--others", "--exclude-standard", "Scripts"],
        capture_output=True, text=True)
    return [p for p in listing.stdout.split("\n")
            if p.endswith(SUFFIXES) and "ThirdParty" not in p
            and (pathlib.Path(REPO) / p).exists()]


def globCount(pathspec):
    """How many tracked files a pathspec names, or None when git cannot answer.

    - Parameter pathspec: a git pathspec, as written in the claim.
    - Returns: the count, or None.
    """
    listing = subprocess.run(
        ["git", "-C", REPO, "ls-files", "--", pathspec],
        capture_output=True, text=True)
    if listing.returncode != 0:
        return None
    return sum(1 for line in listing.stdout.split("\n") if line)


def claims(text):
    """Every census claim in one file's comments.

    - Parameter text: the file's contents.
    - Returns: a list of (line number, count, noun, settled, pathspec).
    """
    found = []
    lines = text.split("\n")
    for number, line in enumerate(lines, 1):
        comment = COMMENT.search(line)
        if not comment:
            continue
        prose = comment.group("text")
        # Prose wraps, so a claim and its pathspec often land on different
        # comment lines. The next line SETTLES a claim but never carries one:
        # searching the joined text for claims too found every one of them
        # twice, on its own line and again on the line above.
        following = COMMENT.search(lines[number]) if number < len(lines) else None
        continuation = (" " + following.group("text")) if following else ""
        for match in CENSUS.finditer(prose):
            raw = match.group("count").lower()
            count = WORDS.get(raw)
            if count is None:
                if not raw.isdigit():
                    continue
                count = int(raw)
            if count < CENSUS_FLOOR:
                continue
            trailing = prose[match.end():] + continuation
            derivation = DERIVATION.match(trailing)
            if derivation:
                found.append((number, count, match.group("noun"), "derived",
                              derivation.group("pathspec").strip()))
            elif HISTORICAL.search(prose + continuation) or DATED.search(prose):
                found.append((number, count, match.group("noun"), "settled", None))
            else:
                found.append((number, count, match.group("noun"), "undated", None))
    return found


def selfTest():
    """Each way of settling a claim is recognised, and an undated one is not.

    - Returns: 0 when every case holds, 1 otherwise.
    """
    cases = [
        ("# the eighteen files compile together", "undated"),
        ("# eighteen files [= Scripts/*.py]", "derived"),
        ("# the module was eighteen files when this was written", "settled"),
        ("# measured 2026-08-03: 260 cases in 4.5s", "settled"),
        ("# a rule that fires on one file", None),          # below the floor
        ("# two gates disagree", None),                      # below the floor
        ("# 997 files carry prose", "undated"),
        ("int eighteen_files = 0;", None),                   # not a comment
    ]
    failures = []
    for line, expected in cases:
        found = claims(line)
        actual = found[0][3] if found else None
        if actual != expected:
            failures.append("%r settled as %r, wanted %r" % (line, actual, expected))

    # A derived claim whose pathspec disagrees must be caught, not just parsed.
    derived = claims("# five gates [= Scripts/check-style.py]")
    if not derived or derived[0][3] != "derived":
        failures.append("a derived claim was not recognised")
    else:
        actual = globCount(derived[0][4])
        if actual != 1:
            failures.append("Scripts/check-style.py globbed to %r, wanted 1" % actual)
        elif derived[0][1] == actual:
            failures.append("a wrong derived count compared equal")

    for failure in failures:
        print("check-census-claims --self-test: " + failure, file=sys.stderr)
    if failures:
        return 1
    print("check-census-claims --self-test: ok (%d case(s); derived, historical and "
          "dated each settle a claim, and rhetoric below %d is not one)"
          % (len(cases), CENSUS_FLOOR))
    return 0


def main(argv):
    if "--self-test" in argv:
        return selfTest()

    paths = tracked()
    if not paths:
        print("check-census-claims: REFUSED — git listed no script under Scripts/, so "
              "nothing was read.\n  A run that examined no file reports the same "
              "verdict as a clean tree.", file=sys.stderr)
        return 2

    wrong, undated = [], []
    for path in paths:
        try:
            with open(os.path.join(REPO, path), errors="replace") as handle:
                text = handle.read()
        except OSError:
            continue
        for number, count, noun, settled, pathspec in claims(text):
            if settled == "derived":
                actual = globCount(pathspec)
                if actual is None:
                    wrong.append((path, number, count, noun, pathspec, "unreadable"))
                elif actual != count:
                    wrong.append((path, number, count, noun, pathspec, actual))
            elif settled == "undated":
                undated.append((path, number, count, noun))

    if "--list" in argv:
        for path, number, count, noun in undated:
            print("%s:%d: %d %s" % (path, number, count, noun))
        return 0

    try:
        UNDATED_BUDGET = ubq_roots.budget("census-undated", REPO)
    except KeyError:
        print("check-census-claims: REFUSED — no Scripts/gate-ledger.<repository>.txt sets "
              "census-undated, so nothing bounds the undated censuses.", file=sys.stderr)
        return 2

    if wrong:
        print("check-census-claims: a derived count no longer matches its pathspec:",
              file=sys.stderr)
        for path, number, count, noun, pathspec, actual in wrong:
            print("  %s:%d: says %d %s, `%s` names %s"
                  % (path, number, count, noun, pathspec, actual), file=sys.stderr)
        print("\n  The tree moved under the comment. Update the number, or say what "
              "the\n  count was rather than what it is.", file=sys.stderr)
        return 1

    if len(undated) > UNDATED_BUDGET:
        print("check-census-claims: %d undated census(es) against a budget of %d."
              % (len(undated), UNDATED_BUDGET), file=sys.stderr)
        print("  A count of the tree written in prose has an expiry nobody sets. Give "
              "it a\n  pathspec to be checked against, say it in the past tense, or "
              "date it.", file=sys.stderr)
        for path, number, count, noun in undated[-12:]:
            print("      %s:%d: %d %s" % (path, number, count, noun), file=sys.stderr)
        return 1

    if len(undated) < UNDATED_BUDGET:
        print("check-census-claims: %d undated census(es) against a budget of %d."
              % (len(undated), UNDATED_BUDGET), file=sys.stderr)
        print("  Set the budget census-undated to %d in the ledger Scripts/gate-ledger.<repository>.txt, in the commit that earned it."
              % len(undated), file=sys.stderr)
        return 1

    derivedCount = sum(1 for path in paths for _n, _c, _no, s, _p
                       in claims(open(os.path.join(REPO, path), errors="replace").read())
                       if s == "derived")
    print("check-census-claims: ok (%d derived count(s) match their pathspec; %d "
          "undated census(es), at budget — ratchet down, never up)"
          % (derivedCount, len(undated)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
