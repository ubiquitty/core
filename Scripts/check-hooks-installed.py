#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0
"""check-hooks-installed.py — the gates a clone thinks it has are the ones it has.

Every other gate here assumes it runs. `Scripts/install-hooks.sh` is what makes
that true, and nothing invokes it: CONTRIBUTING tells a reader to run it once
per clone, and `Scripts/test-install-hooks.sh` tests it against a throwaway
clone. Being tested is not being run — the same distinction check-gates-invoked
draws about gates, arriving one level lower, at the thing that installs them.

Three states leave a clone gated by less than it appears to be, and none of
them announces itself:

    absent          a gating hook is missing, so what it runs gates nothing.
    symlinked       the hook links into one worktree's Scripts/hooks.
    not executable  the hook is present and correct but lacks its execute bit;
                    git skips it in silence, and every check a reader makes by
                    opening the file says it is fine.

The second is why this checks the hook's shape rather than its presence. Linked
worktrees share one hooks directory, so a hook symlinked into a particular
worktree gates every worktree with that one tree's copy: its idea of which gates
exist, at whatever commit it has checked out, or nothing once it is removed and
the link dangles. `hooks-dispatch` is installed as a copy and runs the hook from
whichever tree git is operating on, so a symlink is never the intended install.

What this cannot do, stated because the gap is the point: it cannot help the
clone that has no hooks, because the places it runs from are the ones that do not
run there. It reports for whoever asks — a person running check-style, or a
push on a clone whose hooks are half-installed. Closing the rest needs a trigger
that fires without a hook, and the build is the only one every checkout reaches.

It fails rather than advising. A checkout with no hooks never reaches this
verdict at push time anyway, so failing cannot obstruct it; it reaches someone
running check-style by hand, for whom a line naming install-hooks.sh is the
answer they came for.

To test this script, never point `git config core.hooksPath` at an empty
directory to watch it fail. Linked worktrees share one .git/config, so that
disables every hook for every worktree of that clone, silently, until someone
unsets it. Use a one-shot environment override instead:

    GIT_CONFIG_COUNT=1 GIT_CONFIG_KEY_0=core.hooksPath \\
    GIT_CONFIG_VALUE_0=/tmp/empty python3 Scripts/check-hooks-installed.py

Stdlib only.

    python3 Scripts/check-hooks-installed.py
    python3 Scripts/check-hooks-installed.py --self-test
"""

import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gate_report  # noqa: E402 — the path above is what makes it importable
import ubq_roots  # noqa: E402

# The clone whose hooks are read is the repository this runs in, which is not the
# checkout holding this file when ubiquitty-core is a library of another.
REPO = ubq_roots.subject_root()

# The dispatcher's own marker. Keyed on a line from the file rather than on a
# size or a hash: a copy is what is installed, so the installed file and the
# source are the same bytes, and any check that pins those bytes fails the day
# the dispatcher gains a comment.
DISPATCH_MARKER = "hooks-dispatch"

# A hook whose absence means work lands ungated. post-* hooks run after the fact
# and are not gates, so they are not required here — a clone missing only those
# is not silently unguarded.
GATING_HOOKS = ("pre-commit", "pre-push", "commit-msg")


def hooksDirectory():
    """The directory git actually consults, or None when it cannot be resolved.

    - Returns: An absolute path. `core.hooksPath` wins when set; otherwise the
      COMMON directory, which is the one shared by every linked worktree —
      `--git-path hooks` is per-worktree and would name a path git never reads.
    """
    configured = subprocess.run(["git", "-C", REPO, "config", "core.hooksPath"],
                                capture_output=True, text=True)
    path = configured.stdout.strip()
    if not path:
        common = subprocess.run(["git", "-C", REPO, "rev-parse", "--git-common-dir"],
                                capture_output=True, text=True)
        if common.returncode != 0:
            return None
        path = os.path.join(common.stdout.strip(), "hooks")
    if not os.path.isabs(path):
        path = os.path.join(REPO, path)
    return os.path.abspath(path)


def describe(path):
    """Classifies one installed hook.

    - Returns: One of "absent", "symlink", "unexecutable", "dispatcher",
      "foreign".
    """
    # islink before exists: a DANGLING symlink is the state git skips silently,
    # and exists() follows the link and answers False for it, which would file it
    # under absent and lose which of the two it is.
    if os.path.islink(path):
        return "symlink"
    if not os.path.exists(path):
        return "absent"
    # git runs the file or skips it, and skipping is silent. The right content
    # without the bit is the worst of the states here: every check a reader makes
    # by opening the file says it is correct.
    if not os.access(path, os.X_OK):
        return "unexecutable"
    try:
        with open(path, encoding="utf-8", errors="replace") as handle:
            body = handle.read()
    except OSError:
        return "foreign"
    return "dispatcher" if DISPATCH_MARKER in body else "foreign"


def selfTest():
    """Checks the classifier against each shape, in a directory built to hold them."""
    import tempfile

    ok = True
    with tempfile.TemporaryDirectory() as scratch:
        absent = os.path.join(scratch, "absent")
        if describe(absent) != "absent":
            print("self-test: a missing file is not reported absent", file=sys.stderr)
            ok = False

        dispatcher = os.path.join(scratch, "dispatcher")
        with open(dispatcher, "w", encoding="utf-8") as handle:
            handle.write("#!/usr/bin/env bash\n# Scripts/hooks-dispatch — installed as a COPY\n")
        os.chmod(dispatcher, 0o755)
        if describe(dispatcher) != "dispatcher":
            print("self-test: the dispatcher is not recognised", file=sys.stderr)
            ok = False

        unexecutable = os.path.join(scratch, "unexecutable")
        with open(unexecutable, "w", encoding="utf-8") as handle:
            handle.write("#!/usr/bin/env bash\n# Scripts/hooks-dispatch\n")
        os.chmod(unexecutable, 0o644)
        if describe(unexecutable) != "unexecutable":
            print("self-test: a dispatcher without the execute bit is not reported — "
                  "git skips it and every read of the file says it is correct",
                  file=sys.stderr)
            ok = False

        foreign = os.path.join(scratch, "foreign")
        with open(foreign, "w", encoding="utf-8") as handle:
            handle.write("#!/bin/sh\nexit 0\n")
        os.chmod(foreign, 0o755)
        if describe(foreign) != "foreign":
            print("self-test: a hand-written hook is not reported foreign", file=sys.stderr)
            ok = False

        link = os.path.join(scratch, "link")
        os.symlink(dispatcher, link)
        if describe(link) != "symlink":
            print("self-test: a symlink to a real file is not reported symlink", file=sys.stderr)
            ok = False

        dangling = os.path.join(scratch, "dangling")
        os.symlink(os.path.join(scratch, "gone"), dangling)
        if describe(dangling) != "symlink":
            print("self-test: a DANGLING symlink is not reported symlink — it would "
                  "be filed as absent and the two states are different", file=sys.stderr)
            ok = False

    print("check-hooks-installed: self-test %s" % ("ok" if ok else "FAILED"))
    return 0 if ok else 1


def main():
    if "--self-test" in sys.argv[1:]:
        return selfTest()

    directory = hooksDirectory()
    if directory is None:
        print("check-hooks-installed: SKIPPED — not a git checkout, so there is "
              "no hooks directory to read")
        return 0

    states = {name: describe(os.path.join(directory, name)) for name in GATING_HOOKS}
    wrong = {name: state for name, state in states.items() if state != "dispatcher"}

    if not wrong:
        print("check-hooks-installed: ok (%d gating hook(s) installed as the "
              "dispatcher in %s)" % (len(GATING_HOOKS), directory))
        return 0

    # The verdict names the offenders on its own line rather than pointing at the
    # detail below it: a reader who sees only the first line has to be able to act
    # on it, which is what check-verdict-lines gates.
    reasons = {
        "absent": "not installed — commits and pushes run no gate at all",
        "symlink": "a pre-dispatcher install, pointing into one checkout's tree",
        "unexecutable": "present and correct but not executable, so git skips it",
        "foreign": "present but not this repository's dispatcher",
    }
    offenders = ["%s (%s)" % (name, states[name])
                 for name in GATING_HOOKS if states[name] != "dispatcher"]
    status = gate_report.failed("check-hooks-installed",
                                "gating hook(s) that will not gate this clone", offenders)
    for name in GATING_HOOKS:
        state = states[name]
        if state == "dispatcher":
            continue
        print("    %-12s %s" % (name, reasons[state]), file=sys.stderr)
    print("\n  Fix: ./Scripts/install-hooks.sh\n"
          "  It installs a copy of Scripts/hooks-dispatch under each hook name, which\n"
          "  resolves the hook from the tree git is operating on and exits non-zero when\n"
          "  that body is missing — so a hook that cannot find its code is never mistaken\n"
          "  for one that ran and passed.", file=sys.stderr)
    return status


if __name__ == "__main__":
    sys.exit(main())
