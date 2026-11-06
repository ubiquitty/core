#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0
"""check-folder-map.py — every drawing of the source tree is the source tree.

The README's repository layout and ARCHITECTURE's layer map each draw `Source/`
as an indented tree, and a new reader takes their bearings from them. A drawing
nothing compares to the filesystem drifts in every way the tree moves: a
directory renamed under it keeps its old name in the drawing, a directory split
in two stays one, and modules drawn at their parent's indent are drawn beside
it, so someone looking for Source/Rendering/ finds nothing.

The rule is one-directional. A name the map draws must exist at the level it is
drawn; a directory the map leaves out is not reported. A drawing that leaves
out most Core submodules reads better for it. Demanding
completeness would turn every new directory into a documentation edit and produce
lines added to satisfy a gate. Naming something that is not there is the other
case: it sends a reader somewhere empty.

A drawing is a fenced block whose first line is `Source/` and nothing else, in
any Markdown file at the repository root or directly under Docs/; a block that
opens on a deeper path is a list of paths, not a tree. The gate finds drawings rather
than naming the files that hold them: a tree that does not carry some document
still has every drawing it does carry checked, and a drawing added later is
checked without an edit here. Finding none fails the gate, since nothing was
compared.

Indentation is the parent. An entry is a child of the nearest entry above it
with a smaller indent, which is how the drawings already read. Box-drawing
characters count as indentation, so a `├──` tree reads the same way as a plain
indented one.

Stdlib only.

    python3 Scripts/check-folder-map.py
    python3 Scripts/check-folder-map.py --self-test
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gate_report  # noqa: E402 — the path above is what makes it importable
import ubq_roots  # noqa: E402

REPO = ubq_roots.subject_of(__file__)

# Where a drawing may live, relative to the repository root, searched one level
# deep: the documents a reader opens first.
DOCUMENT_DIRECTORIES = ("", "Docs")

# The root a drawing hangs from — the first line inside the fence.
ROOT = "Source/"

# A fence opens with three backticks and an optional info string, and closes
# with three backticks at the start of a line.
FENCE = re.compile(r"^```[^\n]*\n(.*?)^```", re.M | re.S)

# The characters a `├──` drawing indents with, each read as one column of indent.
BOX_DRAWING = str.maketrans({character: " " for character in "│├└─"})

# A row in the NAME column: indent, a name, a slash, then the gap before its
# description. The indent bound is what keeps this off the names that appear
# inside a description, which start around column 23 — `Sync/` and `Tmux/` are
# written that way under Protocol/ and are prose about the entry, not rows of it.
NAME_COLUMN = 20
ENTRY = re.compile(r"^( {1,%d})([A-Za-z][A-Za-z0-9_+-]*)/(?: |$)" % NAME_COLUMN)


def read(path):
    try:
        with open(os.path.join(REPO, path), encoding="utf-8", errors="replace") as handle:
            return handle.read()
    except OSError:
        return ""


def documents():
    """Every Markdown file a drawing may live in, relative to the repository root.

    - Returns: The paths in a stable order, root files first.
    """
    found = []
    for directory in DOCUMENT_DIRECTORIES:
        try:
            names = sorted(os.listdir(os.path.join(REPO, directory)))
        except OSError:
            continue
        for name in names:
            path = os.path.join(directory, name)
            if name.endswith(".md") and os.path.isfile(os.path.join(REPO, path)):
                found.append(path)
    return found


def ownedElsewhere(path):
    """Whether a repository this checkout does not hold has files under `path`.

    A drawing of Source/ shows the whole product, and a directory it draws that
    another repository owns is there, only not here.
    """
    prefix = path.rstrip("/") + "/"
    if any(os.path.isdir(os.path.join(root, path))
           for root in (ubq_roots.kit_root(REPO), ubq_roots.core_root(REPO))):
        return True
    return any(listed.startswith(prefix) for listed in ubq_roots.elsewhere(REPO)[0])


def drawings(text):
    """Every fenced tree in `text` that starts at `Source/`.

    - Returns: One list of lines per drawing, box-drawing characters read as
      indentation.
    """
    return [block.translate(BOX_DRAWING).split("\n")
            for block in FENCE.findall(text)
            if block.lstrip().split("\n", 1)[0].rstrip() == ROOT]


def entries(lines):
    """Every drawn directory as (indent, name, parent path).

    - Returns: A list of triples, parent relative to the repo root. A row's
      parent is the nearest row above it drawn at a smaller indent.
    """
    found = []
    stack = []  # (indent, path)
    for line in lines:
        match = ENTRY.match(line)
        if not match:
            continue
        indent, name = len(match.group(1)), match.group(2)
        while stack and stack[-1][0] >= indent:
            stack.pop()
        parent = stack[-1][1] if stack else ROOT.rstrip("/")
        found.append((indent, name, parent))
        stack.append((indent, os.path.join(parent, name)))
    return found


def selfTest():
    ok = True

    sample = [
        "Source/",
        "  Core/                the model",
        "    Rendering/         portable rendering",
        "  Platform/            the backends",
    ]
    got = [(name, parent) for _, name, parent in entries(sample)]
    want = [("Core", "Source"), ("Rendering", "Source/Core"), ("Platform", "Source")]
    if got != want:
        print("self-test: nesting misread, got %s" % got, file=sys.stderr)
        ok = False

    # The pre-fix drawing put Rendering beside Core, which is the defect.
    flat = ["Source/", "  Core/    the model", "  Rendering/   portable rendering"]
    if [p for _, _, p in entries(flat)] != ["Source", "Source"]:
        print("self-test: a flat drawing is not read as siblings", file=sys.stderr)
        ok = False

    # A name inside a description column is prose, not a row.
    described = ["Source/", "  Protocol/            portable wire protocols:",
                 "                         Sync/   the frames"]
    if [n for _, n, _ in entries(described)] != ["Protocol"]:
        print("self-test: a name in the description column was read as a row",
              file=sys.stderr)
        ok = False

    # A box-drawn tree nests by the column its names start at.
    boxed = drawings("```\nSource/\n├── Core/     the model\n│   └── VT/   the parser\n"
                     "└── PAL/      the seam\n```\n")
    got = [(name, parent) for drawn in boxed for _, name, parent in entries(drawn)]
    want = [("Core", "Source"), ("VT", "Source/Core"), ("PAL", "Source")]
    if got != want:
        print("self-test: a box-drawn tree misread, got %s" % got, file=sys.stderr)
        ok = False

    # Every fence that opens on Source/ alone is a drawing; a fence opening on
    # another root, or on a deeper path, is not.
    text = ("```\nSource/\n  Core/  the model\n```\n"
            "```text\nScripts/\n  hooks/  git hooks\n```\n"
            "```\nSource/Core/\n  the model, listed rather than drawn\n```\n"
            "```\nSource/\n  PAL/   the seam\n```\n")
    if len(drawings(text)) != 2:
        print("self-test: expected 2 drawings, found %d" % len(drawings(text)),
              file=sys.stderr)
        ok = False

    print("check-folder-map: self-test %s" % ("ok" if ok else "FAILED"))
    return 0 if ok else 1


def main():
    if "--self-test" in sys.argv[1:]:
        return selfTest()

    compared = []  # (document, directory rows)
    missing = []
    for document in documents():
        for lines in drawings(read(document)):
            drawn = entries(lines)
            if not drawn:
                print("check-folder-map: FAILED — a %s tree in %s has no directory "
                      "rows, so nothing was compared" % (ROOT, document), file=sys.stderr)
                return 1
            compared.append((document, len(drawn)))
            for _, name, parent in drawn:
                path = os.path.join(parent, name)
                if os.path.isdir(os.path.join(REPO, path)) or ownedElsewhere(path):
                    continue
                elsewhere = ""
                for base, directories, _files in os.walk(os.path.join(REPO, "Source")):
                    if name in directories:
                        elsewhere = " (it is at %s)" % os.path.relpath(
                            os.path.join(base, name), REPO)
                        break
                missing.append("%s: %s is drawn as %s%s" % (document, name + "/", path,
                                                            elsewhere))

    if not compared and "core" not in ubq_roots.repositories(REPO):
        print("check-folder-map: ok (no drawing of %s in this repository; the drawings are Core's)" % ROOT)
        return 0
    if not compared:
        print("check-folder-map: FAILED — no Markdown file at the repository root or "
              "directly under Docs/ draws a %s tree, so nothing was compared" % ROOT,
              file=sys.stderr)
        return 1

    if missing:
        status = gate_report.failed("check-folder-map",
                                    "directory row(s) the tree does not have", missing)
        print("\n  A drawing of %s is where a reader learns the shape of the tree. A\n"
              "  row that names a directory it does not have sends them somewhere\n"
              "  empty, and a row drawn at the wrong indent moves a module out of its\n"
              "  parent. Leaving a directory out is fine — a drawing is a summary." % ROOT,
              file=sys.stderr)
        return status

    rows = sum(count for _, count in compared)
    print("check-folder-map: ok (%d directory row(s) in %d drawing(s) across %s, each "
          "present at the level it is drawn)"
          % (rows, len(compared), ", ".join(sorted({document for document, _ in compared}))))
    return 0


if __name__ == "__main__":
    sys.exit(main())
