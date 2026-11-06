#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0
"""check-approved-goldens.py — an approved golden keeps the bytes it was
approved with.

Tests/Snapshots/approved.tsv lists the goldens that were checked by eye, each
with the sha256 of its bytes. run.py refuses to re-mint one, but the runner is
one door: a golden can be overwritten by hand, checked out from another branch,
or deleted, and the manifest can be edited so that its hash follows the new
bytes. This closes those doors on the push path.

Three verdicts, each a failure:
  - a listed golden is missing;
  - a listed golden's bytes do not hash to the recorded value;
  - a line was rewritten in place: origin/main lists the same golden with a
    different hash. Replacing an approved picture takes two visible steps —
    delete the line and land that, then approve the new picture — so a hash
    that changes under a path that stays is the shortcut this refuses.

Removing a line is allowed (that is the unlock), and so is adding one.

    python3 Scripts/check-approved-goldens.py
    python3 Scripts/check-approved-goldens.py --self-test

Stdlib only.
"""

import hashlib
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gate_report  # noqa: E402 — the path above is what makes it importable
import ubq_roots  # noqa: E402

ROOT = Path(ubq_roots.subject_of(__file__))
MANIFEST = Path("Tests/Snapshots/approved.tsv")
GATE = "check-approved-goldens"


def parseManifest(text):
    """The entries a manifest's text carries.

    - Parameter text: The TSV text.
    - Returns: A list of (golden, sha256, approved_on, note); comment and blank
      lines are skipped, and a short line is padded.
    """
    entries = []
    for line in text.split("\n"):
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        fields = line.split("\t")
        while len(fields) < 4:
            fields.append("")
        entries.append(tuple(fields[:4]))
    return entries


def baselineText(root):
    """The manifest as origin/main has it, or empty when there is no such ref.

    - Parameter root: The repository root.
    - Returns: The text, empty when origin/main is absent or lacks the file.
    """
    try:
        return subprocess.run(
            ["git", "-C", str(root), "show", f"origin/main:{MANIFEST.as_posix()}"],
            capture_output=True, text=True, check=True).stdout
    except (subprocess.CalledProcessError, FileNotFoundError):
        return ""


def verify(root, manifestText, baseline):
    """The failures a tree holds against a manifest.

    - Parameters:
      - root: The repository root the golden paths are relative to.
      - manifestText: The manifest under test.
      - baseline: The manifest origin/main carries.
    - Returns: A list of failure lines; empty when every approval holds.
    """
    failures = []
    entries = parseManifest(manifestText)
    seen = {}
    for golden, digest, on, _ in entries:
        if golden in seen:
            failures.append(f"{golden}: listed twice")
            continue
        seen[golden] = digest
        path = root / golden
        if not path.exists():
            failures.append(f"{golden}: approved on {on}, and the file is gone")
            continue
        actual = hashlib.sha256(path.read_bytes()).hexdigest()
        if actual != digest:
            failures.append(f"{golden}: approved on {on} as {digest[:12]}, now {actual[:12]}")
    for golden, digest, on, _ in parseManifest(baseline):
        if golden in seen and seen[golden] != digest:
            failures.append(f"{golden}: rewritten in place — origin/main approved {digest[:12]} on {on}, "
                            f"this tree records {seen[golden][:12]}; delete the line in one change "
                            f"and approve the new picture in another")
    return failures


def selfTest():
    """Runs the rule against a tree it must fail and one it must pass."""
    ok = True
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        goldenDirectory = root / "Tests" / "Snapshots" / "golden" / "unifont"
        goldenDirectory.mkdir(parents=True)
        golden = goldenDirectory / "case.png"
        golden.write_bytes(b"\x89PNG-original")
        digest = hashlib.sha256(golden.read_bytes()).hexdigest()
        line = f"Tests/Snapshots/golden/unifont/case.png\t{digest}\t2026-09-04\tchecked\n"

        def expect(label, failures, wantFailure):
            nonlocal ok
            if bool(failures) != wantFailure:
                print(f"self-test: {label}: expected {'a failure' if wantFailure else 'ok'}, "
                      f"got {failures or 'ok'}", file=sys.stderr)
                ok = False

        expect("clean tree passes", verify(root, line, line), False)
        expect("a new approval passes", verify(root, line, ""), False)
        expect("an unlock passes", verify(root, "", line), False)
        golden.write_bytes(b"\x89PNG-tampered")
        expect("changed bytes fail", verify(root, line, line), True)
        tampered = hashlib.sha256(golden.read_bytes()).hexdigest()
        rewritten = line.replace(digest, tampered)
        expect("a line rewritten in place fails", verify(root, rewritten, line), True)
        golden.unlink()
        expect("a missing golden fails", verify(root, line, line), True)
        expect("a duplicate line fails", verify(root, line + line, line), True)
    print(f"check-approved-goldens: self-test {'ok' if ok else 'FAILED'}")
    return ok


def main(argv):
    if "--self-test" in argv:
        return 0 if selfTest() else 1
    manifest = ROOT / MANIFEST
    text = manifest.read_text(encoding="utf-8") if manifest.exists() else ""
    failures = verify(ROOT, text, baselineText(ROOT))
    count = len(parseManifest(text))
    if failures:
        for failure in failures:
            gate_report.finding(failure.split(":", 1)[0], GATE, failure.split(":", 1)[1].strip())
        return gate_report.failed(GATE, f"of {count} approved golden(s) did not hold", failures)
    return gate_report.ok(GATE, f"{count} approved golden(s) hold their bytes")


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
