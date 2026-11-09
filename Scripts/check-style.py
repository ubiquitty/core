#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""check-style.py — every content gate, in one run.

  Scripts/check-style.py              # this branch's files, against origin/main
  Scripts/check-style.py --full       # the whole tree
  Scripts/check-style.py --since REF  # this branch's files, against REF
  Scripts/check-style.py --self-test  # no section can go missing

Exit 0 = every gate passed, 1 = at least one failed.

Most sections are the same section: run a gate, print what it said, and record
the title when it fails. Those are rows in the table below; the rest carry a
rule of their own and are written out as handlers.

ubiquitty-core's sections are the table in this file. Another repository's are
Scripts/style_sections_<repository>.py, found in its own Scripts/; a checkout that
holds every repository runs all three. Scripts/repository.txt says
which repository a checkout is, and a section outside it is named, not run.

A gate's verdict is a fact about the bytes it read, so a run that changed none
of them already knows the answer (Scripts/gate_verdict.py). Nothing is narrowed
by that: a gate whose inputs moved still asks the whole question over the whole
tree.
"""


import fnmatch
import glob
import importlib.util
import os
import re
import shlex
import shutil
import subprocess
import sys
import time
import types

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import gate_corpus
import gate_report
import ubq_roots
from style_support import (CAST_PATTERN, DEFERRED, GATE, IN_COMMENT, STOOD_DOWN, Run,
                           grep_lines, scanned, tracked)


REPOSITORY = "core"
EVERYWHERE = ("core", "kit", "app")
SWIFT = ("kit", "app")
APP = ("app",)
SCRIPTS = os.path.dirname(os.path.abspath(__file__))


def clang_format_pin():
    """The clang-format major every C-family formatting check requires.

    It is kept in one file that format-code.sh and the pre-commit hook read as
    well, so the three cannot disagree about which formatter is the oracle.
    """
    with open(os.path.join(SCRIPTS, "clang-format-pin"), encoding="utf-8") as handle:
        return int(handle.read().strip())


CLANG_FORMAT_PIN = clang_format_pin()



# --- the sections that carry a rule of their own -----------------------------

def whitespace_and_conflict_markers(run):
    status, output = run.shell(["git", "diff", "--check"])
    if status == 0:
        print("ok")
    else:
        run.emit(output)
        print("FAIL: git diff --check found issues")
        run.fail()


def clang_format_drift(run):
    found = ""
    for candidate in (f"clang-format-{CLANG_FORMAT_PIN}", "clang-format"):
        if shutil.which(candidate):
            found = candidate
            break
    if not found:
        run.emit("check-format: SKIPPED — no clang-format found")
        return
    version = subprocess.run([found, "--version"], capture_output=True,
                             text=True, check=False).stdout
    if re.search(rf"version {CLANG_FORMAT_PIN}\.", version):
        status, output = run.shell(["./Scripts/format-code.sh", "--check"],
                                   env=dict(os.environ, CLANG_FORMAT=found))
        run.emit(output)
        if status != 0:
            print(f"FAIL: clang-format {CLANG_FORMAT_PIN} drift — run "
                  f"./Scripts/format-code.sh")
            run.fail()
        return
    have = re.search(r"[0-9]+", version)
    run.emit(f"check-format: SKIPPED — clang-format "
             f"{have.group(0) if have else '?'} present, pinned is "
             f"{CLANG_FORMAT_PIN}")


def shellScripts(run):
    """The shell scripts this run covers, narrowed to the branch's own.

    - Parameter run: the run state, which carries the scope.
    - Returns: the paths to examine, sorted.
    """
    paths = sorted(glob.glob("Scripts/**/*.sh", recursive=True))
    if not run.since:
        return paths
    branch = set(run.files)
    return [path for path in paths if path in branch]


def shell_shebang(run):
    paths = shellScripts(run)
    passed = 0
    for path in paths:
        with open(os.path.join(run.root, path), "rb") as handle:
            if handle.read(2) == b"#!":
                passed += 1
            else:
                print(f"FAIL: {path} — no shebang at byte 0 (a shebang below "
                      f"line 1 is only a comment)")
                run.fail()
    scanned(run, "shell script(s) with a shebang", passed, paths, scoped=bool(run.since))


def shell_syntax(run):
    paths = shellScripts(run)
    passed = 0
    for path in paths:
        status, output = run.shell(["bash", "-n", path])
        if status == 0:
            passed += 1
        else:
            run.emit(output)
            print(f"FAIL: {path}")
            run.fail()
    scanned(run, "shell script(s) parsed", passed, paths, scoped=bool(run.since))


def continuation_into_blank_line(run):
    bad = []
    for path in sorted(set(glob.glob("Scripts/**/*.sh", recursive=True))):
        with open(os.path.join(run.root, path), encoding="utf-8",
                  errors="replace") as handle:
            lines = handle.read().split("\n")
        for index in range(len(lines) - 1):
            if lines[index].rstrip().endswith("\\") and not lines[index + 1].strip():
                bad.append(f"{path}:{index + 1}: continuation escapes a blank "
                           f"line; the arguments below run as commands")
    if bad:
        run.emit("\n".join(bad))
        run.fail()
    else:
        run.emit("ok: no continuation escapes a blank line")


def shellcheck_at_warning(run):
    run.scoped(r"\.sh$|^Scripts/hooks/", "check-shell.sh",
               ["./Scripts/check-shell.sh"])


def verdict_cache_key(run):
    # Run directly rather than through the cache: this is the section that says
    # the cache is sound, and a cached answer to that question assumes what it
    # is checking.
    status, output = run.shell(["./Scripts/check-gate-verdict-cache.sh"])
    run.report(output, "last" if status == 0 else "all")
    if status != 0:
        run.fail("the verdict cache would replay a pass over files that changed")


def gate_subjects(run):
    touched = []
    for arguments in (["diff", "--name-only", "origin/main...HEAD", "--", "Scripts/"],
                      ["diff", "--name-only", "--", "Scripts/"],
                      ["diff", "--cached", "--name-only", "--", "Scripts/"]):
        touched += subprocess.run(["git"] + arguments, capture_output=True,
                                  text=True, check=False,
                                  cwd=run.root).stdout.split()
    if touched or os.environ.get("UBIQUITTY_CHECK_GATE_SUBJECTS") == "1":
        run.gate("check-gate-subjects.sh", ["./Scripts/check-gate-subjects.sh"])
        return
    print("check-gate-subjects: no Scripts/ change — a gate's subject is "
          "invariant under source edits")
    print("  (UBIQUITTY_CHECK_GATE_SUBJECTS=1 runs it anyway)")


def slop(run):
    if run.since:
        arguments = ["--since", run.since, "."]
    else:
        arguments = ["--attribute", "origin/main", "."]
    run.gate("check-slop.py",
             ["python3", "./Scripts/check-slop.py"] + arguments, ok="ok")


def acronyms(run):
    run.scoped(r"\.(h|cp|cpp|mm|m|swift)$", "check-acronyms.py",
               ["./Scripts/check-acronyms.py"], ok="ok")


def cstyle_casts(run):
    hits = [line for line in
            grep_lines(run, CAST_PATTERN, ["Source/Core"],
                        ["*.cpp", "*.h"])
            if not IN_COMMENT.search(line)]
    if hits:
        print("C-style pointer casts found — replace with "
              "static_cast/reinterpret_cast:")
        run.emit("\n".join(hits))
        run.fail()
    else:
        print("ok")


CAP_EXEMPT = re.compile(
    r"Capabilit|Capable|Capacit|Captur|Capit|Capsule"
    r"|(line|stroke|miter|join|round|butt|square|flat|corner|end|pill)Cap"
    r"|CALineCap|Caps_?Lock|Capp|escape"
    # Cap height is the typographic metric (CoreText's CTFontGetCapHeight):
    # the height of the capitals, not an abbreviation of anything.
    r"|CapHeight", re.IGNORECASE)


def snapshot_faces(run):
    """Every snapshot profile names a family this project ships, and a reader.

    A family Core ships resolves to a file under Resources/fonts and every arm
    draws the same face. One it does not ship falls through to the platform's
    matcher, and the golden then records whichever face that box happened to
    have — measured on two boxes, one substituted the variable Noto Sans from
    /usr/share/fonts and the other this tree's static NotoSans-Regular.ttf.

    A profile no script names by path and no case names through `native` is
    handed to no renderer, so the directory holds only profiles with a reader.
    """
    # `gate` rather than `scoped`: an unshipped family is a STANDING defect in
    # files nobody is editing, so a check that ran only when a profile or a
    # font table changed would let it sit. It reads the
    # profiles and the tracked text once, in a second or so, every time.
    run.gate("check-snapshot-faces.py", ["./Scripts/check-snapshot-faces.py"],
             ok="last")


def swift_selector_from_string(run):
    """A Selector built from a string literal, which Swift 6 warns about.

    `Selector(("name"))` — double parentheses — does not quiet the warning in
    Swift 6, and the compiler asks for `#selector`. Where the method is
    Objective-C-only, `#selector` cannot name it and `NSSelectorFromString` is
    the spelling that warns about nothing. A branch that predates the fix can
    land the old spelling again over the top, and the warning then comes back in
    a build log, where it takes a census to notice, rather than here, where it
    takes seconds.
    """
    hits = [line for line in
            grep_lines(run, r"Selector\(\(", ["Source", "Tests"], ["*.swift"])
            if not IN_COMMENT.search(line)]
    if hits:
        print("Selector built from a string literal — Swift 6 warns and asks "
              "for #selector; for an Objective-C-only method the spelling is "
              "NSSelectorFromString(\"name\"):")
        run.emit("\n".join(hits))
        run.fail()
    else:
        print("ok")


# Selectors the platform answers, which this tree therefore cannot define.
# A name here is the claim that AppKit or Foundation implements it, which a
# reader can check against the headers. Each carries why it cannot be a
# definition in this tree, so another has to make the same case.
PLATFORM_SELECTORS = {
    # NSMenu sends this to validate a submenu's parent item.
    "submenuAction:",
    # NSWorkspace's accessibility display setting, read by key.
    "accessibilityDisplayShouldShowWindowTitlebarIcons",
    # NSCursor's diagonal resize cursors, which have no public spelling; by
    # name is the only way to ask for them, and a miss degrades to the arrow.
    "_windowResizeNorthEastSouthWestCursor",
    "_windowResizeNorthWestSouthEastCursor",
    # AppKit's _NSWMWindowTilingStateController restores native Fill geometry.
    "restoreTilingState:",
    # NSWindow invalidates its cached corner mask through this AppKit method.
    "_cornerMaskChanged",
    # NSWindow's own live-resize bracket, which a corner drag opens and closes.
    # The resize parity scenario opens and closes it around its drag steps; it
    # checks respondsToSelector: first and fails when AppKit lacks them.
    "_startLiveResize",
    "_endLiveResize",
    # NSObject's allocator, which Swift does not import; a class reached by
    # name is instantiated by sending it and then an initializer.
    "alloc",
}

SELECTOR_LITERAL = re.compile(r'NSSelectorFromString\("([^"]+)"\)')


def selector_names_something(run):
    """A string selector that names a method nobody implements.

    `NSSelectorFromString` is the sanctioned spelling for an Objective-C-only
    method, and it is also a string the compiler never checks. A send to a
    name nothing answers is not a build error and not a warning: it is a
    gesture that silently does nothing, and the reader who tried it concludes
    the feature is broken rather than absent.

    The shape that needs it most is a send down the responder chain from a
    layer that cannot name its target's type. A parity ledger reading such a
    send calls the behaviour present, and only a reader driving the app finds
    out otherwise.

    A sender that cannot see its implementer cannot judge it: ubiquitty-kit sends
    `newTab:` and `selectTab:` to a responder chain the app owns, so the section
    runs where the app is held.

    The sibling gate above bans `Selector(("name"))` because the compiler
    warns about it. This one holds the spelling that replaced it, which
    nothing warns about.

    What this cannot see: a selector assembled at run time, and one whose
    implementation this grep's shapes do not match — the definition is looked
    for as `@objc(whole:selector:)`, a Swift `func base`, or an Objective-C
    `- (type)base`. A miss of the second kind is a false positive, and the
    answer is to widen the shapes rather than to add the name to the platform
    list.
    """
    # The sends and the implementations are read over every root the checkout
    # reads: the kit sends down the responder chain to methods the app answers.
    roots = sorted({run.root, run.kitRoot, run.coreRoot})
    hits = [line for root in roots for line in
            grep_lines(types.SimpleNamespace(root=root), r'NSSelectorFromString\("', ["Source"],
                       ["*.swift", "*.mm", "*.h"])
            if not IN_COMMENT.search(line)]
    names = {}
    for line in hits:
        for selector in SELECTOR_LITERAL.findall(line):
            names.setdefault(selector, line)
    # Only the Objective-C-visible languages can answer a selector, so the
    # haystack is those files rather than the whole tree.
    tree = "".join(subprocess.run(["git", "grep", "-h", "-E", r".", "--",
                                   "Source/*.swift", "Source/*.mm", "Source/*.m", "Source/*.h"],
                                  cwd=root, capture_output=True, text=True).stdout
                   for root in roots)
    unanswered = []
    for selector, line in sorted(names.items()):
        if selector in PLATFORM_SELECTORS:
            continue
        base = selector.split(":")[0]
        shapes = [rf"@objc\(\s*{re.escape(selector)}\s*\)",
                  rf"\bfunc\s+{re.escape(base)}\b",
                  rf"-\s*\([^)]*\)\s*{re.escape(base)}\b"]
        if not any(re.search(shape, tree) for shape in shapes):
            unanswered.append("%s  <- %s" % (line, selector))
    if unanswered:
        print("A string selector naming a method nothing in Source implements. "
              "The send compiles, the gesture does nothing, and no warning is "
              "raised. Call the implementation directly where you can, or name "
              "the selector something that exists:")
        run.emit("\n".join(unanswered))
        run.fail()
    else:
        print("ok (%d string selector(s), each answered by the tree or by the "
              "platform list)" % len(names))


def no_objective_c(run):
    """The tree holds no Objective-C source.

    The Objective-C arm was deleted into Swift and Core, and new code is Swift,
    or C++23 where it is Core. An `.m` or `.mm` file anywhere is either new
    Objective-C or a ported file coming back — a stale branch merged, or a
    conflict resolved to the side that still had it — and both would land
    silently without this, because the toolchain still compiles them.
    """
    tracked = subprocess.run(["git", "ls-files", "*.m", "*.mm"],
                             cwd=run.root, capture_output=True, text=True).stdout.split()
    if tracked:
        print("Objective-C source in the tree. New code is Swift, or C++ where it is "
              "Core; a ported file does not come back:")
        run.emit("\n".join(sorted(tracked)))
        run.fail()
    else:
        print("ok (no .m or .mm file is tracked)")


def cap_abbreviation(run):
    hits = [line for line in
            grep_lines(run, r"[A-Za-z_]*Cap[A-Za-z_0-9]*", ["Source", "Tests"],
                        ["*.cpp", "*.h", "*.mm"])
            if not IN_COMMENT.search(line) and not CAP_EXEMPT.search(line)]
    if hits:
        print("'Cap' abbreviation found — spell it out (domain exceptions are "
              "allowlisted in this check):")
        print("  maxThing        the most allowed, however arrived at — reach "
              "for this first")
        print("  thingCapacity   how much a buffer holds")
        print("  thingLimit      a bound someone CONFIGURED, or a region edge")
        print("  thingCapability what a peer can do")
        run.emit("\n".join(hits))
        run.fail()
    else:
        print("ok")


CID_EXEMPT = re.compile(r"ngtcp2_cid|ngtcp2_version_cid|NGTCP2_")


def cid_abbreviation(run):
    hits = [line for line in
            grep_lines(run, r"[A-Za-z_]*Cid[A-Za-z_0-9]*", ["Source", "Tests"],
                        ["*.cpp", "*.h", "*.mm"])
            if not IN_COMMENT.search(line) and not CID_EXEMPT.search(line)]
    if hits:
        print("'Cid' abbreviation found — spell out ConnectionID (ngtcp2's own "
              "ngtcp2_cid/NGTCP2_* symbols are exempt):")
        run.emit("\n".join(hits))
        run.fail()
    else:
        print("ok")


TERNARY = (r"\?[ \t]*(true[ \t]*:[ \t]*false|false[ \t]*:[ \t]*true)")

TESTS_STYLE_RULES = (
    ("snake_case function name (use lowerCamelCase)",
     r"^[ \t]*static [A-Za-z_:<>,* ]+ [a-z0-9]+_[a-z_0-9]+\("),
    ("(void) parameter list (use ())", r"[A-Za-z0-9_]\(void\)[ \t]*(\{|;|$)"),
    ("C header (use the <cxxx> form)",
     r"#include <(stdio|stdlib|string|assert|math|ctype|time|stdint|inttypes"
     r"|limits|signal|errno)\.h>"),
    ("NULL (use nullptr)", r"\bNULL\b"),
    ("C-style pointer cast (use static_cast/reinterpret_cast)", CAST_PATTERN),
    ("ternary yielding its own condition (drop it; use != 0 to narrow)", TERNARY),
)


def tests_house_style(run):
    known = subprocess.run(["git", "rev-parse", "--verify", "--quiet",
                            "origin/main"], capture_output=True, check=False,
                           cwd=run.root).returncode == 0
    findings = []
    if known:
        base = subprocess.run(["git", "merge-base", "origin/main", "HEAD"],
                              capture_output=True, text=True, check=False,
                              cwd=run.root).stdout.strip()
        diff = subprocess.run(
            ["git", "diff", "-U0", base + "...HEAD", "--",
             "Tests/*.cpp", "Tests/*.h"],
            capture_output=True, text=True, check=False, cwd=run.root).stdout
        added = [line[1:] for line in diff.split("\n")
                 if line.startswith("+") and not line.startswith("+++")]
        for description, pattern in TESTS_STYLE_RULES:
            compiled = re.compile(pattern)
            hits = sum(1 for line in added if compiled.search(line))
            if hits:
                findings.append(f"  {hits} added line(s): {description}")
    if findings:
        print("Tests/ style violations introduced by this branch:")
        run.emit("\n".join(findings))
        print("  (Docs/coding-conventions.md §2 and §3.4)")
        run.fail()
    else:
        print("ok")


def preferences_documented(run):
    run.gate("check-preferences-documented.py",
             ["./Scripts/check-preferences-documented.py"],
             ok="last",
             message="every setting Core reads is named in the config template "
                     "(Scripts/check-preferences-documented.py)")


def vocabulary_types(run):
    # The span budget counts pointer + length parameter pairs, which std::span
    # replaces everywhere the pair is between two pieces of C++. None of the ten
    # is: the host-directory persistence seam's Store and the delegated
    # channel's send are C function pointers Swift implements as
    # @convention(c) closures, the Web arm's loadHostRecord is an emscripten
    # export JavaScript calls, and the two QUIC arms' random, stream-data and
    # datagram handlers fill ngtcp2's C callback table. No foreign ABI can
    # name a std::span, so those sites are the shape the gate exists to remove
    # and cannot be removed here.
    run.gate("check-vocabulary-types.py",
             ["./Scripts/check-vocabulary-types.py", "--gate"],
             ok="last",
             message="Source/ reaches for the C++23 vocabulary types "
                     "(Scripts/check-vocabulary-types.py)")


def ternary_yields_condition(run):
    paths = [path for path in
             tracked(run, "Source/*.cpp", "Source/*.h", "Source/*.mm",
                     "Source/*.m", "Tests/*.cpp", "Tests/*.h")
             if "ThirdParty" not in path]
    hits = []
    compiled = re.compile(TERNARY)
    for path in paths:
        try:
            with open(os.path.join(run.root, path), encoding="utf-8",
                      errors="replace") as handle:
                for number, line in enumerate(handle, start=1):
                    if compiled.search(line):
                        hits.append((path, number))
        except OSError:
            continue
    if hits:
        for path, number in hits[:20]:
            print(f"  {path}:{number}: a ternary yielding its own condition")
        print("  Drop it when the condition is already bool; write != 0 when "
              "it narrows.", file=sys.stderr)
        gate_report.failed_count("check-style", len(hits),
                                 "site(s) where a ternary yields its own condition")
        run.fail()
    else:
        print("ok")


def terminal_pointer_intent(run):
    run.gate("check-terminal-pointer-intent.py",
             ["./Scripts/check-terminal-pointer-intent.py", "--gate",
              "--budget", "0"],
             ok="last", bad="last3",
             message="a Terminal* in Core says null is meaningful "
                     "(Scripts/check-terminal-pointer-intent.py)")


TIMING_FIELD = re.compile(r"[A-Za-z_][A-Za-z0-9_]*\.(median|p95|mean)Seconds")
TIMING_VOCABULARY = re.compile(r"\b(median|p95|mean)Seconds\b")


def paired_bench(run):
    paths = sorted(set(glob.glob("Tests/**/*.cpp", recursive=True))
                   | set(glob.glob("Tests/**/*.h", recursive=True)))
    vocabulary = 0
    paired = []
    for path in paths:
        try:
            with open(os.path.join(run.root, path), encoding="utf-8",
                      errors="replace") as handle:
                body = handle.read()
        except OSError:
            continue
        if TIMING_VOCABULARY.search(body):
            vocabulary += 1
        for number, line in enumerate(body.split("\n"), start=1):
            names = {match.group(0).split(".")[0]
                     for match in TIMING_FIELD.finditer(line)}
            if len(names) >= 2:
                paired.append(f"{path}:{number}:{line}")
    # An empty population is the failure, not a pass: the field names drifting
    # would leave this section scanning nothing and reporting ok.
    if vocabulary == 0:
        print("no .medianSeconds/.p95Seconds/.meanSeconds anywhere in Tests — "
              "the Bench::measure")
        print("  field names drifted, so this section scanned an empty "
              "population. Re-point the")
        print("  grep and awk pattern at the new spelling.")
        run.fail()
    elif paired:
        print("two timings compared to each other — interleave the variants "
              "instead:")
        run.emit("\n".join(paired))
        run.fail()
    else:
        print("ok")


def test_reporting(run):
    run.gate("check-test-reporting.py", ["./Scripts/check-test-reporting.py"],
             ok="ok")


def swift_sources(run):
    run.gate("check-swift-sources.py", ["./Scripts/check-swift-sources.py"])


def python_syntax(run):
    paths = list(tracked(run, "*.py"))
    passed = 0
    for path in paths:
        status, output = run.shell(["python3", "-m", "py_compile", path])
        if status == 0:
            passed += 1
        else:
            print(f"FAIL: {path}")
            run.emit(output)
            run.fail()
    scanned(run, "python file(s) compiled", passed, paths, scoped=True)


def stale_landing(run):
    """No commit on the branch takes back recent work it does not name."""
    if not run.since:
        print("ok (no branch range under --full)")
        return
    run.gate("check-stale-landing.py",
             ["python3", "./Scripts/check-stale-landing.py", "--base",
              run.since, "--tip", "HEAD"], ok="last")



HAND = {
    "whitespace / conflict markers (git diff --check)": whitespace_and_conflict_markers,
    "clang-format drift (Scripts/format-code.sh --check)": clang_format_drift,
    "shell scripts begin with a shebang": shell_shebang,
    "shell syntax (bash -n)": shell_syntax,
    "continuation into a blank line": continuation_into_blank_line,
    "shell scripts, at severity warning (Scripts/check-shell.sh)": shellcheck_at_warning,
    "the verdict cache's key covers what it claims (Scripts/check-gate-verdict-cache.sh)": verdict_cache_key,
    "every content gate reaches a new file (Scripts/check-gate-subjects.sh)": gate_subjects,
    "prose that performs understanding instead of carrying it (Scripts/check-slop.py)": slop,
    "WebKit/Cocoa naming lint (Scripts/check-acronyms.py)": acronyms,
    "C-style casts in Core/Image (WebKit C++: use static_cast/reinterpret_cast)": cstyle_casts,
    "every snapshot profile names a face we ship and has a reader (Scripts/check-snapshot-faces.py)": snapshot_faces,
    "no Selector from a string literal in Swift (NSSelectorFromString or #selector)": swift_selector_from_string,
    "a string selector names a method something implements": selector_names_something,
    "no Objective-C source in the tree (.m or .mm)": no_objective_c,
    "no 'Cap' abbreviation in identifiers (max / Capacity / Limit / Capability)": cap_abbreviation,
    "no 'Cid' abbreviation in identifiers (spell out ConnectionID)": cid_abbreviation,
    "Tests/ house style (ratchet: ADDED lines only)": tests_house_style,
    "every setting Core reads is named in the config template": preferences_documented,
    "Source/ reaches for the C++23 vocabulary types": vocabulary_types,
    "a ternary that yields its own condition": ternary_yields_condition,
    "a Terminal* in Core says null is meaningful": terminal_pointer_intent,
    "paired bench comparison through Bench::measure": paired_bench,
    "every test reports through the harness (Scripts/check-test-reporting.py)": test_reporting,
    "every Swift file is compiled (Scripts/check-swift-sources.py)": swift_sources,
    "python syntax (python3 -m py_compile)": python_syntax,
    "a commit names the recent work it takes back (Scripts/check-stale-landing.py)": stale_landing,
}


# Every section, in the order the report is read and diffed in.
#
# A row runs a gate and reports what it said; a `hand` entry names a section
# with a rule of its own, dispatched through HAND above. Order is part of the
# answer: the report is diffed between runs, so a list that lost the
# interleaving would give the same verdicts in a different order and read as a
# change on every line.
SECTIONS = (
    {
        "title": 'the gates below can still fail (Scripts/run-gate-self-tests.py)',
        "in": EVERYWHERE,
        "kind": "gate",
        "gate": 'run-gate-self-tests.py',
        "command": ['python3', './Scripts/run-gate-self-tests.py'],
        "ok": 'all',
        "bad": 'all',
        "fail": None,
    },
    {
        "title": 'no member changes a layout under the test hook (Scripts/check-test-hook-layout.py)',
        "in": EVERYWHERE,
        "kind": "gate",
        "gate": 'check-test-hook-layout.py',
        "command": ['python3', './Scripts/check-test-hook-layout.py'],
        "ok": 'all',
        "bad": 'all',
        "fail": None,
    },
    {
        "title": 'a count of the tree, written in prose, still holds (Scripts/check-census-claims.py)',
        "in": EVERYWHERE,
        "kind": "gate",
        "gate": 'check-census-claims.py',
        "command": ['python3', './Scripts/check-census-claims.py'],
        "ok": 'all',
        "bad": 'all',
        "fail": None,
    },
    {
        "title": 'a scoped gate declares its inputs (Scripts/check-gate-scopes.py)',
        "in": EVERYWHERE,
        "kind": "gate",
        "gate": 'check-gate-scopes.py',
        "command": ['python3', './Scripts/check-gate-scopes.py'],
        "ok": 'all',
        "bad": 'all',
        "fail": None,
    },
    {
        # Diagnostic only: an existing index exits zero whatever its verdict,
        # so a stale one is printed here and blocks nothing.
        "title": 'the covering-tests index is current (Scripts/coverage_index_freshness.py)',
        "kind": "gate",
        "gate": 'coverage_index_freshness.py',
        "command": ['python3', './Scripts/coverage_index_freshness.py', '--root', '.',
                    '--index', 'Tests/coverage/covering-tests.tsv'],
        "ok": 'all',
        "bad": 'all',
        "fail": None,
    },
    {"title": 'whitespace / conflict markers (git diff --check)', "kind": "hand", "in": EVERYWHERE},
    {"title": 'a commit names the recent work it takes back (Scripts/check-stale-landing.py)', "kind": "hand", "in": EVERYWHERE},
    {"title": 'clang-format drift (Scripts/format-code.sh --check)', "kind": "hand", "in": EVERYWHERE},
    {"title": 'shell scripts begin with a shebang', "kind": "hand", "in": EVERYWHERE},
    {"title": 'shell syntax (bash -n)', "kind": "hand", "in": EVERYWHERE},
    {"title": 'continuation into a blank line', "kind": "hand", "in": EVERYWHERE},
    {
        "title": 'module boundaries (Scripts/check-boundaries.py)',
        "in": EVERYWHERE,
        "kind": "gate",
        "gate": 'check-boundaries.py',
        "command": ['./Scripts/check-boundaries.py'],
        "ok": 'ok',
        "bad": 'all',
        "fail": None,
    },
    {
        "title": 'the training profile reaches only a compiler that reads it (Scripts/check-pgo-profile-consumer.sh)',
        "kind": "gate",
        "gate": 'check-pgo-profile-consumer.sh',
        "command": ['./Scripts/check-pgo-profile-consumer.sh'],
        "ok": 'ok',
        "bad": 'all',
        "fail": None,
    },
    {
        "title": 'a finding is tracked on the board, not in a doc (Scripts/check-findings-on-board.py)',
        "in": EVERYWHERE,
        "kind": "gate",
        "gate": 'check-findings-on-board.py',
        "command": ['./Scripts/check-findings-on-board.py'],
        "ok": 'ok',
        "bad": 'all',
        "fail": None,
    },
    {
        "title": 'a diagnostic write cannot end the process (Scripts/check-log-writes.py)',
        "in": EVERYWHERE,
        "kind": "gate",
        "gate": 'check-log-writes.py',
        "command": ['./Scripts/check-log-writes.py'],
        "ok": 'ok',
        "bad": 'all',
        "fail": None,
    },
    {
        "title": 'Swift bridging reaches portable code through one door (Scripts/check-swift-interop.py)',
        "in": EVERYWHERE,
        "kind": "gate",
        "gate": 'check-swift-interop.py',
        "command": ['./Scripts/check-swift-interop.py'],
        "ok": 'ok',
        "bad": 'all',
        "fail": None,
    },
    {
        "title": 'a frontend call that waits on the parser is counted (Scripts/check-model-lock-holds.py)',
        "kind": "gate",
        "gate": 'check-model-lock-holds.py',
        "command": ['python3', './Scripts/check-model-lock-holds.py'],
        "ok": 'all',
        "bad": 'all',
        "fail": None,
    },
    {
        "title": 'the OSC payload moves one direction (Scripts/check-osc-payload-view.py)',
        "kind": "gate",
        "gate": 'check-osc-payload-view.py',
        "command": ['python3', './Scripts/check-osc-payload-view.py'],
        "ok": 'all',
        "bad": 'all',
        "fail": None,
    },
    {
        "title": 'the canonical Core source list matches its generator (Scripts/gen-portable-core-sources.sh)',
        "kind": "gate",
        "gate": 'gen-portable-core-sources.sh',
        "command": ['./Scripts/gen-portable-core-sources.sh', '--check'],
        "ok": 'all',
        "bad": 'all',
        "fail": None,
    },
    {
        "title": 'core layer partition (Scripts/check-core-layer-partition.sh)',
        "kind": "gate",
        "gate": 'check-core-layer-partition.sh',
        "command": ['./Scripts/check-core-layer-partition.sh'],
        "ok": 'ok',
        "bad": 'all',
        "fail": None,
    },
    {
        "title": 'core reaches the OS through PAL (Scripts/check-core-posix.py)',
        "kind": "gate",
        "gate": 'check-core-posix.py',
        "command": ['./Scripts/check-core-posix.py'],
        "ok": 'ok',
        "bad": 'all',
        "fail": None,
    },
    {
        "title": 'core namespaces match their folders (Scripts/check-core-namespaces.sh)',
        "kind": "gate",
        "gate": 'check-core-namespaces.sh',
        "command": ['./Scripts/check-core-namespaces.sh'],
        "ok": 'ok',
        "bad": 'all',
        "fail": None,
    },
    {
        "title": 'one declaration, one home (Scripts/check-duplicate-decls.py)',
        "in": EVERYWHERE,
        "kind": "gate",
        "gate": 'check-duplicate-decls.py',
        "command": ['python3', './Scripts/check-duplicate-decls.py', '.'],
        "ok": 'ok',
        "bad": 'all',
        "fail": None,
    },
    {
        "title": 'one case does not hide many (Scripts/check-aggregate-cases.py)',
        "in": EVERYWHERE,
        "kind": "gate",
        "gate": 'check-aggregate-cases.py',
        "command": ['python3', './Scripts/check-aggregate-cases.py', '.'],
        "ok": 'ok',
        "bad": 'all',
        "fail": None,
    },
    {
        "title": 'a reprint compares every pen field (Scripts/check-pen-idempotency-fields.py)',
        "kind": "gate",
        "gate": 'check-pen-idempotency-fields.py',
        "command": ['python3', './Scripts/check-pen-idempotency-fields.py', '.'],
        "ok": 'all',
        "bad": 'all',
        "fail": None,
    },
    {
        "title": 'a file that lost its contents (Scripts/check-file-content-loss.py)',
        "in": EVERYWHERE,
        "kind": "gate",
        "gate": 'check-file-content-loss.py',
        "command": ['python3', './Scripts/check-file-content-loss.py', '--diff', 'origin/main'],
        "ok": 'all',
        "bad": 'all',
        "fail": None,
    },
    {
        "title": 'an approved golden keeps its bytes (Scripts/check-approved-goldens.py)',
        "in": APP,
        "kind": "gate",
        "gate": 'check-approved-goldens.py',
        "command": ['python3', './Scripts/check-approved-goldens.py'],
        "ok": 'all',
        "bad": 'all',
        "fail": None,
    },
    {
        "title": 'a gate nobody calls (Scripts/check-gates-invoked.py)',
        "in": EVERYWHERE,
        "kind": "gate",
        "gate": 'check-gates-invoked.py',
        "command": ['python3', './Scripts/check-gates-invoked.py'],
        "ok": 'all',
        "bad": 'all',
        "fail": None,
    },
    {
        "title": 'every compile carries the launcher (Scripts/check-compiler-launcher.py)',
        "kind": "gate",
        "gate": 'check-compiler-launcher.py',
        "command": ['./Scripts/check-compiler-launcher.py'],
        "ok": 'all',
        "bad": 'all',
        "fail": None,
    },
    {
        "title": 'a mirrored Terminal has no parser (Scripts/check-mirror-modules-do-not-parse.py)',
        "in": EVERYWHERE,
        "kind": "gate",
        "gate": 'check-mirror-modules-do-not-parse.py',
        "command": ['python3', './Scripts/check-mirror-modules-do-not-parse.py'],
        "ok": 'all',
        "bad": 'all',
        "fail": None,
    },
    {
        "title": 'the wire catalog lists every frame (Scripts/check-sync-catalog-complete.py)',
        "kind": "gate",
        "gate": 'check-sync-catalog-complete.py',
        "command": ['python3', './Scripts/check-sync-catalog-complete.py'],
        "ok": 'all',
        "bad": 'all',
        "fail": None,
    },
    {
        "title": 'the shipped config reference lists the keys (Scripts/check-config-template-covers-keys.py)',
        "kind": "gate",
        "gate": 'check-config-template-covers-keys.py',
        "command": ['python3', './Scripts/check-config-template-covers-keys.py'],
        "ok": 'all',
        "bad": 'all',
        "fail": None,
    },
    {
        "title": 'the blink hint and its stamp move together (Scripts/check-blink-hint-chokepoint.py)',
        "kind": "gate",
        "gate": 'check-blink-hint-chokepoint.py',
        "command": ['python3', './Scripts/check-blink-hint-chokepoint.py'],
        "ok": 'all',
        "bad": 'all',
        "fail": None,
    },
    {
        "title": 'no header Swift imports changes with NDEBUG (Scripts/check-imported-headers-ndebug.py)',
        "in": EVERYWHERE,
        "kind": "gate",
        "gate": 'check-imported-headers-ndebug.py',
        "command": ['python3', './Scripts/check-imported-headers-ndebug.py'],
        "ok": 'all',
        "bad": 'all',
        "fail": None,
    },
    {
        "title": 'tools with their own runner still parse (Scripts/check-standalone-tools-compile.py)',
        "in": EVERYWHERE,
        "kind": "gate",
        "gate": 'check-standalone-tools-compile.py',
        "command": ['python3', './Scripts/check-standalone-tools-compile.py'],
        "ok": 'all',
        "bad": 'all',
        "fail": None,
    },
    {
        "title": "every drawing of the source tree (Scripts/check-folder-map.py)",
        "in": EVERYWHERE,
        "kind": "gate",
        "gate": 'check-folder-map.py',
        "command": ['python3', './Scripts/check-folder-map.py'],
        "ok": 'all',
        "bad": 'all',
        "fail": None,
    },
    {
        "title": "the ledger of other repositories' names lists only names used here (Scripts/check-other-repositories.py)",
        "in": EVERYWHERE,
        "kind": "gate",
        "gate": 'check-other-repositories.py',
        "command": ['python3', './Scripts/check-other-repositories.py'],
        "ok": 'all',
        "bad": 'all',
        "fail": None,
    },
    {
        "title": 'what quotes or names a wire number tracks the headers (Scripts/check-wire-version-pins.py)',
        "kind": "gate",
        "gate": 'check-wire-version-pins.py',
        "command": ['python3', './Scripts/check-wire-version-pins.py'],
        "ok": 'all',
        "bad": 'all',
        "fail": None,
    },
    {
        "title": 'doc references resolve (Scripts/check-doc-references.py)',
        "in": EVERYWHERE,
        "kind": "gate",
        "gate": 'check-doc-references.py',
        "command": ['python3', './Scripts/check-doc-references.py'],
        "ok": 'all',
        "bad": 'all',
        "fail": None,
    },
    {
        "title": 'the hooks this clone will actually run (Scripts/check-hooks-installed.py)',
        "in": EVERYWHERE,
        "kind": "gate",
        "gate": 'check-hooks-installed.py',
        "command": ['python3', './Scripts/check-hooks-installed.py'],
        "ok": 'all',
        "bad": 'all',
        "fail": None,
    },
    {
        "title": 'a hand-copied struct still matches (Scripts/check-bench-mirrors.py)',
        "kind": "gate",
        "gate": 'check-bench-mirrors.py',
        "command": ['python3', './Scripts/check-bench-mirrors.py'],
        "ok": 'all',
        "bad": 'all',
        "fail": None,
    },
    {
        # Here as well as in the push gate's ctest stanza: lanes push with
        # --no-verify, and a trim that strands a kept file's callee reaches a
        # maintainer's link before any gate has read it. Without a host test
        # build it reports SKIPPED, naming the build that supplies its objects.
        "title": 'a kept Core file needs a trimmed one (Scripts/check-core-subset-symbols.py)',
        "kind": "gate",
        "gate": 'check-core-subset-symbols.py',
        "command": ['python3', './Scripts/check-core-subset-symbols.py', '--if-built'],
        "ok": 'all',
        "bad": 'all',
        "fail": None,
    },
    {
        "title": 'a test comment with no test under it (Scripts/check-stanza-comments.py)',
        "in": EVERYWHERE,
        "kind": "gate",
        "gate": 'check-stanza-comments.py',
        "command": ['python3', './Scripts/check-stanza-comments.py'],
        "ok": 'all',
        "bad": 'all',
        "fail": None,
    },
    {
        "title": 'a comment naming a file that is not there (Scripts/check-comment-file-references.py)',
        "in": EVERYWHERE,
        "kind": "gate",
        "gate": 'check-comment-file-references.py',
        "command": ['python3', './Scripts/check-comment-file-references.py'],
        "ok": 'all',
        "bad": 'all',
        "fail": None,
    },
    {
        "title": 'a file under Source/ no suffix glob can see (Scripts/check-source-file-names.py)',
        "in": EVERYWHERE,
        "kind": "gate",
        "gate": 'check-source-file-names.py',
        "command": ['python3', './Scripts/check-source-file-names.py'],
        "ok": 'all',
        "bad": 'all',
        "fail": None,
    },
    {
        "title": 'a Unicode property has one source (Scripts/check-graphemebreak-generated.py)',
        "kind": "gate",
        "gate": 'check-graphemebreak-generated.py',
        "command": ['python3', './Scripts/check-graphemebreak-generated.py'],
        "ok": 'all',
        "bad": 'all',
        "fail": None,
    },
    {
        "title": 'the Welcome icon Core embeds is the committed PNGs (Scripts/export-welcome-icon.py --check)',
        "kind": "gate",
        "gate": 'export-welcome-icon.py',
        "command": ['python3', './Scripts/export-welcome-icon.py', '--check'],
        "ok": 'all',
        "bad": 'all',
        "fail": None,
    },
    {
        "title": 'Swift gains no new raw pointer (Scripts/check-swift-unsafe.py)',
        "in": SWIFT,
        "kind": "gate",
        "gate": 'check-swift-unsafe.py',
        "command": ['python3', './Scripts/check-swift-unsafe.py'],
        "ok": 'all',
        "bad": 'all',
        "fail": None,
    },
    {
        "title": 'the capability escape hatch is counted (Scripts/check-effect-escape-hatch.py)',
        "kind": "gate",
        "gate": 'check-effect-escape-hatch.py',
        "command": ['python3', './Scripts/check-effect-escape-hatch.py'],
        "ok": 'all',
        "bad": 'all',
        "fail": None,
    },
    {"title": 'shell scripts, at severity warning (Scripts/check-shell.sh)', "kind": "hand", "in": EVERYWHERE},
    {"title": "the verdict cache's key covers what it claims (Scripts/check-gate-verdict-cache.sh)", "kind": "hand"},
    {"title": 'every content gate reaches a new file (Scripts/check-gate-subjects.sh)', "kind": "hand"},
    {
        "title": "the tree's clang-tidy rules, on what the branch wrote (Scripts/check-tidy.sh)",
        "kind": "deferred",
        "gate": 'check-tidy.sh',
        "to": '"macOS snapshot regressions"',
        "why": 'its subject is a compile database, and that job builds the one '
               'carrying the most translation units — every .mm among them',
        "command": ['./Scripts/check-tidy.sh', '--since', 'origin/main'],
        "ok": 'all',
        "bad": 'all',
        "fail": None,
    },
    {"title": 'prose that performs understanding instead of carrying it (Scripts/check-slop.py)', "kind": "hand", "in": EVERYWHERE},
    {
        "title": "a stated length is its literal's (Scripts/check-literal-lengths.py)",
        "in": EVERYWHERE,
        "kind": "gate",
        "gate": 'check-literal-lengths.py',
        "command": ['python3', './Scripts/check-literal-lengths.py'],
        "ok": 'ok',
        "bad": 'all',
        "fail": None,
    },
    {
        "title": 'a destructive git command gets its tree (Scripts/check-git-isolation.py)',
        "in": EVERYWHERE,
        "kind": "gate",
        "gate": 'check-git-isolation.py',
        "command": ['python3', './Scripts/check-git-isolation.py', '.'],
        "ok": 'ok',
        "bad": 'all',
        "fail": None,
    },
    {
        "title": 'a path that asserts nothing (Scripts/check-conditional-assertions.py)',
        "in": EVERYWHERE,
        "kind": "gate",
        "gate": 'check-conditional-assertions.py',
        "command": ['python3', './Scripts/check-conditional-assertions.py', '.', '--gate'],
        "ok": 'ok',
        "bad": 'all',
        "fail": None,
    },
    {
        "title": 'the acronym scanners agree (Scripts/check-acronym-rules.py)',
        "kind": "gate",
        "gate": 'check-acronym-rules.py',
        "command": ['python3', './Scripts/check-acronym-rules.py'],
        "ok": 'last',
        "bad": 'all',
        "fail": None,
    },
    {"title": 'WebKit/Cocoa naming lint (Scripts/check-acronyms.py)', "kind": "hand", "in": EVERYWHERE},
    {"title": 'no Selector from a string literal in Swift (NSSelectorFromString or #selector)',
     "kind": "hand", "in": SWIFT},
    {"title": 'a string selector names a method something implements', "kind": "hand", "in": APP},
    {"title": 'no Objective-C source in the tree (.m or .mm)', "kind": "hand", "in": EVERYWHERE},
    {"title": 'every snapshot profile names a face we ship and has a reader (Scripts/check-snapshot-faces.py)',
     "kind": "hand"},
    {
        "title": 'comment hygiene (Scripts/check-comments.sh)',
        "in": EVERYWHERE,
        "kind": "gate",
        "gate": 'check-comments.sh',
        "command": ['./Scripts/check-comments.sh', '--since', 'origin/main'],
        "ok": 'ok',
        "bad": 'all',
        "fail": None,
    },
    {
        "title": 'personal-path / private-repo hygiene (Scripts/check-personal-paths.py)',
        "in": EVERYWHERE,
        "kind": "gate",
        "gate": 'check-personal-paths.py',
        "command": ['./Scripts/check-personal-paths.py'],
        "ok": 'ok',
        "bad": 'all',
        "fail": None,
    },
    {
        "title": 'project license scope + third-party inventory and notices (Scripts/check-licenses.py)',
        "in": EVERYWHERE,
        "kind": "gate",
        "gate": 'check-licenses.py',
        "command": ['./Scripts/check-licenses.py'],
        "ok": 'all',
        "bad": 'all',
        "fail": None,
    },
    {
        "title": 'doc hygiene — TODO / ticket refs in .md / .toml (Scripts/check-docs.py)',
        "in": EVERYWHERE,
        "kind": "gate",
        "gate": 'check-docs.py',
        "command": ['./Scripts/check-docs.py'],
        "ok": 'ok',
        "bad": 'all',
        "fail": None,
    },
    {"title": 'C-style casts in Core/Image (WebKit C++: use static_cast/reinterpret_cast)', "kind": "hand"},
    {"title": "no 'Cap' abbreviation in identifiers (max / Capacity / Limit / Capability)", "kind": "hand", "in": EVERYWHERE},
    {"title": "no 'Cid' abbreviation in identifiers (spell out ConnectionID)", "kind": "hand", "in": EVERYWHERE},
    {"title": 'Tests/ house style (ratchet: ADDED lines only)', "kind": "hand", "in": EVERYWHERE},
    {
        "title": 'a presence guard does not suppress an accessor default',
        "in": EVERYWHERE,
        "kind": "gate",
        "gate": 'check-preference-guard-suppresses-default.py',
        "command": ['./Scripts/check-preference-guard-suppresses-default.py'],
        "ok": 'last',
        "bad": 'all',
        "fail": 'a presence guard does not suppress an accessor default (Scripts/check-preference-guard-suppresses-default.py)',
    },
    {
        "title": 'an optional<bool> is read for its value',
        "in": EVERYWHERE,
        "kind": "gate",
        "gate": 'check-optional-bool-reads.py',
        "command": ['./Scripts/check-optional-bool-reads.py'],
        "ok": 'last',
        "bad": 'all',
        "fail": 'an optional<bool> is read for its value (Scripts/check-optional-bool-reads.py)',
    },
    {
        "title": 'a frontend does not spell a default Core owns',
        "in": EVERYWHERE,
        "kind": "gate",
        "gate": 'check-frontend-default-literals.py',
        "command": ['./Scripts/check-frontend-default-literals.py'],
        "ok": 'last',
        "bad": 'all',
        "fail": 'a frontend restates a default Core owns (Scripts/check-frontend-default-literals.py)',
    },
    {
        "title": 'interface text follows the writing rules (Scripts/check-copy.py)',
        "in": EVERYWHERE,
        "kind": "gate",
        "gate": 'check-copy.py',
        "command": ['python3', './Scripts/check-copy.py'],
        "ok": 'last',
        "bad": 'all',
        "fail": None,
    },
    {
        "title": 'one vocabulary for the preferences store',
        "in": EVERYWHERE,
        "kind": "gate",
        "gate": 'check-preferences-vocabulary.py',
        "command": ['./Scripts/check-preferences-vocabulary.py'],
        "ok": 'last',
        "bad": 'all',
        "fail": 'one vocabulary for the preferences store (Scripts/check-preferences-vocabulary.py)',
    },
    {
        "title": 'every message the daemon dispatches is accounted for',
        "kind": "gate",
        "gate": 'check-message-gates.py',
        "command": ['./Scripts/check-message-gates.py'],
        "ok": 'last',
        "bad": 'all',
        "fail": 'a message type the daemon dispatches states no authorization (Scripts/check-message-gates.py)',
    },
    {"title": 'every setting Core reads is named in the config template', "kind": "hand"},
    {"title": 'Source/ reaches for the C++23 vocabulary types', "kind": "hand", "in": EVERYWHERE},
    {"title": 'a ternary that yields its own condition', "kind": "hand", "in": EVERYWHERE},
    {"title": 'a Terminal* in Core says null is meaningful', "kind": "hand"},
    {
        "title": 'every bench times through the shared harness',
        "kind": "gate",
        "gate": 'check-bench-harness-reach.py',
        "command": ['./Scripts/check-bench-harness-reach.py'],
        "ok": 'all',
        "bad": 'all',
        "fail": 'every bench times through the shared harness (Scripts/check-bench-harness-reach.py)',
    },
    {"title": 'paired bench comparison through Bench::measure', "kind": "hand"},
    {
        "title": 'interface documentation (Scripts/check-interface-docs.py)',
        "in": EVERYWHERE,
        "kind": "gate",
        "gate": 'check-interface-docs.py',
        "command": ['python3', './Scripts/check-interface-docs.py', '--since', 'origin/main'],
        "ok": 'ok',
        "bad": 'all',
        "fail": None,
    },
    {"title": 'every test reports through the harness (Scripts/check-test-reporting.py)', "kind": "hand", "in": EVERYWHERE},
    {"title": 'every Swift file is compiled (Scripts/check-swift-sources.py)', "kind": "hand", "in": SWIFT},
    {
        "title": 'enum/bool-naming lint (Scripts/check-enums.sh)',
        "in": EVERYWHERE,
        "kind": "gate",
        "gate": 'check-enums.sh',
        "command": ['./Scripts/check-enums.sh', '--since', 'origin/main'],
        "ok": 'ok',
        "bad": 'all',
        "fail": None,
    },
    {"title": 'python syntax (python3 -m py_compile)', "kind": "hand", "in": EVERYWHERE},
)


def loadTables(subject):
    """Every section table the subject runs, in the order they are read.

    ubiquitty-core's table, the one in this file, comes first, then the others by
    name. A table a checkout does not carry is not loaded; one it carries for a
    repository the checkout is not is not run.

    - Parameters:
      - subject: the root of the tree being judged.
    - Returns: a tuple of (SECTIONS, HAND, REPOSITORY) per table module.
    """
    here = os.path.dirname(os.path.abspath(__file__))
    found = {}
    for directory in (here, os.path.join(subject, "Scripts")):
        for path in sorted(glob.glob(os.path.join(directory, "style_sections_*.py"))):
            earlier = found.setdefault(os.path.basename(path), path)
            # One table per repository: the same table in the library and in the
            # repository would run the sections of the one that loses twice or not at all.
            if os.path.realpath(earlier) != os.path.realpath(path):
                raise ValueError(f"two checkouts carry {os.path.basename(path)}: {earlier} and {path}")
    tables = [(tuple(SECTIONS), dict(HAND), REPOSITORY)]
    for name in sorted(found):
        spec = importlib.util.spec_from_file_location(name[:-3], found[name])
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        tables.append((tuple(module.SECTIONS), dict(module.HAND), module.REPOSITORY))
    return tuple(tables)


def mergedSections(tables):
    """Every table's rows in the one order the report is read and diffed in.

    ubiquitty-core's rows keep their order. A row of another table names the row
    it follows in `after`, so the report reads in the order the sections had when
    one file held them, whichever repositories a checkout holds. A row naming no
    predecessor goes last, in table order.

    - Parameters:
      - tables: what `loadTables` returned.
    - Returns: a list of (row, owning repository).
    - Raises: ValueError when an `after` names no section, or two sections share a title.
    """
    merged = [(row, tables[0][2]) for row in tables[0][0]]
    pending = [(row, owner) for sections, _hand, owner in tables[1:] for row in sections]
    titles = [row["title"] for row, _owner in merged]
    if len(set(titles)) != len(titles):
        raise ValueError("two sections of one table share a title")
    while pending:
        remaining = []
        for row, owner in pending:
            follows = row.get("after")
            placed = [section["title"] for section, _owner in merged]
            if follows is None:
                merged.append((row, owner))
            elif follows in placed:
                merged.insert(placed.index(follows) + 1, (row, owner))
            else:
                remaining.append((row, owner))
        if len(remaining) == len(pending):
            raise ValueError("a section follows one that no table holds: %s"
                             % ", ".join(repr(row["after"]) for row, _owner in remaining))
        pending = remaining
    return merged


def sectionsOf(tables, repositories):
    """The rows these repositories run, the handlers they dispatch, and the rows left out.

    - Parameters:
      - tables: what `loadTables` returned.
      - repositories: the repositories whose files the subject holds.
    - Returns: (rows, handlers, others), where `others` lists the titles of the rows
      that belong to a repository the subject does not hold.
    """
    handlers = {}
    for _sections, hand, _owner in tables:
        for title in hand:
            if title in handlers:
                raise ValueError(f"two section tables handle {title!r}")
        handlers.update(hand)
    rows = []
    others = []
    for row, owner in mergedSections(tables):
        if set(row.get("in", (owner,))) & set(repositories):
            rows.append(row)
        else:
            others.append(row["title"])
    return tuple(rows), handlers, others


def run_hand_section(title, run, handlers):
    """Runs the hand-written section named `title`, or refuses.

    A section with no handler is a gate that stopped running, and that reads
    exactly like a gate that found nothing — so the absence is a refusal rather
    than a skip. Separated from the loop because the claim below asserts this
    behaviour, and asserting it against `HAND.get` instead only asserted that a
    dict returns None for a key it does not hold.

    - Parameters:
      - title: The section's title, as the manifest spells it.
      - run: The run state handed to the handler.
      - handlers: The handlers of every loaded table, by section title.
    - Returns: 0 when the handler ran, 2 when no handler is registered.
    """
    handler = handlers.get(title)
    if handler is None:
        print(f"{GATE}: FAILED — no handler for section {title!r}", file=sys.stderr)
        return 2
    handler(run)
    return 0



def _self_test():
    """No section of this runner can go missing, and none can name nothing.

    - Returns: 0 when the claims hold, 1 otherwise.
    """
    failures = []

    def check(claim, held):
        print(f"  {'ok      ' if held else 'BROKEN  '} {claim}")
        if not held:
            failures.append(claim)

    subject = ubq_roots.subject_root()
    tables = loadTables(subject)
    manifest, handlers, others = sectionsOf(tables, ubq_roots.REPOSITORIES)

    check("the table is not empty", bool(manifest))
    titles = [entry["title"] for entry in manifest]
    # A title is how a section is found in the report and how a handler is
    # dispatched, so two sections under one title hide one of them.
    check("no two sections share a title", len(set(titles)) == len(titles))

    hand = [entry["title"] for entry in manifest if entry["kind"] == "hand"]
    check("every hand-written section has a handler",
          all(title in handlers for title in hand))
    check("every handler has a section", all(title in hand for title in handlers))

    # A section with no handler must stop the run. Skipping it would print
    # nothing, which is what a section that passed prints. Driven through the
    # runner's own path: asked of the dict this claim held for any dict that
    # does not contain the made-up title, which is every dict, so it could not
    # fail however the runner behaved. `run` is never reached — a handler that
    # ran would be the failure — so None stands in for it.
    refused = run_hand_section("zz section that does not exist", None, handlers) == 2
    check("a section with no handler is refused, not skipped", refused)

    # A table row carries a command the runner can execute, with no shell left
    # in it: a continuation backslash taken as an argument made the runner try
    # to execute a file named for it.
    # Deferred rows keep their command and their script: the row is what says
    # where the gate went, and a deferral to a job that no longer runs it is
    # the failure this table exists to make visible.
    rows = [entry for entry in manifest if entry["kind"] in ("gate", "deferred")]
    check("every table row has a command", all(row["command"] for row in rows))
    check("no table row carries a shell continuation",
          not any("\\" in argument for row in rows for argument in row["command"]))
    check("every table row reports in a shape the runner knows",
          all(row["ok"] in ("all", "ok", "last", "last3")
              and row["bad"] in ("all", "ok", "last", "last3")
              for row in rows))

    # A row naming a script that is not there is a gate that stopped running,
    # and a run of the remaining gates reports the same `ok` as a clean tree.
    # Every table is loaded, so a monorepo checkout accounts for all of them.
    roots = (subject, ubq_roots.core_root(subject), ubq_roots.kit_root(subject))

    def commandExists(command):
        for argument in command:
            if argument.endswith((".py", ".sh")):
                relative = argument.lstrip("./")
                return any(os.path.exists(os.path.join(root, relative)) for root in roots)
        return True

    dangling = [row["gate"] for row in rows if not commandExists(row["command"])]
    check(f"every table row names a script that is there{'' if not dangling else f' ({dangling})'}",
          not dangling)

    if failures:
        print(f"{GATE} --self-test: {len(failures)} claim(s) the runner does "
              f"not keep", file=sys.stderr)
        return 1
    print(f"{GATE} --self-test: ok ({len(manifest)} section(s), "
          f"{len(rows)} table-driven, {len(hand)} hand-written, each accounted "
          f"for)")
    return 0


def main(argv):
    since = "origin/main"
    arguments = list(argv)
    while arguments:
        if arguments[0] in ("--help", "-h"):
            print(__doc__.strip())
            return 0
        if arguments[0] in ("--self-test", "--selftest"):
            return _self_test()
        if arguments[0] == "--full":
            since, arguments = "", arguments[1:]
        elif arguments[0] == "--since":
            if len(arguments) < 2:
                print(f"{GATE}: --since needs a git ref", file=sys.stderr)
                return 2
            since, arguments = arguments[1], arguments[2:]
        else:
            print(f"{GATE}: unknown argument: {arguments[0]}", file=sys.stderr)
            return 2

    root = ubq_roots.subject_root()
    os.chdir(root)

    # A ref absent from this checkout cannot scope anything, and a shallow clone
    # or a fresh worktree may have no origin/main. Fall back to the whole tree
    # and say so, rather than narrowing against nothing and reporting a pass
    # over no files.
    if since and subprocess.run(["git", "rev-parse", "--verify", "--quiet", since],
                                capture_output=True, check=False).returncode != 0:
        print(f"{GATE}: {since} is not in this checkout — scanning the whole tree")
        since = ""

    run = Run(since)
    sections, handlers, others = sectionsOf(loadTables(root), run.repositories)
    if others:
        print(f"{GATE}: {len(others)} section(s) belong to a repository this checkout "
              f"does not hold ({', '.join(run.repositories)} here):")
        for title in others:
            print(f"  - {title}")
    for entry in sections:
        began = time.monotonic()
        section_status = 0
        failed_before = len(run.failed)
        outputs_before = len(run.outputs)
        reused_before = run.cache.reused
        run.section(entry["title"])
        if entry["kind"] == "gate":
            run.gate(entry["gate"], entry["command"], entry["ok"], entry["bad"],
                     entry["fail"] or None)
        elif entry["kind"] == "deferred":
            run.emit(f"{entry['gate']}: DEFERRED to {entry['to']} — "
                     f"{entry['why']}.")
        else:
            section_status = run_hand_section(entry["title"], run, handlers)
        cached = run.cache.reused > reused_before
        state = "failed" if section_status != 0 or len(run.failed) > failed_before else (
            "deferred" if entry["kind"] == "deferred" else "passed")
        command = shlex.join(entry.get("command", [])) or "./Scripts/check-style.py"
        elapsed = None if cached else time.monotonic() - began
        new_outputs = run.outputs[outputs_before:]
        reason = run.reporter.failure_reason("\n".join(new_outputs)) if state == "failed" else ""
        run.reporter.record(
            "gate", label=entry.get("gate", entry["title"]), state=state,
            elapsed_seconds=elapsed,
            command=command, exitStatus=1 if state == "failed" else 0,
            cached=cached, section=entry["title"], reason=reason)
        for output in new_outputs:
            run.reporter.record_output_findings(
                entry.get("gate", entry["title"]), output,
                command=command, status=1 if state == "failed" else 0,
                elapsed_seconds=elapsed)
        if section_status != 0:
            return section_status

    print()
    if run.failed:
        gate_report.failed(GATE, "check(s)", run.failed)
        return 1

    lines = [line for output in run.outputs for line in output.split("\n")]
    stoodDownLines = [line for line in lines if STOOD_DOWN.match(line)]
    deferredLines = [line for line in lines if DEFERRED.match(line)]
    if deferredLines:
        print(f"{GATE}: PASSED — with {len(deferredLines)} gate(s) deferred to "
              f"the job that builds their subject:", file=sys.stderr)
        for line in deferredLines:
            print(f"  {line}", file=sys.stderr)
        print("  A green here does not cover them; their verdict is in the "
              "job each names.", file=sys.stderr)
    if stoodDownLines:
        print(f"{GATE}: PASSED — with {len(stoodDownLines)} gate(s) stood down "
              f"for a missing tool:", file=sys.stderr)
        for line in stoodDownLines:
            print(f"  {line}", file=sys.stderr)
        print("  A green here does not cover them. Install what they name, or "
              "accept the gap knowingly.", file=sys.stderr)
    if deferredLines or stoodDownLines:
        return 0
    print(f"{GATE}: PASSED (every gate ran)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
