#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0
"""Hold the remote reference to FrameKind names, byte values and directionOf.

Every declaration needs exactly one row. A default arm, skipped declaration or
duplicate row cannot count as coverage. Unfamiliar enum/switch forms fail closed
rather than being silently read as a partial catalog. Stdlib only.
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gate_report  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HEADER = "Source/Core/Protocol/Sync/Envelope.h"
SOURCE = "Source/Core/Protocol/Sync/Envelope.cpp"
DOC = "Docs/sync-protocol.md"
CATALOG_START = "<!-- sync-frame-catalog:start -->"
CATALOG_END = "<!-- sync-frame-catalog:end -->"
DIRECTIONS = {"ClientToHost": "client → host", "HostToClient": "host → client", "Both": "both"}


class ParseError(ValueError):
    """A required source or table form could not be read completely."""


def read(path):
    with open(os.path.join(REPO, path), encoding="utf-8") as handle:
        return handle.read()


def withoutComments(text):
    # The checked enum and direction switch contain no string literals.
    return re.sub(r"/\*.*?\*/|//[^\n]*", "", text, flags=re.S)


def unique(entries, label):
    """Preserve entries until both name and byte uniqueness are checked."""
    result, values = {}, {}
    for name, value in entries:
        if name in result:
            raise ParseError("%s repeats %s" % (label, name))
        if value in values:
            raise ParseError("%s shares value %d between %s and %s" % (label, value, values[value], name))
        result[name], values[value] = value, name
    if not result:
        raise ParseError("%s has no entries" % label)
    return result


def kindsFrom(text):
    matches = re.findall(r"\benum\s+class\s+FrameKind\s*:\s*std::uint8_t\s*\{([^{}]*)\}\s*;",
                         withoutComments(text), re.S)
    if len(matches) != 1:
        raise ParseError("expected one explicit FrameKind enum, found %d" % len(matches))
    members = matches[0].split(",")
    if not members[-1].strip():
        members.pop()
    entries = []
    for member in members:
        found = re.fullmatch(r"\s*([A-Za-z_]\w*)\s*=\s*(0[xX][0-9A-Fa-f]+|0|[1-9][0-9]*)\s*", member)
        if not found:
            raise ParseError("unreadable FrameKind declaration: %r" % member.strip())
        literal = found.group(2)
        value = int(literal, 16 if literal.lower().startswith("0x") else 10)
        if not 0 <= value <= 255:
            raise ParseError("FrameKind %s is outside a byte" % found.group(1))
        entries.append((found.group(1), value))
    return unique(entries, "FrameKind enum")


def directionsFrom(text):
    """Every kind must have an explicit case, never just the fallback return."""
    text = withoutComments(text)
    functions = list(re.finditer(r"\bDirection\s+directionOf\s*\(\s*FrameKind\s+(\w+)\s*\)\s*\{", text))
    if len(functions) != 1:
        raise ParseError("expected one directionOf definition, found %d" % len(functions))
    function = functions[0]
    found = re.match(r"\s*switch\s*\(\s*" + re.escape(function.group(1))
                     + r"\s*\)\s*\{([^{}]*)\}\s*return\s+Direction::Both\s*;\s*\}",
                     text[function.end():], re.S)
    if not found:
        raise ParseError("directionOf is not an explicit case/return switch")
    switch = found.group(1)
    group = re.compile(r"\s*((?:case\s+FrameKind::[A-Za-z_]\w*\s*:\s*)+)"
                       r"return\s+Direction::(ClientToHost|HostToClient|Both)\s*;")
    result, offset = {}, 0
    while switch[offset:].strip():
        matched = group.match(switch, offset)
        if not matched:
            raise ParseError("unreadable directionOf switch near %r" % switch[offset:offset + 80].strip())
        for name in re.findall(r"case\s+FrameKind::(\w+)\s*:", matched.group(1)):
            if name in result:
                raise ParseError("directionOf repeats %s" % name)
            result[name] = DIRECTIONS[matched.group(2)]
        offset = matched.end()
    if not result:
        raise ParseError("directionOf has no explicit cases")
    return result


def rowsFrom(text):
    if text.count(CATALOG_START) != 1 or text.count(CATALOG_END) != 1:
        raise ParseError("expected one marked frame catalog")
    start, end = text.index(CATALOG_START) + len(CATALOG_START), text.index(CATALOG_END)
    if end < start:
        raise ParseError("frame catalog markers are reversed")
    lines = [line.strip() for line in text[start:end].splitlines() if line.strip()]
    if len(lines) < 3 or lines[0] != "| Kind | Value | Direction | Channel | Body |":
        raise ParseError("frame catalog header is missing or changed")
    if not re.fullmatch(r"\|\s*-+\s*\|\s*-+\s*\|\s*-+\s*\|\s*-+\s*\|\s*-+\s*\|", lines[1]):
        raise ParseError("frame catalog separator is malformed")
    entries, directions = [], {}
    for line in lines[2:]:
        found = re.fullmatch(
            r"\|\s*\x60([A-Za-z_]\w*)\x60\s*\|\s*(0[xX][0-9A-Fa-f]+|0|[1-9][0-9]*)\s*"
            r"\|\s*(client → host|host → client|both)\s*\|\s*([^|]+?)\s*\|\s*([^|]+?)\s*\|", line)
        if not found:
            raise ParseError("unreadable frame catalog row: %r" % line)
        name, literal, direction, _channel, _body = found.groups()
        value = int(literal, 16 if literal.lower().startswith("0x") else 10)
        if not 0 <= value <= 255:
            raise ParseError("documented %s is outside a byte" % name)
        entries.append((name, value))
        directions[name] = direction
    return unique(entries, "frame catalog"), directions


def failures(header, source, document):
    try:
        kinds, directions = kindsFrom(header), directionsFrom(source)
        rows, documentedDirections = rowsFrom(document)
    except ParseError as error:
        return [str(error)]
    out = []
    for name in sorted(kinds):
        if name not in directions:
            out.append("directionOf has no explicit case for %s" % name)
        if name not in rows:
            out.append("FrameKind %s = %d has no catalog row" % (name, kinds[name]))
        elif rows[name] != kinds[name]:
            out.append("%s is %d in the catalog, %d in FrameKind" % (name, rows[name], kinds[name]))
        elif name in directions and documentedDirections[name] != directions[name]:
            out.append("%s is %s in the catalog, %s in directionOf"
                       % (name, documentedDirections[name], directions[name]))
    out += ["directionOf names undeclared %s" % name for name in sorted(directions.keys() - kinds.keys())]
    out += ["catalog names undeclared %s" % name for name in sorted(rows.keys() - kinds.keys())]
    return out


def selfTest():
    header = "enum class FrameKind : std::uint8_t { Greet = 1, Welcome = 0x02, Bye = 10, };"
    source = """Direction directionOf(FrameKind kind) {
        switch (kind) {
        case FrameKind::Greet: return Direction::ClientToHost;
        case FrameKind::Welcome: return Direction::HostToClient;
        // case FrameKind::Ignored: return Direction::Both;
        case FrameKind::Bye: return Direction::Both;
        } return Direction::Both;
    }"""
    row = "| \x60Greet\x60 | 1 | client → host | 0 | opening request |"
    doc = (CATALOG_START + "\n| Kind | Value | Direction | Channel | Body |\n"
           "| --- | --- | --- | --- | --- |\n" + row
           + "\n| \x60Welcome\x60 | 2 | host → client | 0 | selection |"
           + "\n| \x60Bye\x60 | 10 | both | 0 | close |\n" + CATALOG_END)
    cases = [
        ("complete", header, source, doc, False),
        ("commented enum", header.replace("Greet = 1,", "/* Ghost = 99, */ Greet = 1,"), source, doc, False),
        ("grouped directions", header, source.replace(
            "case FrameKind::Welcome: return Direction::HostToClient;", "case FrameKind::Welcome:").replace(
                "case FrameKind::Bye: return Direction::Both;", "case FrameKind::Bye: return Direction::HostToClient;"),
         doc.replace("| 10 | both |", "| 10 | host → client |"), False),
        ("missing enum", "", source, doc, True),
        ("two enums", header + header, source, doc, True),
        ("duplicate enum name", header.replace("Welcome =", "Greet ="), source, doc, True),
        ("duplicate enum value", header.replace("0x02", "1"), source, doc, True),
        ("implicit enum value", header.replace("Greet = 1", "Greet"), source, doc, True),
        ("expression enum value", header.replace("Greet = 1", "Greet = 1 << 0"), source, doc, True),
        ("octal enum value", header.replace("Greet = 1", "Greet = 010"), source, doc, True),
        ("out-of-byte enum", header.replace("Greet = 1", "Greet = 256"), source, doc, True),
        ("missing function", header, "", doc, True),
        ("missing explicit direction", header, source.replace(
            "case FrameKind::Greet: return Direction::ClientToHost;", ""), doc, True),
        ("duplicate direction", header, source.replace(
            "case FrameKind::Welcome:", "case FrameKind::Greet: case FrameKind::Welcome:"), doc, True),
        ("stale direction", header, source.replace("FrameKind::Greet", "FrameKind::Ghost"), doc, True),
        ("unknown direction", header, source.replace("Direction::ClientToHost", "Direction::Unexpected"), doc, True),
        ("default direction", header, source.replace("case FrameKind::Greet:", "default:"), doc, True),
        ("missing table", header, source, "", True),
        ("two tables", header, source, doc + doc, True),
        ("reversed markers", header, source, CATALOG_END + CATALOG_START, True),
        ("missing row", header, source, doc.replace(row + "\n", ""), True),
        ("duplicate row name", header, source, doc.replace(row, row + "\n" + row), True),
        ("duplicate row value", header, source, doc.replace("\x60Welcome\x60 | 2", "\x60Welcome\x60 | 1"), True),
        ("stale row", header, source, doc.replace("\x60Greet\x60", "\x60Ghost\x60"), True),
        ("changed value", header, source, doc.replace("\x60Greet\x60 | 1", "\x60Greet\x60 | 99"), True),
        ("changed direction", header, source, doc.replace("1 | client → host", "1 | host → client"), True),
        ("malformed row", header, source, doc.replace("1 | client → host", "1 | client"), True),
        ("octal row value", header, source, doc.replace("\x60Greet\x60 | 1", "\x60Greet\x60 | 010"), True),
        ("out-of-byte row", header, source, doc.replace("\x60Greet\x60 | 1", "\x60Greet\x60 | 256"), True),
    ]
    problems = []
    for title, testHeader, testSource, testDoc, expected in cases:
        reported = bool(failures(testHeader, testSource, testDoc))
        if reported != expected:
            problems.append("%s: failure=%s, expected=%s" % (title, reported, expected))
    if problems:
        return gate_report.failed("check-sync-catalog-complete", "self-test failed", problems)
    print("check-sync-catalog-complete: self-test ok (%d cases)" % len(cases))
    return 0


def main():
    if "--self-test" in sys.argv[1:]:
        return selfTest()
    try:
        header, source, document = (read(path) for path in (HEADER, SOURCE, DOC))
    except (OSError, UnicodeError) as error:
        return gate_report.failed("check-sync-catalog-complete", "required input could not be read", [str(error)])
    problems = failures(header, source, document)
    if problems:
        return gate_report.failed("check-sync-catalog-complete", "frame catalog disagrees with the remote grammar", problems)
    print("check-sync-catalog-complete: ok (%d FrameKind names, values and directions)" % len(kindsFrom(header)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
