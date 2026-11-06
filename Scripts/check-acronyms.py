#!/usr/bin/env python3
# Copyright (C) 2026 Stanislav Yaglo
# SPDX-License-Identifier: Apache-2.0

"""check-acronyms.py — WebKit/Cocoa naming: acronyms all-caps, abbreviations spelled out.

Two halves, and they answer different questions.

The whole-file scan reads every source file the tree carries and applies the
rules in Scripts/acronym_rules.py. It is the tree-must-stay-clean half: a rule
there has no backlog to work through, so it may block any edit to any file.

The added-line ratchet reads only what a commit ADDS. A rule lives here while
the tree still carries the shape it forbids, because a whole-file rule with a
backlog blocks an unrelated edit to any file still carrying it -- what
Docs/coding-conventions.md means by a lint never preceding its rename.

  Scripts/check-acronyms.py            # the whole tree
  Scripts/check-acronyms.py --staged   # staged files, plus the added-line ratchets
  Scripts/check-acronyms.py file ...   # the named files
  Scripts/check-acronyms.py --self-test

Exit 0 = clean, 1 = violations, 2 = the scan could not run.

The scan is Scripts/acronym_scan.py, which replaced a batched, parallel perl.
Scripts/check-acronym-rules.py holds the two to reporting the same tree.
"""

import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import acronym_scan
import gate_corpus

GATE = "check-acronyms"

SOURCE_SUFFIXES = (".h", ".cp", ".mm", ".cpp", ".m", ".swift")
VENDORED = re.compile(r"(^|/)ThirdParty/")

# A vendor call is not our name. `g_child_watch_add(` is GLib's; a `g`-prefixed
# name this project chose is not, and the difference is the paren.
VENDOR_CALL = re.compile(r"(^|[^A-Za-z0-9_])(g|gtk|gdk|glib)_[a-z0-9_]+ *\(")
ADDRESS_MODE = re.compile(r"addressMode[UVW]([^A-Za-z0-9_])")
# A vendor field reached through the struct that owns it is not our name.
# FreeType spells `FT_Affine23`'s and `FT_PaintTranslate`'s offsets `dx`/`dy`
# and `FT_BBox`'s edges `xMin`/`xMax`/`yMin`/`yMax`. The exemption needs the
# receiver spelled, the way `obj.maximumFoo` does, so a field of ours given one
# of these names still flags: `double dx` and `box.xMin` are ours.
VENDOR_FIELD = re.compile(
    r"\b(?:affine|translate|bbox)(\.|->)(?:dx|dy|xMin|xMax|yMin|yMax)\b")
# A Swift argument label is the callee's name, not ours. CGRect's
# `insetBy(dx:dy:)` and `offsetBy(dx:dy:)` spell their labels for every
# caller, and a call site cannot rename them; the colon is what marks a
# label. A `dx` of ours appears bare — a variable, a field — and flags at
# every such use, a declaration of our own included.
VENDOR_LABEL = re.compile(r"\bd([xy]):")
# Fontconfig's API is `Fc`-prefixed CamelCase, and its names are the vendor's:
# `FcCharSetHasChar` and `FcChar8` cannot be respelled at a call site. The
# prefix is the mark, so a name of ours spelled `characterSet` still flags.
VENDOR_FONTCONFIG = re.compile(r"\bFc[A-Z][A-Za-z0-9]*")
# The SDK enum's spelling is fixed. Requiring its qualified type keeps a
# similarly named project variable subject to the normal character rule.
VENDOR_ENUM_CASE = re.compile(r"\bNSLineBreakMode\.byCharWrapping\b")
STRING_LITERAL = re.compile(r'"([^"\\]|\\.)*"')

RATCHETS = (
    ('\\bg[A-Z][a-z]|\\bg[A-Z]{2,}|\\bg_[a-z]',
     'g-prefix → drop it, spell the name out'),
    ('[a-z]Ids\\b',
     'Ids → IDs (acronym all-caps; the full perl blocks singular Id)'),
    ('[a-z]Certs?\\b',
     'Cert → Certificate (the \\b skips the correct Certificate; "pin" stays a word)'),
    ('\\bd[xy]\\b',
     'dx/dy → xOffset/yOffset (offset field) or deltaX/deltaY (delta)'),
    ('[a-z]Dims\\b',
     'Dims → Dimensions (the \\b spares Dimensions/GridDimensions)'),
    ('[a-z]Dim\\b',
     'Dim → Dimension (the \\b spares Dimension/maxDimension; e.g. maxAtlasDim → maxAtlasDimension, cellDim → cellDimension)'),
    ('[a-z]Bot\\b',
     'Bot → Bottom (camelCase trailing Bot only; the \\b spares Bottom/scrollBottom and lowercase robot/chatbot; e.g. scrollBot → scrollBottom, gridScrollBot → gridScrollBottom)'),
    ('[a-z]Ch\\b',
     'Ch → CodePoint for a code-point value, else Character (camelCase trailing Ch only; the capital C spares arch/search/match; e.g. wideContinuationSentinelCh → wideContinuationSentinelCodePoint)'),
    ('[a-z]Chars?([^a-z]|$)',
     'Char → Character (the guard is [^a-z] not [^a-zA-Z]: Character continues in lowercase, so excluding every following letter would also spare CharClass, CharSet and CharBuffer; UniChar, UTF16Char and FcChar8 are vendor types and stay — the full perl scan excludes them, this fast ratchet cannot)'),
    ('[Cc]harset',
     'Charset → CharacterSet, charset → characterSet (spell both words out; NSCharacterSet already reads correctly)'),
    ('[a-z]Ms\\b',
     'Ms → Milliseconds (spell out time units, matches the Nanos rule)'),
    ('[a-zA-Z0-9]Ok\\b|\\bOk[A-Z]|::Ok\\b|\\bOk = ',
     'Ok → OK (acronym all-caps: sixelOk → sixelOK, Status::Ok → ::OK, Ok = 0 → OK = 0)'),
    ('[A-Za-z]Uint\\b',
     'Uint → the C type (uint32_t) or a meaningful name, not "UnsignedInteger"'),
    ('[A-Za-z]Var\\b',
     'Var → Variable (spell out)'),
    ('\\bcont\\b|\\bcont[A-Z_]|[a-z]Cont\\b|[a-z]Cont[A-Z_]|\\bCont\\b|\\bCont[A-Z_]',
     'cont → continuation (spell out: contPen → continuationPen, sameCont → sameContinuation)'),
    ('(^|[^A-Za-z])(Usascii|Ascii|Ansi|Csi|Dcs|Apc|Osc|Sgr|Esc|Nrcs|Drcs|Utf|Iso|Pty|Tty|Pid|Cpu|Gpu|Dpi|Fps|Url|Uri|Http|Html|Json|Rtf|Png|Tiff|Rgba|Rgb|Srgb|Crlf|Eol|Eof|Pal|Mru|Xdg|Alpn|Quic|Tls|Ssh|Udp|Tcp|Mcp|Ime|Cjk|Zwj|Rtl|Ltr|Xtwinops|Xtgettcap|Decrqm|Decrqss|Decset|Decrst|Decstr|Decsca|Decscl)([^a-z]|$)|[a-z0-9](Usascii|Ascii|Ansi|Csi|Dcs|Apc|Osc|Sgr|Esc|Nrcs|Drcs|Utf|Iso|Pty|Tty|Pid|Cpu|Gpu|Dpi|Fps|Url|Uri|Http|Html|Json|Rtf|Png|Tiff|Rgba|Rgb|Srgb|Crlf|Eol|Eof|Pal|Mru|Xdg|Alpn|Quic|Tls|Ssh|Udp|Tcp|Mcp|Ime|Cjk|Zwj|Rtl|Ltr|Xtwinops|Xtgettcap|Decrqm|Decrqss|Decset|Decrst|Decstr|Decsca|Decscl)([^a-z]|$)',
     'acronym written as a word → ALL-CAPS (Usascii → USASCII, Sgr → SGR, Pty → PTY); a vendor API name keeps its own spelling'),
    ('[a-z]Max\\b',
     'Max suffix → max prefix (maxFoo, not fooMax)'),
    ('[a-z]Min\\b',
     'Min suffix → min prefix (minFoo, not fooMin)'),
    ('[a-z]Maximum\\b',
     'Maximum suffix → max prefix (WTF uses the max/min abbreviation ~8:1)'),
    ('[a-z]Minimum\\b',
     'Minimum suffix → min prefix'),
    ('(^|[^A-Za-z._>])maximum[A-Z]',
     'maximum prefix → max (maxFoo). A vendor property is exempt when written as obj.maximumFoo — spell the receiver so the rule can see it is not ours'),
    ('(^|[^A-Za-z._>])minimum[A-Z]',
     'minimum prefix → min (minFoo). A vendor property is exempt when written as obj.minimumFoo — spell the receiver so the rule can see it is not ours'),
    ('[a-z]W\\b',
     'W suffix → Width (cellW → cellWidth)'),
    ('[a-z]H\\b',
     'H suffix → Height (cellH → cellHeight)'),
    ('[a-z]Ceiling',
     'Ceiling → max (maxFoo); Limit only for a value someone configured'),
)


REMEDY = (
    "  Rule: acronyms ALL-CAPS (GPU/UTF8/CSI/SGR/…); spell out abbreviations:",
    "  Id/Ids→ID/IDs, Idx→Index, Nav→Navigation, Len→Length, Buf→Buffer, Pos→Position,",
    "  Params→Parameters, Ctx→Context, Num→Number, Str→String, Attr/Attrs/attr/attrs→Attribute(s),",
    "  Src→Source, Dst→Destination, Tmp→Temporary, Fs→FileSystem, Msg→Message, Tok→Token, Err→Error,",
    "  Cmd→Command, Fmt→Format, Acc→Accumulator, Val→Value, Hdr→Header,",
    "  Pkt→Packet, Fd→FileDescriptor, fd→fileDescriptor; cp→codePoint; codepoint→codePoint;",
    "  Dhl→DoubleHeight; Dwl→DoubleWidth; ul→underline; cur→cursor or current.",
    "  WASM→Wasm (WebAssembly is a name, not an acronym).",
    "  Max/Min suffix→max/min prefix; Uint→uint32_t or a meaningful name; Var→Variable; Dim→Dimension; Bot→Bottom.",
)


def sources(argument):
    """The files one run reads.

    Vendored third-party code keeps its upstream naming and is exempt.
    """
    if argument == "--staged":
        listing = gate_corpus.staged()
    elif argument:
        listing = list(argument)
    else:
        listing = gate_corpus.tracked()
    return [path for path in listing
            if path.endswith(SOURCE_SUFFIXES) and not VENDORED.search("/" + path)]


def added_lines(root):
    """The words this commit ADDS, one line each, string literals blanked.

    A word diff rather than a line diff: an edit to one identifier on a long
    line should be read as that identifier, not as the whole line, or every
    rule fires on every neighbour the line happens to carry.
    """
    finished = subprocess.run(
        ["git", "-C", root, "diff", "--cached", "-U0", "--word-diff=porcelain",
         "--word-diff-regex=[A-Za-z_][A-Za-z0-9_]*|[^[:space:]]",
         "--diff-filter=ACM", "--",
         "*.h", "*.cp", "*.cpp", "*.mm", "*.m", "*.swift",
         ":(exclude)*/ThirdParty/*", ":(exclude)ThirdParty/*"],
        capture_output=True, text=True, check=False)
    lines = finished.stdout.split("\n")
    return added_word_lines(lines)


def added_word_lines(lines):
    """Preserves vendor call and label context across word-diff fragments."""

    # A vendor symbol whose call paren is on the NEXT word-diff line is a call,
    # and the word diff has split it in two. Drop the symbol so it cannot read
    # as a name of ours.
    vendorSymbol = re.compile(r"^[+ ][ \t]*(g|gtk|gdk|glib)_[a-z0-9_]+$")
    opensCall = re.compile(r"^[+ ][ \t]*\(")
    endsLabel = re.compile(r"\bd[xy]$")
    continuesLabel = re.compile(r"^[+ ][ \t]*:")
    kept = []
    for index, line in enumerate(lines):
        following = lines[index + 1] if index + 1 < len(lines) else ""
        if vendorSymbol.match(line) and opensCall.match(following):
            kept.append(line[:1])
            continue
        # Word diff can leave an unchanged colon outside the added label.
        # Retain that punctuation for the same label rule used on whole calls.
        if line.startswith("+") and endsLabel.search(line) and continuesLabel.match(following):
            line += ":"
        kept.append(line)

    added = [line for line in kept
             if line.startswith("+") and not line.startswith("+++")]
    return [STRING_LITERAL.sub('""', line) for line in added]


def ratchet_hits(added, pattern):
    """Added lines a ratchet rule reports, vendor calls masked first."""
    compiled = re.compile(pattern)
    found = []
    for line in added:
        masked = ADDRESS_MODE.sub(r"addressMode\\1", VENDOR_CALL.sub(r"\\1(", line))
        masked = VENDOR_FIELD.sub(r"vendor\\1field", masked)
        masked = VENDOR_LABEL.sub(r"labelD\1:", masked)
        masked = VENDOR_FONTCONFIG.sub("Fc", masked)
        masked = VENDOR_ENUM_CASE.sub("vendorEnumCase", masked)
        masked = re.sub(r"\bKey[A-Z]\b", "letterKey", masked)
        if compiled.search(masked):
            found.append(line)
    return found


def main(argv):
    if argv and argv[0] in ("--help", "-h"):
        print(__doc__.strip())
        return 0
    if argv and argv[0] in ("--self-test", "--selftest"):
        return _self_test()

    root = gate_corpus.root()
    os.chdir(root)
    staged = bool(argv) and argv[0] == "--staged"
    paths = sources("--staged" if staged else (argv or None))

    report, filesWithHits = acronym_scan.scan_parallel(paths, root)
    if report:
        sys.stdout.buffer.write(b"\n".join(report) + b"\n")
    total = filesWithHits

    if staged:
        added = added_lines(root)
        for pattern, message in RATCHETS:
            hits = ratchet_hits(added, pattern)
            if not hits:
                continue
            print(f"  {message}:")
            for hit in hits:
                print("    " + hit[1:])
            total += 1

    if total:
        print()
        print(f"{GATE}: {total} file(s) with violations — fix before merging to "
              f"main.")
        for line in REMEDY:
            print(line)
        return 1
    return 0


def _self_test():
    """A vendor call is not a violation; a name this project chose still is.

    The two shapes differ only by the paren, and a rule that cannot tell them
    apart either rejects GLib on sight or lets every g-prefixed name of ours
    through.
    """
    status = 0
    for letter, word in (("W", "Width"), ("H", "Height")):
        rule = next(pattern for pattern, label in RATCHETS if label.startswith(f"{letter} suffix"))
        if ratchet_hits([f"+ VirtualKey::Key{letter}"], rule) or not ratchet_hits([f"+ int cell{letter};"], rule):
            print(f"{GATE} self-test: FAIL — keyboard letter confused with {word}")
            status = 1
    vendor = ["+    g_child_watch_add(processID, onExit, nullptr);"]
    ours = ["+int g_counter = 0;"]
    pattern = RATCHETS[0][0]

    if not ratchet_hits(vendor, pattern):
        print(f"{GATE} self-test: ok — a newly written GLib call is not a "
              f"naming violation")
    else:
        print(f"{GATE} self-test: FAIL — a GLib call was reported")
        status = 1

    if ratchet_hits(ours, pattern):
        print(f"{GATE} self-test: ok — a g-prefixed name this project chose "
              f"still flags")
    else:
        print(f"{GATE} self-test: FAIL — a g-prefixed name of ours went "
              f"unreported")
        status = 1

    # A FreeType field is exempt only through its receiver: the same offset
    # name on a struct of ours, or bare, is still a violation.
    offsetRule = next(pattern for pattern, _ in RATCHETS if pattern.startswith("\\bd[xy]"))
    splitLabels = added_word_lines(["+CGVector(dx", " : x,", "+dy", " : y)"])
    splitVariable = added_word_lines(["+let dx", " = 0"])
    if not ratchet_hits(splitLabels, offsetRule) and ratchet_hits(splitVariable, offsetRule):
        print(f"{GATE} self-test: ok — split Swift labels retain their colon; bare names still flag")
    else:
        print(f"{GATE} self-test: FAIL — word-diff fragments lost Swift label context")
        status = 1
    edgeRule = next(pattern for pattern, _ in RATCHETS if pattern.startswith("[a-z]Min"))
    vendorFields = ["+            .xOffset = fromFixed(affine.dx),",
                    "+            .yOffset = fromFixed(paint.u.translate.dy),",
                    "+        boxLeft = m_emojiFace->bbox.xMin * unitsToPixels;"]
    ourFields = ["+    double dx = 0.0;",
                 "+    const int left = box.xMin;"]
    if not ratchet_hits(vendorFields, offsetRule) and not ratchet_hits(vendorFields, edgeRule):
        print(f"{GATE} self-test: ok — a FreeType field read through its "
              f"receiver is not a naming violation")
    else:
        print(f"{GATE} self-test: FAIL — a FreeType field was reported")
        status = 1
    if ratchet_hits(ourFields[:1], offsetRule) and ratchet_hits(ourFields[1:], edgeRule):
        print(f"{GATE} self-test: ok — the same names on a field of ours "
              f"still flag")
    else:
        print(f"{GATE} self-test: FAIL — a field of ours named like a vendor "
              f"field went unreported")
        status = 1

    # A fontconfig call is exempt by its prefix; the same words in a name of
    # ours still flag.
    characterRule = next(pattern for pattern, _ in RATCHETS if pattern.startswith("[a-z]Chars?"))
    characterSetRule = next(pattern for pattern, _ in RATCHETS if pattern.startswith("[Cc]harset"))
    fontconfigNames = ["+FcCharSetHasChar", "+FcChar8", "+FcPatternGetCharSet"]
    ourCharacterNames = ["+candidateCharset", "+firstChar"]
    if (ratchet_hits(["+label.lineBreakMode = NSLineBreakMode.byCharWrapping"], characterRule)
            or not ratchet_hits(["+let byCharWrapping = 1"], characterRule)):
        print(f"{GATE} self-test: FAIL — the qualified SDK enum exemption is not scoped")
        status = 1
    if not ratchet_hits(fontconfigNames, characterRule) and not ratchet_hits(fontconfigNames, characterSetRule):
        print(f"{GATE} self-test: ok — a fontconfig name is not a naming violation")
    else:
        print(f"{GATE} self-test: FAIL — a fontconfig name was reported")
        status = 1
    if ratchet_hits(ourCharacterNames[:1], characterSetRule) and ratchet_hits(ourCharacterNames[1:], characterRule):
        print(f"{GATE} self-test: ok — the same words in a name of ours still flag")
    else:
        print(f"{GATE} self-test: FAIL — a name of ours spelled like fontconfig went unreported")
        status = 1

    # Every ratchet compiles. A pattern that does not is a rule that stops
    # matching, and an unasked rule reports nothing, same as a clean tree.
    broken = []
    for pattern, message in RATCHETS:
        try:
            re.compile(pattern)
        except re.error:
            broken.append(message)
    if broken:
        for message in broken:
            print(f"{GATE} self-test: FAIL — pattern does not compile: {message}")
        status = 1
    else:
        print(f"{GATE} self-test: ok — {len(RATCHETS)} ratchet pattern(s) compile")
    return status


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
