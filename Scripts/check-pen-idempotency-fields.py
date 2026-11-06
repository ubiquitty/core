#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Every Pen field a renderer reads is named in the write path's idempotency probe.

cellMatchesPen (Source/Core/VT/VT.cpp) decides whether a same-character reprint
touches its cell. When the reprint matches on every field the probe compares, the
write is skipped whole — so a field the probe leaves out keeps the first pen's
value under the second print, and the cell renders with stale styling until some
differing write lands on that exact cell. Nothing reports it: the row is not
dirtied, the snapshot is not taken, the eye is the only oracle.

A probe comparing attributes and the colour indices alone leaves out the SGR 1:n
weight subfield, the underline shape, the underline colour and the font, so a
weight-3 glyph survives an SGR 0 redraw and a curly underline survives a 4:1
one. This gate keeps the correspondence from drifting: a Pen field added to the
struct and not named in cellMatchesPen fails here, at the commit that adds it,
rather than as a sticky-styling report weeks later.

Naming, not comparison depth: the probe compares the colour channels only under a
`colorRGB` index (stale channels beneath an indexed pen must not force a rewrite),
so a field can appear inside a conditional. The check is that the field is
mentioned at all — enough to force the author to decide, and to make an omission
a failure rather than a silence. A field that genuinely never reaches a renderer
goes in the exclusion set below, with the reason it does not.
"""

import contextlib
import io
import pathlib
import re
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).parent))
import gate_report

GATE = "check-pen-idempotency-fields"

# Reserved bits are always zero and carry no cell state.
RENDER_IRRELEVANT = {"underlineReserved"}


def pen_fields(root):
    """The member names of `struct Pen` in Source/Core/Pen.h.

    Read from the struct body between `struct Pen {` and its closing brace, so a
    member added to the struct joins this set with no separate list to update. A
    separate list kept by hand is the one a struct change can fail to update.
    """
    text = (root / "Source" / "Core" / "Pen.h").read_text(encoding="utf-8")
    match = re.search(r"struct Pen\s*\{(.*?)\n\};", text, re.S)
    if not match:
        return None
    fields = []
    for line in match.group(1).split("\n"):
        code = line.split("//", 1)[0]
        # One declaration line may name several members: `uint8_t a, b, c;`.
        decl = re.match(r"\s*[A-Za-z_][A-Za-z0-9_:<>]*\s+([A-Za-z_].*?);\s*$", code)
        if not decl:
            continue
        for name in decl.group(1).split(","):
            field = re.sub(r"\s*:\s*\d+$", "", name.strip())
            if re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", field):
                fields.append(field)
    return fields


def cell_matches_pen_body(root):
    """The text of cellMatchesPen in Source/Core/VT/VT.cpp, or None."""
    text = (root / "Source" / "Core" / "VT" / "VT.cpp").read_text(encoding="utf-8")
    match = re.search(r"\bcellMatchesPen\b[^\{]*\{(.*?)\n\}", text, re.S)
    return match.group(1) if match else None


def self_test():
    with tempfile.TemporaryDirectory() as directory:
        root = pathlib.Path(directory)
        core = root / "Source" / "Core"
        (core / "VT").mkdir(parents=True)
        (core / "Pen.h").write_text("struct Pen {\n    uint16_t underlineIndex : 9;\n    Role semanticRole : 2;\n    uint16_t underlineReserved : 5;\n};\n")
        body = core / "VT" / "VT.cpp"
        body.write_text("bool cellMatchesPen() {\n    return pen.underlineIndex == other.underlineIndex;\n}\n")
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            assert main(["check", str(root)]) == 1
            body.write_text(body.read_text().replace(";", " && pen.semanticRole == other.semanticRole;"))
            assert main(["check", str(root)]) == 0
    return gate_report.ok(GATE, "a missing semantic bit field fails; reserved bits are excluded")


def main(argv):
    # Anchored to the script's location when no root is given, so the verdict does
    # not depend on the caller's directory.
    root = pathlib.Path(argv[1]) if len(argv) > 1 else pathlib.Path(__file__).resolve().parent.parent

    fields = pen_fields(root)
    if fields is None:
        return gate_report.failed(
            GATE, "no `struct Pen { ... }` found in Source/Core/Pen.h; the pattern "
            "stopped matching rather than the struct stopping existing",
            ["struct Pen not found"])
    if not fields:
        return gate_report.failed(
            GATE, "struct Pen parsed to zero fields; the member spelling drifted "
            "and this gate would pass having read nothing",
            ["0 Pen fields"])

    body = cell_matches_pen_body(root)
    if body is None:
        return gate_report.failed(
            GATE, "no cellMatchesPen definition found in Source/Core/VT/VT.cpp; the "
            "reprint idempotency probe moved or was renamed",
            ["cellMatchesPen not found"])

    missing = []
    for field in fields:
        if field in RENDER_IRRELEVANT:
            continue
        if not re.search(r"[.>]" + re.escape(field) + r"\b", body):
            missing.append(field)

    if missing:
        for field in missing:
            gate_report.finding(
                "Source/Core/VT/VT.cpp", GATE,
                f"Pen.{field} is not named in cellMatchesPen")
        print("  A reprint that matches on the compared fields skips the write, so a "
              "field left out keeps the first pen's value on the cell.", file=sys.stderr)
        print("  Compare it in cellMatchesPen (Source/Core/VT/VT.cpp), or add it to "
              "RENDER_IRRELEVANT here with the reason no renderer reads it.", file=sys.stderr)
        return gate_report.failed(
            GATE, "Pen field(s) a reprint would leave stale, unnamed in cellMatchesPen",
            [f"Pen.{f}" for f in missing])

    return gate_report.ok(
        GATE, "%d Pen field(s), each named in cellMatchesPen%s"
        % (len(fields),
           "" if not RENDER_IRRELEVANT else f" ({len(RENDER_IRRELEVANT)} excluded as render-irrelevant)"))


if __name__ == "__main__":
    sys.exit(self_test() if "--self-test" in sys.argv else main(sys.argv))
