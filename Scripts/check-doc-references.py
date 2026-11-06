#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0
"""check-doc-references.py — a reference doc names code that exists.

Two rules, because a doc reference rots in two different ways and only one of
them is visible.

**No line numbers.** A citation such as `VT.cpp:5821` is right the day it is
typed and drifts with every edit above it: it lands mid-function, on a bare
closing brace, or past the end of a file that has shrunk. Arithmetic catches
the last and nothing catches the rest, which is the problem with the form
rather than with the people maintaining it. A file plus a symbol is greppable
and survives every edit above it, so that is what the rule requires.

Three spellings are read: `VT.cpp:885`, `NativeAttachment.h,203`, and
`(line 877)` in prose. A rule keyed on one spelling of a form vouches for every
doc written in another, so the count it reports is about the spelling, not the
tree.

**A named symbol exists.** A rename sweep that reaches the code and the
conventions doc can miss a doc that names the symbol under its own section
heading — `emitChar` renamed to `emitCharacter` by the `Char -> Character`
ratchet in coding-conventions.md — and a reader grepping for the old name then
finds documents describing it and no code.

The second rule is scoped and the first is not. A design doc proposes names that
do not exist yet, which is its purpose; `Docs/naming-conventions.md` names
`fooCount` and `maxXxx` as illustrations. Failing either would be reporting a doc
for doing what it is for. So symbols are checked only in docs whose subject is
the tree as it stands, listed in DESCRIBES_THE_TREE below. Line numbers rot the
same way in every doc, so that rule covers all of them.

Adding a doc to DESCRIBES_THE_TREE is how the second rule grows. A list rather
than a heuristic, because "description or proposal" is a judgement about intent:
a heuristic that guesses wrong either lets rot through or reports a design doc
for proposing a name.

**Code comments navigate by name too.** A comment that sends its reader to
`CellInstanceBuilder.cpp:878` rots exactly as a doc does, and a file several
thousand lines long moves under it with every edit. So the line-number rule
reads the comments of every tracked source file as well, through check-slop.py's
`prose`, which keeps a file's comments and docstrings and blanks its code and
string literals: a fixture that feeds a gate `Source/Core/Foo.cpp:42` as data is
not a comment. One spelling is a format rather than a reference:
`Source/Core/Foo.cpp:42:` with a colon after the number is how a compiler or a
gate prints a finding, and a comment documenting that output names no line of
any file. This file is left out, because its own prose spells the forms it
rejects.

A gate's record is source too: a `.txt` list under Scripts/ or Tests/ whose
`#` comments give the reason for each entry, as Scripts/core-srcs-excluded.txt
and the snapshot exemption lists do. Its comments cite code as any comment does
and rot the same way, so they are read too, and a citation may name a `.txt`
or `.cmake` file, `CMakeLists.txt:22` among them. Fixtures, corpora and
goldens under those directories are data and are not read.

The bare continuation references in the Emacs client's terminal spec —
`(:1746)`, `(:231-245)`, about fifteen of them — inherit whichever file an
earlier sentence named, and this gate does not match them. They rot faster than
a full citation, since a reader cannot tell which file is meant without scanning
back. Matching one needs the doc's running file context, and rewriting one needs
to know which site the author meant; both belong to whoever owns that spec.

Stdlib only.

    python3 Scripts/check-doc-references.py
    python3 Scripts/check-doc-references.py --self-test
"""

import importlib.util
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gate_report  # noqa: E402 — the path above is what makes it importable

import ubq_roots  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = ubq_roots.subject_root()

# Docs whose subject is the tree as it stands, so a name in one is a claim that
# the name is there. Everything else is checked for line numbers only. Each entry
# is a doc read against the tree — adding a name here without reading the doc is
# the same mistake this gate exists to catch, one level up.
DESCRIBES_THE_TREE = (
    "Docs/architecture.md",
    "Docs/sync-protocol.md",
    "Docs/capability-origin.md",
    # Each of these says what it is in its own first lines, which is what made it
    # safe to add: a shipped socket protocol, a shipped library at coreVersion
    # 0.1.0, the font-routing contract ("the shipped contract is the full
    # pipeline described here"), and a doc calling itself a code map. All four
    # were already clean when listed, so none arrived with a repair attached.
    "Docs/control-socket.md",
    "Docs/libubiquittycore.md",
    "Docs/font-routing.md",
    "Docs/tmux-control-mode.md",
)

# Checked and left out, each for the reason beside it:
#
#   Docs/session-model.md          names UIWindowScene, a UIKit type with no
#                                  counterpart here. A doc naming an external API
#                                  is not a doc naming a symbol that vanished.
#   Docs/capability-host-policy.md argues about grants over identityA/identityB,
#                                  placeholders standing for two principals.
#   Docs/coding-conventions.md     spells the forbidden forms on purpose —
#                                  "foregroundIndex not fgIdx" needs fgIdx.
#   Docs/naming-conventions.md     names fooCount and maxXxx as illustrations.
#
# Each would need a per-doc exemption list to join, which buys less than it costs
# while the rot lives in the docs that carry no placeholders at all.

# Where identifiers may come from. CMake and the source registries are in here
# because a doc legitimately names a build target or a gate script, and leaving
# them out reports `check_ipo_supported` as missing code.
IDENTIFIER_SOURCES = (".c", ".cpp", ".h", ".hpp", ".mm", ".m", ".swift",
                      ".py", ".sh", ".ts", ".js", ".cmake", ".txt", ".json")

# `VT.cpp:885` / `Daemon.cpp#402` / `NativeAttachment.h,203`. The extension list
# is what keeps this off `Docs/font-routing.md:12` in prose and off a bare
# `1024:768`.
#
# The comma separator is here because a colon-only pattern passes
# `Daemon.cpp,1163,3045-3088` while failing the identical claim written with a
# colon. A gate keyed on one spelling of a form reports the docs that happen to
# use the other as clean.
CITATION = re.compile(
    r"`?([A-Za-z0-9_/.+-]+\.(?:cpp|h|hpp|mm|m|py|sh|swift|ts|js|txt|cmake))[:#,](\d+)`?")

# Where a gate keeps a record, and the directories under them that hold data.
RECORD_PREFIXES = ("Scripts/", "Tests/")
RECORD_DATA = re.compile(r"(?i)(?:^|/)[^/]*(?:fixture|corpus|golden|crash|feeds|unicode)[^/]*/")

# The same claim spelled in prose — "(line 877)", "~line 6500". It goes stale
# the same way: the line names a function since renamed, or sits past the end of
# a file that has shrunk. A pattern keyed on the punctuation form reports a doc
# written in this one as clean, the same hole a colon-only pattern leaves for
# the comma form.
#
# Three digits, because a terminal's own documentation says "line 24" about a
# screen row and means it. A source citation here runs to the hundreds. The floor
# is a guess about magnitude rather than a parse, and it is stated so a doc that
# trips it can say why rather than wonder.
PROSE_LINE = re.compile(r"\blines?\s+~?(\d{3,})\b")

# A backticked identifier presented as code. Requires an interior capital, which
# is what separates `emitCharacter` from `the`, and allows a `Foo::bar` path.
IDENTIFIER = re.compile(r"`([A-Za-z_][A-Za-z0-9_]*(?:::[A-Za-z_][A-Za-z0-9_]*)*)`")

# SCREAMING_SNAKE, which the mixed-case rule above skips as an acronym. Worth its
# own pattern because the defect that prompted this gate was one: sync-protocol.md
# documented `COLOR_DEFAULT` (0x100) and `COLOR_RGB`, and neither has existed since
# they became `colorDefault` / `colorRGB`. The underscore is the discriminator —
# `DECRQSS` and `DA1` are acronyms, `COLOR_DEFAULT` is a constant.
CONSTANT = re.compile(r"`([A-Z][A-Z0-9]*(?:_[A-Z0-9]+)+)`")

# A wire tag is a name on the protocol, not in the code, so it is exempt from the
# rule above. sync-protocol.md spells its tags SCREAMING_SNAKE (`ROW_DELTA`) while
# MessageType spells them PascalCase (`RowDelta`) — the doc is naming what crosses
# the wire. They are recognised structurally rather than listed: a catalog row
# gives the tag beside its hex value, which is the doc saying "this is a tag".
WIRE_TAG_ROW = re.compile(r"^\s*\|\s*`([A-Z][A-Z0-9_]*)`\s*\|\s*0x[0-9A-Fa-f]+", re.M)


def tracked(prefix=""):
    listing = subprocess.run(["git", "-C", REPO, "ls-files", prefix] if prefix
                             else ["git", "-C", REPO, "ls-files"],
                             capture_output=True, text=True)
    return [path for path in listing.stdout.split("\n") if path]


def read(path):
    try:
        with open(os.path.join(REPO, path), encoding="utf-8", errors="replace") as handle:
            return handle.read()
    except OSError:
        return ""


def vocabulary(paths):
    """Every identifier token the tree defines or mentions.

    - Returns: A set of bare words. Mentions count, not just definitions: a doc
      naming a callback type used only in a comment is still naming something a
      reader can find, and demanding a definition site would need a C++ parser.

    This file is excluded because its own prose names dead symbols. The docstring
    above spells `COLOR_DEFAULT` and `fgIdx` to say what the rules catch; scanned
    like any other script, that would put both into the vocabulary, and the gate
    would answer "present" for the two names it exists to reject.
    """
    mine = os.path.relpath(os.path.abspath(__file__), REPO)
    words = set()
    for path in paths:
        if path.endswith(IDENTIFIER_SOURCES) and path != mine:
            words.update(re.findall(r"\b[A-Za-z_][A-Za-z0-9_]*\b", read(path)))
    return words


def citations(docs):
    """Every line reference, punctuated or in prose.

    - Returns: (doc, line number, the matched text) triples.
    """
    found = []
    for doc in docs:
        for number, line in enumerate(read(doc).split("\n"), 1):
            for match in CITATION.finditer(line):
                found.append((doc, number, match.group(0).strip("`")))
            for match in PROSE_LINE.finditer(line):
                found.append((doc, number, match.group(0)))
    return found


def loadSlop():
    """check-slop.py, whose `prose` separates a file's comments from its code."""
    spec = importlib.util.spec_from_file_location("check_slop", os.path.join(HERE, "check-slop.py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def commentCitations(paths, slop):
    """Every line reference in a source file's comments.

    - Parameters:
      - paths: Tracked paths; the ones check-slop.py reads as source are read.
      - slop: check-slop.py, for what counts as source and what is a comment.
    - Returns: (path, line number, the matched text) triples. A match followed
      by a colon is a diagnostic's format, `Source/Core/Foo.cpp:42:`, and is
      not returned.
    """
    mine = os.path.relpath(os.path.abspath(__file__), REPO)
    found = []
    for path in paths:
        if path == mine or path.endswith(".md") or path.startswith("Source/ThirdParty/"):
            continue
        if any(part in slop.BINARY_DIRECTORIES for part in path.split("/")):
            continue
        suffix = os.path.splitext(path)[1]
        full = os.path.join(REPO, path)
        record = (suffix == ".txt" and path.startswith(RECORD_PREFIXES)
                  and os.path.basename(path) not in slop.NAMES and not RECORD_DATA.search(path))
        if not (record or suffix in slop.SUFFIXES or os.path.basename(path) in slop.NAMES
                or (not suffix and slop.hasShebang(full))):
            continue
        text = read(path)
        if not CITATION.search(text):
            continue
        comments = slop.hash_comments(text, docstrings=False) if record else slop.prose(full, text)
        for number, line in enumerate(comments.split("\n"), 1):
            for match in CITATION.finditer(line):
                if line[match.end():match.end() + 1] == ":":
                    continue
                found.append((path, number, match.group(0).strip("`")))
    return found


def absentIdentifiers(doc, words):
    """Identifiers a doc presents as code that no source file contains.

    - Returns: A sorted list of (line number, identifier).
    """
    body = read(doc)
    tags = set(WIRE_TAG_ROW.findall(body))
    missing = {}
    for number, line in enumerate(body.split("\n"), 1):
        for name in IDENTIFIER.findall(line):
            # The last component is what a reader greps for: `Attribute::Wide`
            # is findable through `Wide`, and demanding the qualified spelling
            # appear verbatim would report every correctly-namespaced reference.
            leaf = name.split("::")[-1]
            if not re.search(r"[a-z]", leaf) or not re.search(r"[A-Z]", leaf):
                continue  # all-caps is an acronym; all-lower is an English word
            if leaf in words or name in missing:
                continue
            missing[name] = number
        for name in CONSTANT.findall(line):
            # A tag the doc itself defines in a catalog row, and every tag built
            # from one — ROW_DELTA_COMPRESSED is ROW_DELTA compressed, and the
            # catalog gives both, so the prefix test only covers what is declared.
            if name in tags or any(name.startswith(tag + "_") for tag in tags):
                continue
            if name in words or name in missing:
                continue
            missing[name] = number
    return sorted((number, name) for name, number in missing.items())


def selfTest():
    ok = True

    # A citation is found wherever it appears, backticked or bare.
    doc = "Docs/architecture.md"
    if not CITATION.search("see `VT.cpp:885` for the detail"):
        print("self-test: a backticked citation was not matched", file=sys.stderr)
        ok = False
    if not CITATION.search("see VT.cpp:885 for the detail"):
        print("self-test: a bare citation was not matched", file=sys.stderr)
        ok = False

    # A markdown doc reference is not a source citation.
    if CITATION.search("`Docs/font-routing.md:12`"):
        print("self-test: a .md reference was read as a source citation", file=sys.stderr)
        ok = False

    # The prose spelling, and the screen row it must not report.
    if not PROSE_LINE.search("`emitCharacter()` (line 877) — the hot path"):
        print("self-test: a prose line reference was not matched", file=sys.stderr)
        ok = False
    if not PROSE_LINE.search("a batch fast path (`emitClusterRun`, ~line 6500)"):
        print("self-test: an approximate prose line reference was not matched", file=sys.stderr)
        ok = False
    if PROSE_LINE.search("DECSTBM sets the region to line 24"):
        print("self-test: a screen row was read as a source citation", file=sys.stderr)
        ok = False

    # An identifier absent from the vocabulary is reported; one present is not.
    words = {"emitCharacter", "Wide"}
    probe = "`emitChar` was renamed, and `Attribute::Wide` was not."
    saved = globals()["read"]
    globals()["read"] = lambda path: probe
    try:
        found = absentIdentifiers(doc, words)
        if [name for _, name in found] != ["emitChar"]:
            print("self-test: expected exactly emitChar absent, got %s"
                  % [name for _, name in found], file=sys.stderr)
            ok = False

        # An English word in backticks is not an identifier claim.
        globals()["read"] = lambda path: "the `resize` hook and the `pen`"
        if absentIdentifiers(doc, set()):
            print("self-test: a lowercase word was read as an identifier", file=sys.stderr)
            ok = False

        # Nor is an acronym.
        globals()["read"] = lambda path: "replies to `DECRQSS` and `DA1`"
        if absentIdentifiers(doc, set()):
            print("self-test: an acronym was read as an identifier", file=sys.stderr)
            ok = False
    finally:
        globals()["read"] = saved

    # A citation in a comment is found; the same text as a string literal, and
    # a diagnostic's `Source/Core/Foo.cpp:42:` format in a comment, are not.
    # A record's `#` comment is read too, and may cite a CMakeLists.txt; its
    # entries and a fixture's text are not comments.
    import tempfile
    slop = loadSlop()
    with tempfile.TemporaryDirectory() as scratch:
        files = {
            "probe.cpp": ('// the builder tests this at CellInstanceBuilder.cpp:878\n'
                          'const char* fixture = "Source/Core/Foo.cpp:42 data";\n'
                          '// a finding prints as Source/Core/Foo.cpp:42: check-style: ...\n'),
            "Tests/record.txt": ('# the launcher is set in CMakeLists.txt:36\n'
                                 'Source/Core/Foo.cpp:42  an entry, not a comment\n'),
            "Tests/Snapshots/fixtures/sample.txt": '# Source/Core/Foo.cpp:42 is sample text\n',
        }
        for name, content in files.items():
            os.makedirs(os.path.dirname(os.path.join(scratch, name)), exist_ok=True)
            with open(os.path.join(scratch, name), "w", encoding="utf-8") as handle:
                handle.write(content)
        global REPO
        savedRepo, REPO = REPO, scratch
        try:
            found = commentCitations(list(files), slop)
        finally:
            REPO = savedRepo
        expected = [("probe.cpp", 1, "CellInstanceBuilder.cpp:878"),
                    ("Tests/record.txt", 1, "CMakeLists.txt:36")]
        if found != expected:
            print("self-test: expected %s, got %s" % (expected, found), file=sys.stderr)
            ok = False

    print("check-doc-references: self-test %s" % ("ok" if ok else "FAILED"))
    return 0 if ok else 1


def main():
    if "--self-test" in sys.argv[1:]:
        return selfTest()

    paths = tracked()
    docs = [path for path in paths if path.startswith("Docs/") and path.endswith(".md")]
    if not docs:
        print("check-doc-references: FAILED — no Docs/*.md found, so the rules "
              "were applied to nothing", file=sys.stderr)
        return 1

    lineNumbers = citations(docs) + commentCitations(paths, loadSlop())
    # A symbol another repository owns is named here, not missing.
    words = vocabulary(paths) | ubq_roots.elsewhere(REPO)[1]

    # The list is ubiquitty-core's docs. A repository that carries ubiquitty-core
    # as a library holds none of them, and a doc of its own joins the list when it
    # describes the tree it is in.
    described = [doc for doc in DESCRIBES_THE_TREE if doc in docs]
    if "core" in ubq_roots.repositories(REPO) and len(described) != len(DESCRIBES_THE_TREE):
        missing = sorted(set(DESCRIBES_THE_TREE) - set(described))
        print("check-doc-references: FAILED — listed doc(s) not tracked: %s. "
              "Remove them from DESCRIBES_THE_TREE or restore them; a list "
              "naming a file that is gone checks nothing." % ", ".join(missing),
              file=sys.stderr)
        return 1

    absent = []
    for doc in described:
        absent.extend((doc, number, name) for number, name in absentIdentifiers(doc, words))

    if not lineNumbers and not absent:
        print("check-doc-references: ok (%d doc(s) and the source comments, no "
              "line-number citations; every identifier named by %d described doc(s) "
              "is in the tree)" % (len(docs), len(described)))
        return 0

    offenders = ["%s:%d %s" % (doc, number, text) for doc, number, text in lineNumbers]
    offenders += ["%s:%d `%s`" % (doc, number, name) for doc, number, name in absent]
    status = gate_report.failed("check-doc-references",
                                "doc reference(s) that do not resolve", offenders)
    if lineNumbers:
        print("\n  A line number is right the day it is written and silently wrong after\n"
              "  the next edit above it — it does not fail, it points somewhere else.\n"
              "  Name the file and the symbol instead: `emitCharacter` (`VT.cpp`), in a\n"
              "  doc or a comment alike.",
              file=sys.stderr)
    if absent:
        print("\n  These are named as code and are not in the tree. Usually a rename the\n"
              "  doc missed — check Docs/coding-conventions.md for the ratchet that moved\n"
              "  it. If the doc is proposing a name rather than describing one, it does\n"
              "  not belong in DESCRIBES_THE_TREE in this gate.", file=sys.stderr)
    return status


if __name__ == "__main__":
    sys.exit(main())
