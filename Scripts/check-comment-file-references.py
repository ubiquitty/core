#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""Check that a file named in prose is a file that exists.

A comment or a document that names a source file is a navigation instruction.
"Defined in OSCColor.cpp", "see VT.h", "the arrangement GlyphKey.h uses" —
each tells a reader where to go next, and each is the only link between the two
places. Nothing compiles prose, so when the file is split, renamed or moved the
pointer stays behind and keeps giving the same directions to somewhere that is
not there.

Comments and Markdown are one population because the defect is one defect. The
difference is only in extraction: a source file is reduced to its comments,
and a document is prose from the first line, so it is read whole — running it
through the comment extractor would blank the backticked spans, which is exactly
where a document puts a path.

It fails the way a wrong sign fails rather than the way a missing one does. A
reader who follows "OSCColor.cpp" finds nothing and concludes the function is
gone, or greps the basename, finds `Color.cpp`, and cannot tell whether that is
the same thing renamed or a different thing that happens to fit. Both readings
cost more than no pointer would have.

Two bounds, and they differ in what they can prove.

    a name this repository once tracked and no longer does   always fails
    a name this repository never had                         allowlisted below

The first needs no exemption and never will. Git holds both halves of it: the
path was in the tree, the path is not in the tree, and no reading of those two
facts makes the pointer right. That is the whole of the rot this check exists
for — our own file moved, and the comment stayed.

The second cannot be settled from the tree, because a name that was never here
is as likely to be a citation of somebody else's source as a mistake. libX11's
`ParseCol.c`, tmux's `control.c` and XNU's `bsd/kern/tty.c` are named for the
same reason a paper cites a page number, and asking whether they are tracked
here answers nothing.

A design document adds a third case: it names the files a plan would create, and
until the plan lands the reference is doing its job by not resolving. Those are
keyed by site rather than by name — the same spelling is rot in any file that is
not the document proposing it.

A name is compared only if it is ours to begin with. Every `#include <...>` and
`#import <...>` spelling anywhere in the tree is collected first, so `unistd.h`
is never read as a claim about this repository.

Three shapes are filtered before matching, each a filename in appearance only.

    a one-character stem      `M.m` is BCD version notation, `A.cpp` is a
                              stand-in in a script's usage text
    a glob                    `*.tables.inc` names a set to expand, and a match
                              starting inside it reads as a missing file
    a slash-joined pair       a directory does not end in `.cpp`, so
                              `a.cpp/b.cpp` is two names, not one path

Stdlib only.

    python3 Scripts/check-comment-file-references.py
"""

import collections
import os
import re
import subprocess
import sys
from importlib import import_module

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

# The comment extractor is check-slop's, which blanks code rather than dropping
# it so offsets — and therefore line numbers — survive. One implementation of
# "what part of this file is addressed to a reader" serves both checks.
slop = import_module("check-slop")
import ubq_roots  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = ubq_roots.subject_root()

# Where a repository mounts a library it depends on: Core or the Kit directly,
# or Core inside the Kit, as the app carries it.
LIBRARY_MOUNT = re.compile(r"^(?:Libraries/ubiquitty-kit/)?Libraries/ubiquitty-(?:core|kit)/")

SUFFIXES = (".c", ".cc", ".cpp", ".h", ".hpp", ".inc", ".m", ".mm", ".swift",
            ".js", ".mjs", ".metal", ".py", ".sh", ".cmake", ".md",
            ".yml", ".yaml", ".toml", ".frag", ".vert")

# Read whole rather than reduced to comments. Markdown is prose end to end, and
# a workflow or a config names a script in a `run:` step or a value — passing
# either through the comment extractor keeps only the `#` lines and drops the
# references that matter.
#
# A workflow naming a script that moved fails in CI and nowhere else, which is
# the furthest a reference can rot before anyone sees it.
PROSE_SUFFIXES = (".md", ".yml", ".yaml", ".toml")

SKIP_MARKERS = ("ThirdParty/", "third-party-licenses/", "node_modules/")

# The extensions a reference is recognised by. A name ending in anything else is
# prose: `Info.plist` and a Markdown file are named as artefacts rather than as
# places to read code, and a document that links to another document does it
# with a Markdown link, which is a different claim from "the code is here".
EXTENSIONS = (".c", ".cc", ".cpp", ".h", ".hpp", ".inc", ".m", ".mm", ".swift",
              ".js", ".mjs", ".metal", ".sh", ".py")

SEGMENT = r'[A-Za-z][A-Za-z0-9_+.-]*'
# The lookbehind keeps a match from starting inside a longer token. Without it
# `*.tables.inc` matches from `tables`, and the glob a reader is meant to expand
# is reported as a file that is missing. `$` and `{` are there for the same
# reason one level out: `$UBIQUITTY_INTEGRATION_DIR/zsh.sh` and
# `${RUNNER_TEMP}/tools/check-board-ref.sh` are expansions whose first segment is
# a variable, and matching from after the sigil reports a path nobody wrote.
REFERENCE = re.compile(
    r'(?<![*.\w/${-])%s(?:/%s)*(?:%s)\b'
    % (SEGMENT, SEGMENT, "|".join(re.escape(e) for e in EXTENSIONS)))

INCLUDE = re.compile(r'#\s*(?:include|import)\s*<([^>]+)>')

# The reference has to be a filename, not a fragment that reads like one.
MINIMUM_STEM = 2

# Names this repository never had. Each says why it points outside the tree, so
# a reader can tell a citation from a mistake without running git.
#
# An entry whose file exists now fails rather than passing: an exemption that
# outlives its condition is one nobody will think to delete.
ALLOWED = {
    # Products and system headers named as themselves, not as files of this tree.
    "Node.js": "the JavaScript runtime the web build's tooling runs on, named as a product",
    "ubiquitty.js": "the web build's output bundle, a build product staged beside ubiquitty.wasm",
    "ci_pre_xcodebuild.sh": "Xcode Cloud's pre-build hook, named to say this repository needs none",
    # Cited sources in other projects — read the way a page number is read.
    "ParseCol.c": "libX11, the colour-name parser this one is written against",
    "src/xcms/LRGB.c": "libX11's CIE conversions, cited for their coefficients",
    "src/xcms/HVC.c": "libX11's TekHVC conversion",
    "HVC.c": "libX11's TekHVC conversion",
    "HVCMxC.c": "libX11's TekHVC maximum-chroma solver",
    "Lab.c": "libX11's CIELab conversion",
    "Luv.c": "libX11's CIELuv conversion",
    "uvY.c": "libX11's CIEuvY conversion",
    "xyY.c": "libX11's CIExyY conversion",
    "tmux/control.c": "tmux's control-mode writer, the protocol reference",
    "misc.c": "xterm's source, cited at xterm-411a for its reply formats",
    "charproc.c": "xterm's source, cited at xterm-411a for its control dispatch",
    "charsets.c": "xterm's source, cited at xterm-411a for national character-set translation",
    "input.c": "xterm's source, cited at xterm-411a for its key encoding",
    "screen.c": "xterm's source, cited at xterm-411a for its checksum",
    "doublechr.c": "xterm's double-width line implementation, cited for its retained hidden cells",
    "graphics_sixel.c": "xterm's sixel parser, cited for cursor and pending-wrap behavior",
    "xtermcap.c": "xterm's source, cited at xterm-411a for its capability table",
    "gtk/gtkrenderlayout.c": "GTK 4's insertion-cursor geometry, the reference the Linux bar caret matches",
    "gtk/gtktext.c": "GTK 4's GtkText caret blink, the reference the Linux bar caret's blink follows",
    "cmd-control-client.c": "tmux's control-mode client",
    "ptydata.c": "xterm's UTF-8 decoder, the conformance reference",
    "cursor.c": "xterm's cursor motion, the conformance reference",
    "VTPrsTbl.c": "xterm's parse tables, the conformance reference",
    "main.h": "xterm's compiled-in defaults, the conformance reference",
    "tabs.c": "xterm's tab stops, the conformance reference",
    "charsets.h": "xterm's character-set mappings, the conformance reference",
    "util.c": "xterm's scrolling and attribute operations, the conformance reference",
    "ptyx.h": "xterm's attribute and operation constants, the conformance reference",

    # Headers from an SDK or a framework that a checkout does not carry.
    "Lists.h": "a Carbon SDK header, named because its `Cell` typedef collides "
               "with Core's",
    "Events.h": "the HIToolbox SDK header the kVK_* virtual key codes come from",
    "ColorSyncDeprecated.h": "a macOS SDK header, named in a warning "
                             "suppression that has to spell it",
    "rzsz.h": "the ZMODEM reference implementation's header, cited for its "
              "control-byte values",
    "Ubiquitty-Swift.h": "Xcode generates it from the Swift sources; it exists "
                         "in the build directory and is never tracked",

    # esctest2's own files, cited the way a conformance table cites its suite.
    "esctest.py": "esctest2's entry point, in the suite this drives",

    # Sources in other projects that a document reads alongside its own.
    "repro.c": "the reproducer in an upstream report's shell transcript",
    "CheckedArithmetic.h": "WebKit's overflow-safe arithmetic header",
    "Source/WTF/wtf/CheckedArithmetic.h": "the same header, by its path in "
                                          "WebKit's tree",

    # Stand-ins in usage text and worked examples. A real path here would send a
    # reader to a real file the example is not about.
    "Source/Core/Foo.cpp": "the example path in a gate's own output format",
    "src/foo.cpp": "the example path in a diff-shape comment",
    "Source/a1.h": "the fixture name in check-mutants' worked example",
    "a1.h": "the fixture name in the changed-line mapping test",
    "out.cpp": "the name a generator writes to in its usage line",
}


# Sites that name a file the tree does not hold, on purpose. A tool for finding
# where a file went has to name one that went somewhere. Keyed by site rather
# than by name: the same spelling is rot everywhere else, and a name-wide entry
# would excuse it.
ALLOWED_SITES = {
    ("Scripts/where-did-it-go.sh", "Source/Core/OSC.cpp"):
        "the worked example of a path this tool resolves across a rename",
    ("Scripts/where-did-it-go.sh", "OSC.cpp"):
        "the worked example of a basename that would resolve ambiguously",
    # The Swift/C++ export guard's whole subject is a header the BUILD emits
    # into the build directory. It is not a source file and never will be, so
    # a checker that resolves against the tree cannot find it — which is the
    # gate reporting that this reference is doing its job.
    ("Scripts/check-swift-cxx-exports.py", "UbiquittyRenderKit-Swift.h"):
        "generated by swiftc into the build directory; the guard reads it there",
    ("Source/Core/Attributes/LinkRun.h", "readme.txt.sh"):
        "an example of a name whose suffix match is exact — the sentence is "
        "about the spelling, not about a file",
    ("Scripts/format-code.sh", "emoji_atlas_data.c"):
        "a generated file, excluded by name whether or not a generator has run",

    # A rule about how a script is NAMED has to show a script being named, and
    # the example is the sentence's subject rather than a pointer anywhere.
    # Per site rather than per name, so the same spelling stays rot elsewhere.
    ("Source/PAL/Process.h", "deploy.py"):
        "the worked example of an interpreter stepping aside for its script",
    ("Source/PAL/Process.cpp", "deploy.py"):
        "the same example, at the predicate that decides a token is a script",
    ("Tests/Core/test_script_naming.cpp", "deploy.py"):
        "the script the naming cases are written about; every row invents it",
}


# The repositories whose files name an entry, for the entries that are not Core's.
# A checkout that holds none of an entry's repositories cannot tell whether it is
# still named, so it judges only the entries of the repositories it holds; a
# checkout that holds them all judges every entry.
ALLOWED_OWNERS = {
    "ci_pre_xcodebuild.sh": ("app",),
    "Node.js": ("app",),
    "ColorSyncDeprecated.h": ("app",),
    "esctest.py": ("app",),
    "ubiquitty.js": ("core", "app"),
    ("Scripts/check-swift-cxx-exports.py", "UbiquittyRenderKit-Swift.h"): ("kit",),
}


def tracked():
    listing = subprocess.run(["git", "-C", REPO, "ls-files"],
                             capture_output=True, text=True)
    # Audits/ quotes stale paths on purpose (a report names what moved); it never ships.
    return [path for path in listing.stdout.split("\n")
            if path and not path.startswith("Audits/")]


def libraryTracked():
    """Every path the libraries this checkout carries track, relative to their roots.

    A file a library holds is one the checkout can read, so a pointer to it is
    followed rather than reported. A checkout that holds every repository is its
    own library and adds nothing.
    """
    found = []
    for root in sorted({ubq_roots.core_root(REPO), ubq_roots.kit_root(REPO)} - {REPO}):
        listing = subprocess.run(["git", "-C", root, "ls-files"],
                                 capture_output=True, text=True,
                                 env=ubq_roots.foreign_git_environment())
        found += [path for path in listing.stdout.split("\n") if path]
    return found


def everTracked():
    """Every path this repository has held, across all refs.

    A pointer to a path that was here and is gone is rot with no second reading,
    and this is the half of the evidence the working tree cannot supply.
    """
    listing = subprocess.run(
        ["git", "-C", REPO, "log", "--pretty=format:", "--name-only", "--all"],
        capture_output=True, text=True)
    return set(path for path in listing.stdout.split("\n") if path)


def externalHeaders(texts):
    """Every angle-bracket include spelling in the tree, by full name and base.

    A name this repository includes from outside is not a claim about this
    repository, and asking whether `sys/socket.h` is tracked here answers a
    question nobody asked.
    """
    external = set()
    for text in texts.values():
        for name in INCLUDE.findall(text):
            external.add(name)
            external.add(os.path.basename(name))
    return external


def matcher(paths):
    """A callable answering whether a spelling names one of `paths`."""
    exact = set(paths)
    bases = set(os.path.basename(path) for path in paths)
    # A partial path can only end a path that ends with its own last component,
    # so the search is grouped by that component. Scanning every path for every
    # such name multiplies endswith calls to answer a question one bucket
    # settles -- and a bucket usually holds one path.
    byBase = {}
    for path in paths:
        byBase.setdefault(os.path.basename(path), []).append(path)

    def matches(name):
        if name in exact:
            return True
        if "/" in name:
            tail = "/" + name
            return any(path.endswith(tail)
                       for path in byBase.get(os.path.basename(name), ()))
        return name in bases

    return matches


def references(path, text):
    """[(line, name)] for every filename spelled in a comment."""
    readable = text if path.endswith(PROSE_SUFFIXES) else slop.prose(path, text)
    # This file's own docstring names dead paths on purpose: they are the worked
    # examples of the defect, mentioned rather than followed. check-slop blanks
    # its own the same way and for the same reason.
    if os.path.abspath(os.path.join(REPO, path)) == os.path.abspath(__file__):
        readable = slop.blank_module_docstring(readable)
    found = []
    for match in REFERENCE.finditer(readable):
        # A directory does not end in a source extension, so `a.cpp/b.cpp` is two
        # names a writer separated with a slash rather than one path.
        segments = match.group(0).split("/")
        while len(segments) > 1 and segments[0].endswith(EXTENSIONS):
            segments.pop(0)
        name = "/".join(segments)
        if len(os.path.splitext(os.path.basename(name))[0]) < MINIMUM_STEM:
            continue
        found.append((readable.count("\n", 0, match.start()) + 1, name))
    return found


def main():
    paths = tracked()
    # Extension or shebang. Every git hook here is extensionless, so a suffix
    # filter would exclude the six scripts that run every other gate.
    sources = [path for path in paths
               if (path.endswith(SUFFIXES) or slop.hasShebang(os.path.join(REPO, path)))
               and not any(marker in path for marker in SKIP_MARKERS)
               and os.path.basename(os.path.dirname(path)) not in slop.BINARY_DIRECTORIES]
    if not sources:
        print("check-comment-file-references: no tracked sources matched — the "
              "walk found nothing,", file=sys.stderr)
        print("  which is not the same as every reference resolving.",
              file=sys.stderr)
        return 2

    texts = {}
    for path in sources:
        try:
            with open(os.path.join(REPO, path), encoding="utf-8",
                      errors="replace") as handle:
                texts[path] = handle.read()
        except OSError:
            continue

    external = externalHeaders(texts)
    # A file another repository owns is named, not missing: one a library this
    # checkout carries holds, or one listed in Docs/other-repositories.txt.
    holds = matcher(paths + libraryTracked() + sorted(ubq_roots.elsewhere(REPO)[0]))

    def here(name):
        # A path through a library mount names the library's own file, which a
        # checkout holding the library keeps at its root.
        return holds(name) or holds(LIBRARY_MOUNT.sub("", name))

    # Reading every path across every ref costs about as much as the scan it
    # informs, and it answers a question that only arises once a reference has
    # already failed to resolve. On a tree with none, it is never asked.
    history = []

    def everHere(name):
        if not history:
            history.append(matcher(everTracked()))
        return history[0](name)

    moved, unknown = [], []
    seen = 0
    usedAllowance = set()
    for path, text in texts.items():
        for line, name in references(path, text):
            if name in external or os.path.basename(name) in external:
                continue
            seen += 1
            if here(name):
                continue
            # The allowlist is read before the history, because a name outside
            # this tree can still collide with a path this tree once had:
            # `Events.h` is HIToolbox's, and an old file of ours by that name
            # does not make the reference to Apple's one wrong.
            if name in ALLOWED:
                usedAllowance.add(name)
            elif (path, name) in ALLOWED_SITES:
                usedAllowance.add((path, name))
            elif everHere(name):
                moved.append((path, line, name))
            else:
                unknown.append((path, line, name))

    if not seen:
        print("check-comment-file-references: not one comment named a source "
              "file, across %d files —" % len(sources), file=sys.stderr)
        print("  the extractor is reading something other than comments.",
              file=sys.stderr)
        return 2

    arrived = [(name, why) for name, why in sorted(ALLOWED.items())
               if here(name)]
    arrived += [("%s at %s" % (name, site), why)
                for (site, name), why in sorted(ALLOWED_SITES.items())
                if here(name)]
    held = set(ubq_roots.repositories(REPO))
    unused = ([name for name in ALLOWED
               if name not in usedAllowance and held & set(ALLOWED_OWNERS.get(name, ("core",)))]
              + ["%s at %s" % (name, site) for site, name in ALLOWED_SITES
                 if (site, name) not in usedAllowance
                 and held & set(ALLOWED_OWNERS.get((site, name), ("core",)))])

    for path, line, name in sorted(moved):
        print("check-comment-file-references: %s:%d names %s, which this "
              "repository had and moved" % (path, line, name), file=sys.stderr)
    for path, line, name in sorted(unknown):
        print("check-comment-file-references: %s:%d names %s, which is not a "
              "file here and never was" % (path, line, name), file=sys.stderr)
    for name, why in arrived:
        print("check-comment-file-references: %s is allowed as \"%s\" but the "
              "file exists now — delete the entry" % (name, why),
              file=sys.stderr)

    if moved or unknown or arrived:
        print("""
  A comment naming a file is the only link between two places, and no build
  reads it. Point it at the file that holds the thing now, or drop the pointer.

  `git log --diff-filter=D --name-only -- '*<name>'` says where a moved file
  went. A name that was never here belongs in ALLOWED with its reason.""",
              file=sys.stderr)
        return 1

    if unused:
        print("check-comment-file-references: %d allowlist entries excuse "
              "nothing — no comment names them:" % len(unused), file=sys.stderr)
        for entry in sorted(unused):
            print("  %s" % entry, file=sys.stderr)
        print("\n  An entry nobody reaches is a note about a reference that is "
              "gone. Delete it.", file=sys.stderr)
        return 1

    print("check-comment-file-references: ok (%d references across %d files "
          "resolve; %d allowed by name, %d by site)"
          % (seen, len(sources), len(ALLOWED), len(ALLOWED_SITES)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
