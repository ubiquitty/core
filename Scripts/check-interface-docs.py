#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""check-interface-docs.py — every declared interface carries a DocC comment.

  ./Scripts/check-interface-docs.py --staged      ADDED lines in the index
  ./Scripts/check-interface-docs.py --since REF   ADDED lines against REF
  ./Scripts/check-interface-docs.py               whole tree
  ./Scripts/check-interface-docs.py --self-test   the rules against their cases

Three rules, all from Docs/coding-conventions.md §3.4 and §3.5.

Declarations are documented. A type or function declared in a header is the API
someone reads instead of the implementation, so it says what it is. `///` on the
declaration, one third-person sentence ending in a period, then a blank `///`
and the discussion. A function defined inline in a header publishes the same
interface a declaration does and carries the same comment; a block comment or a
plain `//` above a declaration is prose, not documentation, because DocC reads
`///`.

And the comment covers the signature. A summary that stops before
`- Parameters:` or `- Returns:` leaves the caller reading the implementation for
the two things they came for. A void return and an unnamed parameter list have
nothing to describe and are not asked for.

COMMENTS STATE INVARIANTS, not history. "used to", "previously", "now returns"
describe a diff, and a diff is true for one commit. Design rationale that
implies a past is fine — the bug class a design prevents is why the invariant
exists — but a changelog entry is not.

Ratcheted on ADDED lines: the pre-existing backlog is tolerated and a change
cannot grow it.

The rules are unchanged from the shell-and-awk version this replaces. What
changed is that the scan was one `awk` per file, and a whole-tree
run is 1261 files — the program was parsed again for every one of them, and that
parse was most of what the gate cost. One process reads the tree once. The files
are independent, so the work also spreads across cores.
"""

import argparse
import os
import re
import subprocess
import sys
from concurrent.futures import ProcessPoolExecutor

ROOT = subprocess.run(["git", "rev-parse", "--show-toplevel"],
                      capture_output=True, text=True).stdout.strip() or "."

# Vendored code keeps its upstream shape, and generated tables have no interface
# to document. The tables are named rather than detected, so a new one is read
# in full until it is added here: UnifontData.cpp is a large table of hex and
# was read on every run to find the nothing its banner already promised.
#
# Its header is not excluded with it. That file is small and declares the four
# names the table is reached through, which is an interface this gate is for;
# the cost being avoided is the table of hex, not the declarations.
IGNORE = re.compile(r"^(Source/ThirdParty/"
                    r"|Source/Core/Rendering/emoji_atlas_data\.c"
                    r"|Source/Core/Rendering/UnifontData\.cpp)")
SOURCE = re.compile(r"\.(h|hpp|cpp|mm|c|m)$")

# Phrases that describe a change rather than what the code is. Two were too
# broad and are narrowed to the code-shaped forms: "no longer" states a runtime
# transition as often as a diff, and "recently" caught "least-recently-used".
SLOP = re.compile(
    r"(used to |previously|formerly|originally|"
    r"no longer (returns|takes|uses|needs|exists|lives|compiles|happens|fires|applies)|"
    r"will now |now returns|now takes|now uses|"
    r"recently (changed|added|removed|renamed|fixed|landed)|"
    r"as of [0-9]|this (change|commit|PR|patch))")

SPACE = r"[ \t\n\r\f\v]"
SIGNATURE = re.compile(
    r"^[ \t\n\r\f\v]*[A-Za-z_~][A-Za-z0-9_:<>,&*  \t\n\r\f\v]*"
    r"[ \t\n\r\f\v*&~][A-Za-z_][A-Za-z0-9_]*[ \t\n\r\f\v]*\([^)]*\)")
CONSTRUCTOR = re.compile(r"^[ \t\n\r\f\v]*~?[A-Z][A-Za-z0-9_]*[ \t\n\r\f\v]*\([^)]*\)")

SWIFT_NAME = re.compile(r"UBIQUITTY_SWIFT_NAME\([^)]*\)")
LINE_COMMENT = re.compile(r"//.*$")
TEMPLATE_HEAD = re.compile(r"^[ \t\n\r\f\v]*template[ \t\n\r\f\v]*<")
DOC_LINE = re.compile(r"^[ \t\n\r\f\v]*///")

SKIP_LINE = (
    re.compile(r"^[ \t]*(\*|$)"),
    re.compile(r"=[ \t]*(delete|default)[ \t]*;"),
    re.compile(r"^[ \t]*(public|private|protected)[ \t]*:"),
    re.compile(r"^[ \t\n\r\f\v]*(return|if|for|while|switch|case|else)[ \t\n\r\f\v(;]"),
    re.compile(r"^[ \t\n\r\f\v]*(using|typedef|friend|template|namespace|static_assert)"
               r"[ \t\n\r\f\v(<]"),
    re.compile(r"^[ \t\n\r\f\v]*[A-Z][A-Z0-9_]*[ \t\n\r\f\v]*\("),
)

TYPE_OPEN = re.compile(r"^[ \t]*(class|struct|enum[ \t]+class|enum)[ \t]+[A-Z][A-Za-z0-9_]*")
FORWARD_ONLY = re.compile(
    r"^[ \t\n\r\f\v]*(class|struct|enum([ \t\n\r\f\v]+class)?)[ \t\n\r\f\v]+"
    r"[A-Za-z_][A-Za-z0-9_]*[ \t\n\r\f\v]*;[ \t\n\r\f\v]*$")
SPECIFIER = re.compile(r"^[ \t\n\r\f\v]*(explicit|constexpr|inline)[ \t\n\r\f\v]+")
QUALIFIER = re.compile(r"^(const|noexcept|override|final|mutable)")
DECLARATIONS_SCOPE = re.compile(
    r"^[ \t\n\r\f\v]*(export[ \t\n\r\f\v]+)?"
    r"(class|struct|union|enum([ \t\n\r\f\v]+class)?|namespace)[ \t\n\r\f\v{]")
EXTERN_C = re.compile(r'^[ \t\n\r\f\v]*extern[ \t\n\r\f\v]+"C"')
VOID_RETURN = re.compile(r"(^|[^A-Za-z0-9_])void[ \t\n\r\f\v]*$")
HAS_PARAMETERS = re.compile(r"- Parameters?[ :]")
HAS_RETURNS = re.compile(r"- Returns:")

TYPE_WORDS = {"int", "char", "bool", "void", "float", "double", "long", "short",
              "unsigned", "signed", "size_t", "ptrdiff_t", "wchar_t"}

BLOCK_COMMENT_IN_PARAMS = re.compile(r"/\*[^*]*\*/")
NAMED_BY_PUNCTUATION = re.compile(r"[*&>][ \t\n\r\f\v]*[A-Za-z_][A-Za-z0-9_]*$")
NAMED_BY_SPACE = re.compile(r"[A-Za-z0-9_][ \t\n\r\f\v]+[A-Za-z_][A-Za-z0-9_]*$")


def anyParameterIsNamed(parameters):
    """Whether any parameter in the list carries a name.

    A parameter the signature does not name is one a `- Parameter` block cannot
    refer to. WebKit spelling leaves a parameter unnamed when its type is the
    whole of what it is (`start(ChannelObserver*)`), so demanding documentation
    there demands an identifier the naming rule says not to write. The exemption
    is narrow on purpose: one named parameter puts the whole list back under the
    rule, so a signature cannot escape it by dropping some of its names.

    - Parameter parameters: the text between the parentheses.
    - Returns: True when at least one parameter is named.
    """
    listing = BLOCK_COMMENT_IN_PARAMS.sub("", parameters)
    if re.match(r"^[ \t\n\r\f\v]*(void)?[ \t\n\r\f\v]*$", listing):
        return False
    for part in listing.split(","):
        part = re.sub(r"=.*$", "", part).strip(" \t\n\r\f\v")
        if NAMED_BY_PUNCTUATION.search(part):
            return True
        if NAMED_BY_SPACE.search(part):
            tail = re.sub(r"^.*[ \t\n\r\f\v]", "", part)
            if tail not in TYPE_WORDS:
                return True
    return False


def docBlockAbove(lines, at):
    """The contiguous run of `///` lines immediately above a declaration.

    A block comment or a plain `//` above a declaration is not documentation:
    DocC reads `///` only. A template head is part of the declaration, so the
    search steps over it — stopping there finds nothing and calls a documented
    function undocumented.

    - Parameters:
      - lines: the file's lines.
      - at: the zero-based index of the declaration.
    - Returns: the joined block, empty when there is none.
    """
    k = at - 1
    while k >= 0 and TEMPLATE_HEAD.match(lines[k]):
        k -= 1
    block = []
    while k >= 0 and DOC_LINE.match(lines[k]):
        block.append(lines[k])
        k -= 1
    return "\n".join(reversed(block)) + ("\n" if block else "")


def examine(path, text, owned, declarations):
    """Every finding in one file.

    - Parameters:
      - path: the repository-relative path, as reported.
      - text: the file's contents.
      - owned: the line numbers this change added, or None for the whole file.
      - declarations: whether declarations are this file's business (a header).
    - Returns: (missing, sloppy, incomplete), each a list of report strings.
    """
    lines = text.split("\n")
    if lines and lines[-1] == "":
        lines.pop()

    missing, sloppy, incomplete = [], [], []
    depth, bodies = 0, 0
    scope = {}

    for index, raw in enumerate(lines):
        number = index + 1
        stripped = SWIFT_NAME.sub("", LINE_COMMENT.sub("", raw))
        inBody = bodies > 0
        wanted = owned is None or number in owned

        if wanted and re.match(r"^[ \t]*//", raw):
            # A comment inside a function body describes the code just as one
            # above a declaration does, so §3.5 reaches both.
            if SLOP.search(raw.lower()):
                sloppy.append("%s:%d: comment states history, not an invariant\n    %s"
                              % (path, number, raw))
        elif wanted and not inBody and declarations:
            if not any(pattern.search(raw) for pattern in SKIP_LINE):
                isType = bool(TYPE_OPEN.match(stripped)) and not FORWARD_ONLY.match(stripped)

                # `explicit` and `constexpr` sit where a return type would, so a
                # constructor carrying either matches the function pattern first
                # and is read as returning `explicit`.
                classify = stripped
                while SPECIFIER.match(classify):
                    classify = SPECIFIER.sub("", classify, count=1)
                isCtor = bool(CONSTRUCTOR.match(classify))
                isFunc = (not isCtor) and bool(SIGNATURE.match(stripped))

                if isFunc or isCtor:
                    # A semicolon declares an interface and a brace defines one;
                    # anything else is an expression that contains a call.
                    pattern = SIGNATURE if isFunc else CONSTRUCTOR
                    rest = pattern.sub("", stripped, count=1)
                    rest = re.sub(r"[ \t\n\r\f\v]", "", rest)
                    while QUALIFIER.match(rest):
                        rest = QUALIFIER.sub("", rest, count=1)
                    if rest == "":
                        k = index + 1
                        while k < len(lines) and re.match(r"^[ \t\n\r\f\v]*$", lines[k]):
                            k += 1
                        rest = "{" if k < len(lines) and re.match(r"^[ \t\n\r\f\v]*\{", lines[k]) else "?"
                    if not re.match(r"^[;{]", rest) and not rest.startswith("=0"):
                        isFunc = isCtor = False

                if isType or isFunc or isCtor:
                    block = docBlockAbove(lines, index)
                    if block == "":
                        missing.append("%s:%d: declaration has no /// comment\n    %s"
                                       % (path, number, raw))
                    elif isFunc or isCtor:
                        # Spacing is kept here, unlike the classification above:
                        # whether a parameter carries a name is the question, and
                        # the space before it is the evidence.
                        params = re.sub(r"\).*$", "", re.sub(r"^[^(]*\(", "", stripped, count=1))
                        hasParameters = anyParameterIsNamed(params)

                        signature = re.sub(r"\(.*$", "", stripped)
                        name = re.sub(r"^.*[^A-Za-z0-9_]", "", signature)
                        returnType = signature[:len(signature) - len(name)]
                        wantsReturns = (not isCtor) and not VOID_RETURN.search(returnType)

                        if hasParameters and not HAS_PARAMETERS.search(block):
                            incomplete.append(
                                "%s:%d: comment documents no parameter, and the signature "
                                "takes one\n    %s" % (path, number, raw))
                        if wantsReturns and not HAS_RETURNS.search(block):
                            incomplete.append(
                                "%s:%d: comment documents no return value, and the signature "
                                "has one\n    %s" % (path, number, raw))

        # Scopes are tracked after judging, so a declaration that opens a body is
        # judged at the scope it was declared in. Only class, struct, enum, union
        # and namespace bodies hold declarations; a bare block nested in a
        # function closes back to the function's depth, which a single marker
        # cannot express.
        for _ in range(stripped.count("{")):
            depth += 1
            scope[depth] = ("declarations"
                            if DECLARATIONS_SCOPE.match(stripped) or EXTERN_C.match(stripped)
                            else "code")
            if scope[depth] == "code":
                bodies += 1
        for _ in range(stripped.count("}")):
            if depth >= 1 and scope.get(depth) == "code":
                bodies -= 1
            scope.pop(depth, None)
            if depth > 0:
                depth -= 1

    return missing, sloppy, incomplete


def _examineOne(job):
    """Worker entry: read a file and examine it."""
    path, owned, declarations = job
    try:
        with open(os.path.join(ROOT, path), errors="replace") as handle:
            text = handle.read()
    except OSError:
        return [], [], []
    return examine(path, text, set(owned) if owned is not None else None, declarations)


def git(*arguments):
    """Run git in the repository and return its stdout."""
    return subprocess.run(["git", "-C", ROOT, *arguments],
                          capture_output=True, text=True).stdout


def changedFiles(mode, ref):
    """The files this run judges."""
    if mode == "tree":
        listing = git("ls-files")
    elif mode == "staged":
        listing = git("diff", "--cached", "--name-only", "--diff-filter=ACMR")
    else:
        base = git("merge-base", ref, "HEAD").strip()
        listing = git("diff", "--name-only", "--diff-filter=ACMR", "%s...HEAD" % base)
    return [p for p in listing.split("\n")
            if p and SOURCE.search(p) and not IGNORE.match(p)]


def renamedFrom(mode, ref, path):
    """Where a renamed file came from, or the path itself."""
    if mode == "tree":
        return path
    if mode == "staged":
        listing = git("diff", "--cached", "--name-status", "-M", "--diff-filter=R")
    else:
        base = git("merge-base", ref, "HEAD").strip()
        listing = git("diff", "--name-status", "-M", "--diff-filter=R", "%s...HEAD" % base)
    for row in listing.split("\n"):
        fields = row.split("\t")
        if len(fields) >= 3 and fields[0].startswith("R") and fields[2] == path:
            return fields[1]
    return path


HUNK = re.compile(r"^@@ [^ ]+ \+(\d+)(?:,(\d+))?")


def addedLines(mode, ref, path):
    """The line numbers this change added to a file, or None for the whole file."""
    if mode == "tree":
        return None
    origin = renamedFrom(mode, ref, path)
    if mode == "staged":
        arguments = ["diff", "--cached", "-U0"]
    else:
        base = git("merge-base", ref, "HEAD").strip()
        arguments = ["diff", "-U0", "%s...HEAD" % base]
    arguments += ["-M", "--", origin, path] if origin != path else ["--", path]
    owned = set()
    for row in git(*arguments).split("\n"):
        found = HUNK.match(row)
        if found:
            start = int(found.group(1))
            count = 1 if found.group(2) is None else int(found.group(2))
            owned.update(range(start, start + count))
    return owned


DECLARED_TYPE = re.compile(r"(struct|class|enum)[ \t\n\r\f\v]+[A-Za-z_][A-Za-z0-9_]*")
DECLARED_CALL = re.compile(r"[A-Za-z_][A-Za-z0-9_]*[ \t\n\r\f\v]*\(")


def declarationNames(text):
    """Every name a source text declares, over-approximated.

    Over-approximation is the safe direction: this set can only EXEMPT a
    declaration from the missing-comment rule, so a name it invents costs a
    report and a name it misses costs nothing.
    """
    names = set()
    for row in text.split("\n"):
        row = LINE_COMMENT.sub("", row)
        found = DECLARED_TYPE.search(row)
        if found:
            names.add(re.sub(r"^.*[ \t\n\r\f\v]", "", found.group(0)))
        found = DECLARED_CALL.search(row)
        if found:
            names.add(re.sub(r"[ \t\n\r\f\v]*\($", "", found.group(0)))
    return names


def preexistingNames(mode, ref, path):
    """The names the file already declared before this change."""
    if mode == "tree":
        return set()
    origin = renamedFrom(mode, ref, path)
    base = "HEAD" if mode == "staged" else git("merge-base", ref, "HEAD").strip()
    shown = subprocess.run(["git", "-C", ROOT, "show", "%s:%s" % (base, origin)],
                           capture_output=True, text=True)
    if shown.returncode != 0:
        return set()
    return declarationNames(shown.stdout)


RULES = """The rules these come from — Docs/coding-conventions.md:

  §3.4  A declaration comment is reference documentation. `///` on the
        declaration, a one-sentence third-person summary ending in a period
        ("Parses one line", not "Parse a line" or "This function parses"), a
        blank `///`, then discussion, then `- Parameters:` / `- Returns:` last.
        Backticks for symbols. Present tense, active voice, sentence case.
        A function defined inline in a header publishes the same interface a
        declaration does. A summary that stops before the parameter and return
        blocks sends the caller to the implementation for what to pass and what
        comes back — a void return and an empty parameter list are exempt
        because they have nothing to describe.

  §3.5  A comment states a constraint, an invariant, or a failure mode the code
        cannot show. Not what the diff did. "used to", "previously", "now
        returns" describe one commit; the test is whether the sentence is still
        true after the next ten. Design rationale that implies a past is fine —
        the bug class a design prevents is why the invariant exists.

  §2    WebKit naming: UpperCamelCase types, lowerCamelCase functions and
        locals, m_ members, s_ statics, no k-prefix on constants, acronyms
        ALL-CAPS by position (URLParser, mimeType), words spelled out.

  §10   One implementation. Portable policy lives in Core and every frontend
        routes through it; a private fast path in a frontend is a fork.

Document the declaration, or narrow what it exposes."""


def quietSelfTest():
    """The parameter-naming rule, checked before a scan trusts it.

    Silent on success: a scan's report is what the caller reads, and a line
    above it about the rule's own health is noise in every green run.

    - Returns: True when the rule answers both ways.
    """
    failures = namingFailures()
    for failure in failures:
        print("check-interface-docs SELFTEST: " + failure, file=sys.stderr)
    if failures:
        print("check-interface-docs: the parameter-naming rule failed its own cases",
              file=sys.stderr)
        return False
    return True


def namingFailures():
    """Cases the parameter-naming rule gets wrong, empty when it is sound.

    A rule that always returned False would silence the parameter check
    tree-wide and read as a clean run, so it is held to cases it must accept and
    cases it must refuse.

    - Returns: a list of descriptions.
    """
    named = ("ChannelObserver* observer", "std::span<const std::uint8_t> data",
             "const char* buffer, int length", "int rows, int columns",
             "const std::string& reason")
    unnamed = ("ChannelObserver*", "std::span<const std::uint8_t>",
               "const char*, std::size_t", "std::string_view /*utf8*/",
               "unsigned int", "void", "")
    failures = []
    for case in named:
        if not anyParameterIsNamed(case):
            failures.append("%s reads as unnamed" % case)
    for case in unnamed:
        if anyParameterIsNamed(case):
            failures.append("%s reads as named" % case)
    return failures


def selfTest():
    """The parameter-naming rule against both answers.

    A rule that always returned False would silence the parameter check tree-wide
    and read as a clean run, so it is checked against cases it must accept and
    cases it must refuse.
    """
    named = ("ChannelObserver* observer", "std::span<const std::uint8_t> data",
             "const char* buffer, int length", "int rows, int columns",
             "const std::string& reason")
    unnamed = ("ChannelObserver*", "std::span<const std::uint8_t>",
               "const char*, std::size_t", "std::string_view /*utf8*/",
               "unsigned int", "void", "")
    failures = []
    for case in named:
        if not anyParameterIsNamed(case):
            failures.append("%s reads as unnamed" % case)
    for case in unnamed:
        if anyParameterIsNamed(case):
            failures.append("%s reads as named" % case)
    for failure in failures:
        print("check-interface-docs SELFTEST: " + failure, file=sys.stderr)
    if failures:
        print("check-interface-docs: the parameter-naming rule failed its own cases",
              file=sys.stderr)
        return 2
    print("check-interface-docs --self-test: ok (%d named + %d unnamed case(s))"
          % (len(named), len(unnamed)))
    return 0


def main(argv):
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--staged", action="store_true")
    parser.add_argument("--since", metavar="REF")
    parser.add_argument("--self-test", dest="selfTest", action="store_true")
    parser.add_argument("--help", "-h", action="store_true", dest="help")
    try:
        arguments = parser.parse_args(argv[1:])
    except SystemExit:
        return 2
    if arguments.help:
        print(__doc__.strip())
        return 0
    if arguments.selfTest:
        return selfTest()

    mode = "staged" if arguments.staged else ("since" if arguments.since else "tree")
    ref = arguments.since

    if mode == "since" and not git("rev-parse", "--verify", "--quiet", ref).strip():
        print("check-interface-docs: --since %s not found; nothing to ratchet" % ref)
        return 0

    if not quietSelfTest():
        return 2

    # This ratchet reads committed content, so a fix still in the working tree
    # is invisible to it and the finding repeats unchanged. Judging what would
    # land is the right question; letting the reader find that out by doubting
    # a correct edit is not.
    if mode == "since":
        dirty = [line for line in git("status", "--porcelain").split("\n") if line.strip()]
        if dirty:
            print("check-interface-docs: judging the lines HEAD adds over %s; %d path(s) "
                  "differ in the working tree and are not read — commit to re-check."
                  % (ref, len(dirty)), file=sys.stderr)

    paths = changedFiles(mode, ref)
    if not paths:
        print("check-interface-docs: no source files to scan")
        return 0

    jobs = []
    for path in paths:
        owned = addedLines(mode, ref, path)
        jobs.append((path, None if owned is None else sorted(owned),
                     path.endswith((".h", ".hpp"))))

    # One process per file was the shell version's cost; one process for the tree
    # is this one's floor. Above that floor the files are independent, so the
    # scan spreads — half the cores, because several builds may share the machine.
    workers = max(1, (os.cpu_count() or 4) // 2)
    if len(jobs) < 64 or workers < 2:
        results = [_examineOne(job) for job in jobs]
    else:
        with ProcessPoolExecutor(max_workers=workers) as pool:
            results = list(pool.map(_examineOne, jobs, chunksize=16))

    missing, sloppy, incomplete = [], [], []
    for path, job in zip(paths, results):
        fileMissing, fileSloppy, fileIncomplete = job
        if mode != "tree" and fileMissing:
            existed = preexistingNames(mode, ref, path)
            fileMissing = [report for report in fileMissing
                           if declaredName(report) not in existed]
        missing += fileMissing
        sloppy += fileSloppy
        incomplete += fileIncomplete

    status = 0
    if missing:
        print("Declarations added without a DocC comment:")
        print("\n".join(missing))
        print()
        status = 1
    if incomplete:
        print("Declaration comments that stop short of what the signature promises:")
        print("\n".join(incomplete))
        print()
        status = 1
    if sloppy:
        print("Comments added that state history rather than an invariant:")
        print("\n".join(sloppy))
        print()
        status = 1

    if status != 0:
        # The findings go to stdout and this goes to stderr, and stdout is block
        # buffered when redirected — without the flush the rules print above the
        # report they explain, and the last finding is run together with them.
        sys.stdout.flush()
        print(RULES, file=sys.stderr)
        return 1

    print("ok")
    return 0


NAME_FROM_REPORT = re.compile(r"\n    (.*)$", re.S)


def declaredName(report):
    """The name a missing-comment report is about, for the already-existed test."""
    found = NAME_FROM_REPORT.search(report)
    if not found:
        return ""
    stripped = SWIFT_NAME.sub("", LINE_COMMENT.sub("", found.group(1)))
    if TYPE_OPEN.match(stripped):
        name = re.sub(r"^.*(struct|class|enum)[ \t\n\r\f\v]+", "", stripped)
        return re.sub(r"[^A-Za-z0-9_].*$", "", name)
    name = re.sub(r"\(.*$", "", stripped).rstrip(" \t\n\r\f\v")
    return re.sub(r"^.*[^A-Za-z0-9_]", "", name)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
