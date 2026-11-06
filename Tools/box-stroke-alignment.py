#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0
"""box-stroke-alignment.py — measure where a rendered box rule changes place or weight.

A box-drawing rule is one stroke from end to end: every scanline across a
vertical rule, and every column across a horizontal one, carries the same
coverage profile. A rule assembled from a font piece and a renderer-drawn
piece, or from two cells whose pieces disagree, shows as a place where that
profile moves (its centroid) or changes mass (its weight).

The tool reads a PNG, converts it to coverage against the background, and for
both axes traces every narrow stroke scanline by scanline. A trace is split
into runs of constant centroid and mass; each boundary between two runs at
least `--min-run` long is reported with where it lies, how far the stroke
moved and how its weight changed. A 1-2 px break in a trace whose two sides
line up is reported as a gap.

The report is the measurement, not a verdict: a rounded corner's arc or a
glyph's own design can produce boundaries too, so the cases this was written
for keep their text rows out with `--skip-top`. Exit status is 1 when any
boundary or gap is found, so a fresh render can be held to zero.

Standard library only, so it runs on every build host.

Usage:
  box-stroke-alignment.py IMAGE [--pitch W H] [--origin X Y] [--skip-top PX]
"""

import argparse
import struct
import sys
import zlib


def readPNG(path):
    """Decode an 8-bit non-interlaced PNG into (width, height, luminance rows)."""
    with open(path, "rb") as handle:
        data = handle.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise SystemExit(f"{path}: not a PNG")
    position = 8
    width = height = 0
    colorType = None
    bitDepth = None
    interlace = 0
    palette = None
    compressed = bytearray()
    while position < len(data):
        (length,) = struct.unpack(">I", data[position:position + 4])
        kind = data[position + 4:position + 8]
        body = data[position + 8:position + 8 + length]
        position += 12 + length
        if kind == b"IHDR":
            width, height, bitDepth, colorType, _, _, interlace = struct.unpack(">IIBBBBB", body)
        elif kind == b"PLTE":
            palette = [tuple(body[i:i + 3]) for i in range(0, len(body), 3)]
        elif kind == b"IDAT":
            compressed += body
        elif kind == b"IEND":
            break
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}.get(colorType)
    if bitDepth != 8 or interlace != 0 or channels is None:
        raise SystemExit(f"{path}: only 8-bit non-interlaced PNGs are read")
    raw = zlib.decompress(bytes(compressed))
    stride = width * channels
    previous = bytearray(stride)
    rows = []
    offset = 0
    for _ in range(height):
        filterType = raw[offset]
        line = bytearray(raw[offset + 1:offset + 1 + stride])
        offset += 1 + stride
        for i in range(stride):
            left = line[i - channels] if i >= channels else 0
            up = previous[i]
            upLeft = previous[i - channels] if i >= channels else 0
            if filterType == 1:
                line[i] = (line[i] + left) & 0xFF
            elif filterType == 2:
                line[i] = (line[i] + up) & 0xFF
            elif filterType == 3:
                line[i] = (line[i] + ((left + up) >> 1)) & 0xFF
            elif filterType == 4:
                estimate = left + up - upLeft
                distanceLeft, distanceUp, distanceUpLeft = abs(estimate - left), abs(estimate - up), abs(estimate - upLeft)
                predictor = left if distanceLeft <= distanceUp and distanceLeft <= distanceUpLeft else (up if distanceUp <= distanceUpLeft else upLeft)
                line[i] = (line[i] + predictor) & 0xFF
        previous = line
        luminance = []
        for x in range(width):
            pixel = line[x * channels:(x + 1) * channels]
            if colorType == 3:
                red, green, blue = palette[pixel[0]]
            elif channels <= 2:
                red = green = blue = pixel[0]
            else:
                red, green, blue = pixel[0], pixel[1], pixel[2]
            luminance.append(0.2126 * red + 0.7152 * green + 0.0722 * blue)
        rows.append(luminance)
    return width, height, rows


def coveragePlane(width, height, rows):
    """Coverage in [0, 1] against the most common luminance (the background)."""
    histogram = {}
    for row in rows:
        for value in row:
            key = int(round(value))
            histogram[key] = histogram.get(key, 0) + 1
    background = max(histogram, key=histogram.get)
    farthest = max(histogram, key=lambda key: abs(key - background) if histogram[key] >= 8 else -1)
    span = (farthest - background) or 1
    return [[min(1.0, max(0.0, (value - background) / span)) for value in row] for row in rows]


def strokeSlices(plane, width, height, maximumWidth, threshold):
    """Per scanline, the narrow inked runs across a vertical stroke."""
    slices = []
    for y in range(height):
        row = plane[y]
        found = []
        x = 0
        while x < width:
            if row[x] < threshold:
                x += 1
                continue
            start = x
            while x < width and row[x] >= threshold:
                x += 1
            if x - start <= maximumWidth:
                mass = sum(row[start:x])
                centroid = sum((i + 0.5) * row[i] for i in range(start, x)) / mass
                if max(row[start:x]) >= 0.5:
                    found.append((start, x, centroid, mass))
        slices.append(found)
    return slices


def traces(slices):
    """Links each scanline's runs to the overlapping run on the scanline before."""
    open_ = []
    done = []
    for y, found in enumerate(slices):
        nextOpen = []
        taken = set()
        for start, end, centroid, mass in found:
            match = None
            for index, trace in enumerate(open_):
                if index in taken:
                    continue
                lastStart, lastEnd = trace[-1][1], trace[-1][2]
                if start < lastEnd and lastStart < end:
                    match = index
                    break
            if match is None:
                nextOpen.append([(y, start, end, centroid, mass)])
            else:
                taken.add(match)
                open_[match].append((y, start, end, centroid, mass))
                nextOpen.append(open_[match])
        done.extend(trace for index, trace in enumerate(open_) if index not in taken)
        open_ = nextOpen
    done.extend(open_)
    return done


def runsOf(trace, tolerance):
    """Splits a trace into runs of constant centroid and mass."""
    runs = [[trace[0]]]
    for sample in trace[1:]:
        last = runs[-1][-1]
        if sample[0] == last[0] + 1 and abs(sample[3] - last[3]) <= tolerance and abs(sample[4] - last[4]) <= tolerance:
            runs[-1].append(sample)
        else:
            runs.append([sample])
    return runs


def cellLabel(along, across, axis, pitch, origin):
    if pitch is None:
        return ""
    if axis == "vertical":
        x, y = across, along
    else:
        x, y = along, across
    column = (x - origin[0]) / pitch[0]
    row = (y - origin[1]) / pitch[1]
    alongPitch = pitch[1] if axis == "vertical" else pitch[0]
    alongOrigin = origin[1] if axis == "vertical" else origin[0]
    boundary = (along - alongOrigin) / alongPitch
    where = "cell edge" if abs(boundary - round(boundary)) * alongPitch <= 1.0 else "inside cell"
    return f" r{int(row)}c{int(column)} {where}"


def analyse(plane, width, height, axis, arguments):
    if axis == "horizontal":
        plane = [[plane[y][x] for y in range(height)] for x in range(width)]
        width, height = height, width
    findings = []
    for trace in traces(strokeSlices(plane, width, height, arguments.max_width, arguments.threshold)):
        if trace[0][0] < arguments.skip_top and axis == "vertical":
            trace = [sample for sample in trace if sample[0] >= arguments.skip_top]
        if len(trace) < 2 * arguments.min_run:
            continue
        runs = [run for run in runsOf(trace, arguments.tolerance)]
        for before, after in zip(runs, runs[1:]):
            if len(before) < arguments.min_run or len(after) < arguments.min_run:
                continue
            a, b = before[-1], after[0]
            kind = "gap" if b[0] != a[0] + 1 else "step"
            findings.append((axis, kind, a, b))
    if axis == "horizontal":
        findings = [finding for finding in findings if finding[2][3] >= arguments.skip_top]
    return findings


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("image")
    parser.add_argument("--pitch", type=float, nargs=2, metavar=("W", "H"), help="cell pitch, to label findings by cell")
    parser.add_argument("--origin", type=float, nargs=2, metavar=("X", "Y"), default=(0.0, 0.0), help="top-left of cell r0c0")
    parser.add_argument("--skip-top", type=int, default=0, help="ignore strokes above this pixel row (a text row)")
    parser.add_argument("--max-width", type=int, default=6, help="widest run read as one stroke, in pixels")
    parser.add_argument("--threshold", type=float, default=0.04, help="coverage that counts as ink")
    parser.add_argument("--tolerance", type=float, default=0.12, help="centroid/mass change tolerated between scanlines, in pixels")
    parser.add_argument("--min-run", type=int, default=3, help="shortest constant run either side of a reported boundary")
    arguments = parser.parse_args()

    width, height, rows = readPNG(arguments.image)
    plane = coveragePlane(width, height, rows)
    findings = analyse(plane, width, height, "vertical", arguments) + analyse(plane, width, height, "horizontal", arguments)
    print(f"{arguments.image}: {width}x{height}, {len(findings)} misalignment(s)")
    for axis, kind, a, b in findings:
        along = b[0]
        across = (a[3] + b[3]) / 2
        label = cellLabel(along, across, axis, arguments.pitch, arguments.origin)
        if axis == "vertical":
            where = f"x~{across:6.2f} at y={along:3d}"
        else:
            where = f"y~{across:6.2f} at x={along:3d}"
        print(f"  {axis:10s} {kind:4s} {where}{label}: centre {a[3]:6.2f} -> {b[3]:6.2f} ({b[3] - a[3]:+.2f} px),"
              f" weight {a[4]:4.2f} -> {b[4]:4.2f} ({b[4] - a[4]:+.2f} px)")
    return 1 if findings else 0


if __name__ == "__main__":
    sys.exit(main())
