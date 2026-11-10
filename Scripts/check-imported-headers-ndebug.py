#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0
"""check-imported-headers-ndebug.py — no header Swift imports changes with NDEBUG.

A Swift module reads C++ headers through its C++ importer, which compiles them
in the module's own translation units with what -Xcc hands it, not with the C++
build's flags. A release build compiles the C++ with NDEBUG, and an importer
not handed the same reads a header that tests NDEBUG as a different header. A
member under `#ifndef NDEBUG` lays a class out one member longer for Swift than
for the archive it links against, and every Swift-side access past it reads
the wrong bytes; an inline function under it behaves one way in the copy Swift
emits and another in the C++ build's, and the linker keeps whichever it meets
first.

So every header the importer can reach is held to one shape whatever NDEBUG
says. The importer reaches what a module map lists and everything those headers
include. The maps are the subject's own, every module.modulemap the checkout
tracks outside vendored code and the libraries it carries, and the maps under
the Source/ of each library it reads (ubq_roots): Core's
Source/Platform/Apple/module.modulemap, which every Apple arm imports, and the
Kit's. So the gate runs in ubiquitty-core, the Kit and the app alike, and holds
each repository's imports where they are written. A test of NDEBUG belongs in
a .cpp, which only the C++ build compiles. Headers no map reaches, included by
.cpp files alone, are not this gate's business.

Stdlib only.

    python3 Scripts/check-imported-headers-ndebug.py
    python3 Scripts/check-imported-headers-ndebug.py --self-test
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gate_report  # noqa: E402 — the path above is what makes it importable
import ubq_roots  # noqa: E402

GATE = "check-imported-headers-ndebug"

CORE_MAP = os.path.join("Source", "Platform", "Apple", "module.modulemap")

# Where the importer looks for a quoted include, after the including header's
# own directory: the -I roots every Apple arm hands it, in each checkout.
SEARCH_DIRECTORIES = (
    "Source",
    os.path.join("Source", "Platform", "Apple"),
    os.path.join("Source", "Platform", "macOS"),
)

# Vendored code keeps its upstream spelling, and a library a checkout carries is
# judged as the root it is, not as the subject's files.
EXCLUDED_PARTS = ("ThirdParty", "Libraries")

HEADER = re.compile(r'^\s*(?:private\s+|textual\s+)?header\s+"([^"]+)"', re.M)
INCLUDE = re.compile(r'^\s*#\s*(?:include|import)\s+"([^"]+)"', re.M)
# A preprocessor test of NDEBUG, in any of its spellings. A comment or an
# `assert()` call is not a test of the macro and is not matched.
TEST = re.compile(r"^\s*#\s*(?:if|ifdef|ifndef|elif|elifdef|elifndef)\b.*\bNDEBUG\b")


def read(path):
    try:
        with open(path, encoding="utf-8", errors="replace") as handle:
            return handle.read()
    except OSError:
        return None


def isExcluded(relative):
    return any(part in EXCLUDED_PARTS for part in relative.split(os.sep))


def trackedMaps(root):
    """The module maps a checkout tracks, as paths relative to it."""
    listed = subprocess.run(["git", "ls-files", "-z", "--", "*module.modulemap"], cwd=root,
                            env=ubq_roots.foreign_git_environment(), capture_output=True, text=True, check=False)
    if listed.returncode != 0:
        return None
    return sorted(path for path in listed.stdout.split("\0") if path and not isExcluded(path))


def maps(subject, libraries):
    """Every map the subject's importer reads, as absolute paths, or None when the subject is no checkout.

    The subject's are every map it tracks. A library's are the ones under its
    Source/, which its embedders import: Core's Apple map, and the Kit's.
    """
    own = trackedMaps(subject)
    if own is None:
        return None
    found = [os.path.join(subject, path) for path in own]
    for library in libraries:
        listed = trackedMaps(library) or []
        for path in listed:
            full = os.path.join(library, path)
            if path.startswith("Source" + os.sep) and full not in found:
                found.append(full)
        coreMap = os.path.join(library, CORE_MAP)
        if os.path.isfile(coreMap) and coreMap not in found:
            found.append(coreMap)
    return found


def resolve(name, fromDirectory, roots):
    """The absolute path an include names, or None outside every root."""
    candidates = [fromDirectory] + [os.path.join(root, directory) for root in roots for directory in SEARCH_DIRECTORIES]
    for base in candidates:
        candidate = os.path.normpath(os.path.join(base, name))
        if os.path.isfile(candidate):
            return candidate
    return None


def holder(path, roots):
    """(index, relative path) of the deepest root holding `path`, or None outside them all.

    The deepest, because a checkout can carry Core inside it, as the Kit's
    Libraries/ubiquitty-core: a Core header there is Core's, not the subject's.
    """
    held = [(len(root), index, os.path.relpath(path, root)) for index, root in enumerate(roots)
            if not os.path.relpath(path, root).startswith("..")]
    if not held:
        return None
    _, index, relative = max(held)
    return index, relative


def label(path, roots, names):
    """`path` relative to the root that holds it, prefixed with that root's name."""
    held = holder(path, roots)
    if held is None:
        return path
    index, relative = held
    return relative if names[index] is None else "%s:%s" % (names[index], relative)


def closure(mapPaths, roots):
    """Every header the maps list and every header those reach, by absolute path."""
    pending = []
    for mapPath in mapPaths:
        for name in HEADER.findall(read(mapPath) or ""):
            listed = os.path.normpath(os.path.join(os.path.dirname(mapPath), name))
            if os.path.isfile(listed):
                pending.append(listed)
    seen = set()
    while pending:
        path = pending.pop()
        if path in seen:
            continue
        held = holder(path, roots)
        if held is not None and isExcluded(held[1]):
            continue
        seen.add(path)
        for name in INCLUDE.findall(read(path) or ""):
            found = resolve(name, os.path.dirname(path), roots)
            if found:
                pending.append(found)
    return seen


def check(subject, core, kit=None):
    """Returns (offenders, headers), or (None, 0) when there was nothing to compare."""
    roots, names = [os.path.realpath(subject)], [None]
    for library, name in ((core, "core"), (kit, "kit")):
        if library and os.path.realpath(library) not in roots:
            roots.append(os.path.realpath(library))
            names.append(name)
    subject = roots[0]
    mapPaths = maps(subject, roots[1:])
    if not mapPaths:
        return None, 0
    headers = closure(mapPaths, roots)
    if not headers:
        return None, 0
    offenders = []
    for path in sorted(headers, key=lambda header: label(header, roots, names)):
        for number, line in enumerate((read(path) or "").split("\n"), start=1):
            if TEST.match(line):
                offenders.append("%s:%d %s" % (label(path, roots, names), number, line.strip()))
    return offenders, len(headers)


def selfTest():
    ok = True
    trees = []

    def tree(files):
        directory = tempfile.mkdtemp(prefix="check-imported-headers-ndebug.")
        trees.append(directory)
        for path, body in files.items():
            full = os.path.join(directory, path)
            os.makedirs(os.path.dirname(full), exist_ok=True)
            with open(full, "w", encoding="utf-8") as handle:
                handle.write(body)
        environment = ubq_roots.foreign_git_environment()
        subprocess.run(["git", "init", "-q"], cwd=directory, env=environment, check=True)
        subprocess.run(["git", "add", "-A"], cwd=directory, env=environment, check=True)
        return directory

    def fail(message):
        nonlocal ok
        print("self-test: %s" % message, file=sys.stderr)
        ok = False

    coreMap = 'module Core {\n    header "../../Core/Listed.h"\n    export *\n}\n'
    clean = {
        CORE_MAP: coreMap,
        "Source/Core/Listed.h": '#pragma once\n#include "Core/Reached.h"\n',
        "Source/Core/Reached.h": "#pragma once\n// NDEBUG in a comment is prose.\ninline int f(int x) { return x; }\n",
        "Source/Core/CppOnly.h": "#pragma once\n#ifndef NDEBUG\nint debugOnly;\n#endif\n",
        "Source/ThirdParty/vendor/module.modulemap": 'module Vendor {\n    header "vendor.h"\n}\n',
        "Source/ThirdParty/vendor/vendor.h": "#ifdef NDEBUG\n#endif\n",
    }
    core = tree(clean)
    offenders, headers = check(core, core)
    if offenders or headers != 2:
        fail("a clean closure was rejected or miscounted: %s (%d)" % (offenders, headers))

    # The defect, one include away from the map: a member the archive lacks.
    reached = dict(clean)
    reached["Source/Core/Reached.h"] = "#pragma once\nstruct S {\n#ifndef NDEBUG\n    bool flag;\n#endif\n};\n"
    root = tree(reached)
    offenders, _ = check(root, root)
    if not offenders or "Source/Core/Reached.h:3" not in offenders[0]:
        fail("a member under #ifndef NDEBUG in a reached header passed: %s" % offenders)

    # Every spelling of the test.
    for spelling in ("#ifdef NDEBUG", "#if !defined(NDEBUG)", "#  elif defined(NDEBUG)", "#if NDEBUG + 0"):
        variant = dict(clean)
        variant["Source/Core/Listed.h"] = "#pragma once\n#if 0\n%s\n#endif\n" % spelling
        if spelling.lstrip("# ").startswith("if"):
            variant["Source/Core/Listed.h"] = "#pragma once\n%s\n#endif\n" % spelling
        root = tree(variant)
        offenders, _ = check(root, root)
        if not offenders:
            fail("%r passed" % spelling)

    # A checkout that imports Core through a map of its own, as the Kit and the
    # app do: its own header the map lists, and Core's header that one reaches,
    # are both the importer's, and each is named by the root that holds it.
    kitHeader = '#pragma once\n#include "Core/Listed.h"\nstruct KitView {\n#ifdef NDEBUG\n    int spare;\n#endif\n};\n'
    kit = tree({
        "Source/Platform/macOS/module.modulemap": 'module Kit {\n    header "../../Kit/View.h"\n    export *\n}\n',
        "Source/Kit/View.h": kitHeader,
        "Source/Kit/Unlisted.h": "#pragma once\n#ifndef NDEBUG\nint debugOnly;\n#endif\n",
    })
    offenders, headers = check(kit, tree(reached))
    named = " ".join(offenders or [])
    if "Source/Kit/View.h:4" not in named or "core:Source/Core/Reached.h:3" not in named:
        fail("a checkout's own map and the Core header it reaches were not both judged: %s" % offenders)
    if "Unlisted.h" in named or headers != 3:
        fail("a header no map reaches was judged, or the closure miscounted: %s (%d)" % (offenders, headers))
    offenders, _ = check(kit, core)
    if offenders != ["Source/Kit/View.h:4 #ifdef NDEBUG"]:
        fail("a clean Core under a checkout's own test was misjudged: %s" % offenders)

    # A checkout that embeds the Kit imports the Kit's maps as well as its own.
    app = tree({"Source/App/Main.h": "#pragma once\n"})
    offenders, _ = check(app, core, kit)
    if offenders != ["kit:Source/Kit/View.h:4 #ifdef NDEBUG"]:
        fail("a Kit header an embedding checkout imports was not judged: %s" % offenders)

    # A checkout with no map and a Core without one compare nothing, which must
    # read as nothing compared, never as a pass.
    root = tree({"Source/Core/Listed.h": "#pragma once\n"})
    offenders, headers = check(root, root)
    if offenders is not None or headers:
        fail("a checkout without a module map did not report as nothing compared")

    for directory in trees:
        shutil.rmtree(directory, ignore_errors=True)
    print("%s: self-test %s" % (GATE, "ok" if ok else "FAILED"))
    return 0 if ok else 1


def main():
    if "--self-test" in sys.argv:
        return selfTest()

    subject = ubq_roots.subject_root()
    offenders, headers = check(subject, ubq_roots.core_root(subject), ubq_roots.kit_root(subject))
    if offenders is None:
        print("%s: FAILED — %s tracks no module map and Core's %s lists no header that exists, so nothing "
              "was compared" % (GATE, subject, CORE_MAP), file=sys.stderr)
        return 1
    if offenders:
        status = gate_report.failed(GATE, "test(s) of NDEBUG in a header the Swift importer reaches", offenders)
        print("\n  A release build compiles the C++ with NDEBUG and imports these headers into\n"
              "  Swift without it, so each test above makes the two compilers read a\n"
              "  different header: a member present for one and absent for the other moves\n"
              "  every member after it. Declare the member in every build and keep the test\n"
              "  in a .cpp, which only the C++ build compiles.", file=sys.stderr)
        return status
    print("%s: ok (%d header(s) the module maps reach, none testing NDEBUG)" % (GATE, headers))
    return 0


if __name__ == "__main__":
    sys.exit(main())
