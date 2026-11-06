#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""A backend that leaves a lossy entry null has to say which one, and where.

`GlyphBackend`'s optional entries mostly degrade fairly — a procedural area fill
beats the font's, an unligatured row is still correct text. Four do not, and
`optionalDefaultsAreLossy` names them: without `clusterGlyph` a combining mark is
dropped, without `wideClusterGlyph` the same happens on a two-cell base, without
`colorGlyph` an emoji loses its colour, and without `boxGlyph` a box-drawing line
loses the font's weight and dashes. All four leave the cell inked and the count
right, so every check downstream passes and only the picture is wrong.

The frontend is the one that decides, and Core cannot decide for it. What Core
cannot do either is notice when a frontend's prose and its table disagree: a
comment claiming a complete backend costs nothing to write and nothing to keep
true. One such claim survived a `wideClusterGlyph` landing and read as accurate
until someone ran the parity check.

So the rule is not "fill every entry" — that would be false for backends with no
colour sheet and no shaper. It is that an omission is **stated**: a lossy entry
a construction site leaves null must be named in a comment in the same file. A
reader then meets the omission where the table is built, and a frontend that
grows a new lossy entry cannot acquire it silently.

What this cannot see, demonstrated rather than supposed. It compares a copy of
the lossy set against `optionalDefaultsAreLossy` and reports which backends fill
those entries. Both the copy and the predicate say a thing is lossy; neither can
say whether the predicate is COMPLETE.

An entry can be lossy for only part of its range, which is where a comment goes
wrong. `BoxDrawing::prefersProcedural` splits `boxGlyph`'s: for blocks, shades
and the diagonals the entry is never consulted and a null costs nothing, but for
box-drawing lines 2500..257F the builder reaches for it FIRST — so a null there
drops the font's weight and dashes, which is the predicate's own test for a
loss. A comment that judged the entry by the first half and called it lossless
to omit would keep it out of the predicate, and every stream and encode parity
lane would still agree, because each arm agrees with its own table; a
dense-sampled pixel comparison is what finds U+2551 as an antialiased 8px font
raster on one arm and two crisp 3px procedural strokes on the other.

So the limit is precise: this gate catches an entry nobody classified, and
cannot catch one classified WRONGLY. A claim true of the common case,
generalised in a comment, then relied on by a check, is the shape it is blind
to — and reading the predicate is exactly how it inherits the comment.

    ./Scripts/check-glyph-backend-claims.py

Prints the matrix every run, because the interesting output is which backend
supplies what, and exits 1 naming any omission no comment mentions. Stdlib only.
"""

import os
import re
import sys


HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ubq_roots  # noqa: E402

# The backends judged are the ones the repository run from holds; the function
# that defines which omissions are lossy is Core's, read from where Core is.
REPO = ubq_roots.subject_of(__file__)
CORE = ubq_roots.core_root(REPO)

# The entries `optionalDefaultsAreLossy` reads. Kept beside the function it
# mirrors rather than derived, because deriving it from the source would make
# this agree with that function by construction and it is the disagreement that
# is worth catching.
LOSSY_ENTRIES = ("clusterGlyph", "wideClusterGlyph", "colorGlyph", "boxGlyph")

# One loss this cannot model, named here rather than left as a silent gap. The
# wide loss is a ROUTE: `hasWideGlyphRoute` accepts either `wideGlyph` or the
# `bindWideAtlas` + `rasteriseWideGlyph` pair, so no single entry being null is
# the defect and the matrix below has no column shape for it. A backend with
# neither route draws every two-cell cell as bare background, which
# `optionalDefaultsAreLossy` reports and this check does not see.

LOSSY_SOURCE = os.path.join("Source", "Core", "Rendering", "GlyphBackend.h")

# Reported, not enforced. An entry here is one whose classification is open: the
# matrix shows who fills it so the question is visible, and nothing fails until
# `optionalDefaultsAreLossy` decides. Enforcing a row the predicate has not
# classified would be this check asserting its own opinion of Core's contract.
UNCLASSIFIED_ENTRIES = ()

# A file builds a table when it assigns an entry every backend must supply.
CONSTRUCTS = re.compile(r"\bbackend\.monoGlyph\s*=")

COMMENT = re.compile(r"^\s*(?:///|//|#|\*)")


def constructionSites():
    """Files that build a `GlyphBackend`.

    - Returns: Repository-relative paths, sorted.
    """
    found = []
    for root, directories, names in os.walk(os.path.join(REPO, "Source")):
        directories[:] = [name for name in directories if name != "ThirdParty"]
        for name in names:
            if not name.endswith((".cpp", ".mm", ".m", ".swift", ".h")):
                continue
            path = os.path.join(root, name)
            try:
                with open(path, encoding="utf-8", errors="replace") as handle:
                    text = handle.read()
            except OSError:
                continue
            if CONSTRUCTS.search(text):
                found.append(os.path.relpath(path, REPO))
    return sorted(found)


def siblingHeader(site):
    """The header beside a definition file, read as source.

    - Parameter site: A repository-relative path to a construction site.
    - Returns: The header's text, or an empty string when there is none.
    """
    stem, extension = os.path.splitext(site)
    if extension not in (".cpp", ".mm", ".m"):
        return ""
    try:
        with open(os.path.join(REPO, stem + ".h"), encoding="utf-8", errors="replace") as handle:
            return handle.read()
    except OSError:
        return ""


def entriesFilled(text):
    """Which lossy entries a file assigns.

    - Parameter text: The file's source.
    - Returns: The subset of `LOSSY_ENTRIES` assigned.
    """
    return {entry for entry in LOSSY_ENTRIES
            if re.search(r"\bbackend\.%s\s*=" % entry, text)}


def entriesFilledNamed(text, entry):
    """Whether a file assigns one named entry.

    - Parameters:
      - text: The file's source.
      - entry: The entry name.
    - Returns: A set holding the entry when assigned, empty otherwise.
    """
    return {entry} if re.search(r"\bbackend\.%s\s*=" % entry, text) else set()


GUARD = re.compile(r"\b(if|else|for|while|switch|guard)\b")


def assignmentIsGuarded(text, name):
    """Whether a backend assignment sits inside a conditional block.

    A route assigned under a condition can be absent when it matters, and which
    condition decides it is not in this file's reach: Linux's colour route is
    guarded by `hasColorGlyphs()`, which answers false on a machine with no
    emoji font installed, so the same source yields a colour path on one machine
    and none on another. A text scan cannot see a font set.

    Walks back from the assignment to the brace that opened its block and reads
    the line that opened it. A brace-matching walk rather than an indentation
    test, because the assignments here are multi-line lambdas whose bodies are
    indented past their own statement.

    - Parameters:
      - text: The file's source.
      - name: The backend member assigned.
    - Returns: True when the nearest enclosing block is conditional.
    """
    match = re.search(r"\bbackend\.%s\s*=" % name, text)
    if not match:
        return False
    depth = 0
    for index in range(match.start() - 1, -1, -1):
        character = text[index]
        if character == "}":
            depth += 1
        elif character == "{":
            if depth == 0:
                start = text.rfind("\n", 0, index) + 1
                return GUARD.search(text[start:index]) is not None
            depth -= 1
    return False


def entriesNamedInComments(text):
    """Which lossy entries a comment in the file mentions.

    An omission stated in prose is a decision; an omission mentioned nowhere is
    indistinguishable from an oversight, which is the whole difference this
    check exists to make.

    - Parameter text: The file's source.
    - Returns: The subset of `LOSSY_ENTRIES` named on a comment line.
    """
    named = set()
    for line in text.split("\n"):
        if not COMMENT.match(line):
            continue
        for entry in LOSSY_ENTRIES:
            if entry in line:
                named.add(entry)
    return named


def lossyEntriesInSource():
    """The entries `optionalDefaultsAreLossy` actually reads.

    - Returns: The entry names it tests, or an empty set when the function moved.
    """
    try:
        with open(os.path.join(CORE, LOSSY_SOURCE), encoding="utf-8") as handle:
            text = handle.read()
    except OSError:
        return set()
    match = re.search(r"optionalDefaultsAreLossy\([^)]*\)\s*\{(.*?)\}", text, re.S)
    if not match:
        return set()
    return {name for name in re.findall(r"backend\.(\w+)\s*==\s*nullptr", match.group(1))}


# The second way in for each capability, where there is one. Reported, never
# enforced: the verdict stays on the per-cell entry alone.
#
# `CellInstanceBuilder.cpp` tries a bound cache before the per-cell entry —
# colour at :878 then :899, clusters at :945 then :955 — so a backend supplying
# the cache route draws that capability with the entry left null, and a `stated`
# row is a design choice rather than a gap. A reader who takes every `stated`
# for a loss is reading the table wrongly, and the four colour statements in
# this tree say so in prose the table never showed.
#
# The verdict stays on the entry alone, and the reason is a measurement rather
# than caution. A route is two pointers; the resource behind it is bound at runtime.
# Swift's `bindColorAtlas` binds nothing when `color` is nil, which happens on a
# geometry with no reference extents, and its wide `CoverageSheet` is built only
# when `wideSlotWidthInPixels > 0` — both leave every pointer in place and no
# resource behind them. A predicate reading the pointers calls those backends
# whole in exactly the configuration where they drop to foreground coverage.
#
# So a wrong answer here points one way on purpose. "Might lose colour when it
# does not" costs a comment; "colour is whole" costs a defect no gate reports.
SECOND_ROUTES = {
    "colorGlyph": (("bindColorAtlas", "rasteriseColorGlyph"), ("878", "899")),
    "clusterGlyph": (("bindMonoAtlas", "rasteriseClusterGlyph"), ("945", "955")),
}


def reportSecondRoutes(sites):
    """Names the backends that reach a capability without its per-cell entry.

    - Parameter sites: Repository-relative paths of the construction sites.
    """
    for entry, (route, (cacheLine, entryLine)) in SECOND_ROUTES.items():
        viaRoute = []
        for site in sites:
            try:
                with open(os.path.join(REPO, site), encoding="utf-8",
                          errors="replace") as handle:
                    text = handle.read() + siblingHeader(site)
            except OSError:
                continue
            if re.search(r"\bbackend\.%s\s*=" % entry, text):
                continue
            if all(re.search(r"\bbackend\.%s\s*=" % name, text) for name in route):
                viaRoute.append(site)
        if not viaRoute:
            continue
        print("")
        print("%s is null in %d backend(s) that reach it another way:"
              % (entry, len(viaRoute)))
        for site in viaRoute:
            print("    %s" % site)
        print("  CellInstanceBuilder.cpp:%s tries %s, and only :%s tries the entry."
              % (cacheLine, " + ".join(route), entryLine))
        print("  So `stated` on those rows is a design choice, not a gap. The verdict")
        print("  stays on the entry alone: a route is two pointers, and the resource")
        print("  behind it is bound at runtime — an empty binding leaves both set and")
        print("  still draws foreground coverage.")


def main():
    sites = constructionSites()
    if not sites and CORE != REPO:
        print("check-glyph-backend-claims: ok (no backend is built in this repository)")
        return 0
    if not sites:
        print("check-glyph-backend-claims: no file assigns backend.monoGlyph, so "
              "nothing was compared — has the seam been renamed?", file=sys.stderr)
        return 2

    # The list above is a copy, so it is checked against the function it copies.
    # A fourth lossy entry landing in Core with this file unchanged would
    # otherwise leave every frontend unexamined for it.
    inSource = lossyEntriesInSource()
    if inSource and inSource != set(LOSSY_ENTRIES):
        print("check-glyph-backend-claims: FAILED — optionalDefaultsAreLossy reads "
              "%s and this check knows %s"
              % (sorted(inSource), sorted(LOSSY_ENTRIES)), file=sys.stderr)
        print("  Update LOSSY_ENTRIES here, then re-run: until it matches, the "
              "frontends are unchecked for the difference.", file=sys.stderr)
        return 1

    width = max(len(site) for site in sites)
    print("%-*s  %s" % (width, "backend", "  ".join(LOSSY_ENTRIES)))
    problems = []
    for site in sites:
        with open(os.path.join(REPO, site), encoding="utf-8", errors="replace") as handle:
            text = handle.read()
        filled = entriesFilled(text)
        # The sibling header counts, because that is where a declaration's
        # documentation belongs and where Linux states its colour omission. A
        # check reading only the definition file would call that stated omission
        # silent and teach the author to duplicate the prose.
        named = entriesNamedInComments(text) | entriesNamedInComments(siblingHeader(site))
        marks = []
        for entry in LOSSY_ENTRIES:
            if entry in filled:
                # `supplied` alone would conflate an unconditional assignment
                # with a guarded one, and three of the four backends here are
                # guarded.
                # Which condition decides it is outside this file's
                # reach — Linux's colour route answers to whether the machine has
                # an emoji font — so the word says guarded and leaves the axis to
                # the reader. Not a verdict: a guarded route is a real route, and
                # the entry test below is what fails.
                marks.append(("guarded" if assignmentIsGuarded(text, entry)
                              else "supplied").center(len(entry)))
            elif entry in named:
                marks.append("stated".center(len(entry)))
            else:
                marks.append("SILENT".center(len(entry)))
                problems.append((site, entry))
        print("%-*s  %s" % (width, site, "  ".join(marks)))

    unclassified = {}
    for entry in UNCLASSIFIED_ENTRIES:
        if entry in LOSSY_ENTRIES:
            continue
        for site in sites:
            with open(os.path.join(REPO, site), encoding="utf-8", errors="replace") as handle:
                text = handle.read()
            if entry not in entriesFilledNamed(text, entry):
                unclassified.setdefault(entry, []).append(site)
    for entry, nullers in unclassified.items():
        print("")
        print("%s is null in %d backend(s) and outside optionalDefaultsAreLossy, so "
              "nothing here fails on it:" % (entry, len(nullers)))
        for site in nullers:
            print("    %s" % site)

    reportSecondRoutes(sites)

    if not problems:
        print("check-glyph-backend-claims: ok (%d backend(s); every null lossy entry "
              "is named where the table is built)" % len(sites))
        # What a supplied entry is not. This reads the table, so it can see that
        # a slot is filled and never that what fills it draws the right pixels —
        # and filling one flips this line to ok, which is the moment the null
        # stops asking for the fix. A fill that draws a marked wide base from
        # the wrong font still turns its claim to `supplied`: the null fails
        # this gate and names the work, and the fill passes it and draws the
        # wrong pixels.
        print("  A supplied entry is a filled slot, not a correct one: no pixel is "
              "compared here.")
        print("  Whether the render is right is the snapshot arms' question, and "
              "whether the")
        print("  shipping glyph path consults the entry at all is a third question "
              "neither asks —")
        print("  an entry is an entry plus a call site per path.")
        return 0

    print("", file=sys.stderr)
    print("check-glyph-backend-claims: FAILED — %d null lossy entry/entries no "
          "comment names" % len(problems), file=sys.stderr)
    for site, entry in problems:
        print("  %s leaves %s null and says so nowhere" % (site, entry), file=sys.stderr)
    print("  Each drops information the cell carried while leaving it inked, so "
          "no count downstream can report it.", file=sys.stderr)
    print("  Name the entry in a comment beside the table with what the frontend "
          "draws instead, or supply it.", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
