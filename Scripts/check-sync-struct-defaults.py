#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Hold the Sync seam's undefaulted scalars to a recorded set.

A caller composes one of these structs, sets the fields it means, and hands it to
an encoder. A scalar the caller skipped is indeterminate, and the encoder reads
it: a bool field can read as `load of value 18`.

Every seam struct carries default member initialisers, so the recorded residue
is empty. A struct left bare is a decision rather than an oversight only with
its reason beside it — the `RowDelta` family sits on the row-delta path
`bench_stack` times, so a construction cost there belongs in a commit that
carries a measurement.

So any residue is a decision, and this pins it. Two-sided, because a record that
can only fail one way stops describing the header:

    a struct gains an undefaulted scalar   -> FAIL, and it is probably new
    a recorded field gains a default       -> FAIL, delete its line
    the sets match                         -> pass, and say so

The first bound is the one this exists for. A struct added to this header with
bare scalars is indistinguishable, at every other signal in the tree, from one
that was considered and left.

## Why this is a check and not a sentence

The fact is easy to enumerate wrongly, even with a tool written for the job:

    a pattern requiring a field's line to end in `;`   a trailing comment hides
                                                       the field, and a struct
                                                       whose scalars all carry
                                                       comments vanishes whole
    a pattern matching names ending in `Message`       RowDelta and four
                                                       siblings escape it
    a field regex accepting any leading token          std::string and
                                                       std::vector count as
                                                       scalars, reporting every
                                                       struct as uncovered

Each reads as a confident answer. The enumerator below takes the struct list
from the header's own `struct` lines and strips comments before matching, and
its self-test proves it sees a field whose line carries a trailing comment —
the case that hides an entire struct.
"""

import argparse
import re
import subprocess
import sys
from pathlib import Path

# Anchored to this script's own location, not the invoker's directory: the
# ctest shell wrapper runs stanzas from its build tree, and a cwd-relative
# default makes the gate green from the repo root and a FileNotFoundError from
# everywhere else — two verdicts about one tree, decided by where you stand.
HEADER = str(Path(__file__).resolve().parent.parent / "Source/Core/Protocol/Sync/NativeAttachment.h")

# The scalar spellings the seam uses. A default on `std::string` or a container
# is not the question — those are already empty on construction.
SCALAR = r"(?:std::)?(?:u?int(?:8|16|32|64)_t|bool|float|double|size_t|char)"
FIELD = re.compile(r"^\s+" + SCALAR + r"\s+([a-z][A-Za-z0-9_]*)\s*;\s*$")
STRUCT = re.compile(r"^struct\s+([A-Z][A-Za-z0-9_]*)\s*\{(.*?)^\};", re.M | re.S)

# No residue: every seam struct defaults its scalars, the RowDelta family
# included.
RECORDED = {}


def undefaultedScalars(text):
    """Returns each struct's scalar fields that carry no default initialiser.

    Comments are stripped before matching: a field whose line ends in a trailing
    comment is still a field, and a pattern anchored on `;` at end-of-line skips
    it — and a struct whose fields all carry one goes unseen whole.

    - Parameter text: the header's contents.
    - Returns: {structName: [fieldName, ...]} for structs with any such field.
    """
    found = {}
    for match in STRUCT.finditer(text):
        name, body = match.group(1), match.group(2)
        fields = []
        for line in body.splitlines():
            code = line.split("//")[0].rstrip()
            field = FIELD.match(code)
            if field:
                fields.append(field.group(1))
        if fields:
            found[name] = fields
    return found


def judge(measured, record=None):
    """Compares the header against the recorded residue.

    - Parameters:
      - measured: the result of undefaultedScalars.
      - record: the residue to judge against; the tracked RECORDED when omitted.
        A parameter rather than the global so the self-test can hold every arm
        against a synthetic record — arms keyed on the live residue vanish the
        day the residue drains.
    - Returns: a list of finding strings, empty when they agree.
    """
    if record is None:
        record = RECORDED
    findings = []
    for name in sorted(set(measured) - set(record)):
        findings.append(f"{name} has undefaulted scalar(s) "
                        f"({', '.join(measured[name])}) and is not recorded — a struct "
                        "left bare and a struct nobody looked at read the same here")
    for name in sorted(set(record) - set(measured)):
        findings.append(f"{name} is recorded and now defaults every scalar — delete "
                        "its entry")
    for name in sorted(set(record) & set(measured)):
        if sorted(record[name]) != sorted(measured[name]):
            findings.append(f"{name} is recorded as {sorted(record[name])} and "
                            f"measures {sorted(measured[name])}")
    return findings


def selfTest():
    """Proves each bound fires, and that the parser sees a commented field.

    The last arm is the one that matters: a trailing comment hides a field from
    a parser blind to it, and that parser reports a clean header for the wrong
    reason.

    - Returns: 0 when every arm behaves as stated, 1 otherwise.
    """
    failures = []

    commented = "struct Probe {\n    uint16_t rows; // how many\n};\n"
    if undefaultedScalars(commented) != {"Probe": ["rows"]}:
        failures.append("a field whose line carries a trailing comment was not seen — "
                        "the defect that hid a whole struct from an earlier count")

    defaulted = "struct Probe {\n    uint16_t rows = 0; // how many\n};\n"
    if undefaultedScalars(defaulted):
        failures.append("a defaulted field was reported as undefaulted")

    container = "struct Probe {\n    std::string label;\n    std::vector<int> rows;\n};\n"
    if undefaultedScalars(container):
        failures.append("a container member was counted as a scalar — the error that "
                        "reported every struct in the header as uncovered")

    # A synthetic record, not the live one: an arm keyed on the tracked
    # residue stops running the day the residue drains, and its failure then
    # reads as the gate breaking rather than the fixture vanishing.
    synthetic = {"Probe": ["rows"], "Sibling": ["seq", "baseSeq"]}
    if judge(dict(synthetic), record=synthetic):
        failures.append("the recorded residue was flagged against itself")
    if not judge({**synthetic, "Newcomer": ["field"]}, record=synthetic):
        failures.append("an unrecorded struct with a bare scalar was not flagged")
    if not judge({k: v for k, v in synthetic.items() if k != "Probe"}, record=synthetic):
        failures.append("a recorded struct that gained defaults was not flagged")
    if not judge({**synthetic, "Probe": ["rows", "extra"]}, record=synthetic):
        failures.append("a changed field list was not flagged")

    for failure in failures:
        print("check-sync-struct-defaults --self-test: " + failure, file=sys.stderr)
    if failures:
        return 1
    print("check-sync-struct-defaults --self-test: ok (each bound fires, and the "
          "parser sees a field behind a trailing comment, ignores a defaulted one, "
          "and does not count containers as scalars)")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--header", default=HEADER)
    parser.add_argument("--self-test", action="store_true",
                        help="prove each bound fires, and that the parser is not the zero")
    arguments = parser.parse_args()

    if arguments.self_test:
        return selfTest()

    text = Path(arguments.header).read_text()
    structs = STRUCT.findall(text)
    if not structs:
        print(f"check-sync-struct-defaults: REFUSED — no struct parsed from "
              f"{arguments.header}. A header that changed shape and a header with no "
              "residue print the same verdict, so this is not a pass.", file=sys.stderr)
        return 2

    findings = judge(undefaultedScalars(text))
    if findings:
        print("check-sync-struct-defaults: the header and the recorded residue "
              "disagree:", file=sys.stderr)
        for finding in findings:
            print("    " + finding, file=sys.stderr)
        print("  A scalar with no default is indeterminate in any bare composition, "
              "and the encoder reads it. Default it, or record why it stays.",
              file=sys.stderr)
        return 1

    total = sum(len(v) for v in RECORDED.values())
    print(f"check-sync-struct-defaults: ok ({len(structs)} struct(s) parsed, "
          f"{total} undefaulted scalar(s) across {len(RECORDED)} recorded struct(s))")
    return 0


if __name__ == "__main__":
    sys.exit(main())
