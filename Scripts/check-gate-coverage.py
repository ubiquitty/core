#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""A gate the commit runs is a gate the push runs, or it says why not.

Two hooks guard this tree. `pre-commit`
invokes its gates as a commit is made; `pre-push-gates.sh` invokes a larger set
before anything reaches origin. The sets overlap heavily, which reads as a
ladder — commit checks a little, push checks more — and is not one. They are two
parallel lists, and the difference between them is the set of rules that hold
only as long as every commit runs its hook.

`git commit --no-verify` is one flag. It is documented at the top of pre-commit
as turning off all of them, and it exists for good reasons. What makes it a
problem is not the bypass but that nothing downstream repeats the work: a rule
enforced at commit alone is a rule a single flag retires, and the tree then
carries the violation with every push gate green above it: a gate that runs at
commit only can sit failing on main while push reports every gate it calls
passing.

So the rule is the nesting the two lists already imply: every gate pre-commit
runs is reachable from the push tier too, or is named in COMMIT_ONLY with the
reason it cannot be. Push is the boundary that matters — it is where work stops
being local — and a check that does not run there is advisory.

What this cannot see. It reads invocations, not outcomes: a gate wired into both
hooks passes here while doing nothing in either, and a gate whose push
invocation is under a tier nobody runs counts as reached. It matches on the
script's filename, so a gate invoked through a variable holding its path is
invisible, and it takes reachability through check-style.py at face value rather
than confirming that file's own dispatch. Each of those is a way to satisfy this
gate without gaining the coverage it describes.
"""

import glob
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ubq_roots

REPO = ubq_roots.subject_root()
CORE = ubq_roots.core_root(REPO)
KIT = ubq_roots.kit_root(REPO)


def existing(*paths):
    """The files among `paths` that exist, each once, as absolute paths."""
    seen = []
    for path in paths:
        if os.path.isfile(path) and path not in seen:
            seen.append(path)
    return seen


def listed(directory, pattern):
    """Every file of a directory matching a glob, sorted."""
    return sorted(glob.glob(os.path.join(directory, pattern)))


# The commit hook is the subject's own or, for a repository that carries
# ubiquitty-core as a library, the library's; the files of Scripts/hooks/pre-commit-parts
# are the stanzas a repository adds to it.
PRE_COMMIT = existing(os.path.join(REPO, "Scripts", "hooks", "pre-commit"),
                      os.path.join(CORE, "Scripts", "hooks", "pre-commit")) + [
    path for path in listed(os.path.join(REPO, "Scripts", "hooks", "pre-commit-parts"), "*")
    if os.path.isfile(path)]

# The push tier is the repository's Scripts/pre-push-gates*.sh and the gate files of
# the libraries it sources.
PRE_PUSH = existing(*(listed(os.path.join(REPO, "Scripts"), "pre-push-gates*.sh")
                      + [os.path.join(CORE, "Scripts", "pre-push-gates-core.sh"),
                         os.path.join(KIT, "Scripts", "pre-push-gates-kit.sh")]))

# Files the push tier reaches gates through rather than naming them itself. A
# gate listed by one of these counts as run at push: the style runner and the
# section tables it loads.
PUSH_DELEGATES = existing(
    os.path.join(CORE, "Scripts", "check-style.py"),
    *(listed(os.path.join(CORE, "Scripts"), "style_sections_*.py")
      + listed(os.path.join(REPO, "Scripts"), "style_sections_*.py")))

# A gate that runs at commit and cannot run at push, with the reason. The reason
# has to be about the gate, not about the schedule: "slow" is what tiers are
# for, and every gate here walks the tree in well under a second.
#
# A commit-time gate that reads the STAGED set is the shape that belongs here,
# because at push time there is no staged set to read. None of the current gates
# are in that position — the three that take `--staged` all walk the whole tree
# when given no argument, which is what the push tier passes them.
COMMIT_ONLY = {}


def gatesInvokedBy(paths):
    """Returns the check-script filenames a hook or script invokes.

    - Parameters:
      - paths: Absolute paths of the files to read.
    - Returns: A set of bare filenames such as `check-enums.sh`, empty if no
      file can be read.
    """
    names = set()
    for path in ([paths] if isinstance(paths, str) else paths):
        try:
            with open(path, encoding="utf-8") as handle:
                text = handle.read()
        except OSError:
            continue
        names |= set(re.findall(r"(check-[a-z0-9-]+\.(?:py|sh))", text))
    return names


def findingsFor(commitGates, pushGates, commitOnly):
    """Returns what is wrong with one pairing of gate sets, as sentences.

    Split from the file reading so both arms can be exercised on sets nobody
    has to write to disk: the rule is about the difference between two sets,
    and a gate that only ever saw this repository's own would report zero
    whether it worked or not.

    - Parameters:
      - commitGates: Filenames the commit hook invokes.
      - pushGates: Filenames the push tier reaches, directly or by delegation.
      - commitOnly: Filename to reason, for gates that cannot run at push.
    - Returns: A list of failure sentences, empty when the sets nest.
    """
    findings = []
    for name in sorted(set(commitGates) - set(pushGates) - set(commitOnly)):
        findings.append("  %s runs at commit and not at push — one --no-verify retires it. "
                        "Invoke it from %s, or name it in COMMIT_ONLY with the reason it "
                        "cannot run there" % (name, "the repository's Scripts/pre-push-gates*.sh"))
    for name, why in sorted(commitOnly.items()):
        if name not in commitGates:
            findings.append("  %s is excused as \"%s\" but pre-commit does not run it — "
                            "delete the entry" % (name, why))
        elif name in pushGates:
            findings.append("  %s is excused as \"%s\" and the push tier runs it anyway — "
                            "delete the entry" % (name, why))
    return findings


def selfTest():
    """Checks the rule against a pairing it must fault and ones it must not.

    Every arm below is a way the sets can relate, and the one that matters is
    the third: a gate excused in COMMIT_ONLY is the intended escape, so a rule
    that faulted it would be unusable, and one that stopped faulting anything
    would pass a clean tree in exactly the same way as a working one.

    - Returns: Zero when every arm answered as it must.
    """
    arms = [
        ("a gate at commit and not at push is faulted",
         {"check-a.py"}, set(), {}, 1),
        ("a gate at both is not",
         {"check-a.py"}, {"check-a.py"}, {}, 0),
        ("a gate excused by name is not",
         {"check-a.py"}, set(), {"check-a.py": "reason"}, 0),
        ("an excuse for a gate commit does not run is faulted",
         set(), set(), {"check-a.py": "reason"}, 1),
        ("an excuse for a gate push runs anyway is faulted",
         {"check-a.py"}, {"check-a.py"}, {"check-a.py": "reason"}, 1),
        ("a gate at push and not at commit is nobody's problem",
         set(), {"check-a.py"}, {}, 0),
        ("two uncovered gates are two findings, not one",
         {"check-a.py", "check-b.py"}, set(), {}, 2),
    ]
    failures = 0
    for description, commitGates, pushGates, commitOnly, expected in arms:
        actual = len(findingsFor(commitGates, pushGates, commitOnly))
        if actual != expected:
            failures += 1
        print("  %s %s (reported %d, must be %d)"
              % ("ok " if actual == expected else "FAIL", description, actual, expected))

    # The read has to find something, or every arm above is about a set that is
    # always empty and the gate passes a tree it never looked at.
    found = gatesInvokedBy(PRE_COMMIT)
    if found:
        print("  ok  the commit hook read names %d gate(s)" % len(found))
    else:
        print("  FAIL the commit hook read names no gate")
        failures += 1

    if failures:
        print("check-gate-coverage: %d self-test arm(s) failed" % failures, file=sys.stderr)
        return 1
    print("check-gate-coverage: self-test ok (%d arms)" % (len(arms) + 1))
    return 0


def main():
    if "--self-test" in sys.argv:
        return selfTest()

    commitGates = gatesInvokedBy(PRE_COMMIT)
    if not commitGates:
        print("check-gate-coverage: pre-commit names no gate — the read found nothing, "
              "which is not the same as the hook running none", file=sys.stderr)
        return 1

    pushGates = gatesInvokedBy(PRE_PUSH) | gatesInvokedBy(PUSH_DELEGATES)

    # A gate cannot reach itself: pre-commit names this script, and pre-push
    # names it too, but neither invocation says anything about coverage.
    selfName = os.path.basename(__file__)
    commitGates.discard(selfName)
    pushGates.discard(selfName)

    failures = findingsFor(commitGates, pushGates, COMMIT_ONLY)
    if failures:
        print("check-gate-coverage: a rule holds at commit and not at push", file=sys.stderr)
        print("\n".join(failures), file=sys.stderr)
        return 1

    print("check-gate-coverage: ok (%d gate(s) at commit, every one reachable from the "
          "push tier%s)" % (len(commitGates),
                            "" if not COMMIT_ONLY else "; %d excused" % len(COMMIT_ONLY)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
