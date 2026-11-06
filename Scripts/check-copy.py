#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""check-copy.py — the mechanical part of the writing rules for interface text.

Docs/naming-conventions.md ("Writing") is the rule book: the Apple Style Guide
and the Human Interface Guidelines for the macOS and iOS apps, the GNOME Human
Interface Guidelines for the Linux frontend, and one shared lexicon for both.
Most of it is judgement — whether an error says what happened and what to do —
and no script decides that. What a script can hold is the part a reviewer
misses because it looks right at a glance:

  lexicon      a product or acronym spelled the way its owner does not (ssh,
               Wifi, Macos), a British spelling, or internal jargon (decode,
               transport, payload) in text a person reads;
  typography   a straight apostrophe or quote, three periods for an ellipsis,
               a hyphen between the numbers of a range, an exclamation mark;
  title        a title, button or menu item that is not in title case (Apple)
               or header capitalization (GNOME), found by the API it is
               passed to;
  body         a multi-word alert message, informative text or footer with no
               terminal punctuation;
  term         the other platform's word for a platform concept (Preferences
               on macOS, Settings or Exit on Linux).

Which literals a person reads is decided by where they sit. In the Apple and
Linux frontends, a literal is interface text when it is passed to an API that
shows text, or when it reads as prose (words and spaces) outside a logging,
assertion or test call. In Core, only the files listed in CORE_TEXT_FILES carry
interface text, and every prose literal in them is checked.

A site the rule gets wrong is allowed with a comment on the same line or the
line above: `copy-ok: <reason>`. The reason is required; a bare marker is a
finding of its own.

    Scripts/check-copy.py               # the whole scope
    Scripts/check-copy.py FILE...       # these files, with their directory's rules
    Scripts/check-copy.py --self-test   # every rule catches its planted case

Exit 0 = no finding, 1 = at least one.
"""

import os
import re
import sys

GATE = "check-copy"
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ubq_roots  # noqa: E402
ROOT = ubq_roots.subject_of(__file__)

APPLE_DIRS = ("Source/Platform/macOS", "Source/Platform/iOS", "Source/Platform/Apple")
GNOME_DIRS = ("Source/Platform/Linux",)

# Core's interface text: the word tables and notices a frontend shows as they
# are. A file joins this list when it starts owning a sentence a person reads.
CORE_TEXT_FILES = (
    "Source/Core/Runtime/SessionListWords.h",
    "Source/Core/Runtime/SearchCounterText.h",
    "Source/Core/Session/ConnectRemedy.cpp",
    "Source/Core/Preferences/Vocabulary.h",
    "Source/Core/Security/CapabilityConsentText.h",
    "Source/Core/Security/StructureConsentText.h",
    "Source/Core/Session/ConnectRemedy.h",
    "Source/Core/Session/DeployPolicy.h",
    "Source/Core/Session/AttachTarget.h",
    "Source/Core/Session/ClosePrompt.h",
    "Source/Core/Session/SessionDirectoryCore.cpp",
    "Source/Core/Protocol/Sync/AttachVerdictText.h",
    "Source/Core/Protocol/Sync/Envelope.h",
    "Source/Core/Protocol/Sync/Messages.cpp",
    "Source/Core/Transport/PollChannel.cpp",
    "Source/Core/Session/RemoteSessionCore.cpp",
    "Source/Core/Session/SessionShare.cpp",
    "Source/Core/Session/QUICBootstrapRun.h",
    "Source/Core/Runtime/StructureCalls.h",
    "Source/Core/Runtime/DestructiveConfirmation.h",
    "Source/Core/Session/InputInDoubtNotice.h",
)

# Lanes that own a surface's wording while this gate lands. Each entry names
# the owner; it leaves when that lane lands its rewrite.
EXCLUDED = ()

SOURCE_SUFFIXES = (".swift", ".mm", ".m", ".cpp", ".h", ".c")
SKIP_PARTS = ("/UITest/", "/Tests/", "/Generated/", "/.build/")
# In-app smoke drivers: their strings are verdicts for the harness log.
SKIP_NAME = re.compile(r"Smoke\w*\.(?:swift|mm|cpp)$")

MARKER = re.compile(r"copy-ok:(.*)")

# ---------------------------------------------------------------------------
# Literal extraction

PLACEHOLDER = "\x01"


class Literal:
    __slots__ = ("line", "text", "before", "objc")

    def __init__(self, line, text, before, objc):
        self.line = line
        self.text = text
        self.before = before
        self.objc = objc


def _read_string(source, index, swift):
    """Reads a "..." literal starting after its opening quote.

    Returns (text, end index). A Swift interpolation becomes PLACEHOLDER, with
    any literal inside it skipped.
    """
    out = []
    length = len(source)
    while index < length:
        character = source[index]
        if character == "\\" and index + 1 < length:
            following = source[index + 1]
            if swift and following == "(":
                depth = 1
                index += 2
                while index < length and depth:
                    inner = source[index]
                    if inner == '"':
                        _, index = _read_string(source, index + 1, swift)
                        continue
                    if inner == "(":
                        depth += 1
                    elif inner == ")":
                        depth -= 1
                    index += 1
                out.append(PLACEHOLDER)
                continue
            out.append({"n": "\n", "t": "\t", '"': '"', "\\": "\\", "'": "'"}.get(following, "\\" + following))
            index += 2
            continue
        if character == '"' or character == "\n":
            return "".join(out), index + 1
        out.append(character)
        index += 1
    return "".join(out), index


def literals(source, swift):
    """Every single-line string literal, with the code that precedes it."""
    found = []
    index = 0
    line = 1
    length = len(source)
    code_since_line = []
    while index < length:
        character = source[index]
        if character == "\n":
            line += 1
            index += 1
            code_since_line.append("\n")
            continue
        if source.startswith("//", index):
            end = source.find("\n", index)
            index = length if end < 0 else end
            continue
        if source.startswith("/*", index):
            end = source.find("*/", index + 2)
            end = length if end < 0 else end + 2
            line += source.count("\n", index, end)
            index = end
            continue
        if swift and source.startswith('"""', index):
            end = source.find('"""', index + 3)
            end = length if end < 0 else end + 3
            line += source.count("\n", index, end)
            index = end
            continue
        if not swift and source.startswith('R"', index) and (index == 0 or not source[index - 1].isalnum()):
            open_paren = source.find("(", index)
            delimiter = source[index + 2:open_paren]
            end = source.find(")" + delimiter + '"', open_paren)
            end = length if end < 0 else end + len(delimiter) + 2
            line += source.count("\n", index, end)
            index = end
            continue
        if not swift and character == "'":
            # A character literal, so its quote does not open a string.
            end = index + 1
            while end < length and source[end] != "'" and source[end] != "\n":
                end += 2 if source[end] == "\\" else 1
            index = end + 1
            continue
        if character == '"':
            objc = index > 0 and source[index - 1] == "@"
            before = "".join(code_since_line)[-160:]
            text, end = _read_string(source, index + 1, swift)
            found.append(Literal(line, text, before, objc))
            code_since_line.append('""')
            index = end
            continue
        code_since_line.append(character)
        if len(code_since_line) > 400:
            del code_since_line[:200]
        index += 1
    return found


# ---------------------------------------------------------------------------
# Contexts

def _ends(pattern):
    return re.compile(r"(?:" + pattern + r")\s*@?$", re.S)


# Text whose API makes it a title, button or menu item.
APPLE_TITLE = _ends("|".join((
    r"\bButton\(",
    r"\bMenu\(",
    r"\.navigationTitle\(",
    r"\.alert\(",
    r"\.confirmationDialog\(",
    r"\bCommandMenu\(",
    r"NSMenuItem\(\s*title:",
    r"\bNSMenu\(\s*title:",
    r"\.messageText\s*=",
    r"addButton\(\s*withTitle:",
    r"setMessageText:",
    r"addButtonWithTitle:",
    r"initWithTitle:",
    r"NSButton\(\s*title:",
    r"\bSection\(",
    r"header:\s*\{?\s*Text\(",
    r"ContentUnavailableView\(",
)))
GNOME_TITLE = _ends("|".join((
    r"gtk_window_set_title\([^;]*,",
    r"g_menu_append\(\s*\w+\s*,",
    r"g_menu_item_new\(",
    r"gtk_button_new_with_label\(",
    r"gtk_alert_dialog_new\(",
    r"g_notification_new\(",
    r"gtk_button_set_label\([^;]*,",
    r"adw_[a-z_]*set_title\([^;]*,",
    r"adw_[a-z_]*set_heading\([^;]*,",
    r"adw_[a-z_]*add_response\([^;]*,\s*\"[^\"]*\"\s*,",
)))
CORE_TITLE = _ends(r"\b(?:\w+\.)?(?:title|\w+Title|heading|buttonLabel|menuTitle)\s*=")

# Text whose API makes it a body sentence.
APPLE_BODY = _ends("|".join((
    r"\.informativeText\s*=",
    r"setInformativeText:",
    r"message:\s*\{\s*Text\(",
    r"footer:\s*\{\s*Text\(",
)))
GNOME_BODY = _ends("|".join((
    r"adw_[a-z_]*set_body\([^;]*,",
    r"gtk_alert_dialog_set_detail\([^;]*,",
    r"g_notification_set_body\([^;]*,",
)))
CORE_BODY = _ends(r"\b(?:\w+\.)?(?:body|detail|explanation|sentence|notice)\s*=")

# Text whose API shows it, whatever its shape.
APPLE_SHOWN = _ends("|".join((
    r"\bText\(", r"\bLabel\(", r"\bToggle\(", r"\bTextField\(", r"\bSecureField\(",
    r"\bSection\(", r"\bPicker\(", r"\bLabeledContent\(", r"\.help\(",
    r"accessibilityLabel\(", r"accessibilityHint\(", r"String\(\s*localized:",
    r"NSLocalizedString\(", r"\btitle:", r"\bmessage:", r"\bsubtitle:", r"\bprompt:",
    r"\.toolTip\s*=", r"\.placeholderString\s*=", r"\.stringValue\s*=", r"\.title\s*=",
    r"setTitle:", r"setStringValue:", r"setToolTip:", r"labelWithString:",
)))
GNOME_SHOWN = _ends("|".join((
    r"gtk_label_new\(", r"gtk_label_set_text\([^;]*,", r"gtk_label_set_markup\([^;]*,",
    r"gtk_widget_set_tooltip_text\([^;]*,", r"_\(", r"N_\(",
)))

# Calls whose text is for a developer, never for the person using the app.
DEVELOPER = re.compile(
    r"(?:\b(?:print|debugPrint|fatalError|precondition|preconditionFailure|assert|assertionFailure|"
    r"NSLog|os_log|log|logLine|debugLog|trace|XCTFail|XCTAssert\w*|fprintf|printf|puts|fputs|perror|"
    r"g_warning|g_message|g_debug|g_critical|g_error|g_printerr|g_print|emitLog|writeLog|logError|"
    r"logNotice|UB_LOG\w*|LOG\w*|syslog|diagnostic|Diag\w*|failure|abort|static_assert|check|expect\w*|"
    r"skip\w*|fail|frame|require)\s*\("
    r"|\b(?:logger|Logger|log|signposter)\.\w+\("
    r"|std::cerr\s*<<|std::cout\s*<<|\.debug\(|\.notice\(|\.fault\(|\.warning\(|\.trace\()",
)
# Things that are not prose even when they contain spaces.
CODE_SHAPE = re.compile(r"[{};<>=|\\`$#]|%[0-9.]*[a-z@]|^\s|\s$|^[-/~.]|://|\b\w+\(|\[\w*\]|^\w+:\s|^[A-Z_]{2,}\b")


def _is_prose(text):
    # A literal that begins or ends a line of a longer message is still prose.
    text = text.strip("\n")
    words = re.findall(r"[A-Za-z’'][A-Za-z’'-]*", text)
    if len(words) < 2 or " " not in text:
        return False
    if CODE_SHAPE.search(text.replace(PLACEHOLDER, "X")):
        return False
    first = text.lstrip(PLACEHOLDER + "“\"(")
    return bool(first) and (first[0].isalpha())


# ---------------------------------------------------------------------------
# Rules

# Each entry: (pattern, replacement or message). A pattern runs over the
# literal's text with interpolations replaced by PLACEHOLDER.
LEXICON = (
    (r"(?<![\w./~@-])(?:ssh|Ssh)(?![\w/:@_-]|\.\w)", "SSH"),
    (r"(?<![\w./-])(?:quic|Quic)(?![\w/:_-]|\.\w)", "QUIC"),
    (r"(?<![\w-])(?:wifi|Wifi|WiFi|Wi-fi|wi-fi|WIFI)(?![\w-])", "Wi-Fi"),
    (r"(?<![\w-])(?:iphone|Iphone|IPhone|IPHONE)(?!\w)", "iPhone"),
    (r"(?<![\w-])(?:ipad|Ipad|IPad|IPAD)(?!\w)", "iPad"),
    (r"(?<![\w-])(?:macos|MacOS|MacOs|Macos|MACOS|Mac OS X|OS X)(?!\w)", "macOS"),
    (r"(?<![\w-])(?:IOS|Ios)(?!\w)", "iOS"),
    (r"(?<![\w./-])(?:Tmux|TMUX)(?!\w)", "tmux"),
    (r"(?<![\w./-])(?:tailscale|TailScale)(?![\w.])", "Tailscale"),
    (r"(?<![\w./-])(?:Url|url)(s?)(?![\w.])", "URL"),
    (r"(?<![\w./-])(?:Pty|pty)(s?)(?![\w./])", "PTY"),
    (r"(?<![\w./-])(?:e-mail|E-mail)(?!\w)", "email"),
    (r"(?<![\w./-])(?:Github|github)(?![\w.])", "GitHub"),
    (r"(?<![\w./-])(?:utf-8|utf8|Utf-8|UTF8)(?!\w)", "UTF-8"),
    (r"(?<![\w./-])(?:json|Json)(?![\w.])", "JSON"),
    (r"(?<![\w./-])(?:gpu|Gpu)(s?)(?!\w)", "GPU"),
    (r"(?<![\w./-])(?:cpu|Cpu)(s?)(?!\w)", "CPU"),
    (r"(?<![\w./-])(?:Ok|ok)(?=[.!]?$)", "OK"),
)
BRITISH = (
    (r"\b([Cc])olour", r"\1olor"), (r"\b([Bb])ehaviour", r"\1ehavior"),
    (r"\b([Ll])icence", r"\1icense"), (r"\b([Cc])ancell(ed|ing)", r"\1ancel\2"),
    (r"\b([Ff])avourite", r"\1avorite"), (r"\b([Cc])entre", r"\1enter"),
    (r"\b([Gg])rey", r"\1ray"), (r"\b([Cc])atalogue", r"\1atalog"),
    (r"\b([Dd])ialogue", r"\1ialog"), (r"\b([Ll])abell(ed|ing)", r"\1abel\2"),
    (r"\b([Mm])odell(ed|ing)", r"\1odel\2"), (r"\b([Tt])ravell(ed|ing)", r"\1ravel\2"),
    (r"\b([Dd])iall(ed|ing)", r"\1ial\2"), (r"\b([Ss])ignall(ed|ing)", r"\1ignal\2"),
    (r"\b(\w*?(?:[Oo]rgan|[Ii]nitial|[Rr]ecogn|[Aa]uthor|[Mm]inim|[Mm]axim|[Cc]ustom|"
     r"[Ss]ynchron|[Oo]ptim|[Nn]ormal|[Pp]rioriti|[Aa]pologi|[Rr]eali|[Ss]ummari))is(e|ed|es|ing|ation)\b",
     r"\1iz\2"),
    (r"\b([Aa]naly)s(e|ed|es|ing)\b", r"\1z\2"),
)
JARGON = re.compile(
    r"(?<![\w-])(decod(?:e|ed|es|ing)|transport|payload|enum|nil|null|errno|EOF|handshake|frame body|socket|carrier|bootstrap|daemon)(?![\w-])",
    re.I)

APPLE_TERMS = ((re.compile(r"\bPreferences\b"), "Settings (macOS and iOS name the window Settings)"),)
GNOME_TERMS = (
    (re.compile(r"\bSettings\b"), "Preferences (GNOME names an app's options Preferences)"),
    (re.compile(r"^Exit\b"), "Quit (GNOME quits an app)"),
)

# Words title case leaves lowercase unless first or last. Apple keeps
# prepositions of four or fewer letters lowercase; GNOME, three or fewer.
SMALL_COMMON = {"a", "an", "the", "and", "but", "or", "nor", "for", "yet", "so", "as", "at", "by",
                "in", "of", "on", "to", "via", "per", "vs", "vs."}
# Up, Off and Out are left out: as verb particles ("Set Up", "Turn Off") both
# guides capitalize them, and a title rarely uses them as prepositions.
APPLE_SMALL = SMALL_COMMON | {"from", "into", "onto", "over", "with"}
GNOME_SMALL = SMALL_COMMON

# Spellings that are lowercase on purpose.
LOWERCASE_NAMES = {"ubq", "tmux", "macOS", "iOS", "iPhone", "iPad", "iPadOS", "ubiquitty-host", "sRGB",
                   "known_hosts", "lrzsz", "rz", "sz", "zsh", "bash", "vim", "x", "px", "pt", "ms", "s"}


def _title_findings(text, small):
    """The words of a title that title case would capitalize."""
    # A literal that starts or ends with a space continues into a value, so
    # its edge word is not the title's.
    text = ("X " if text[:1].isspace() else "") + text + (" X" if text[-1:].isspace() else "")
    words = [w for w in re.split(r"[\s/]+", text.replace(PLACEHOLDER, " X ")) if w]
    tokens = [w.strip("“”\"()[],:;!?…") for w in words]
    tokens = [t for t in tokens if t]
    wrong = []
    for position, token in enumerate(tokens):
        if not token[0].isalpha() or token[0].isupper():
            continue
        if token in LOWERCASE_NAMES or any(c.isupper() for c in token[1:]) or re.search(r"[\d._~@/-]", token):
            continue
        edge = position == 0 or position == len(tokens) - 1
        if edge or token.lower() not in small:
            wrong.append(token)
    return wrong


def _title_case(text, small):
    tokens = re.split(r"(\s+)", text)
    words = [i for i, t in enumerate(tokens) if t.strip()]
    for order, index in enumerate(words):
        token = tokens[index]
        bare = token.strip("“”\"()[],:;!?…")
        if not bare or bare in LOWERCASE_NAMES or not bare[0].isalpha() or any(c.isupper() for c in bare[1:]):
            continue
        edge = order == 0 or order == len(words) - 1
        if edge or bare.lower() not in small:
            tokens[index] = token.replace(bare, bare[0].upper() + bare[1:], 1)
        else:
            tokens[index] = token.replace(bare, bare.lower(), 1)
    return "".join(tokens)


def _shown(text):
    return text.replace(PLACEHOLDER, "…")


class RuleSet:
    def __init__(self, name, title, body, shown, small, terms, core):
        self.name = name
        self.title = title
        self.body = body
        self.shown = shown
        self.small = small
        self.terms = terms
        self.core = core


APPLE = RuleSet("apple", APPLE_TITLE, APPLE_BODY, APPLE_SHOWN, APPLE_SMALL, APPLE_TERMS, False)
GNOME = RuleSet("gnome", GNOME_TITLE, GNOME_BODY, GNOME_SHOWN, GNOME_SMALL, GNOME_TERMS, False)
# Core text reaches both platforms, so a title has to satisfy both: a word
# either guide capitalizes is capitalized, and neither platform's word for
# its options window is used.
CORE = RuleSet("core", CORE_TITLE, CORE_BODY, None, GNOME_SMALL, (), True)


def check_literal(literal, rules, sentence=False):
    """Findings for one literal: (rule, message)."""
    text = literal.text
    if not text.strip():
        return []
    before = literal.before
    tail = before[-120:]
    is_title = bool(rules.title.search(tail))
    is_body = bool(rules.body.search(tail))
    is_shown = is_title or is_body or (rules.shown is not None and bool(rules.shown.search(tail)))
    prose = _is_prose(text)
    if not (is_shown or prose):
        return []
    if not is_title and not is_body and DEVELOPER.search(before[-100:]):
        return []
    if not is_shown and not rules.core and not prose:
        return []
    if is_shown and not prose and CODE_SHAPE.search(text.replace(PLACEHOLDER, "X")) and not is_title:
        return []
    findings = []
    for pattern, replacement in LEXICON:
        for match in re.finditer(pattern, text):
            findings.append(("lexicon", f"“{match.group(0)}” is spelled {replacement}"))
    for pattern, replacement in BRITISH:
        for match in re.finditer(pattern, text):
            findings.append(("lexicon", f"“{match.group(0)}” is US English: {re.sub(pattern, replacement, match.group(0))}"))
    for match in JARGON.finditer(text):
        findings.append(("lexicon", f"“{match.group(0)}” is an internal word; say what the person sees"))
    if re.search(r"[A-Za-z]'[A-Za-z]|s'(?=\s|$)", text):
        findings.append(("typography", "straight apostrophe; use ’"))
    if re.search(r'"', text):
        findings.append(("typography", "straight double quote; use “ and ”"))
    if "..." in text:
        findings.append(("typography", "three periods; use the ellipsis character …"))
    if re.search(r"(?<![\w.-])\d+-\d+(?![\w.-])", text) and prose:
        findings.append(("typography", "hyphen in a range; use an en dash (–)"))
    if re.search(r"[A-Za-z)]!(?:\s|$)", text):
        findings.append(("typography", "exclamation mark"))
    for pattern, message in rules.terms:
        if pattern.search(text) and (is_title or prose or is_shown):
            findings.append(("term", f"say {message}"))
    # A title that is a complete sentence takes sentence-style capitalization
    # and its own punctuation (HIG, Alerts); only its first letter is asked.
    sentence = sentence or bool(re.search(r"[.?]$", text) and not text.endswith("…") and len(text.split()) > 2)
    if is_title and sentence:
        first = text.lstrip(PLACEHOLDER + "“\"( ")
        if first and first[0].islower() and first.split()[0] not in LOWERCASE_NAMES and not text[:1].isspace():
            findings.append(("title", "a title starts with a capital letter"))
    elif is_title:
        wrong = _title_findings(text, rules.small)
        if wrong:
            findings.append(("title", f"title case capitalizes {', '.join(wrong)}: “{_shown(_title_case(text, rules.small))}”"))
        if re.search(r"[.]$", text) and not text.endswith("…") and len(text.split()) > 1:
            findings.append(("title", "a title fragment has no terminal period"))
    if is_body and len(text.split()) >= 3 and not re.search(r"[.?!…:”)]$|" + PLACEHOLDER + "$", text):
        findings.append(("body", "a body sentence ends with terminal punctuation"))
    return findings


def rules_for(relative):
    if any(relative.startswith(prefix) for prefix in EXCLUDED):
        return None
    if any(part in "/" + relative for part in SKIP_PARTS) or SKIP_NAME.search(relative):
        return None
    if relative in CORE_TEXT_FILES:
        return CORE
    if relative.startswith(APPLE_DIRS):
        return APPLE
    if relative.startswith(GNOME_DIRS):
        return GNOME
    return None


def check_source(source, rules, swift):
    """Findings in one file's text: (line, rule, message, literal)."""
    lines = source.split("\n")
    findings = []
    found_literals = literals(source, swift)
    for position, literal in enumerate(found_literals):
        # A title assembled from pieces is a sentence when its last piece on
        # the same line ends one.
        rest = [other for other in found_literals[position + 1:] if other.line == literal.line]
        sentence = bool(rest) and literal.text[-1:].isspace() and bool(re.search(r"[.?]$", rest[-1].text))
        found = check_literal(literal, rules, sentence)
        if not found:
            continue
        # The marker sits on the literal's line or up to three lines above,
        # so it can precede a call whose arguments wrap.
        nearby = lines[max(0, literal.line - 4):literal.line]
        marker = next((m for m in (MARKER.search(text) for text in reversed(nearby)) if m), None)
        if marker:
            if not marker.group(1).strip():
                findings.append((literal.line, "marker", "copy-ok needs a reason", literal.text))
            continue
        for rule, message in found:
            findings.append((literal.line, rule, message, literal.text))
    return findings


def scope_files():
    files = list(CORE_TEXT_FILES)
    for directory in APPLE_DIRS + GNOME_DIRS:
        for base, _, names in os.walk(os.path.join(ROOT, directory)):
            for name in names:
                if name.endswith(SOURCE_SUFFIXES):
                    files.append(os.path.relpath(os.path.join(base, name), ROOT))
    return sorted(set(files))


def run(paths):
    total = []
    checked = 0
    for relative in paths:
        rules = rules_for(relative)
        if rules is None:
            continue
        absolute = os.path.join(ROOT, relative)
        if not os.path.isfile(absolute):
            continue
        with open(absolute, encoding="utf-8", errors="replace") as handle:
            source = handle.read()
        checked += 1
        for line, rule, message, text in check_source(source, rules, relative.endswith(".swift")):
            total.append(f"  {relative}:{line}: {rule} ({rules.name}): {message} — \"{_shown(text)}\"")
    return checked, total


# ---------------------------------------------------------------------------
# Self-test

SELF_TEST = (
    # (rule set, swift, source, rules expected; empty = clean)
    (APPLE, True, 'Text("Couldn’t connect over ssh.")', {"lexicon"}),
    (APPLE, True, 'Text("Join the Wifi network first.")', {"lexicon"}),
    (APPLE, True, 'Text("Open this on your Iphone.")', {"lexicon"}),
    (APPLE, True, 'Text("The body did not decode.")', {"lexicon"}),
    (APPLE, True, 'Text("Choose a colour.")', {"lexicon"}),
    (CORE, False, 'reason += "\\nThe requester\'s process is gone.";', {"typography"}),
    (APPLE, True, 'Text("Dialling the host again.")', {"lexicon"}),
    (APPLE, True, 'Text("The bridge refused the bootstrap.")', {"lexicon"}),
    (APPLE, True, 'Section("Saved hosts") { }', {"title"}),
    (APPLE, True, 'Button("Set Up") { }', set()),
    (APPLE, True, 'Button("Set up") { }', {"title"}),
    (APPLE, True, 'Text("Can\'t reach the host.")', {"typography"}),
    (APPLE, True, 'Text("Connecting...")', {"typography"}),
    (APPLE, True, 'Text("Rows 1-24 are kept.")', {"typography"}),
    (APPLE, True, 'Text("Connected to the host!")', {"typography"}),
    (APPLE, True, 'Text("Press \\"Connect\\" to start.")', {"typography"}),
    (APPLE, True, 'Button("close window") { }', {"title"}),
    (APPLE, True, 'Button("Open in New Tab") { }', set()),
    (APPLE, True, 'Button("Open in new tab") { }', {"title"}),
    (APPLE, True, '.navigationTitle("workspaces")', {"title"}),
    (APPLE, True, '.alert("Can’t Reach \\(host)", isPresented: $shown) { }', set()),
    (APPLE, True, 'alert.messageText = "Can’t reach host"', {"title"}),
    (APPLE, True, 'alert.messageText = "The arrangement couldn’t be saved."', set()),
    (APPLE, True, 'alert.messageText = "the arrangement couldn’t be saved."', {"title"}),
    (CORE, False, 'notice.title = "A person on " + called + " declined.";', set()),
    (APPLE, True, 'alert.informativeText = "The host closed the connection"', {"body"}),
    (APPLE, True, 'alert.informativeText = "The host closed the connection."', set()),
    (APPLE, True, '} message: { Text("Try again in a moment") }', {"body"}),
    (APPLE, True, 'NSMenuItem(title: "Open Preferences…", action: nil, keyEquivalent: "")', {"term"}),
    (APPLE, True, 'logger.error("ssh couldn\'t reach the host...")', set()),
    (APPLE, True, 'let argument = "ssh"', set()),
    (APPLE, True, 'let url = "ssh://host/path"', set()),
    (APPLE, True, 'Text("Run `ubq attach` in the shell.")', set()),
    (APPLE, True, 'Text("Ready over SSH, QUIC, and Wi-Fi on your iPhone.")', set()),
    (APPLE, True, 'Text("ssh failed.") // copy-ok: names the ssh(1) program', set()),
    (APPLE, True, '// copy-ok:\nText("ssh failed.")', {"marker"}),
    (APPLE, False, 'alert.messageText = @"close the window";', {"title"}),
    (APPLE, False, '[alert setInformativeText:@"Ubiquitty couldn\'t start"];', {"body", "typography"}),
    (GNOME, False, 'g_menu_append(menu, "Check for updates…", "app.check-update");', {"title"}),
    (GNOME, False, 'g_menu_append(menu, "Connect With SSH", "app.connect");', set()),
    (GNOME, False, 'g_menu_append(menu, "Connect with SSH", "app.connect");', {"title"}),
    (GNOME, False, 'g_menu_append(menu, "Settings", "app.preferences");', {"term"}),
    (GNOME, False, 'gtk_label_set_text(GTK_LABEL(label), "Searching...");', {"typography"}),
    (GNOME, False, 'g_warning("ssh couldn\'t reach the host...");', set()),
    (GNOME, False, 'gtk_window_set_title(GTK_WINDOW(window), "Processes");', set()),
    (CORE, False, 'words.title = "Can’t reach " + host;', {"title"}),
    (CORE, False, 'words.title = "No Workspaces on " + host;', set()),
    (CORE, False, 'return "ssh couldn’t reach the host.";', {"lexicon"}),
    (CORE, False, 'return "The host refused the connection.";', set()),
    (CORE, False, 'notice = "The host closed the connection";', {"body"}),
    (CORE, False, "const char c = '\"'; return \"Choose a workspace.\";", set()),
)


def self_test():
    failures = 0
    hits = set()
    for rules, swift, source, expected in SELF_TEST:
        got = {rule for _, rule, _, _ in check_source(source, rules, swift)}
        hits |= got
        if got != expected:
            failures += 1
            print(f"  FAIL  [{rules.name}] {source!r}: reported {sorted(got)}, expected {sorted(expected)}")
    for rule in ("lexicon", "typography", "title", "body", "term", "marker"):
        if rule not in hits:
            failures += 1
            print(f"  FAIL  no case exercises the {rule} rule")
    if not scope_files():
        failures += 1
        print("  FAIL  the scope found no files; the scan is broken, not clean")
    if failures:
        print(f"{GATE} --self-test: FAILED ({failures} of {len(SELF_TEST)} case(s))")
        return 1
    print(f"{GATE} --self-test: ok ({len(SELF_TEST)} case(s))")
    return 0


def main(arguments):
    if arguments and arguments[0] in ("--self-test", "--selftest"):
        return self_test()
    if self_test_quiet() != 0:
        print(f"{GATE}: FAIL — the rules do not pass their own self-test; run {GATE}.py --self-test")
        return 1
    paths = [os.path.relpath(os.path.abspath(p), ROOT) for p in arguments] if arguments else scope_files()
    checked, findings = run(paths)
    if not findings:
        print(f"{GATE}: ok ({checked} file(s), interface text follows Docs/naming-conventions.md#writing)")
        return 0
    print(f"{GATE}: FAILED — {len(findings)} finding(s) in interface text "
          f"(Docs/naming-conventions.md#writing; allow a site with `copy-ok: <reason>`)")
    for finding in findings:
        print(finding)
    return 1


def self_test_quiet():
    for rules, swift, source, expected in SELF_TEST:
        if {rule for _, rule, _, _ in check_source(source, rules, swift)} != expected:
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
