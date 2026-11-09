#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""check-model-lock-holds — a frontend call that waits on the parser fails here.

The model lock is taken by the drain at every parse bite and by a frontend
thread whenever it reads or writes the model. Each frontend acquisition is a
wait on the parser, and the regressions that froze the screen under output were
each one new acquisition on a path a frontend runs per keypress, per delivery
or per display tick: a plain `std::scoped_lock` in `encodeKey` (7167274cb) and
`noteUserActivity` (039a29b39), one per delivery in the selection's update after
output (e0840e52f), and a tick that declined its frame because a selection
stood (07ac11054, 48d144718). Clang's thread-safety analysis proves that the
model is touched only under the lock; it cannot say that a frontend should not
take it. This gate holds the number of places that do.

It counts, in Source/Core/Runtime/TerminalSession.h:

- every acquisition of the model lock in the body of a public member of
  `TerminalSession`, the members a frontend calls (`ModelLockHold`,
  `lockForRead`, `tryLockForRead`, `ModelReadScope`, a plain std lock or a
  `lock()`/`try_lock()` on the session's lock);
- every entry of Scripts/frame-path-blocking.txt, the per-tick calls that still
  wait on the lock and so carry no `UBIQUITTY_NONBLOCKING`.

Their sum is held to the ledger's `budget model-lock-holds`, two-sided: a new
acquisition fails, and one removed must lower the budget in the same commit.

It refuses outright:

- a std lock (`std::scoped_lock`, `std::unique_lock`, `std::lock_guard`) over a
  model lock in a first-party header or source, beyond the ledger's
  `budget plain-model-lock-holds`: it takes the lock and grants the analysis
  nothing, which is the spelling a regression reaches for first;
- a per-tick call, one the display-free cadence test's frontend makes
  (Tests/Core/Runtime/test_frame_cadence.cpp), that is neither declared
  `UBIQUITTY_NONBLOCKING` nor listed in Scripts/frame-path-blocking.txt, and an
  entry there that names no per-tick call or names a marked one;
- a branch in the frame decision (`takeFrame` and the declines it asks:
  `frameHold`, `shouldSkipLiveReadForSaturation`) whose condition reads a
  selection: the frame a view presents never depends on what it selects;
- a `UBIQUITTY_NO_THREAD_SAFETY_ANALYSIS` beyond the ledger's
  `budget thread-safety-escapes`.

What it cannot see: an acquisition made in a private helper that a public
member calls (the analysis and the cadence test hold that), a lock reached
through an alias it does not name, and Swift. It reads text, not types.

    ./Scripts/check-model-lock-holds.py
    ./Scripts/check-model-lock-holds.py --self-test
"""

import os
import pathlib
import re
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gate_framework import Gate
import ubq_roots

SESSION = pathlib.Path("Source/Core/Runtime/TerminalSession.h")
CADENCE = pathlib.Path("Tests/Core/Runtime/test_frame_cadence.cpp")
ALLOWLIST = pathlib.Path("Scripts/frame-path-blocking.txt")
SOURCE_ROOTS = ("Source",)
SOURCE_SUFFIXES = (".h", ".cpp", ".mm")

#: The files that implement the lock and its escape, which name it without
#: holding it as a frontend does.
LOCK_DEFINITIONS = {"Source/Core/ModelLock.h", "Source/Base/ThreadSafety.h"}

#: The ledger budgets this gate holds the tree to.
BUDGETS = ("model-lock-holds", "plain-model-lock-holds", "thread-safety-escapes")

#: The frame decision: the per-tick answer and the declines it asks.
FRAME_DECISION = ("takeFrame", "frameHold", "shouldSkipLiveReadForSaturation")

#: Names a model lock is reached by in first-party code.
MODEL_LOCK_OPERAND = r"(?:m_activeLock|m_modelLock|modelLockPointer\s*\(\s*\)|m_search\.lock|primaryLock|lockHandle\s*\(\s*\)|nativeHandle\s*\(\s*\)|\bmodelLock\b)"

HOLD = re.compile(
    r"\bModelLockHold\s+\w+\s*[({]"
    r"|\blockForRead\s*\(\s*\)"
    r"|\btryLockForRead\s*\(\s*\)"
    r"|\bModelReadScope\s+\w+\s*[({]"
    r"|\bmodelReadScope\s*\(\s*\)"
    r"|\bstd::(?:scoped_lock|unique_lock|lock_guard)\b[^;]*" + MODEL_LOCK_OPERAND +
    r"|(?:m_activeLock|m_modelLock)\s*(?:->|\.)\s*(?:lock|try_lock)\s*\(")

PLAIN_HOLD = re.compile(r"\bstd::(?:scoped_lock|unique_lock|lock_guard)\b[^;]*" + MODEL_LOCK_OPERAND)

ESCAPE = re.compile(r"\bUBIQUITTY_NO_THREAD_SAFETY_ANALYSIS\b")

NONBLOCKING = "UBIQUITTY_NONBLOCKING"

#: A frontend call in the cadence test's tick: `m_session.name(`.
CADENCE_CALL = re.compile(r"\bm_session\s*\.\s*(\w+)\s*\(")


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


class Member:
    """A member function defined in a class body: its name, access, head and body."""

    def __init__(self, name, access, head, body_start, body_end):
        self.name = name
        self.access = access
        self.head = head
        self.body_start = body_start
        self.body_end = body_end


def members(code, class_name):
    """Every member function defined inside `class <class_name> { ... }`.

    Nested types are stepped over whole, so a member of a nested class is not
    taken for one of this class.
    """
    match = re.search(r"\bclass\s+(?:\[\[[^\]]*\]\]\s*)?(?:\w+\s+)*%s\b[^;{]*\{" % re.escape(class_name), code)
    if not match:
        return []
    begin = match.end() - 1
    end = closing(code, begin)
    access = "private"
    found = []
    index = begin + 1
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
            head = code[statement:index]
            is_type = re.match(r"\s*(?:template\s*<[^>]*>\s*)?(?:class|struct|union|enum)\b", head) is not None
            names = [name for name in NAME_BEFORE_PARAMETERS.finditer(head)
                     if name.group(1) not in NOT_A_NAME and not name.group(1).startswith("UBIQUITTY_")]
            if names and not is_type and "=" not in head[:names[0].start()]:
                found.append(Member(names[0].group(1), access, head, index, body_end))
            index = body_end + 1
            statement = index
            continue
        index += 1
    return found


def declared_nonblocking(code, class_name):
    """The names of the class's members whose declaration carries UBIQUITTY_NONBLOCKING."""
    names = set()
    for member in members(code, class_name):
        if NONBLOCKING in member.head:
            names.add(member.name)
    return names


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

def frontend_holds(session_code):
    """[(member, count)] for each public TerminalSession member that takes the lock."""
    result = []
    for member in members(session_code, "TerminalSession"):
        if member.access != "public":
            continue
        count = len(HOLD.findall(session_code[member.body_start:member.body_end]))
        if count:
            result.append((member.name, count))
    return result


def allowlist_entries(text):
    """{name: reason} from the allowlist's `<member> <reason>` lines."""
    entries = {}
    for line in text.split("\n"):
        words = line.split("#", 1)[0].split(None, 1)
        if words:
            entries[words[0]] = words[1].strip() if len(words) > 1 else ""
    return entries


def per_tick_calls(cadence_code):
    """The TerminalSession members the cadence test's frontend calls at its tick."""
    return {match.group(1) for match in CADENCE_CALL.finditer(cadence_code)}


def judge(root, budgets):
    """[(location, message)] for every finding over the tree at `root`."""
    findings = []
    session_path = root / SESSION
    if not session_path.is_file():
        return [(str(SESSION), "is missing, so nothing was counted")], {}
    session_code = blanked(session_path.read_text(encoding="utf-8", errors="replace"))
    allowlist_path = root / ALLOWLIST
    allowlist = allowlist_entries(allowlist_path.read_text(encoding="utf-8")) if allowlist_path.is_file() else {}
    cadence_path = root / CADENCE
    ticks = per_tick_calls(blanked(cadence_path.read_text(encoding="utf-8", errors="replace"))) if cadence_path.is_file() else set()

    holds = frontend_holds(session_code)
    hold_count = sum(count for _, count in holds)
    counts = {"holds": hold_count, "allowlisted": len(allowlist)}

    # The ceiling: frontend acquisitions plus the per-tick calls still waiting.
    total = hold_count + len(allowlist)
    counts["model-lock-holds"] = total
    findings += against(str(SESSION), total, budgets.get("model-lock-holds"),
        "model-lock acquisition(s) in frontend members plus per-tick call(s) still blocking")

    # Per-tick calls: marked, or listed with a reason.
    session_members = {member.name for member in members(session_code, "TerminalSession")}
    marked = declared_nonblocking(session_code, "TerminalSession")
    tick_members = {name for name in ticks if name in session_members}
    for name in sorted(tick_members - marked - set(allowlist)):
        findings.append((str(CADENCE), f"per-tick call `{name}` is neither {NONBLOCKING} nor listed in {ALLOWLIST}"))
    for name in sorted(set(allowlist) - tick_members):
        findings.append((str(ALLOWLIST), f"`{name}` is not a call the cadence test's frontend makes; delete the entry"))
    for name in sorted(set(allowlist) & marked):
        findings.append((str(ALLOWLIST), f"`{name}` is {NONBLOCKING}; delete the entry"))
    for name, reason in sorted(allowlist.items()):
        if not reason:
            findings.append((str(ALLOWLIST), f"`{name}` names no reason"))

    # The frame decision never branches on a selection.
    for member in members(session_code, "TerminalSession"):
        if member.name not in FRAME_DECISION:
            continue
        for condition in conditions(session_code[member.body_start:member.body_end]):
            if re.search(r"selection", condition, re.IGNORECASE):
                findings.append((str(SESSION), f"`{member.name}` branches on a selection: {' '.join(condition.split())[:120]}"))

    # Plain std locks over a model lock, and analysis escapes, across the tree.
    plain = 0
    escapes = 0
    for directory in SOURCE_ROOTS:
        for path in sorted((root / directory).rglob("*")):
            if path.suffix not in SOURCE_SUFFIXES or not path.is_file():
                continue
            relative = path.relative_to(root).as_posix()
            if relative in LOCK_DEFINITIONS:
                continue
            code = blanked(path.read_text(encoding="utf-8", errors="replace"))
            plain += len(PLAIN_HOLD.findall(code))
            escapes += len(ESCAPE.findall(code))
    counts["plain-model-lock-holds"] = plain
    counts["thread-safety-escapes"] = escapes
    findings += against("Source", plain, budgets.get("plain-model-lock-holds"),
        "std lock(s) over a model lock, which grant the thread-safety analysis nothing")
    findings += against("Source", escapes, budgets.get("thread-safety-escapes"),
        "UBIQUITTY_NO_THREAD_SAFETY_ANALYSIS escape(s)")
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
    subject = "TerminalSession.h, the per-tick allowlist and the sources this reads"

    def examine(self):
        root = pathlib.Path(self.root)
        ledger = ubq_roots.ledger(str(root))[0]
        budgets = {name: ledger.get(name) for name in BUDGETS}
        findings, counts = judge(root, budgets)
        if findings:
            for location, message in findings:
                self.finding(location, message)
            return self.failed("model-lock finding(s)", [message for _, message in findings])
        return self.ok(f"{counts['holds']} frontend acquisition(s) and {counts['allowlisted']} blocking per-tick call(s)"
                       f" at budget {counts['model-lock-holds']}, {counts['plain-model-lock-holds']} plain std lock(s),"
                       f" {counts['thread-safety-escapes']} analysis escape(s)")

    def selfTest(self):
        """Each historical shape, planted into a tree the gate passes, fails it."""
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
    ModelLock* m_activeLock = &m_modelLock;
};
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

FIXTURE_ALLOWLIST = """# member reason
takeEventFlags polls the pane link under the model lock
updateSelectionAfterOutput try-locks the model to judge the selection after output
"""

FIXTURE_BUDGETS = {"model-lock-holds": 3, "plain-model-lock-holds": 0, "thread-safety-escapes": 0}


def write_fixture(root):
    (root / SESSION).parent.mkdir(parents=True, exist_ok=True)
    (root / SESSION).write_text(FIXTURE_SESSION, encoding="utf-8")
    (root / CADENCE).parent.mkdir(parents=True, exist_ok=True)
    (root / CADENCE).write_text(FIXTURE_CADENCE, encoding="utf-8")
    (root / ALLOWLIST).parent.mkdir(parents=True, exist_ok=True)
    (root / ALLOWLIST).write_text(FIXTURE_ALLOWLIST, encoding="utf-8")


def replace_in(path, old, new):
    text = path.read_text(encoding="utf-8")
    assert old in text, old
    path.write_text(text.replace(old, new, 1), encoding="utf-8")


def plant_selection_hold(root):
    """e0840e52f: the per-delivery selection update takes a plain hold."""
    replace_in(root / SESSION, "        return Selection::updateAfterOutput(*selection, viewOffset);",
               "        const std::scoped_lock hold(*m_activeLock);\n"
               "        return Selection::updateAfterOutput(*selection, viewOffset);")


def plant_selection_decline(root):
    """07ac11054, 48d144718: the tick declines its frame while a selection stands."""
    replace_in(root / SESSION, "        supply = {};\n",
               "        supply = {};\n"
               "        if (demand.selection && Selection::hasSelection(*demand.selection)) {\n"
               "            supply.decline = FrameDecline::AwaitingCapture;\n"
               "            return nullptr;\n"
               "        }\n")


def plant_keypress_hold(root):
    """7167274cb, 039a29b39: the keypress path waits for the model lock."""
    replace_in(root / SESSION, "    KeyEncoding sendKey(const KeyPress& press) UBIQUITTY_NONBLOCKING\n    {\n",
               "    KeyEncoding sendKey(const KeyPress& press)\n    {\n        const ModelLockHold hold(*m_activeLock);\n")


def plant_analysis_escape(root):
    """A hold hidden from the analysis instead of taken through ModelLockHold."""
    replace_in(root / SESSION, "    std::uint32_t takeEventFlags()\n",
               "    std::uint32_t takeEventFlags() UBIQUITTY_NO_THREAD_SAFETY_ANALYSIS\n")


PLANTS = (
    ("a plain scoped_lock in the per-delivery selection update", plant_selection_hold, "std lock(s) over a model lock"),
    ("a per-tick frame decline that depends on a selection", plant_selection_decline, "branches on a selection"),
    ("a blocking lock in a keypress path", plant_keypress_hold, "model-lock acquisition(s)"),
    ("the keypress path's per-tick call unmarked", plant_keypress_hold, "neither UBIQUITTY_NONBLOCKING"),
    ("an analysis escape", plant_analysis_escape, "UBIQUITTY_NO_THREAD_SAFETY_ANALYSIS escape(s)"),
)


if __name__ == "__main__":
    raise SystemExit(ModelLockHolds.main())
