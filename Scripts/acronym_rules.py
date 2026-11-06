#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""acronym_rules — the naming rules check-acronyms applies, as data.

Each rule is a message and a predicate over one line of stripped source. They
live apart from the scanner so the differential harness can enumerate them and
so a rule reads as a rule rather than as a line of a loop.

ASCII semantics throughout. The perl this replaces read bytes and used perl's
default \\b and \\w, which are [A-Za-z0-9_] with no `use utf8` in force; python's
str patterns would otherwise treat a letter like `é` as a word character and
move a word boundary the perl never moved.

Contraction policy: our own identifiers may contract to Ptr and Ref only
(smart-pointer-style names — RefPtr-era WebKit convention). Every other
contraction (Attr/Num/Err/…) is flagged unless the offending identifier is on
the IS_LIBRARY allowlist — Apple/vendor symbols we cannot rename. New library
symbols go in that list, never as inline skips.
"""

import re

ASCII = re.ASCII

# A contracted suffix (Attr/Num/Err/Idx/…) is permitted only when the whole
# identifier is an external Apple/vendor library symbol we cannot rename.
IS_LIBRARY = frozenset(("noErr", "paramErr", "OSStatus", "GPid", "PangoLogAttr", "leftAlt", "rightAlt", "keyboardLeftAlt", "keyboardRightAlt",
                        # VK_KHR_external_memory_fd's entry point, its pointer type and its info struct.
                        "vkGetMemoryFdKHR", "PFN_vkGetMemoryFdKHR", "VkMemoryGetFdInfoKHR"))


def _all_library(line, tokenPattern):
    """Whether every identifier on the line ending in the contraction is vendor.

    A line mixing a library name with a real violation still flags the
    violation.
    """
    hits = re.findall(r"\b(" + tokenPattern + r")\b", line, ASCII)
    if not hits:
        return False
    return all(hit in IS_LIBRARY for hit in hits)


# Every rule's primary pattern, collected as it is built. Their union is the
# first pass at the foot of this file: a rule is its primary pattern, or that
# pattern narrowed, so a line the pass rejects can trip no rule.
_PRIMARY = []


def _primary(pattern):
    _PRIMARY.append(pattern)
    return pattern


def _hits(pattern):
    compiled = re.compile(_primary(pattern), ASCII)
    return lambda line: bool(compiled.search(line))


def _hits_unless(pattern, forbidden):
    compiled = re.compile(_primary(pattern), ASCII)
    excluded = re.compile(forbidden, ASCII)
    return lambda line: bool(compiled.search(line)) and not excluded.search(line)


def _hits_unless_library(pattern, tokenPattern):
    """Lines matching, minus those whose every match is a name we cannot rename.

    The primary pattern is registered here rather than inside the returned
    test: the first pass at the foot of this file is the union of the patterns
    collected while the rules are built, and a pattern registered when a line
    is scanned is one the pass was assembled without.

    - Parameters:
      - pattern: the rule's primary pattern.
      - tokenPattern: the identifiers to weigh against IS_LIBRARY.
    - Returns: the rule's test.
    """
    compiled = re.compile(_primary(pattern), ASCII)
    return lambda line: (bool(compiled.search(line))
                         and not _all_library(line, tokenPattern))


_primary('\\bcp\\b')


def _codepoint_abbreviation(line):
    """`cp` used as a codepoint, minus hex literals and arithmetic."""
    if not re.search(r"\bcp\b", line, ASCII):
        return False
    if not re.search(r"(?:uint32_t|UInt32|codepoint|CodePoint)", line, ASCII):
        return False
    if re.search(r"\bcodePoint\b", line, ASCII):
        return False
    return not re.search(r"\b(?:cp\s*=\s*0[xX]|cp\s*[+\-\*\/])", line, ASCII)


_primary('(?<=[a-zA-Z])odepoint\\b')


def _codepoint_spelling(line):
    """`codepoint` with a lowercase p, minus HarfBuzz's own field.

    HarfBuzz names hb_glyph_info_t's field `codepoint` although after shaping it
    holds a GLYPH index. It is a vendor field and cannot be renamed, so a member
    access on a line that also names the library is not ours to fix. Keyed on
    both the access and `hb_` so an identifier of our own spelled `codepoint`
    still flags, on this line and every other.
    """
    if not re.search(r"(?<=[a-zA-Z])odepoint\b", line, ASCII):
        return False
    isHarfBuzzField = (re.search(r"[.>]codepoint\b", line, ASCII)
                       and re.search(r"\bhb_", line, ASCII))
    return not isHarfBuzzField


_primary('(?<=[A-Za-z])Num\\b')


def _num(line):
    return (bool(re.search(r"(?<=[A-Za-z])Num\b", line, ASCII))
            and not _all_library(line, r"\w*Num"))


_primary('(?<=[A-Za-z])Attrs?\\b|\\battrs?\\b')


def _attr(line):
    """Attr/Attrs/attr/attrs → Attribute(s).

    The plural Attrs (the hole rectChangeAttrs slipped through: Attr\\b fails on
    the trailing 's') and the lowercase standalone attr/attrs are caught too.
    "attribute"/"attributes" never match — the \\b after attr/attrs needs a word
    end, which the spelled-out forms lack.
    """
    named = (re.search(r"(?<=[A-Za-z])Attrs?\b", line, ASCII)
             or re.search(r"\battrs?\b", line, ASCII))
    return bool(named) and not _all_library(line, r"\w*Attrs?")


_primary('Alnum|\\balnum\\b')


def _alnum(line):
    """Alnum → Alphanumeric(s). isalnum/[[:alnum:]] are the C library's."""
    named = re.search(r"Alnum", line, ASCII) or re.search(r"\balnum\b", line, ASCII)
    return bool(named) and not re.search(r"isalnum|\[\[:alnum:\]\]", line, ASCII)


_primary('\\bWASM\\b')


def _wasm(line):
    """WebAssembly is the WORD Wasm.

    The spec explicitly states it is not an acronym, so it must never be
    all-caps in our names — the inverse of every rule above it.
    """
    return bool(re.search(r"\bWASM\b", line, ASCII)
                or re.search(r"\bWASM(?=[A-Z][a-z])", line, ASCII))


# The rules, in the order the scanner applies them and the report lists them: a
# line reports every rule it trips.
RULES = (
    ("Gpu → GPU", _hits(r"\bGpu(?=[A-Z_])|(?<=[a-z])Gpu\b")),
    ("Ascii → ASCII", _hits(r"\bAscii(?=[A-Z_])|(?<=[a-z])Ascii\b|^Ascii\b")),
    ("Utf8 → UTF8", _hits(r"\bUtf8(?=[A-Z_])|(?<=[a-z])Utf8\b|^Utf8\b")),
    ("Utf16 → UTF16", _hits(r"\bUtf16(?=[A-Z_])|(?<=[a-z])Utf16\b|^Utf16\b")),
    ("Utf32 → UTF32", _hits(r"\bUtf32(?=[A-Z_])|(?<=[a-z])Utf32\b|^Utf32\b")),
    # Sgr and Osc take the Vt shape below, digits included: these name control
    # sequences, so the character after the acronym is a parameter far more
    # often than a capital — `testOsc133`, `testSgr21`. Requiring [A-Z_] there
    # is what let the ratchet flag names the whole-tree scan called clean, so
    # the two halves reported different trees.
    ("Sgr → SGR", _hits(r"\bSgr(?=[0-9A-Z_])|(?<=[a-z])Sgr(?=[0-9A-Z_])|(?<=[a-z])Sgr\b|^Sgr\b")),
    ("Osc → OSC", _hits(r"\bOsc(?=[0-9A-Z_])|(?<=[a-z])Osc(?=[0-9A-Z_])|(?<=[a-z])Osc\b|^Osc\b")),
    # Csi and Dcs go past the Vt shape rather than copying it. That alternation
    # exists to spare a correct lowercase-leading first segment (vtLevel,
    # pngBytes, rgbPen), and it pays for the exemption with a lookbehind of
    # [a-z] — blind to an acronym reached from a digit (…C1Csi8…), from the tail
    # of the acronym before it (…TBCCsi1G…), or carrying a plural (…Csis…).
    # There is no lowercase `csi`/`dcs` segment to buy with that complexity: the
    # capital C/D already excludes prose and lowerCamelCase leads.
    ("Csi → CSI", _hits(r"Csi")),
    ("Dcs → DCS", _hits(r"Dcs")),
    # Toml → TOML (Tom's Obvious Minimal Language — an acronym, all-caps). The
    # capital T already excludes prose and a lowerCamelCase lead: `tomlInt`,
    # `config.toml` are lowercase `toml` and correct.
    ("Toml → TOML", _hits(r"Toml")),
    ("Gin → GIN (Tek graphic input)",
     _hits(r"(?<=[a-z])Gin(?=[A-Z_])|\bGin(?=[A-Z_])|(?<=[a-z])Gin\b|^Gin\b")),
    ("Vt → VT", _hits(r"\bVt(?=[0-9A-Z_])|(?<=[a-z])Vt(?=[0-9A-Z_])|(?<=[a-z])Vt\b|^Vt\b")),
    ("Pty → PTY",
     _hits(r"\bPty(?=[A-Z_])|(?<=[a-z])Pty\b|^Pty\b|\b(?:class|struct)\s+Pty\b")),
    # A lowercase-leading "rgb" (rgbBlendOperation, rgbPen) is correct WebKit
    # style in any code — ours or a vendor API — and is not flagged. Only a
    # capitalized "Rgb" word-segment is wrong, whether it opens/closes the
    # identifier or sits mid-compound (portableRgbReversed).
    ("Rgb → RGB", _hits(r"\bRgb(?=[A-Z_])|(?<=[a-z])Rgb(?=[A-Z_]|\b)|^Rgb\b")),
    ("Hsb → HSB", _hits(r"\bHsb(?=[A-Z_])|(?<=[a-z])Hsb\b")),
    ("Hsv → HSV", _hits(r"\bHsv(?=[A-Z_])|(?<=[a-z])Hsv\b")),
    ("Url → URL", _hits(r"\bUrl(?=[A-Z_])|(?<=[a-z])Url\b")),
    ("Nanos → Nanoseconds (spell out time units)", _hits(r"(?<=[a-z])Nanos\b")),
    ("Api → API", _hits(r"\bApi(?=[A-Z_])|(?<=[a-z])Api\b")),
    ("Tiff → TIFF", _hits(r"\bTiff(?=[A-Z_])|(?<=[a-z])Tiff\b|^Tiff\b")),
    # Leading-lowercase "png" (pngBytes) is correct WebKit style and is not
    # flagged; only a capitalized "Png" word-segment is wrong.
    ("Png → PNG", _hits(r"\bPng(?=[A-Z_])|(?<=[a-z])Png(?=[A-Z_]|\b)|^Png\b")),
    ("Udk → UDK (DEC user-defined keys)",
     _hits(r"\bUdk(?=[A-Z_])|(?<=[a-z])Udk\b|^Udk\b")),
    # The digit anchor never hits Shape/Shader/Shadow (no digit follows).
    ("Sha[N] → SHA[N] (hash acronym all-caps)", _hits(r"Sha[0-9]")),
    # The lowercase-lead anchor spares the correct absoluteRow.
    ("Abs → Absolute (spell out; an absolute row is xAbsoluteRow)",
     _hits(r"(?<=[a-z])Abs\b")),
    ("WASM -> Wasm (WebAssembly is a name, not an acronym)", _wasm),
    ("[name]Id → [name]ID", _hits(r"(?<=[a-z])Id\b")),
    ("Term → Terminal (spell out the central type)", _hits(r"\bTerm\b")),
    ("fg[X] → foreground[X]", _hits(r"\bfg[A-Z]")),
    ("bg[X] → background[X]", _hits(r"\bbg[A-Z]")),
    # The capitalized word-segment forms of the two rules above:
    # selectionFgColumn, testOSC10DefaultFg. A lone "Fg"/"Bg" word is caught by
    # the \b tail.
    ("Fg → Foreground (spell out)",
     _hits(r"(?<=[a-z_0-9])Fg(?=[A-Z_0-9]|\b)|\bFg(?=[A-Z_])|^Fg\b")),
    ("Bg → Background (spell out)",
     _hits(r"(?<=[a-z_0-9])Bg(?=[A-Z_0-9]|\b)|\bBg(?=[A-Z_])|^Bg\b")),
    ("Conn → Connection", _hits(r"(?<=[a-z])Conn(?![a-z])|\bConn(?![a-z])|\bconn(?![a-z_])")),
    # Alt always abbreviates "Alternate" (alt-screen, the Alt/Option modifier;
    # Cocoa itself spells NSAlternateKeyMask). "alternate"/AltiVec/calt never
    # match: each has a lowercase letter where the rule needs an uppercase/_ or
    # a word-end. leftAlt/rightAlt are GameController's own GCKeyCode names
    # (GCKeyCodeLeftAlt/GCKeyCodeRightAlt) and stay, on IS_LIBRARY.
    ("Alt → Alternate (spell out)",
     _hits_unless_library(
         r"\bAlt(?=[A-Z_])|(?<=[a-z])Alt(?=[A-Z_])|(?<=[a-z])Alt\b|\bAlt\b|\balt(?=[A-Z_])|\balt\b",
         r"\w*[Aa]lt")),
    # ul → underline, cur → cursor/current. "underline"/"cursor"/"current"
    # themselves never match.
    ("ul[X] → underline[X]", _hits(r"\bul(?=[A-Z_])|\bul\b")),
    ("cur[X] → cursor[X] or current[X]", _hits(r"\bcur(?=[A-Z_])|\bcur\b")),
    ("[name]Idx → [name]Index", _hits(r"(?<=[A-Za-z])Idx\b")),
    ("Nav[X] → Navigation[X]",
     _hits(r"\bNav(?=[A-Z_])|\bNav\b|(?<=[A-Za-z])Nav(?=[A-Z_]|\b)")),
    # Len/Buf: match at word-end and mid-compound (plural Lens, xxxLenOf).
    ("[name]Len → [name]Length",
     _hits_unless(r"(?<=[A-Za-z])Len(?:\b|s\b|[A-Z])", r"(?<=[A-Za-z])Length")),
    ("[name]Buf → [name]Buffer",
     _hits_unless(r"(?<=[A-Za-z])Buf(?:\b|s\b|[A-Z])", r"(?<=[A-Za-z])Buffer")),
    # 'Cap' is check-style.py's single concern (its allowlist is the one source
    # of truth for the graphics-cap exemptions); not duplicated here, to avoid
    # the two gates drifting.
    ("[name]Pos → [name]Position", _hits(r"(?<=[A-Za-z])Pos\b")),
    ("Alnum → Alphanumeric(s)", _alnum),
    # Fam: spell out Family. "family" itself and prose carrying "fam" inside
    # never match.
    ("Fam[X] → Family[X]",
     _hits_unless(r"\bfam(?=[A-Z_])|(?<=[A-Za-z])Fam(?=[A-Z_]|\b)", r"[Ff]amil")),
    ("[name]Params → [name]Parameters", _hits(r"(?<=[A-Za-z])Params\b")),
    ("cp → codePoint (codepoint abbreviation)", _codepoint_abbreviation),
    ("codepoint → codePoint (lowercase p)", _codepoint_spelling),
    # Single-letter colour channels R/G/B on colour identifiers. The spelled-out
    # Red/Green/Blue never match (the \b needs a word end right after).
    ("[colour]R/G/B → spell out Red/Green/Blue (colour channel)",
     _hits(r"\b\w*(?:olor|ground)[RGB]\b")),
    # DEC double-line attributes. The all-caps LINE_ATTR_* constants and
    # DHL/DWL in comments are left alone.
    ("Dhl → DoubleHeight (DEC double-height line; spell out)", _hits(r"(?<=[a-z])Dhl")),
    ("Dwl → DoubleWidth (DEC double-width line; spell out)", _hits(r"(?<=[a-z])Dwl")),
    ("[name]Ctx → [name]Context", _hits(r"(?<=[A-Za-z])Ctx\b")),
    # Cb: the callback suffix. A discrete-event handler is [name]Handler; a
    # streaming sink is [name]Callback.
    ("[name]Cb → [name]Handler or [name]Callback", _hits(r"(?<=[a-z])Cb\b")),
    ("[name]Num → [name]Number", _num),
    ("[name]Str → [name]String", _hits(r"(?<=[A-Za-z])Str\b")),
    ("[name]Attr/attr → [name]Attribute (incl. plural Attrs/attrs)", _attr),
    ("[name]Src → [name]Source", _hits(r"(?<=[A-Za-z])Src\b")),
    ("[name]Dst → [name]Destination", _hits(r"(?<=[A-Za-z])Dst\b")),
    ("[name]Tmp → [name]Temporary", _hits(r"(?<=[A-Za-z])Tmp\b")),
    # The lowercase lookbehind spares a plural acronym like "LFs" (LF + s).
    ("[name]Fs → [name]FileSystem", _hits(r"(?<=[a-z])Fs(?:\b|[A-Z])")),
    ("[name]Msg → [name]Message", _hits(r"(?<=[A-Za-z])Msg\b")),
    ("[name]Tok → [name]Token", _hits(r"(?<=[A-Za-z])Tok\b")),
    ("[name]Err → [name]Error",
     _hits_unless_library(r"(?<=[A-Za-z])Err\b", r"\w*Err")),
    ("[name]Cmd → [name]Command", _hits(r"(?<=[A-Za-z])Cmd\b")),
    ("[name]Fmt → [name]Format", _hits(r"(?<=[A-Za-z])Fmt\b")),
    ("[name]Acc(um) → [name]Accumulator",
     _hits_unless(r"(?<=[A-Za-z])Acc(?:um)?\b", r"\bacc\b")),
    ("[name]Val → [name]Value", _hits(r"(?<=[A-Za-z])Val\b")),
    ("[name]Pkt → [name]Packet", _hits(r"(?<=[A-Za-z])Pkt\b")),
    ("[name]Hdr → [name]Header", _hits(r"(?<=[A-Za-z])Hdr\b")),
    ("[name]Arg → [name]Argument",
     _hits_unless(r"(?<=[A-Za-z])Arg\b", r"\b(?:argv|argc|ARG_MAX)\b")),
    ("[name]Sz → [name]Size (or descriptive)", _hits(r"(?<=[A-Za-z])Sz\b")),
    ("[name]Pt → [name]InPoints or [name]Point", _hits(r"(?<=[A-Za-z])Pt\b")),
    ("[name]Px → [name]InPixels or [name]Pixels", _hits(r"(?<=[A-Za-z])Px\b")),
    ("[name]Fd → [name]FileDescriptor", _hits_unless_library(r"(?<=[A-Za-z])Fd(?=[A-Z]|\b)", r"\w*Fd\w*")),
    # fd: exempt the POSIX struct pollfd ".fd" member (a system field we cannot
    # rename) and the terminfo focus-event capability data, which is payload.
    ("fd → fileDescriptor",
     _hits_unless(r"(?<![\w.])fd\b", r"fd=\\\\E")),
    # Pid: pid_t and getpid/fork/waitpid/setsid are lowercase and never match.
    # GPid is GLib's own type name and g_spawn_close_pid its own function.
    ("[name]Pid → [name]ProcessID",
     _hits_unless(r"(?<=[A-Za-z])Pid(?=[A-Z]|\b)", r"\bGPid\b|g_spawn_close_pid")),
    # Ptr and Ref are WebKit C++ convention — not flagged.
)


# --- the cheap first pass ----------------------------------------------------
#
# Asking every rule of every line spends the whole scan proving a negative: the
# union of all the patterns rejects nearly every line of the tree, and slowly.
# Almost all of that is the lookbehinds and lookaheads, which have to be tried
# at every position of every line.
#
# Most rules are built around a literal fragment -- Gpu, Idx, codepoint -- and a
# line without one cannot trip the rule whatever surrounds it. A plain
# alternation of those fragments carries no assertions and costs little.
#
# A rule whose pattern cannot be reduced to mandatory literals is asked of every
# line instead. That is the whole reason reduction refuses rather than guesses:
# a reduction that splits every `|` cuts `(?:olor|ground)` in half, and one that
# reads `\w` as a literal `w` makes the colour-channel rule unreachable behind
# its own filter while the scan still reports a clean tree.
# Scripts/check-acronym-rules.py holds `may_trip` to admitting every
# line any rule reports.

# What a pattern may contain and still be reducible: letters, digits and the
# few pieces that cannot shorten the mandatory run.
_REDUCIBLE = re.compile(r"^(?:\\b|\^|\$|\(\?<[=!]\[[^\]]*\]\)|\(\?[=!][^)]*\)"
                        r"|\(\?:[A-Za-z]+\)\?|[A-Za-z0-9]|\?)*$")


def _mandatory(alternative):
    """The literal run an alternative must contain, or None if unprovable."""
    if not _REDUCIBLE.match(alternative):
        return None
    bare = _ASSERTION.sub("", alternative)
    bare = re.sub(r"\(\?:[A-Za-z]+\)\?", "", bare)
    bare = bare.replace(r"\b", "").replace("^", "").replace("$", "")
    run = re.match(r"([A-Za-z][A-Za-z0-9]*)", bare)
    if not run:
        return None
    word = run.group(1)
    tail = bare[run.end():]
    # A letter carrying ? or * is optional, so it ends the mandatory run rather
    # than joining it: `Attrs?` yields `Attr`. With `Attrs` as the fragment a
    # line saying `attr` was rejected before its rule was ever asked.
    if tail[:1] in ("?", "*") and len(word) > 1:
        word = word[:-1]
    return word or None


_ASSERTION = re.compile(r"\(\?<[=!](?:\[[^\]]*\]|[^)]*)\)|\(\?[=!][^)]*\)")


def _split_alternatives(pattern):
    """Top-level alternatives, leaving any inside a group alone."""
    parts = []
    depth = 0
    current = []
    for character in pattern:
        if character == "(":
            depth += 1
        elif character == ")":
            depth -= 1
        if character == "|" and depth == 0:
            parts.append("".join(current))
            current = []
        else:
            current.append(character)
    parts.append("".join(current))
    return parts


def _fragments(pattern):
    """Literals any matching line must contain, or None if unprovable."""
    found = set()
    for alternative in _split_alternatives(pattern):
        word = _mandatory(alternative)
        if word is None:
            return None
        found.add(word)
    return found or None


_FRAGMENTS = set()
_ALWAYS = []
for _index, _pattern in enumerate(_PRIMARY):
    _reduced = _fragments(_pattern)
    if _reduced is None:
        _ALWAYS.append(re.compile(_pattern, ASCII))
    else:
        _FRAGMENTS |= _reduced

# Longest first so the alternation prefers the most specific fragment.
PREFILTER = re.compile("|".join(sorted(_FRAGMENTS, key=lambda f: (-len(f), f))),
                       ASCII)


def may_trip(line):
    """Whether any rule could report this line.

    A superset of what the rules report, and the one thing the scanner asks
    before asking them. The differential gate holds it to that.
    """
    if PREFILTER.search(line):
        return True
    return any(pattern.search(line) for pattern in _ALWAYS)
