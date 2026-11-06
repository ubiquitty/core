#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Say what two snapshot rasters disagree about, with the predicate taken from the image.

`run.py` reports that a case differs and by how much. Acting on a difference
needs its shape — which rows, how much ink each side draws,
whether ink moved or stopped being drawn — and that measurement was hand-rolled
four times in one night, once wrongly:

    lit = pixel != (0, 0, 0)          # on Solarized Light, true for every pixel
    lit = sum(pixel) > 200            # background sums to 726, so also every pixel

Both make the whole row lit, both yield an affirmative whatever the image holds,
and one of them circulated a row-ink table as evidence that nothing
had moved. The trap is recorded in `Tests/Snapshots/known-failing-portable.txt`
too, where it reached nobody: prose beside an instance is found by someone
already reading that instance, not by the next person about to repeat it.

So the background is not an argument here. It comes from the top-left pixel of
the render, the way `run.py` takes it, because neither this script nor the
person running it knows the theme.

    ./Scripts/compare-snapshot.py <golden.png> <render.png>

Decoding is `run.py`'s `read_png_rgb`, imported rather than copied, so a raster
this disagrees with the tier about is a bug in one place.
"""

import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, ".."))
sys.path.insert(0, os.path.join(REPO, "Tests", "Snapshots"))

import pathlib  # noqa: E402

import run  # noqa: E402


def backgroundColour(pixels):
    """The image's own background, read from its top-left pixel.

    Not a parameter and not a default. A caller who supplies this supplies the
    answer, and a predicate that decides what the background is instead of
    asking the image is wrong for every image it did not anticipate.

    - Parameter pixels: The decoded RGB bytes.
    - Returns: The `(r, g, b)` the image uses as its ground.
    """
    return tuple(pixels[0:3])


def rowInk(pixels, width, y, background):
    """How many pixels in a row differ from the background.

    - Parameters:
      - pixels: The decoded RGB bytes.
      - width: The image width in pixels.
      - y: The row to count.
      - background: The colour that counts as unlit.
    - Returns: The count of lit pixels in that row.
    """
    start = y * width * 3
    return sum(1 for x in range(width)
               if tuple(pixels[start + x * 3:start + x * 3 + 3]) != background)


def litRuns(pixels, width, y, background, shortest):
    """The contiguous lit spans in a row, as `(first, last)` inclusive.

    Runs are what make a translation legible: a rule that moved keeps its extent
    and changes its row, where a rule that stopped being drawn loses the span.

    - Parameters:
      - pixels: The decoded RGB bytes.
      - width: The image width in pixels.
      - y: The row to scan.
      - background: The colour that counts as unlit.
      - shortest: The shortest run worth reporting.
    - Returns: The qualifying runs, in order.
    """
    start = y * width * 3
    runs = []
    opened = None
    for x in range(width):
        lit = tuple(pixels[start + x * 3:start + x * 3 + 3]) != background
        if lit and opened is None:
            opened = x
        elif not lit and opened is not None:
            runs.append((opened, x - 1))
            opened = None
    if opened is not None:
        runs.append((opened, width - 1))
    return [span for span in runs if span[1] - span[0] + 1 >= shortest]


def inkMoved(golden, render, width, height, background):
    """Lit pixels present in one raster and absent from the other.

    Counted per pixel rather than per row. A row delta subtracts one side from
    the other, so ink that moves along a row cancels to zero and a translation
    within one row reads as no change at all — the same cancellation, one axis
    down, that makes a differing-pixel fraction miss a one-row subject.

    - Parameters:
      - golden: The reference RGB bytes.
      - render: The produced RGB bytes.
      - width: The shared image width.
      - height: The shared image height.
      - background: The colour that counts as unlit.
    - Returns: `(lost, gained)` — lit only in the golden, and lit only in the
      render.
    """
    lost = gained = 0
    for offset in range(0, width * height * 3, 3):
        goldenLit = tuple(golden[offset:offset + 3]) != background
        renderLit = tuple(render[offset:offset + 3]) != background
        if goldenLit and not renderLit:
            lost += 1
        elif renderLit and not goldenLit:
            gained += 1
    return lost, gained


def differingRows(golden, render, width, height):
    """Rows where the two rasters disagree at all, with the count for each.

    - Parameters:
      - golden: The reference RGB bytes.
      - render: The produced RGB bytes.
      - width: The shared image width.
      - height: The shared image height.
    - Returns: `(y, differing)` pairs, in row order.
    """
    rows = []
    for y in range(height):
        start = y * width * 3
        differing = sum(1 for x in range(width)
                        if golden[start + x * 3:start + x * 3 + 3]
                        != render[start + x * 3:start + x * 3 + 3])
        if differing:
            rows.append((y, differing))
    return rows


def report(goldenPath, renderPath, shortestRun):
    """Print what the two rasters disagree about.

    - Parameters:
      - goldenPath: The reference image.
      - renderPath: The image under test.
      - shortestRun: The shortest lit run worth printing.
    - Returns: 0 when they agree, 1 when they differ, 2 when they cannot be
      compared.
    """
    goldenWidth, goldenHeight, golden = run.read_png_rgb(pathlib.Path(goldenPath))
    renderWidth, renderHeight, render = run.read_png_rgb(pathlib.Path(renderPath))

    print("golden %dx%d  %s" % (goldenWidth, goldenHeight, goldenPath))
    print("render %dx%d  %s" % (renderWidth, renderHeight, renderPath))

    if (goldenWidth, goldenHeight) != (renderWidth, renderHeight):
        print("", file=sys.stderr)
        print("compare-snapshot: cannot compare — the two differ in size, so no "
              "pixel has a counterpart.", file=sys.stderr)
        return 2

    background = backgroundColour(render)
    # The share is printed because it is the number that exposes a wrong
    # predicate. Assume black on a light theme and every pixel counts as lit:
    # the share reads 0%, row ink equals the image width, and lost/gained comes
    # out (0, 0) for a raster missing 243 pixels of ink. A reader who sees 0%
    # here knows the rows below say nothing.
    ground = sum(1 for offset in range(0, len(render), 3)
                 if tuple(render[offset:offset + 3]) == background)
    print("background %s, from the render's top-left pixel — not assumed "
          "(%.1f%% of the render)"
          % (background, 100.0 * ground / (renderWidth * renderHeight)))

    rows = differingRows(golden, render, goldenWidth, goldenHeight)
    if not rows:
        print("")
        print("identical: 0 differing pixels")
        return 0

    total = sum(count for _, count in rows)
    print("")
    print("%d differing pixel(s) over %d row(s) (%.4f%% of the image)"
          % (total, len(rows), 100.0 * total / (goldenWidth * goldenHeight)))
    print("")
    print("  row   golden ink   render ink   delta   runs (>=%dpx)" % shortestRun)
    lost, gained = inkMoved(golden, render, goldenWidth, goldenHeight, background)
    for y, _ in rows:
        goldenInk = rowInk(golden, goldenWidth, y, background)
        renderInk = rowInk(render, goldenWidth, y, background)
        goldenRuns = litRuns(golden, goldenWidth, y, background, shortestRun)
        renderRuns = litRuns(render, goldenWidth, y, background, shortestRun)
        print("  %4d   %10d   %10d   %+5d   golden %s"
              % (y, goldenInk, renderInk, renderInk - goldenInk, goldenRuns or "-"))
        print("  %4s   %10s   %10s   %5s   render %s"
              % ("", "", "", "", renderRuns or "-"))

    print("")
    print("ink lost %d, gained %d" % (lost, gained))

    # A shape, not a verdict. Balanced ink is consistent with a golden that
    # predates a geometry change AND with a glyph that moved because something
    # broke; an imbalance is consistent both with ink that stopped being drawn
    # and with a case whose subject legitimately shrank. Triage from these numbers
    # reversed on pixels once already, so the script names what it can separate
    # and stops there.
    if lost and gained and 0.5 <= lost / gained <= 2.0:
        print("shape: comparable ink either side — consistent with a translation.")
    elif lost > gained:
        print("shape: ink lost without matching gain — consistent with something "
              "no longer drawn.")
    else:
        print("shape: ink gained without matching loss — consistent with "
              "something newly drawn.")
    print("Both readings of any shape here are open: look at the images before "
          "recording or re-recording a golden.")
    return 1


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("golden", help="the reference PNG")
    parser.add_argument("render", help="the PNG under test")
    parser.add_argument("--shortest-run", type=int, default=8,
                        help="shortest lit run to print (default 8)")
    arguments = parser.parse_args()

    for path in (arguments.golden, arguments.render):
        if not os.path.isfile(path):
            print("compare-snapshot: no such file: %s" % path, file=sys.stderr)
            return 2

    return report(arguments.golden, arguments.render, arguments.shortest_run)


if __name__ == "__main__":
    sys.exit(main())
