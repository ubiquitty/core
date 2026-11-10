#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""check-model-lock-holds — a frontend call that waits on the parser fails here.

The model lock is taken by the drain at every parse bite and by a frontend
thread whenever it reads or writes the model. Each frontend acquisition is a
wait on the parser, and the regressions that froze the screen under output were
each one new acquisition on a path a frontend runs per keypress, per delivery
or per display tick: a plain `std::scoped_lock` in `encodeKey` and in
`noteUserActivity`, one per delivery in the selection's update after output,
and a tick that declined its frame because a selection stood. Clang's
thread-safety analysis proves that the model is touched only under the lock;
it cannot say that a frontend should not take it. This gate holds the number of
places that do.

It counts, each against its own ledger budget, two-sided: a new one fails, and
one removed must lower the budget in the same commit.

- `model-lock-holds`: every acquisition of a model lock in a function body of
  every file under Source/ that names the lock, private members and the
  members of nested types included, and every call of a tmux pane's
  `withTermLocked`. An acquisition is a `ModelLockHold` or a `ModelWrite`,
  however it is spelled or held (`= ModelLockHold(`, `ModelWrite const`, a
  `std::optional` of one), `lockForRead`, `lockForWrite`, `tryLockForRead`, a
  `ModelReadScope`, a drain host's `lockModel`, a `lockForDrain`, a plain std
  lock, or a `lock()`/`try_lock()` on a model lock however it is reached.
- `frame-decision-holds`: those acquisitions in the frame decision's own
  members (`takeFrame`, `frameHold`, `publishedFrameHold`, `modelFrameHold`,
  `shouldSkipLiveReadForSaturation`) and in `framePresentationState`, pinned
  apart, so a hold removed elsewhere cannot pay for one there.
- `per-tick-waits`: the entries of Scripts/frame-path-blocking.txt marked
  `waits`, the per-tick calls that still wait on the lock and so carry no
  `UBIQUITTY_NONBLOCKING`.
- `plain-model-lock-holds`: a std lock (`std::scoped_lock`, `std::unique_lock`,
  `std::lock_guard`) over a model lock in a first-party header or source. It
  takes the lock and grants the analysis nothing, which is the spelling a
  regression reaches for first.
- `thread-safety-escapes` under Source/ and `thread-safety-escapes-tests`
  under Tests/, apart, so a test claim removed cannot pay for an escape in
  the product: every place the analysis is told rather than shown,
  `UBIQUITTY_NO_THREAD_SAFETY_ANALYSIS`, a function declared
  `UBIQUITTY_ASSERTS_CAPABILITY` and every call of one (a test's
  `ownModelOnTestThread()` among them), and a lambda that declares
  `UBIQUITTY_REQUIRES` (it is reached through a pointer the analysis cannot
  follow), outside the files that implement the lock.

It refuses outright:

- a per-tick call, one the display-free cadence test's frontend makes
  (Tests/Core/Runtime/test_frame_cadence.cpp), that is neither declared
  `UBIQUITTY_NONBLOCKING` nor listed in Scripts/frame-path-blocking.txt, and an
  entry there that names no per-tick call, names a marked one, or names no kind
  or no reason;
- a branch in the frame decision whose condition reads a selection: the frame
  a view presents never depends on what it selects;
- a toolchain rule that lets `UBIQUITTY_OWNERSHIP_ANALYSIS` stand without the
  function-effect analysis: the `UBIQUITTY_NONBLOCKING` this gate takes as
  proof is proof only where Clang's -Wfunction-effects checked it.

What it cannot see: it reads text, not types. An acquisition through a name
for the lock it does not spell (a `ModelLock&` parameter named otherwise,
locked by a spelling none of the above), one in a file that never names the
lock, one reached through a function pointer, and Swift. The cadence test's
decision counter is what catches a lock the frame decision takes by any of
those routes: it counts the lock's own acquisitions around each decision.

    ./Scripts/check-model-lock-holds.py
    ./Scripts/check-model-lock-holds.py --self-test
"""

import os
import pathlib
import re
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gate_framework import Gate, Level
import ubq_roots

SESSION = pathlib.Path("Source/Core/Runtime/TerminalSession.h")
WORKSPACE = pathlib.Path("Source/Core/Runtime/WorkspaceSession.h")
CADENCE = pathlib.Path("Tests/Core/Runtime/test_frame_cadence.cpp")
ALLOWLIST = pathlib.Path("Scripts/frame-path-blocking.txt")
TOOLCHAIN = pathlib.Path("Scripts/cmake/UbiquittyToolchain.cmake")
SOURCE_ROOTS = ("Source",)
ESCAPE_ROOTS = ("Source", "Tests")
SOURCE_SUFFIXES = (".h", ".cpp", ".mm")

#: A file that names the model lock, whose function bodies are counted.
NAMES_MODEL_LOCK = re.compile(r"\b(?:ModelLock|ModelLockHold|ModelReadScope|modelLockRole|lockModel|lockForDrain|lockForRead"
                              r"|tryLockForRead|withTermLocked|modelLockPointer|m_activeLock|m_modelLock)\b")

#: The files that implement the lock and its escape, which name it without
#: holding it as a frontend does.
LOCK_DEFINITIONS = {"Source/Core/ModelLock.h", "Source/Core/ModelLock.cpp", "Source/Base/ThreadSafety.h"}

#: The ledger budgets this gate holds the tree to.
BUDGETS = ("model-lock-holds", "frame-decision-holds", "per-tick-waits", "plain-model-lock-holds", "thread-safety-escapes",
           "thread-safety-escapes-tests")

#: The frame decision: the per-tick answer and the declines it asks.
FRAME_DECISION = ("takeFrame", "frameHold", "publishedFrameHold", "modelFrameHold", "shouldSkipLiveReadForSaturation")

#: The members whose acquisitions the pinned frame-decision budget counts: the
#: decision, and the frame clock's presentation read every tick asks.
FRAME_DECISION_HOLDS = FRAME_DECISION + ("framePresentationState",)

#: Where each escape budget counts.
ESCAPE_BUDGETS = (("thread-safety-escapes", "Source"), ("thread-safety-escapes-tests", "Tests"))

#: The kinds an allowlist entry names.
WAITS = "waits"
NO_WAIT = "nowait"

#: Names a model lock is reached by in first-party code.
MODEL_LOCK_OPERAND = (r"(?:m_activeLock|m_modelLock|modelLockPointer\s*\(\s*\)|m_search\.lock|primaryLock|lockHandle\s*\(\s*\)"
                      r"|nativeHandle\s*\(\s*\)|\bmodelLock\b)")

HOLD = re.compile(
    r"\b(?:ModelLockHold|ModelWrite)\s+(?:const\s+)?\w+\s*[({]"
    r"|\b(?:ModelLockHold|ModelWrite)\s*[({]"
    r"|\boptional\s*<\s*(?:const\s+)?(?:ModelLockHold|ModelWrite)\s*>"
    r"|\blockForRead\s*\(\s*\)"
    r"|\blockForWrite\s*\(\s*\)"
    r"|\btryLockForRead\s*\(\s*\)"
    r"|\bModelReadScope\s+(?:const\s+)?\w+\s*[({]"
    r"|\bmodelReadScope\s*\(\s*\)"
    r"|\blockModel\s*\(\s*\)"
    r"|\blockForDrain\s*\(\s*\)"
    r"|\bstd::(?:scoped_lock|unique_lock|lock_guard)\b[^;]*" + MODEL_LOCK_OPERAND +
    r"|" + MODEL_LOCK_OPERAND + r"\s*(?:->|\.)\s*(?:lock|try_lock)\s*\("
    r"|\(\s*\*\s*" + MODEL_LOCK_OPERAND + r"\s*\)\s*\.\s*(?:lock|try_lock)\s*\(")

#: A tmux pane's locked run, which takes the pane's model lock.
TERM_LOCKED_RUN = re.compile(r"(?:->|\.)\s*withTermLocked\s*\(")

PLAIN_HOLD = re.compile(r"\bstd::(?:scoped_lock|unique_lock|lock_guard)\b[^;]*" + MODEL_LOCK_OPERAND)

ESCAPES = (
    ("analysis escape(s)", re.compile(r"\bUBIQUITTY_NO_THREAD_SAFETY_ANALYSIS\b")),
    ("asserted capabilit(ies)", re.compile(r"\bUBIQUITTY_ASSERTS_CAPABILITY\b")),
    ("lambda(s) declaring a requirement", re.compile(r"\]\s*\([^()]*\)\s*(?:mutable\s+)?(?:noexcept\s+)?UBIQUITTY_REQUIRES\b")),
)

#: A function declared to assert the capability: its name is group 1.
ASSERTING_DECLARATION = re.compile(r"\b(\w+)\s*\([^()]*\)\s*(?:const\s*)?(?:noexcept\s*)?UBIQUITTY_ASSERTS_CAPABILITY\b")

#: The label a call of an asserting function is counted under.
ASSERTING_CALLS = "call(s) of an asserting function"

NONBLOCKING = "UBIQUITTY_NONBLOCKING"

#: A frontend call in the cadence test's tick: `m_session.name(`.
CADENCE_CALL = re.compile(r"\bm_session\s*\.\s*(\w+)\s*\(")

#: The toolchain's refusal of the ownership analysis without function effects.
FUNCTION_EFFECTS_REQUIRED = re.compile(r"UBIQUITTY_OWNERSHIP_ANALYSIS[^\n]*\n(?:[^\n]*\n){0,6}?[^\n]*FATAL_ERROR[^\n]*function-effects",
                                       re.IGNORECASE)


# ---- a small reader of C++ text ----------------------------------------------

def blanked(text):
    """The text with comments and string and character literals blanked.

    Newlines stay, so offsets and line numbers still match the file. A rule
    then never fires on a comment that quotes the shape it refuses.
    """
    out = []
    index = 0
    length = len(text)
    while index < length:
        if text.startswith("//", index):
            end = text.find("\n", index)
            end = length if end == -1 else end
            out.append(" " * (end - index))
            index = end
        elif text.startswith("/*", index):
            end = text.find("*/", index + 2)
            end = length if end == -1 else end + 2
            out.append(re.sub(r"[^\n]", " ", text[index:end]))
            index = end
        elif text.startswith('R"', index) and (index == 0 or not (text[index - 1].isalnum() or text[index - 1] == "_")):
            match = re.match(r'R"([^(\s]*)\(', text[index:])
            if not match:
                out.append(text[index])
                index += 1
                continue
            terminator = ")" + match.group(1) + '"'
            end = text.find(terminator, index + match.end())
            end = length if end == -1 else end + len(terminator)
            out.append(re.sub(r"[^\n]", " ", text[index:end]))
            index = end
        elif text[index] in "\"'":
            quote = text[index]
            # A digit separator: 1'000'000.
            if quote == "'" and index > 0 and text[index - 1].isalnum() and index + 1 < length and text[index + 1].isalnum():
                out.append(quote)
                index += 1
                continue
            end = index + 1
            while end < length and text[end] != quote and text[end] != "\n":
                end += 2 if text[end] == "\\" else 1
            end = min(end + 1, length)
            out.append(quote + re.sub(r"[^\n]", " ", text[index + 1:end - 1]) + quote)
            index = end
        else:
            out.append(text[index])
            index += 1
    return "".join(out)


def closing(code, opening):
    """The index of the bracket that closes the one at `opening`."""
    pair = {"{": "}", "(": ")"}[code[opening]]
    depth = 0
    for index in range(opening, len(code)):
        if code[index] == code[opening]:
            depth += 1
        elif code[index] == pair:
            depth -= 1
            if depth == 0:
                return index
    return len(code) - 1


ACCESS = re.compile(r"\b(public|private|protected)\s*:(?!:)")
NAME_BEFORE_PARAMETERS = re.compile(r"(~?[A-Za-z_]\w*)\s*\(")
NOT_A_NAME = {"if", "for", "while", "switch", "return", "sizeof", "alignof", "decltype", "static_assert",
              "noexcept", "requires", "alignas", "explicit"}
CONTAINER = re.compile(r"\s*(?:template\s*<[^{;]*>\s*)?(?:(class|struct|union)\b|(namespace)\b|(enum)\b|(extern)\b[^{]*$)")
PREPROCESSOR_LINE = re.compile(r"^[ \t]*#.*$", re.MULTILINE)


class Member:
    """A function defined in a class, namespace or file: its name, the types it is
    nested in, its access, its head and its body."""

    def __init__(self, name, scope, access, head, body_start, body_end):
        self.name = name
        self.scope = scope
        self.access = access
        self.head = head
        self.body_start = body_start
        self.body_end = body_end

    @property
    def qualified_name(self):
        return "::".join(self.scope + [self.name])


def walk(code, begin, end, scope, access, found):
    """Collects every function defined between `begin` and `end`, descending into
    the classes, structs, unions and namespaces defined there."""
    index = begin
    statement = index
    while index < end:
        character = code[index]
        specifier = ACCESS.match(code, index)
        if specifier and code[index - 1] in " \n\t;{}":
            access = specifier.group(1)
            index = specifier.end()
            statement = index
            continue
        if character == ";":
            index += 1
            statement = index
            continue
        if character == "{":
            body_end = closing(code, index)
            head = PREPROCESSOR_LINE.sub("", code[statement:index])
            container = CONTAINER.match(head)
            if container and (container.group(1) or container.group(2) or container.group(4)):
                name = re.search(r"(\w+)\s*(?:final\s*)?(?::[^:{][^{]*)?$", head.strip())
                kind = container.group(1)
                walk(code, index + 1, body_end, scope + [name.group(1) if name and kind else ""],
                     "private" if kind == "class" else "public", found)
            elif not container:
                names = [name for name in NAME_BEFORE_PARAMETERS.finditer(head)
                         if name.group(1) not in NOT_A_NAME and not name.group(1).startswith("UBIQUITTY_")]
                if names and "=" not in head[:names[0].start()]:
                    found.append(Member(names[0].group(1), [part for part in scope if part], access, head, index, body_end))
            index = body_end + 1
            statement = index
            continue
        index += 1
    return found


def functions(code):
    """Every function defined in `code`, members of nested types included."""
    return walk(code, 0, len(code), [], "public", [])


def members(code, class_name):
    """The functions defined directly in `class <class_name> { ... }`."""
    return [member for member in functions(code) if member.scope and member.scope[-1] == class_name]


def declared_nonblocking(code, class_name):
    """The names of the class's members whose declaration carries UBIQUITTY_NONBLOCKING."""
    return {member.name for member in members(code, class_name) if NONBLOCKING in member.head}


def conditions(body):
    """The text of every `if`/`while` condition and every ternary's test in a body."""
    found = []
    for match in re.finditer(r"\b(?:if|while)\s*(?:constexpr\s*)?\(", body):
        opening = match.end() - 1
        found.append(body[opening:closing(body, opening) + 1])
    for match in re.finditer(r"[^;{}]*\?[^:;]*:", body):
        found.append(match.group(0))
    return found


# ---- the counts -----------------------------------------------------------------

def holds_in(code):
    """[(function, count)] for each function in `code` that takes a model lock."""
    result = []
    for member in functions(code):
        count = len(HOLD.findall(code[member.body_start:member.body_end]))
        if count:
            result.append((member.qualified_name, count))
    return result


def allowlist_entries(text):
    """{name: (kind, reason)} from the allowlist's `<member> <kind> <reason>` lines."""
    entries = {}
    for line in text.split("\n"):
        words = line.split("#", 1)[0].split(None, 2)
        if words:
            entries[words[0]] = (words[1] if len(words) > 1 else "", words[2].strip() if len(words) > 2 else "")
    return entries


def per_tick_calls(cadence_code):
    """The TerminalSession members the cadence test's frontend calls at its tick."""
    return {match.group(1) for match in CADENCE_CALL.finditer(cadence_code)}


def source_files(root, directories):
    for directory in directories:
        for path in sorted((root / directory).rglob("*")):
            if path.suffix in SOURCE_SUFFIXES and path.is_file():
                yield path, path.relative_to(root).as_posix()


def judge(root, budgets):
    """([(location, message)], counts) for the tree at `root`."""
    findings = []
    session_path = root / SESSION
    if not session_path.is_file():
        return [(str(SESSION), "is missing, so nothing was counted")], {}
    session_code = blanked(session_path.read_text(encoding="utf-8", errors="replace"))
    allowlist_path = root / ALLOWLIST
    allowlist = allowlist_entries(allowlist_path.read_text(encoding="utf-8")) if allowlist_path.is_file() else {}
    cadence_path = root / CADENCE
    ticks = per_tick_calls(blanked(cadence_path.read_text(encoding="utf-8", errors="replace"))) if cadence_path.is_file() else set()
    counts = {}

    # Every acquisition of the model lock, in every file under Source/ that
    # names it, wherever in the file it sits.
    holds = []
    for path, relative in source_files(root, SOURCE_ROOTS):
        if relative in LOCK_DEFINITIONS:
            continue
        code = session_code if relative == SESSION.as_posix() else blanked(path.read_text(encoding="utf-8", errors="replace"))
        if NAMES_MODEL_LOCK.search(code):
            holds += [(f"{relative}: {name}", count) for name, count in holds_in(code)]
        runs = len(TERM_LOCKED_RUN.findall(code))
        if runs:
            holds.append((f"{relative}: withTermLocked", runs))
    counts["holds-by-function"] = holds
    counts["model-lock-holds"] = sum(count for _, count in holds)
    findings += against("Source", counts["model-lock-holds"], budgets.get("model-lock-holds"),
        "model-lock acquisition(s) under Source/")

    # The frame decision's own acquisitions, pinned apart.
    decision_holds = sum(len(HOLD.findall(session_code[member.body_start:member.body_end]))
                         for member in members(session_code, "TerminalSession") if member.name in FRAME_DECISION_HOLDS)
    counts["frame-decision-holds"] = decision_holds
    findings += against(str(SESSION), decision_holds, budgets.get("frame-decision-holds"),
        "model-lock acquisition(s) in the frame decision and the frame clock's presentation read")

    # Per-tick calls: marked, or listed with a kind and a reason.
    waiting = sorted(name for name, (kind, _) in allowlist.items() if kind == WAITS)
    counts["per-tick-waits"] = len(waiting)
    counts["per-tick-nowait"] = len(allowlist) - len(waiting)
    findings += against(str(ALLOWLIST), counts["per-tick-waits"], budgets.get("per-tick-waits"),
        "per-tick call(s) that still wait on the model lock")
    session_members = {member.name for member in members(session_code, "TerminalSession")}
    marked = declared_nonblocking(session_code, "TerminalSession")
    tick_members = {name for name in ticks if name in session_members}
    for name in sorted(tick_members - marked - set(allowlist)):
        findings.append((str(CADENCE), f"per-tick call `{name}` is neither {NONBLOCKING} nor listed in {ALLOWLIST}"))
    for name in sorted(set(allowlist) - tick_members):
        findings.append((str(ALLOWLIST), f"`{name}` is not a call the cadence test's frontend makes; delete the entry"))
    for name in sorted(set(allowlist) & marked):
        findings.append((str(ALLOWLIST), f"`{name}` is {NONBLOCKING}; delete the entry"))
    for name, (kind, reason) in sorted(allowlist.items()):
        if kind not in (WAITS, NO_WAIT):
            findings.append((str(ALLOWLIST), f"`{name}` names no kind; say `{WAITS}` or `{NO_WAIT}`"))
        elif not reason:
            findings.append((str(ALLOWLIST), f"`{name}` names no reason"))

    # The frame decision never branches on a selection.
    for member in members(session_code, "TerminalSession"):
        if member.name not in FRAME_DECISION:
            continue
        for condition in conditions(session_code[member.body_start:member.body_end]):
            if re.search(r"selection", condition, re.IGNORECASE):
                findings.append((str(SESSION), f"`{member.name}` branches on a selection: {' '.join(condition.split())[:120]}"))

    # Plain std locks over a model lock, across the sources.
    plain = 0
    for path, relative in source_files(root, SOURCE_ROOTS):
        if relative not in LOCK_DEFINITIONS:
            plain += len(PLAIN_HOLD.findall(blanked(path.read_text(encoding="utf-8", errors="replace"))))
    counts["plain-model-lock-holds"] = plain
    findings += against("Source", plain, budgets.get("plain-model-lock-holds"),
        "std lock(s) over a model lock, which grant the thread-safety analysis nothing")

    # Every place the analysis is told rather than shown, under each root apart.
    texts = {relative: blanked(path.read_text(encoding="utf-8", errors="replace")) for path, relative in source_files(root, ESCAPE_ROOTS)}
    asserting = {match.group(1) for code in texts.values() for match in ASSERTING_DECLARATION.finditer(code)}
    asserting_call = re.compile(r"\b(?:" + "|".join(sorted(map(re.escape, asserting))) + r")\s*\(") if asserting else None
    for budget, directory in ESCAPE_BUDGETS:
        escapes = {label: 0 for label, _ in ESCAPES}
        escapes[ASSERTING_CALLS] = 0
        for relative, code in texts.items():
            if relative in LOCK_DEFINITIONS or not relative.startswith(directory + "/"):
                continue
            for label, pattern in ESCAPES:
                escapes[label] += len(pattern.findall(code))
            if asserting_call:
                declared = {match.start(1) for match in ASSERTING_DECLARATION.finditer(code)}
                escapes[ASSERTING_CALLS] += sum(1 for match in asserting_call.finditer(code) if match.start() not in declared)
        counts[budget] = sum(escapes.values())
        counts[budget + " by kind"] = escapes
        findings += against(directory, counts[budget], budgets.get(budget),
            f"thread-safety escape(s) under {directory}/ (" + ", ".join(f"{count} {label}" for label, count in escapes.items()) + ")")

    # The nonblocking mark is proof only where the analysis that checks it runs.
    toolchain = root / TOOLCHAIN
    if toolchain.is_file() and not FUNCTION_EFFECTS_REQUIRED.search(toolchain.read_text(encoding="utf-8", errors="replace")):
        findings.append((str(TOOLCHAIN), f"UBIQUITTY_OWNERSHIP_ANALYSIS can stand without -Wfunction-effects, so {NONBLOCKING} proves nothing"
                                         " there; configure must refuse that"))
    return findings, counts


def against(location, count, budget, what):
    """The finding a two-sided budget yields for one count."""
    if budget is None:
        return [(location, f"{count} {what}, and the ledger names no budget for them")]
    if count > budget:
        return [(location, f"{count} {what} against a budget of {budget}: a new one appeared")]
    if count < budget:
        return [(location, f"{count} {what} against a budget of {budget}: lower the budget to {count} in this commit")]
    return []


class ModelLockHolds(Gate):
    name = "check-model-lock-holds"
    subject = "the model-lock acquisitions a frontend reaches, the per-tick allowlist and the analysis escapes"

    def examine(self):
        root = pathlib.Path(self.root)
        ledger = ubq_roots.ledger(str(root))[0]
        budgets = {name: ledger.get(name) for name in BUDGETS}
        findings, counts = judge(root, budgets)
        for name, count in counts.get("holds-by-function", []):
            self.detail(f"{count} model-lock acquisition(s) in {name}", Level.DEBUG)
        if findings:
            for location, message in findings:
                self.finding(location, message)
            return self.failed("model-lock finding(s)", [message for _, message in findings])
        return self.ok(f"{counts['model-lock-holds']} model-lock acquisition(s), {counts['frame-decision-holds']} in the frame"
                       f" decision, {counts['per-tick-waits']} per-tick call(s) that wait and {counts['per-tick-nowait']} that do"
                       f" not, {counts['plain-model-lock-holds']} plain std lock(s), {counts['thread-safety-escapes']} analysis"
                       f" escape(s) under Source/ and {counts['thread-safety-escapes-tests']} under Tests/")

    def selfTest(self):
        """Each shape, planted into a tree the gate passes, fails it."""
        failures = []
        for name, plant, expected in PLANTS:
            with tempfile.TemporaryDirectory() as directory:
                root = pathlib.Path(directory)
                write_fixture(root)
                clean, counts = judge(root, FIXTURE_BUDGETS)
                if clean:
                    failures.append(f"the clean fixture fails before `{name}` is planted: {clean[0][1]}")
                    continue
                plant(root)
                findings, _ = judge(root, FIXTURE_BUDGETS)
                if not any(expected in message for _, message in findings):
                    failures.append(f"`{name}` planted, and no finding names `{expected}`"
                                    f" ({'; '.join(message for _, message in findings) or 'none'})")
        if failures:
            return self.failed("self-test finding(s)", failures)
        return self.ok(f"each of {len(PLANTS)} planted regression shape(s) fails, and the clean fixture passes")


# ---- the self-test's tree ---------------------------------------------------------

FIXTURE_SESSION = """
class TerminalSession {
public:
    std::uint32_t takeEventFlags()
    {
        const ModelLockHold hold(*m_activeLock);
        return 0;
    }
    RuntimeSnapshot readSnapshot() UBIQUITTY_NONBLOCKING { return m_runtime->link().reads.read(); }
    FrameCapture* takeFrame(const FrameDemand& demand, FrameSupply& supply) UBIQUITTY_NONBLOCKING
    {
        supply = {};
        publishSelectionForCaptures(demand.selection);
        if (FrameCapture* capture = takeSyncFrame()) {
            supply.selection = selectionOverCapture(demand.selection, *capture, demand.selectionSpans, supply.selectionEnds);
            return capture;
        }
        return nullptr;
    }
    OutputUpdate updateSelectionAfterOutput(Selection::Session* selection, std::ptrdiff_t viewOffset)
    {
        return Selection::updateAfterOutput(*selection, viewOffset);
    }
    KeyEncoding sendKey(const KeyPress& press) UBIQUITTY_NONBLOCKING
    {
        return postKey(press);
    }

private:
    FrameHold frameHold(Ticks nowTicks)
    {
        return FrameHold::None;
    }

    void assertModelLockHeld() const UBIQUITTY_ASSERTS_CAPABILITY(modelLockRole) { }

    struct LocalPaneLink final : Session::PaneLink {
        void pollEvents(std::uint64_t now) override
        {
            m_events.deliver();
        }
    };

    ModelLock* m_activeLock = &m_modelLock;
};
"""

FIXTURE_WORKSPACE = """
class WorkspaceSession {
public:
    void describe(TerminalSession& session)
    {
        const ModelLockHold hold(*session.modelLockPointer());
    }
};
"""

FIXTURE_TMUX_HOST = """
void TmuxControlHost::deliver(Pane* pane)
{
    pane->withTermLocked([&](Ubiquitty::Terminal& terminal) { });
}
"""

FIXTURE_CADENCE = """
void tick()
{
    m_session.takeEventFlags();
    m_session.readSnapshot();
    m_session.takeFrame(demand, supply);
    m_session.updateSelectionAfterOutput(&m_selection, m_viewOffset);
    m_session.sendKey(press);
}
"""

FIXTURE_SOLE_MODEL_THREAD = """
inline void ownModelOnTestThread() UBIQUITTY_ASSERTS_CAPABILITY(Ubiquitty::modelLockRole) { }
"""

FIXTURE_RUNTIME_TEST = """
TEST(aRuntimeOnItsOwnThread)
{
    ownModelOnTestThread();
    runtime.prepareAdoption();
}
"""

FIXTURE_ALLOWLIST = """# member kind reason
takeEventFlags waits polls the pane link under the model lock
updateSelectionAfterOutput nowait try-locks the model to judge the selection after output
"""

FIXTURE_TOOLCHAIN = """
if(UBIQUITTY_OWNERSHIP_ANALYSIS AND NOT UBIQUITTY_HAS_FUNCTION_EFFECTS)
    message(FATAL_ERROR "UBIQUITTY_OWNERSHIP_ANALYSIS needs -Wfunction-effects")
endif()
"""

FIXTURE_BUDGETS = {"model-lock-holds": 3, "frame-decision-holds": 0, "per-tick-waits": 1, "plain-model-lock-holds": 0,
                   "thread-safety-escapes": 1, "thread-safety-escapes-tests": 2}

#: Where the fixture's test files sit.
FIXTURE_SOLE_MODEL_THREAD_PATH = pathlib.Path("Tests/SoleModelThread.h")
FIXTURE_RUNTIME_TEST_PATH = pathlib.Path("Tests/Core/Runtime/test_runtime.cpp")


def write_fixture(root):
    for relative, text in ((SESSION, FIXTURE_SESSION), (WORKSPACE, FIXTURE_WORKSPACE),
                           (pathlib.Path("Source/Core/Runtime/TmuxControlHost.cpp"), FIXTURE_TMUX_HOST),
                           (CADENCE, FIXTURE_CADENCE), (ALLOWLIST, FIXTURE_ALLOWLIST), (TOOLCHAIN, FIXTURE_TOOLCHAIN),
                           (FIXTURE_SOLE_MODEL_THREAD_PATH, FIXTURE_SOLE_MODEL_THREAD), (FIXTURE_RUNTIME_TEST_PATH, FIXTURE_RUNTIME_TEST)):
        (root / relative).parent.mkdir(parents=True, exist_ok=True)
        (root / relative).write_text(text, encoding="utf-8")


def replace_in(path, old, new):
    text = path.read_text(encoding="utf-8")
    assert old in text, old
    path.write_text(text.replace(old, new, 1), encoding="utf-8")


def plant_in_session(old, new):
    return lambda root: replace_in(root / SESSION, old, new)


def plant_selection_hold(root):
    """The per-delivery selection update takes a plain hold."""
    replace_in(root / SESSION, "        return Selection::updateAfterOutput(*selection, viewOffset);",
               "        const std::scoped_lock hold(*m_activeLock);\n"
               "        return Selection::updateAfterOutput(*selection, viewOffset);")


def plant_selection_decline(root):
    """The tick declines its frame while a selection stands."""
    replace_in(root / SESSION, "        supply = {};\n",
               "        supply = {};\n"
               "        if (demand.selection && Selection::hasSelection(*demand.selection)) {\n"
               "            supply.decline = FrameDecline::AwaitingCapture;\n"
               "            return nullptr;\n"
               "        }\n")


def plant_keypress_hold(root):
    """The keypress path waits for the model lock."""
    replace_in(root / SESSION, "    KeyEncoding sendKey(const KeyPress& press) UBIQUITTY_NONBLOCKING\n    {\n",
               "    KeyEncoding sendKey(const KeyPress& press)\n    {\n        const ModelLockHold hold(*m_activeLock);\n")


def plant_offset(root):
    """One per-tick call marked nonblocking while a keypress takes the lock: the two
    budgets are separate, so the one does not pay for the other."""
    replace_in(root / SESSION, "    std::uint32_t takeEventFlags()\n", "    std::uint32_t takeEventFlags() UBIQUITTY_NONBLOCKING\n")
    replace_in(root / ALLOWLIST, "takeEventFlags waits polls the pane link under the model lock\n", "")
    replace_in(root / SESSION, "    {\n        return postKey(press);\n",
               "    {\n        const ModelLockHold hold(*m_activeLock);\n        return postKey(press);\n")


def plant_analysis_escape(root):
    """A hold hidden from the analysis instead of taken through ModelLockHold."""
    replace_in(root / SESSION, "    std::uint32_t takeEventFlags()\n",
               "    std::uint32_t takeEventFlags() UBIQUITTY_NO_THREAD_SAFETY_ANALYSIS\n")


def plant_decision_swap(root):
    """A hold added to the frame decision, paid for by one removed elsewhere."""
    replace_in(root / SESSION, "        const ModelLockHold hold(*m_activeLock);\n        return 0;\n", "        return 0;\n")
    replace_in(root / SESSION, "        return FrameHold::None;\n", "        const ModelLockHold hold(*m_activeLock);\n        return FrameHold::None;\n")


def plant_helper_elsewhere(root):
    """The lock handed to a helper in another file, which takes it there."""
    path = root / "Source/Core/Selection/TerminalSelection.cpp"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("OutputUpdate updateAfterOutput(Session& selection, ModelLock& lock)\n"
                    "{\n    const ModelLockHold hold(lock);\n    return {};\n}\n", encoding="utf-8")


def plant_escape_paid_by_a_test(root):
    """An escape in the product, paid for by a test claim removed."""
    replace_in(root / FIXTURE_RUNTIME_TEST_PATH, "    ownModelOnTestThread();\n", "")
    replace_in(root / SESSION, "    std::uint32_t takeEventFlags()\n", "    std::uint32_t takeEventFlags() UBIQUITTY_NO_THREAD_SAFETY_ANALYSIS\n")


def plant_toolchain_without_effects(root):
    """The ownership analysis allowed to stand without function effects."""
    (root / TOOLCHAIN).write_text("option(UBIQUITTY_OWNERSHIP_ANALYSIS \"check\" ON)\n", encoding="utf-8")


PLANTS = (
    ("a plain scoped_lock in the per-delivery selection update", plant_selection_hold, "std lock(s) over a model lock"),
    ("a per-tick frame decline that depends on a selection", plant_selection_decline, "branches on a selection"),
    ("a blocking lock in a keypress path", plant_keypress_hold, "model-lock acquisition(s)"),
    ("the keypress path's per-tick call unmarked", plant_keypress_hold, "neither UBIQUITTY_NONBLOCKING"),
    ("a hold in a private member",
     plant_in_session("        return FrameHold::None;\n", "        const ModelLockHold hold(*m_activeLock);\n        return FrameHold::None;\n"),
     "model-lock acquisition(s)"),
    ("a hold in a nested type's member",
     plant_in_session("            m_events.deliver();\n", "            const ModelLockHold hold(*m_owner.m_activeLock);\n            m_events.deliver();\n"),
     "model-lock acquisition(s)"),
    ("a hold assigned from a temporary",
     plant_in_session("        return FrameHold::None;\n", "        auto hold = ModelLockHold(*m_activeLock);\n        return FrameHold::None;\n"),
     "model-lock acquisition(s)"),
    ("a hold spelt const after its type",
     plant_in_session("        return FrameHold::None;\n", "        ModelLockHold const hold(*m_activeLock);\n        return FrameHold::None;\n"),
     "model-lock acquisition(s)"),
    ("a hold emplaced into an optional",
     plant_in_session("        return FrameHold::None;\n",
                      "        std::optional<ModelLockHold> hold;\n        hold.emplace(*m_activeLock);\n        return FrameHold::None;\n"),
     "model-lock acquisition(s)"),
    ("a drain host's lock taken by a frontend call",
     plant_in_session("        return FrameHold::None;\n", "        m_host.lockModel();\n        m_host.unlockModel();\n        return FrameHold::None;\n"),
     "model-lock acquisition(s)"),
    ("a drain's lock taken directly",
     plant_in_session("        return FrameHold::None;\n", "        m_modelLock.lockForDrain();\n        return FrameHold::None;\n"),
     "model-lock acquisition(s)"),
    ("a lock taken through a dereference",
     plant_in_session("        return FrameHold::None;\n", "        (*m_activeLock).lock();\n        return FrameHold::None;\n"),
     "model-lock acquisition(s)"),
    ("a lock taken through the lock pointer accessor",
     plant_in_session("        return FrameHold::None;\n", "        modelLockPointer()->lock();\n        return FrameHold::None;\n"),
     "model-lock acquisition(s)"),
    ("a hold in WorkspaceSession",
     lambda root: replace_in(root / WORKSPACE, "    }\n};", "        const ModelLockHold again(*session.modelLockPointer());\n    }\n};"),
     "model-lock acquisition(s)"),
    ("a tmux pane's locked run",
     lambda root: replace_in(root / "Source/Core/Runtime/TmuxControlHost.cpp", "{ });\n", "{ });\n    pane->withTermLocked([&](Ubiquitty::Terminal&) { });\n"),
     "model-lock acquisition(s)"),
    ("a per-tick wait removed while a keypress lock is added", plant_offset, "model-lock acquisition(s)"),
    ("an analysis escape", plant_analysis_escape, "analysis escape(s)"),
    ("an asserted capability",
     plant_in_session("private:\n", "private:\n    void assertHeld() const UBIQUITTY_ASSERTS_CAPABILITY(modelLockRole) { }\n"),
     "asserted capabilit(ies)"),
    ("a lambda declaring a requirement",
     plant_in_session("        return FrameHold::None;\n",
                      "        auto hook = [](void* context) UBIQUITTY_REQUIRES(modelLockRole) { };\n        return FrameHold::None;\n"),
     "lambda(s) declaring a requirement"),
    ("an ownership analysis that stands without function effects", plant_toolchain_without_effects, "-Wfunction-effects"),
    ("a hold in the frame decision paid for by one removed elsewhere", plant_decision_swap, "in the frame decision"),
    ("a write scope",
     plant_in_session("            m_events.deliver();\n", "            const ModelWrite write(m_owner);\n            m_events.deliver();\n"),
     "model-lock acquisition(s)"),
    ("a write scope spelt const after its type",
     plant_in_session("            m_events.deliver();\n", "            ModelWrite const write(m_owner);\n            m_events.deliver();\n"),
     "model-lock acquisition(s)"),
    ("a write scope assigned from a temporary",
     plant_in_session("            m_events.deliver();\n", "            auto write = ModelWrite(m_owner);\n            m_events.deliver();\n"),
     "model-lock acquisition(s)"),
    ("a write scope emplaced into an optional",
     plant_in_session("            m_events.deliver();\n",
                      "            std::optional<ModelWrite> write;\n            write.emplace(m_owner);\n            m_events.deliver();\n"),
     "model-lock acquisition(s)"),
    ("a write bracketed by hand",
     plant_in_session("            m_events.deliver();\n",
                      "            m_owner.lockForWrite();\n            m_events.deliver();\n            m_owner.unlockAfterWrite();\n"),
     "model-lock acquisition(s)"),
    ("a lock handed to a helper in another file", plant_helper_elsewhere, "model-lock acquisition(s) under Source/"),
    ("a call of an asserting function",
     plant_in_session("        const ModelLockHold hold(*m_activeLock);\n        return 0;\n",
                      "        const ModelLockHold hold(*m_activeLock);\n        assertModelLockHeld();\n        return 0;\n"),
     ASSERTING_CALLS),
    ("an escape in the product paid for by a test claim removed", plant_escape_paid_by_a_test, "under Source/"),
)


if __name__ == "__main__":
    raise SystemExit(ModelLockHolds.main())
