#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""check-boundaries.py — the module boundaries the tree is built on.

Each rule names a direction code must not flow: Core must not reach
for a frontend, a portable module must not import an Apple framework or touch
raw POSIX, a platform seam must not name a Core model type, a frontend must not
redeclare a Core enum.

  Scripts/check-boundaries.py
  Scripts/check-boundaries.py --self-test

Exit 0 = every boundary holds, 1 = at least one does not.

Every root a rule searches is checked to exist before any rule runs. A grep over
a directory that moved produces no output, which is exactly what no violation
produces — so a renamed module turns its rule off and the section still prints
ok, passing without reading a file.

The corpus is read once and every rule asked of it, rather than a recursive grep
per rule over the same trees.

Findings name a repository-relative path. A grep of "$ROOT/Source/..." prints
the checkout's own location on every line, which is not something a
reader or an editor can use; this is the same difference already taken in
check-compiler-launcher, and it is the shape gate_report.finding emits.
"""

import os
import posixpath
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import gate_corpus
import gate_report
import ubq_roots

GATE = "check-boundaries.py"

# Every directory a rule searches, checked before any of them is searched.
REQUIRED_ROOTS = (
    "Source/Core/App", "Source/Core", "Source/Core/Diagnostics", "Source/Core/Network",
    "Source/PAL", "Source/Platform", "Source/Platform/Host",
    "Source/Platform/macOS", "Source/Core/Protocol", "Source/Core/Rendering",
    "Samples",
)

APPLE_FRAMEWORKS = ("AppKit|UIKit|Cocoa|Metal|MetalKit|QuartzCore|CoreGraphics"
                    "|CoreText|CoreVideo|Foundation|ImageIO")

FRAMEWORK_BACKENDS = tuple(f"Source/Platform/{backend}/" for backend in
                          ("Apple", "macOS", "Host", "Linux", "Emacs"))

PORTABLE_MODULES = (
    "Core", "Codec", "Runtime", "Protocol", "Image", "Themes", "Selection",
    "Preferences", "Input", "Terminfo", "Unicode", "Session", "Transport",
    "Network", "Agent", "Diagnostics", "Rendering", "UbiquittyKit",
)

# The files whose whole job is to be the POSIX seam.
POSIX_SEAM = re.compile(
    r"Source/Platform/POSIX/(SystemPAL|PTYStream|SerialStream)\.cpp"
    r"|Source/Core/Transport/QUICClient\.cpp"
    r"|Source/Core/Protocol/Sync/QUICBootstrap\.cpp"
    r"|Source/Agent/(AgentSession|AgentMCP|RemoteCarrier)\.cpp")

# The one model header that reads and writes through the wire's varint
# grammar: the row form is the scrollback's storage codec and the Sync wire's
# row grammar at once, and the reader and writer it uses carry no behaviour.
# Named as a pair so every other include of the protocol from the model layer
# stays a failure, and so moving the varints under Core/Codec retires this
# line rather than widening it.
MODEL_WIRE_GRAMMAR = {("Source/Core/Screen/RowForm.h", "Core/Protocol/Sync/Wire.h")}

MIX_IMPLEMENTATION = "Source/Core/Rendering/SoftwareRenderer.cpp"
PAL_SOCKET_IMPLEMENTATION = "Source/Platform/POSIX/Socket.cpp"
PAL_WRAPPED_SOCKET_OPTIONS = "SO_RCVTIMEO|SO_SNDTIMEO"
LINUX_MAIN = "Source/Platform/Linux/main.cpp"


def sibling(script):
    """The path of a gate that sits beside this one, whichever checkout holds this file."""
    return os.path.join(os.path.dirname(os.path.abspath(__file__)), os.path.basename(script))


def treeRoots(subject=None):
    """The checkouts whose directories name the modules a rule is about.

    The rules read files of the repository run from. The modules they are
    written against may live in a library it carries, so a directory counts when
    any of these holds it.
    """
    subject = subject or gate_corpus.root()
    return list(dict.fromkeys((subject, ubq_roots.kit_root(subject), ubq_roots.core_root(subject))))


def isDirectory(relative):
    """Whether the repository or a library it carries holds the directory."""
    return any(os.path.isdir(os.path.join(root, relative)) for root in treeRoots())


class Run:
    """One pass over the boundaries, and what it read to decide them."""

    def __init__(self):
        self.failures = 0
        self.rules = 0
        self.pending = ""
        self.root = gate_corpus.root()
        self._bodies = {}

    def section(self, title):
        """Names the rule about to run, for a verdict that has something to say.

        A rule that holds is a banner and the word `ok`, twice per rule and
        two-thirds of a clean run's output — read, almost always, by an agent
        paying for every line and acting only on what failed. The title is
        held here and printed by `verdict` when the rule has hits, so a
        failure keeps its heading immediately above it and a clean run keeps
        the count.
        """
        self.pending = title
        self.rules += 1

    def bodies(self, prefixes):
        """Every tracked and untracked file under prefixes, read once each."""
        if not self._bodies:
            paths = [path for path in gate_corpus.tracked_and_untracked()
                     if path.startswith(("Source/", "Samples/", "Tests/",
                                         "Tools/"))]
            self._bodies = dict(gate_corpus.texts(sorted(paths)))
        return [(path, text) for path, text in self._bodies.items()
                if path.startswith(prefixes)]

    def hits(self, prefixes, pattern, exclude=None, exclude_path=None):
        """Matching lines under prefixes, as grep -rEn writes them."""
        compiled = re.compile(pattern)
        dropped = re.compile(exclude) if exclude else None
        droppedPath = re.compile(exclude_path) if exclude_path else None
        found = []
        for path, text in self.bodies(prefixes):
            if droppedPath and droppedPath.search(path):
                continue
            for number, line in enumerate(text.split("\n"), start=1):
                if compiled.search(line) and not (dropped and dropped.search(line)):
                    found.append(f"{path}:{number}:{line}")
        return found

    def verdict(self, hits, heading, trailer=None, ok="ok"):
        """The shape every rule reports in.

        A rule with hits prints its section title, then the heading, then what
        it found. A rule that holds prints nothing and is counted.
        """
        if not hits:
            return
        print()
        print(f"=== {self.pending} ===")
        print(heading)
        for hit in hits:
            print(f"  {hit}")
        if trailer:
            for line in trailer:
                print(line)
        self.failures += 1


def alternation(run, parent):
    """The subdirectory names under parent, as a regex alternation.

    Read off the tree rather than written down: a list of names has to be
    extended by whoever adds the next module, and nothing tells them — the rule
    keeps passing and the module it does not know about is the one nobody is
    checking.
    """
    return "|".join(subdirectories(parent))


def subdirectories(parent):
    """The subdirectory names under parent in the repository and the libraries it carries."""
    names = set()
    for root in treeRoots():
        full = os.path.join(root, parent)
        if os.path.isdir(full):
            names.update(name for name in os.listdir(full) if os.path.isdir(os.path.join(full, name)))
    return sorted(names)


def _core_upper_layer_imports(run, siblings):
    """Finds Core includes of modules above its public dependencies."""
    return run.hits(("Source/Core/",),
                    rf'#include[ \t]*"({"|".join(siblings)})/')


def _foundation_backend_imports(run):
    """Finds Base and PAL imports of platform implementations."""
    directive = re.compile(r'^[ \t]*#[ \t]*(?:include|import)[ \t]*[<"]([^>"]+)[>"]')
    found = []
    for path, text in run.bodies(("Source/Base/", "Source/PAL/")):
        for number, line in enumerate(text.split("\n"), start=1):
            match = directive.match(line)
            if not match:
                continue
            header = match.group(1)
            # Both module-prefixed and relative includes reach the same backend.
            targets = (posixpath.normpath(posixpath.join("Source", header)),
                       posixpath.normpath(posixpath.join(posixpath.dirname(path), header)))
            if any(target.startswith("Source/Platform/") for target in targets):
                found.append(f"{path}:{number}:{line}")
    return found


def _pal_framework_imports(run):
    """Finds native SDK imports in PAL and shared platform contract headers."""
    return [hit for hit in run.hits(
        ("Source/PAL/", "Source/Platform/"),
        rf'#[ \t]*(include|import)[ \t]*[<"]({APPLE_FRAMEWORKS}'
        rf'|CoreFoundation|dispatch)[/.>"]')
        if hit.split(":", 1)[0].endswith(".h")
        and not hit.split(":", 1)[0].startswith(FRAMEWORK_BACKENDS)]


def _sample_internal_imports(run):
    """Keeps samples on the umbrellas and the public socket-adoption contract."""
    modules = alternation(run, "Source")
    directive = r'^[ \t]*#[ \t]*(?:include|import)[ \t]*'
    return run.hits(
        ("Samples/",), directive + rf'(?:"[A-Za-z][A-Za-z0-9]*/|<(?:{modules})/)',
        exclude=directive + r'[<"](?:UbiquittyCore/(?:UbiquittyCore|Version)'
        r'|UbiquittyKit/(?:UbiquittyKit|Version)|PAL/Socket|Forward)\.h[>"]')


def main(argv):
    if argv and argv[0] in ("--help", "-h"):
        print(__doc__.strip())
        return 0
    os.chdir(gate_corpus.root())
    if argv and argv[0] in ("--self-test", "--selftest"):
        return _self_test()

    run = Run()

    missing = [root for root in REQUIRED_ROOTS if not isDirectory(root)]
    for root in missing:
        print(f"check-boundaries: no such directory: {root}", file=sys.stderr)
    if missing:
        print("check-boundaries: a rule whose directory is gone reports ok "
              "forever.", file=sys.stderr)
        print("  Point it at where the code moved, or delete the rule with the "
              "module.", file=sys.stderr)
        return 1

    run.section("Source/Core/ must not include frontend/sibling modules or a "
                "PAL backend")
    coreModules = alternation(run, "Source/Core")
    platformBackends = alternation(run, "Source/Platform")
    if not coreModules or not platformBackends:
        print("check-boundaries: no modules or backends found; the rule would "
              "pass on nothing", file=sys.stderr)
        return 1
    run.verdict(
        run.hits(("Source/Core/",),
                 rf'#include[ \t]*"(({coreModules})/|Platform/({platformBackends})/)',
                 exclude=r'"Core/'),
        "FAIL: Source/Core/ includes UI / app-layer modules:")

    run.section("Source/Core/ must not include a module that sits above it")
    # The rule above catches a Core file naming another Core directory without
    # its Core/ prefix. It cannot catch a module that sits above Core, because
    # its alternation is read from the subdirectories of Source/Core, and a
    # top-level module is not one of them. Source/App was such a module for as
    # long as it existed: Core/Session/RestorePlan.h included App/AppRuntime.h
    # for a two-bool classification, an L2 file reaching upward, and this gate
    # passed on it every run.
    #
    # Derived from the tree rather than written down, for the reason
    # `alternation` gives: a list has to be extended by whoever adds the next
    # module beside Core, and nothing tells them. Core reaches its own package
    # and the layers below it — Base and the PAL contract — and vendored
    # third-party, and nothing else.
    lowerLayers = {"Core", "Base", "PAL", "ThirdParty"}
    siblings = [name for name in subdirectories("Source") if name not in lowerLayers]
    if not siblings:
        print("check-boundaries: no module above Source/Core found; the rule "
              "would pass on nothing", file=sys.stderr)
        return 1
    run.verdict(
        _core_upper_layer_imports(run, siblings),
        "FAIL: Source/Core/ includes a module above it. Hoist the dependency "
        "to the caller, or move the shared value type down into Core "
        "(Docs/package-layers.md, \"Rule for new files\"):")

    run.section("Source/Core/Protocol/ and Source/Core/Network/ must not "
                "include UI / rendering modules")
    run.verdict(
        run.hits(("Source/Core/Protocol/", "Source/Core/Network/"),
                 r'#include[ \t]*"(Rendering|Diagnostics|Input|Preferences)/'),
        "FAIL: Source/Core/Protocol/ or Source/Core/Network/ includes "
        "UI/rendering headers:")

    run.section("Source/Core/Rendering/ stays portable (no Apple-framework "
                "imports)")
    run.verdict(
        run.hits(("Source/Core/Rendering/",),
                 rf'#(include|import)[ \t]*[<"]({APPLE_FRAMEWORKS})[/.>"]'),
        "FAIL: Source/Core/Rendering/ imports Apple frameworks (move to "
        "Platform/macOS):")

    run.section("Source/Core/Diagnostics/ must not include Rendering modules")
    run.verdict(
        run.hits(("Source/Core/Diagnostics/",), r'#include[ \t]*"Core/Rendering/'),
        "FAIL: Source/Core/Diagnostics/ includes Rendering headers:")

    run.section("Platform/Host and Platform/macOS must not include each other")
    both = (run.hits(("Source/Platform/Host/",),
                     r'#(include|import)[ \t]*[<"]Platform/macOS/')
            + run.hits(("Source/Platform/macOS/",),
                       r'#(include|import)[ \t]*[<"]Platform/Host/',
                       exclude=r'"Platform/Host/HostSocketName\.h"'))
    run.verdict(both, "FAIL: Platform backends include each other (keep them "
                      "independent):")

    run.section("Platform*.h seams name no Core model type; Source/Core/App names "
                "no native framework")
    seams = [(path, text) for path, text in run.bodies(("Source/Platform/",))
             if os.path.basename(path).startswith("Platform")
             and path.endswith(".h")
             and path.count("/") >= 3]
    seamHits = []
    seamPattern = re.compile(r"(Ubiquitty::Terminal\b|\bTerm[ *&]|\bTerm\*"
                             r"|\bCell\b|Panes::Tree|\bGridCell\b|\bRow\b)")
    for path, text in seams:
        for number, line in enumerate(text.split("\n"), start=1):
            if seamPattern.search(line):
                seamHits.append(f"{path}:{number}:{line}")
    appHits = run.hits(("Source/Core/App/",), r"\b(NS|Gtk|MTL|Vk|CT|CG)[A-Z][A-Za-z]+")
    if seamHits or appHits:
        run.verdict(seamHits + appHits,
                    "FAIL: a platform seam names a Core model type, or "
                    "Source/Core/App names a native framework:")
    elif not seams:
        print("ok (0 header(s) match Platform*.h — the seam clause is vacuous "
              "until one exists; Source/Core/App checked)")
    else:
        print(f"ok ({len(seams)} seam header(s) checked, Source/Core/App checked)")

    portable = tuple(f"Source/{module}/" for module in PORTABLE_MODULES
                     if os.path.isdir(f"Source/{module}"))

    run.section("Core + portable modules import no Apple framework")
    run.verdict(
        run.hits(portable,
                 rf'#(include|import)[ \t]*[<"]({APPLE_FRAMEWORKS}'
                 rf'|CoreFoundation|os|dispatch)[/.>"]'),
        "FAIL: a portable module imports an Apple framework (move it behind a "
        "Platform/ backend):")

    run.section("Core + portable modules use no raw POSIX (the OS lives behind "
                "PAL/Transport)")
    run.verdict(
        [hit for hit in run.hits(
            portable,
            r'#(include|import)[ \t]*[<"](unistd|sys/|pthread|termios|fcntl'
            r'|signal|poll|spawn)')
         if not POSIX_SEAM.search(hit.split(":")[0])],
        "FAIL: a portable module reaches raw POSIX (route it through "
        "Platform/PAL or a Transport carrier):")

    run.section("the CPU<->GPU mix is written once")
    mix = run.hits(("Source/",),
                   r"\* *\(?255 *- *coverage\)? *\+ *[A-Za-z_]* *\* *coverage"
                   r" *\+ *127\) */ *255")
    if len(mix) > 1:
        run.verdict(mix, "FAIL: the coverage mix is spelled more than once "
                         "(call mixCoverage/writeMixedPixel):",
                    trailer=[f"  {MIX_IMPLEMENTATION} defines it; the shader is "
                             f"held to that one spelling."])
    else:
        print(f"ok (one spelling, in {MIX_IMPLEMENTATION})")

    run.section("an option PAL wraps is set through PAL, not by hand")
    run.verdict(
        run.hits(("Source/",),
                 rf"setsockopt[^;]*({PAL_WRAPPED_SOCKET_OPTIONS})",
                 exclude_path=rf"^{re.escape(PAL_SOCKET_IMPLEMENTATION)}$"),
        "FAIL: a socket option PAL wraps is set by hand (call the PAL wrapper, "
        "or add one):",
        trailer=[f"  PAL declares these in Source/PAL/Socket.h; "
                 f"{PAL_SOCKET_IMPLEMENTATION} implements them."],
        ok=f"ok (every {PAL_WRAPPED_SOCKET_OPTIONS} goes through the seam)")

    run.section("Base and PAL import no platform implementation")
    run.verdict(
        _foundation_backend_imports(run),
        "FAIL: Base or PAL imports a Platform implementation. Keep the "
        "contract portable and place native helpers under Platform/:")

    run.section("PAL contract headers (PAL.h, …) import no Apple framework / "
                "libdispatch")
    run.verdict(
        _pal_framework_imports(run),
        "FAIL: a PAL contract header imports a framework/libdispatch (pulls "
        "into Core unseen):")

    run.section("no 'Util' dumping-ground folder (use a purposeful home, e.g. "
                "Base/)")
    utils = set()
    for path in gate_corpus.eligible_under(run.root) or []:
        parts = path.split("/")
        if not parts or parts[0] not in ("Source", "Tests"):
            continue
        for index, name in enumerate(parts[:-1]):
            if name == "Util":
                utils.add("/".join(parts[:index + 1]))
    run.verdict(sorted(utils),
                "FAIL: a 'Util' folder is banned (generic dumping-ground); move "
                "its files to a purposeful home (e.g. Source/Base/):")

    run.section("a frontend names a Core enum, never redeclares it")
    # Tracked paths only, which is what the shell's `git grep` lists. An
    # untracked file is invisible to both; on the commit path that costs
    # nothing, because a new file is staged and therefore in the index. Widening
    # it here would hide a behaviour change inside a translation.
    tracked = set(gate_corpus.tracked())
    run.verdict(
        [hit for hit in run.hits(
            ("Source/Platform/", "Source/Core/App/", "Source/Agent/", "Tools/",
             "Tests/"),
            r"^[ \t]*enum[ \t]+class[ \t]+[A-Za-z_][A-Za-z0-9_]*::")
         if hit.split(":")[0] in tracked],
        "FAIL: an enum declared elsewhere is re-declared here; alias it instead "
        "(using X = Ns::X;):")

    run.section("a mirrored enum numbers its enumerators explicitly")
    run.failures += _delegate("Scripts/check-mirrored-enums.py")

    run.section("a line that says FAILED says what failed")
    run.failures += _delegate("Scripts/check-verdict-lines.py")

    run.section("L1 model layer must not include an L2 behaviour module "
                "(Decision 0)")
    run.failures += _decision_zero(run)

    run.section("external consumers (Samples/) use published umbrellas or the "
                "PAL socket-adoption contract")
    run.verdict(
        _sample_internal_imports(run),
        "FAIL: a Samples/ consumer includes a non-published internal header "
        "(use a Core/Kit umbrella or PAL/Socket.h for the public adapter factory):")

    run.section("a Linux frame is asked for through requestRepaint(), not by "
                "setting the flag")
    run.failures += _linux_repaint(run)

    run.section("the scroll-follow verdict is composed in Core, not per frontend")
    run.verdict(
        run.hits(("Source/Platform/", "Source/Core/App/"),
                 r"viewOffsetAfterOutput|heldViewOffsetForSelection"),
        "FAIL: a frontend composes the scroll-follow verdict from Core's "
        "fragments.\n      Call Input::viewportAfterOutput, which is that "
        "composition:")

    run.section("a key's bytes are the terminal's, not a frontend's")
    # A cursor, editing or function key spelled as a literal escape in a
    # frontend. Those forms are mode-dependent — under DECCKM the cursor keys
    # are SS3 and not CSI, and Home/End move with them — so a frontend that
    # writes one is right until the first program that sets the mode, and
    # silently wrong after. It also cannot reach the Kitty protocol,
    # modifyOtherKeys or DECUDK at all.
    #
    # This is what the iOS shell did, twice: its key view and its on-screen
    # control row each spelled the cursor keys as CSI. Both compiled, both
    # looked right, and neither could be told from correct by reading it.
    # Report a KeyPress and let Runtime::encodeKey answer.
    #
    # Matches the escape forms only, not every ESC in a frontend: a title
    # query, a paste bracket and a mouse report are the frontend's to send.
    # It also matches only bytes the frontend sends to the program: the same
    # spelling handed to processBytes is CUP arriving at the parser, which is a
    # smoke painting a screen rather than a frontend answering a key.
    #
    # C-family and Swift only, which is the set that can reach Core. The Emacs
    # client is a remote one written in Lisp: it cannot link the encoder, so the
    # rule has nothing to offer it, and the sequences it spells are a protocol
    # gap rather than a frontend ignoring an encoder within reach.
    run.verdict(
        run.hits(("Source/Platform/", "Source/UbiquittyKit/"),
                 r"""["'](?:\\u\{1b\}|\\x1b|\\033|\\e)(?:\[|O)[A-DHF]["']""",
                 exclude=r"processBytes\(|feedBytes\(|processInput\(",
                 exclude_path=r"\.(?:el|js|mjs|py|sh|html)$"),
        "FAIL: a frontend spells a key's escape sequence itself.\n"
        "      Fill a Runtime::KeyPress and call Runtime::encodeKey — DECCKM, "
        "the Kitty flags,\n      modifyOtherKeys and DECUDK are the terminal's "
        "answer, not the caller's:")

    run.section("a texture cache keys an image by the image, not by fields it "
                "lists")
    run.verdict(
        run.hits(("Source/Platform/",), r"imageContentStamp\(",
                 exclude=r"imageContentStamp\([A-Za-z_*&]+\)"),
        "FAIL: a backend spells out imageContentStamp's fields. Pass the image "
        "instead,\n      so a field cannot be left out of the key that decides "
        "whether its\n      cached texture is still the image:")

    print()
    if run.failures:
        return gate_report.failed_count(
            "check-boundaries.py", run.failures,
            "boundary check(s) failed; each is a FAIL: line above")
    print(f"check-boundaries.py: PASSED ({run.rules} boundary rule(s) held)")
    return 0


def _delegate(script):
    """Run a gate that reports for itself, and count its verdict.

    stdout is flushed first: the child writes straight to the file descriptor
    while this process's own section header is still sitting in a buffer, so
    without the flush the header lands after the output it introduces.
    """
    sys.stdout.flush()
    finished = subprocess.run(["python3", sibling(script)], check=False)
    return 1 if finished.returncode != 0 else 0


def _emit(what, layer):
    finished = subprocess.run(
        [sibling("Scripts/check-core-layer-partition.sh"), f"--emit-{what}", layer],
        capture_output=True, text=True, check=False)
    return [line for line in finished.stdout.split("\n") if line]


def _decision_zero(run):
    """L1 must not include L2. Returns the number of failures."""
    l1Modules = _emit("modules", "L1")
    l2Modules = _emit("modules", "L2")
    if not l1Modules or not l2Modules:
        print("  cannot read the layer partition — Decision 0 NOT CHECKED",
              file=sys.stderr)
        return 1
    l1Files = _emit("files", "L1")
    if not l1Files:
        print("  cannot enumerate the L1 files — Decision 0 NOT CHECKED",
              file=sys.stderr)
        return 1
    pattern = re.compile(r'#include[ \t]*"((?:'
                         + "|".join(module + "/" for module in l2Modules) + ')[^"]*)"')
    hits = []
    for path in l1Files:
        try:
            with open(path, encoding="utf-8", errors="replace") as handle:
                for number, line in enumerate(handle, start=1):
                    match = pattern.search(line)
                    if match and (path, match.group(1)) not in MODEL_WIRE_GRAMMAR:
                        hits.append(f"{path}:{number}:{line.rstrip()}")
        except OSError:
            continue
    if hits:
        print("FAIL: an L1 model module includes an L2 behaviour module (see "
              "Docs/package-layers.md):")
        for hit in hits:
            print(f"  {hit}")
        return 1
    return 0


def _linux_repaint(run):
    """A frame is asked for through requestRepaint(). Returns failures.

    The Linux frontend belongs to the app, so a checkout that does not hold the
    app has no frame loop to judge; one that does and lacks the file fails below.
    """
    if "app" not in ubq_roots.repositories(gate_corpus.root()):
        print("ok (the Linux frontend is the app's; this checkout does not hold it)")
        return 0
    try:
        with open(LINUX_MAIN, encoding="utf-8", errors="replace") as handle:
            lines = handle.read().split("\n")
    except OSError:
        lines = []
    setter = re.compile(r"(^|[^A-Za-z0-9_])isRepaintPending[ \t]*=[ \t]*true")
    declaration = re.compile(r"^[ \t]*bool[ \t]+isRepaintPending")
    sets = [(number, line) for number, line in enumerate(lines, start=1)
            if setter.search(line) and not declaration.match(line)]

    # The line inside requestRepaint() itself is the one legitimate setter.
    own = None
    inBody = False
    for number, line in enumerate(lines, start=1):
        if line.startswith("void requestRepaint(") and line.endswith(")"):
            inBody = True
            continue
        if inBody and re.search(r"isRepaintPending[ \t]*=[ \t]*true", line):
            own = number
            break
        if inBody and line == "}":
            inBody = False
    if own is None:
        print("FAIL: requestRepaint() does not mark a frame pending, so nothing "
              "can ask for one")
        return 1
    violations = [(number, line) for number, line in sets if number != own]
    if violations:
        print("FAIL: a frame is asked for by setting isRepaintPending directly. "
              "Call requestRepaint():")
        print("      the frame clock is uninstalled when output goes idle, so "
              "the flag alone paints nothing.")
        for number, line in violations:
            print(f"  {LINUX_MAIN}:{number}:{line}")
        return 1
    return 0


def _self_test():
    """A rule reports a violation planted under the tree it guards.

    A boundary gate on a clean tree prints ok for every rule, which is what it
    would print if a rule had stopped matching. Each arm plants the shape its
    rule forbids and asserts the rule finds it.
    """
    failures = []

    def check(claim, held):
        print(f"  {'ok      ' if held else 'BROKEN  '} {claim}")
        if not held:
            failures.append(claim)

    run = Run()
    check("the corpus is not empty", bool(run.bodies(("Source/",))))
    check("every root a rule searches exists",
          all(isDirectory(root) for root in REQUIRED_ROOTS))
    check("the module alternation is read off the tree",
          bool(alternation(run, "Source/Core")))
    check("the backend alternation is read off the tree",
          bool(alternation(run, "Source/Platform")))

    fixture = Run()
    fixture._bodies = {
        "Source/PAL/AppleOwner.h": '#include "Platform/Apple/CFAuto.h"',
        "Source/PAL/Bridge.h": '#import <Platform/macOS/CFAutoBridge.h>',
        "Source/PAL/Implementation.cpp": '# include "../Platform/POSIX/Socket.h"',
        "Source/Base/AppleOwner.h": '#include "Platform/Apple/CFAuto.h"',
        "Source/PAL/Portable.h": '#include "PAL/FileDescriptor.h"\n#include <memory>',
        "Source/PAL/Native.h": '#include <CoreFoundation/CoreFoundation.h>',
        "Source/PAL/Image.h": '# include <ImageIO/ImageIO.h>',
        "Source/PAL/Dispatch.h": '#include <dispatch/dispatch.h>',
        "Source/Platform/Contract.h": '#include <Foundation/Foundation.h>',
        "Source/Platform/Apple/CFAuto.h": '#include <CoreFoundation/CoreFoundation.h>',
        "Source/Platform/macOS/Native.h": '#import <AppKit/AppKit.h>',
        "Source/Core/Owner.h": '#include "Platform/Apple/CFAuto.h"',
    }
    check("PAL and Base cannot reach an Apple helper or another backend",
          {hit.split(":", 1)[0] for hit in _foundation_backend_imports(fixture)}
          == {"Source/PAL/AppleOwner.h", "Source/PAL/Bridge.h",
              "Source/PAL/Implementation.cpp", "Source/Base/AppleOwner.h"})
    check("native SDK imports stay out of contracts and work in Apple backends",
          {hit.split(":", 1)[0] for hit in _pal_framework_imports(fixture)}
          == {"Source/PAL/Native.h", "Source/PAL/Image.h",
              "Source/PAL/Dispatch.h", "Source/Platform/Contract.h"})
    check("Core cannot include the shared Apple helper",
          {hit.split(":", 1)[0] for hit in _core_upper_layer_imports(fixture, ("Platform",))}
          == {"Source/Core/Owner.h"})

    for header, allowed in (
            ("UbiquittyCore/UbiquittyCore.h", True),
            ("UbiquittyKit/UbiquittyKit.h", True),
            ("PAL/Socket.h", True),
            ("PAL/FileDescriptor.h", False),
            ("PAL/Stream.h", False),
            ("PAL/Socket.h.private", False),
            ("Core/Terminal.h", False),
            ("Platform/POSIX/Socket.h", False)):
        for opening, closing in (("\"", "\""), ("<", ">")):
            fixture._bodies = {
                "Samples/consumer/main.cpp": f"# include {opening}{header}{closing}"}
            check(f"sample import {opening}{header}{closing} has the declared scope",
                  bool(_sample_internal_imports(fixture)) == (not allowed))
    fixture._bodies = {"Samples/consumer/main.cpp": '#include <sys/socket.h>\n#include <memory>'}
    check("sample system headers remain allowed", not _sample_internal_imports(fixture))
    fixture._bodies = {
        "Samples/consumer/main.cpp": '#include "Core/Terminal.h" // "PAL/Socket.h"'}
    check("a public header in a comment cannot exempt an internal include",
          bool(_sample_internal_imports(fixture)))

    # The rules are regexes over lines; each is asserted to match the shape it
    # forbids and to spare the shape it permits.
    cases = (
        ("a Core include of a frontend module",
         r'#include[ \t]*"(Platform/macOS/)', '#include "Platform/macOS/UB.h"', True),
        ("a Core include of its own module",
         r'"Core/', '#include "Core/Cell.h"', True),
        ("an Apple framework import",
         rf'#(include|import)[ \t]*[<"]({APPLE_FRAMEWORKS})[/.>"]',
         "#import <AppKit/AppKit.h>", True),
        ("a raw POSIX include",
         r'#(include|import)[ \t]*[<"](unistd|sys/|pthread|termios|fcntl'
         r'|signal|poll|spawn)', "#include <unistd.h>", True),
        ("a plain portable include is spared",
         r'#(include|import)[ \t]*[<"](unistd|sys/|pthread|termios|fcntl'
         r'|signal|poll|spawn)', "#include <string>", False),
        ("an enum redeclaration",
         r"^[ \t]*enum[ \t]+class[ \t]+[A-Za-z_][A-Za-z0-9_]*::",
         "    enum class Ubiquitty::Mode {", True),
        ("a hand-set socket option",
         rf"setsockopt[^;]*({PAL_WRAPPED_SOCKET_OPTIONS})",
         "setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);", True),
        ("a stamp call spelling its fields",
         r"imageContentStamp\(", "imageContentStamp(a, b, c)", True),
        ("a stamp call passing the image is spared",
         r"imageContentStamp\([A-Za-z_*&]+\)", "imageContentStamp(image)", True),
    )
    for claim, pattern, line, expected in cases:
        check(claim, bool(re.search(pattern, line)) == expected)

    if failures:
        print(f"{GATE} --self-test: {len(failures)} claim(s) the rules do not "
              f"keep", file=sys.stderr)
        return 1
    print(f"{GATE} --self-test: ok (roots present, alternations read off the "
          f"tree, each rule matches the shape it forbids)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
