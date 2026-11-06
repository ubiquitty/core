#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""check-bench-mirrors.py — a hand-copied struct still matches the one it copies.

`Tests/Performance/bench_cellops.cpp` restates `Pen` and `Cell` field for field
because it compiles with a bare clang++ line and no repo headers, so it cannot
`static_assert` against the real types. What it has instead is
`sizeof(BenchCell) == 32`, and its own comment says what that misses:

    The size assertion catches a field added or dropped. It does not catch a
    reorder, which leaves the total at 32 bytes while moving where the stores
    land inside the line — the exact thing every candidate below is timing.

It does not catch a RENAME either, which is how a mirror starts describing a
struct that no longer exists while still measuring 32 bytes of something.

So the cross-check has to be textual, from outside both files. This compares the
FIELD SEQUENCE — names and bit-field widths, in order — and fails on any difference.
A rename fires it, which is intended: a bench whose mirror names a field the
production type dropped is measuring a layout nobody can confirm.

The pairs are derived from the `Bench` prefix, so a mirror added tomorrow is
checked without anyone editing this file. A mirror naming a type no header
declares fails rather than being skipped.
"""

import pathlib
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).parent))
import gate_report

GATE = "check-bench-mirrors"

import re
import sys

BENCH_DIR = "Tests/Performance"
BENCH_PREFIX = "Bench"

# Discovery is derived; the pairing is declared.
#
# `struct BenchCell` finds itself — no list, so a mirror added tomorrow is
# checked without anyone editing this file. What it cannot do is find its own
# original: seven headers declare a `Cell` and three declare a `Pen`, so the
# name alone picks the wrong one as readily as the right one. A resolver that
# guesses would pin the bench to a struct nobody meant, and report a match.
#
# So each mirror names its header, and both directions fail: a mirror with no
# declaration, and a declaration whose header has no such struct.
#
#     // mirrors: Source/Core/Cell.h Cell
#     struct BenchCell {
DECLARATION = re.compile(
    r"^//\s*mirrors:\s*(\S+)\s+(\w+)\s*$\n\s*struct\s+(%s\w+)\b" % BENCH_PREFIX, re.M)


def mirrors():
    """Returns [(bench path, bench struct, real path, real struct)], and errors."""
    found, errors = [], []
    for path in sorted(pathlib.Path(BENCH_DIR).rglob("*")):
        if path.suffix not in (".cpp", ".h"):
            continue
        text = path.read_text(encoding="utf-8")
        declared = {m.group(3): (m.group(1), m.group(2)) for m in DECLARATION.finditer(text)}
        for name in re.findall(r"^struct\s+(%s\w+)\b" % BENCH_PREFIX, text, re.M):
            if name not in declared:
                errors.append(
                    "%s::%s is a mirror with no `// mirrors: <header> <struct>` line "
                    "above it, so nothing compares it to the struct it copies"
                    % (path, name))
                continue
            real_path, real_name = declared[name]
            found.append((str(path), name, real_path, real_name))
    return found, errors


# A declaration line. The type may be templated and so may contain commas and
# spaces — `std::array<uint32_t, cellMaxCombining>` is a field in the struct
# this checks, and a pattern that stops at the comma drops it from one side
# while matching the other, which reads as a mismatch in the field it did parse.
#
# Skipping a line it cannot parse is worse than misreading it, because it is
# silent: the production struct comes back one field short and the diff blames
# the bench. A line inside a struct body that this cannot read is reported,
# never dropped.
FIELD = re.compile(
    r"^\s*(?:const\s+)?"
    r"((?:[A-Za-z_][\w:]*)(?:\s*<[^;]*>)?)"  # type, templates included
    r"\s+([A-Za-z_]\w*(?:\s*,\s*[A-Za-z_]\w*)*)"  # one or more names
    r"\s*(?:\[[^\]]*\])?\s*(?::\s*(\d+))?\s*;")


def fields(path, name):
    """Returns [(type, field, bit width)] in declaration order."""
    try:
        text = open(path, encoding="utf-8").read()
    except OSError as error:
        return None, "cannot read %s: %s" % (path, error)
    match = re.search(r"\bstruct\s+%s\b[^{;]*\{" % re.escape(name), text)
    if not match:
        return None, "no `struct %s` in %s" % (name, path)
    depth, i = 1, match.end()
    while i < len(text) and depth:
        depth += (text[i] == "{") - (text[i] == "}")
        i += 1
    out, unread, nested = [], [], 0
    for line in text[match.end():i - 1].splitlines():
        line = line.split("//")[0].rstrip()
        stripped = line.strip()
        # Only lines at the struct's own level are fields. A method body is
        # nested, and its statements look enough like declarations to be
        # counted as layout by a scan that does not track depth.
        was_nested = nested
        nested += stripped.count("{") - stripped.count("}")
        if was_nested or nested:
            continue
        # Methods, static members, and anything that is not a declaration.
        if not stripped or "(" in stripped or stripped.startswith(("static", "using", "#", "}")):
            continue
        found = FIELD.match(line)
        if not found:
            unread.append(stripped)
            continue
        for field in (n.strip() for n in found.group(2).split(",")):
            out.append((found.group(1), field, found.group(3)))
    if unread:
        return None, ("%s::%s has %d line(s) this cannot parse, so the field list is "
                      "incomplete and any verdict from it is worthless:\n      %s"
                      % (path, name, len(unread), "\n      ".join(unread)))
    return out, None


def self_test():
    with tempfile.TemporaryDirectory() as directory:
        path = pathlib.Path(directory) / "fields.h"
        path.write_text("struct Pen {\n    uint16_t index : 9;\n    Role role : 2;\n    uint8_t red, green;\n};\n")
        expected = [("uint16_t", "index", "9"), ("Role", "role", "2"),
                    ("uint8_t", "red", None), ("uint8_t", "green", None)]
        assert fields(path, "Pen") == (expected, None)
        path.write_text(path.read_text().replace("role : 2", "role : 3"))
        changed, error = fields(path, "Pen")
        assert error is None and changed != expected
        path.write_text("struct Pen {\n    Role role : unknown;\n};\n")
        assert fields(path, "Pen")[1] is not None
    return gate_report.ok(GATE, "bit fields retain their widths; unread declarations fail")


def main():
    pairs, failures = mirrors()
    for bench_path, bench_name, real_path, real_name in pairs:
        mirrored, error = fields(bench_path, bench_name)
        if error:
            failures.append(error)
            continue
        real, error = fields(real_path, real_name)
        if error:
            failures.append(error)
            continue
        # Names must match; the bench spells its own struct types with the
        # Bench prefix, so compare those by position rather than by spelling.
        mirrored_names = [f if width is None else f"{f}:{width}" for _, f, width in mirrored]
        real_names = [f if width is None else f"{f}:{width}" for _, f, width in real]
        if mirrored_names != real_names:
            only_bench = [f for f in mirrored_names if f not in real_names]
            only_real = [f for f in real_names if f not in mirrored_names]
            failures.append(
                "%s::%s does not mirror %s::%s\n"
                "    bench: %s\n"
                "    real : %s\n"
                "    only in bench: %s\n"
                "    only in real : %s"
                % (bench_path, bench_name, real_path, real_name,
                   " ".join(mirrored_names), " ".join(real_names),
                   " ".join(only_bench) or "(none)", " ".join(only_real) or "(none)"))

    if failures:
        for failure in failures:
            print("  " + failure.replace("\n", "\n  "))
        print("  A bench that hand-copies a production struct measures the layout it")
        print("  copied, not the one that ships. Update the mirror, or delete it and")
        print("  include the real header.")
        return gate_report.failed(GATE, "bench mirror(s) that no longer match the real struct",
                                  [f.split("\n")[0] for f in failures])

    if not pairs:
        return gate_report.failed(GATE, "no `struct Bench*` found under %s — the mirrors "
                                  "were renamed or moved, and nothing here is compared"
                                  % BENCH_DIR, [])
    return gate_report.ok(GATE, f"{len(pairs)} derived mirrored struct(s) match")


if __name__ == "__main__":
    sys.exit(self_test() if "--self-test" in sys.argv else main())
