// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// The parser's byte-level behaviour: what each control does, and what happens
// to input that is not a sequence at all.
//
// C0 and C1, the string sequences and their terminators (DCS, APC, PM, SOS),
// the tmux passthrough that nests one stream inside another, and the aborts —
// can, SUB, an unterminated string, an intermediate with no final.
//
// Parameter arithmetic belongs here too: saturation, missing parameters, and a
// private prefix arriving where a numeric one was expected. These are the cases
// where a terminal either stays in a defined state or does not, and every one
// of them is reachable from a hostile or merely broken program.
//
// Dispatch-table lookup is the neighbouring file, test_vt100_parser.cpp: this
// one drives bytes through a live Terminal, that one asks the tables directly.

#include "Core/LineStore.h"
#include "Core/Rendering/StatusRow.h"
#include "Core/Screen/Addressing.h"
#include "Core/VT/Parser.h"
#include "Harness.h"
#include "VT100Fixture.h"

#include <array>

static std::string compactTmuxPassthroughWrap(std::string_view inner)
{
    std::string outputBuffer;
    outputBuffer.reserve(inner.size() + 16);
    outputBuffer.append("\033Ptmux;", 7);
    for (std::size_t i = 0; i < inner.size(); ++i) {
        if (inner[i] == '\033')
            outputBuffer.push_back('\033');
        outputBuffer.push_back(inner[i]);
    }
    outputBuffer.append("\033\\", 2);
    return outputBuffer;
}

static std::string nestedCompactTmuxPassthrough(int depth, std::string_view leaf)
{
    std::string payload(leaf);
    for (int i = 0; i < depth; ++i)
        payload = compactTmuxPassthroughWrap(payload);
    return payload;
}

/// DECSLE (CSI Ps ' {) selects which button transitions emit a DECLRP.
/// DECRQLP (CSI Ps ' |) → DECLRP reply CSI Pe;Pb;Pr;Pc;Pp & w. Pr;Pc are the
/// LOCATOR position, so the cursor must not leak into the reply: with the locator
/// enabled but no pointer event published the answer is Pe=1 with no coordinates.
/// One-shot DECELR (Ps1=2) auto-disables after the single DECRQLP report:
/// the next request returns the unavailable form.
/// DECEFR's omitted bounds default to the current locator position, so the
/// all-omitted form `CSI ' w` means "report any locator motion" — the case the
/// 0-means-unbounded reading inverted into "report nothing".
/// encodeLocatorReport wire format + locatorButtonBit mapping.
/// locatorReportEvent: a simulated button down/up emits a DECLRP only when
/// the matching DECSLE transition is selected; the Pe + Pb fields are correct
/// per xterm (left 2/3, Pb bit 4).
/// DECSLE gating: with only button-down selected, an up transition emits
/// nothing; with the locator disabled, nothing emits at all.
/// DECEFR (CSI Pt;Pl;Pb;Pr ' w) filter rectangle: a motion that leaves the
/// rect fires one DECLRP with Pe=10 (outside filter rectangle), then the rect
/// disarms (a second leave does not re-fire). Motion inside the rect is
/// silent.
/// DECSTR soft reset returns the locator to its power-on disabled state.
/// DECRQKT (CSI Ps , u) → DECRPKT reply CSI Ps1 ; Ps2 , v. Ps1 echoes the
/// keystation; Ps2 is the key type (0 alphanumeric, 1 function).
/// Verify the paneLookup hook on Ubiquitty::Terminal routes tmux %output bytes to a
/// different Ubiquitty::Terminal when activePaneID changes. Mirrors the production setup
/// where each tmux pane has its own MtSession and the primary's
/// paneLookup returns the child Ubiquitty::Terminal that backs the active pane.
static Ubiquitty::Terminal* fakePaneLookup(void* ctx, int paneID)
{
    Ubiquitty::Terminal* targets = (Ubiquitty::Terminal*)ctx;
    if (paneID == 0)
        return (Ubiquitty::Terminal*)&targets[0];
    if (paneID == 1)
        return (Ubiquitty::Terminal*)&targets[1];
    return nullptr;
}

/// The active-pane route is memoized across a delivery run: the paneLookup hook
/// (an os_unfair_lock + NSMapTable + objc_msgSend round-trip in production) runs
/// once per pane-run, not per %output byte. A pane change re-looks-up, the
/// end-of-pass flush invalidates, and an unresolved (nullptr) pane is never cached.
/// Verify-by-revert: drop the memo in tmuxDeliver and case (1) counts 5.
struct PaneLookupCounter {
    Ubiquitty::Terminal* children;
    int count;
    bool forceNull;
};

static Ubiquitty::Terminal* countingPaneLookup(void* ctx, int paneID)
{
    PaneLookupCounter* c = (PaneLookupCounter*)ctx;
    ++c->count;
    if (c->forceNull)
        return nullptr;
    if (paneID == 0)
        return (Ubiquitty::Terminal*)&c->children[0];
    if (paneID == 1)
        return (Ubiquitty::Terminal*)&c->children[1];
    return nullptr;
}

TEST(testESCUnhandledIntermediateDoesNotLeakFinal)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033%G");
    CHECK(cell(t, 0, 0)->codePoint == 0); // no leaked 'G'

    Ubiquitty::hardReset(t);
    feed(t, "\033%@");
    CHECK(cell(t, 0, 0)->codePoint == 0); // no leaked '@'

    /// A real printable byte after the (now-consumed) final types normally.
    Ubiquitty::hardReset(t);
    feed(t, "\033%GH");
    CHECK(cell(t, 0, 0)->codePoint == 'H');

    /// Any other not-specifically-handled intermediate behaves the same.
    Ubiquitty::hardReset(t);
    feed(t, "\033!XI");
    CHECK(cell(t, 0, 0)->codePoint == 'I');

    /// ECMA-35 sequences carry arbitrarily many intermediates before the
    /// final — "ESC % / G" (DOCS) and "ESC $ ( D" (ISO-2022 multibyte G0
    /// designation) are real two-intermediate forms; the extra intermediate
    /// collects rather than masquerading as the final.
    Ubiquitty::hardReset(t);
    feed(t, "\033%/G");
    CHECK(cell(t, 0, 0)->codePoint == 0); // no leaked 'G'

    Ubiquitty::hardReset(t);
    feed(t, "\033$(D");
    CHECK(cell(t, 0, 0)->codePoint == 0); // no leaked 'D'

    /// And a printable after a two-intermediate final still types.
    Ubiquitty::hardReset(t);
    feed(t, "\033%/GH");
    CHECK(cell(t, 0, 0)->codePoint == 'H');
}

TEST(testStringAbortOnCanSub)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    // OSC 0 title interrupted by can: 'X' prints in Ground (not swallowed).
    unsigned char can[] = { 0x1B, ']', '0', ';', 'a', 'b', 'c', 0x18, 'X' };
    feed(t, can);
    CHECK(cell(t, 0, 0)->codePoint == 'X');
    /// SUB aborts the same way, and draws its error character first.
    Ubiquitty::hardReset(t);
    unsigned char sub[] = { 0x1B, ']', '0', ';', 'd', 'e', 'f', 0x1A, 'Y' };
    feed(t, sub);
    CHECK(cell(t, 0, 0)->codePoint == Ubiquitty::VT::substituteCharacter);
    CHECK(cell(t, 0, 1)->codePoint == 'Y');
    /// APC (Kitty) string aborts on can too.
    Ubiquitty::hardReset(t);
    unsigned char apc[] = { 0x1B, '_', 'G', 'a', '=', 'T', 0x18, 'Z' };
    feed(t, apc);
    CHECK(cell(t, 0, 0)->codePoint == 'Z');
}

TEST(testAStrayEscapeEndsAControlString)
{
    // An ESC inside a string that is not followed by the terminator ends the
    // string, and the byte after it begins an escape: ESC 7 saves the cursor,
    // and the next character prints in Ground rather than into the string.
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    unsigned char osc[] = { 0x1B, ']', '0', ';', 'a', 0x1B, '7', 'X' };
    feed(t, osc);
    CHECK(cell(t, 0, 0)->codePoint == 'X');
    CHECK(t.parser.parseState == Ubiquitty::VT::Parser::State::Ground);
    Ubiquitty::hardReset(t);
    unsigned char apc[] = { 0x1B, '_', 'G', 'a', '=', 'T', 0x1B, '7', 'Y' };
    feed(t, apc);
    CHECK(cell(t, 0, 0)->codePoint == 'Y');
    Ubiquitty::hardReset(t);
    unsigned char pm[] = { 0x1B, '^', 'a', 0x1B, '7', 'Z' };
    feed(t, pm);
    CHECK(cell(t, 0, 0)->codePoint == 'Z');
    // A sizing code whose terminator is mistyped as ESC plus a non-ASCII
    // character leaves the parser in Ground with the rest of the stream printing.
    Ubiquitty::hardReset(t);
    unsigned char sizing[] = { 0x1B, ']', '6', '6', ';', 'p', '=', '1', ':', 'c', '=', '2', ';', 'H', 'i',
        0x1B, 0xE2, 0x9C, 0xA8, '\\', '\n', '\r', 'W' };
    feed(t, sizing);
    CHECK(t.parser.parseState == Ubiquitty::VT::Parser::State::Ground);
    CHECK(cell(t, 1, 0)->codePoint == 'W');
}

TEST(testSOSPmBelIsNotTerminator)
{
    Ubiquitty::Terminal t;
    /// PM: BEL mid-string does not terminate; 'X' 'Y' stay swallowed.
    Ubiquitty::hardReset(t);
    unsigned char pm[] = { 0x1B, '^', 'a', 'b', 0x07, 'X', 'Y' };
    feed(t, pm);
    CHECK(cell(t, 0, 0)->codePoint == 0); // nothing printed
    // ST (ESC \) after the BEL still terminates cleanly; 'Q' then prints.
    unsigned char pmEnd[] = { 0x1B, '\\', 'Q' };
    feed(t, pmEnd);
    CHECK(cell(t, 0, 0)->codePoint == 'Q');
    /// SOS behaves identically.
    Ubiquitty::hardReset(t);
    unsigned char sos[] = { 0x1B, 'X', 'd', 0x07, 'e', 'f' };
    feed(t, sos);
    CHECK(cell(t, 0, 0)->codePoint == 0);
    // `CAN` remains part of the character string until ST.
    unsigned char sosAbort[] = { 0x18, 'Z' };
    feed(t, sosAbort);
    CHECK(cell(t, 0, 0)->codePoint == 0);
    feed(t, "\033\\Q");
    CHECK(cell(t, 0, 0)->codePoint == 'Q');
}

TEST(test94setGrLeavesEndpointsRaw)
{
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        Ubiquitty::Modes::setInputEncoding(&t, Ubiquitty::Unicode::Encoding::Latin1); // GR exists only in an 8-bit coding
        feed(t, "\033)B\033~");
        unsigned char seq[] = { 0xA0, 0xA1, 0xFF };
        feed(t, seq);
        CHECK(cell(t, 0, 0)->codePoint == 0x00A0);
        CHECK(cell(t, 0, 1)->codePoint == '!');
        CHECK(cell(t, 0, 2)->codePoint == 0x00FF);
    }

    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        Ubiquitty::Modes::setInputEncoding(&t, Ubiquitty::Unicode::Encoding::Latin1); // GR exists only in an 8-bit coding
        feed(t, "\033P1;1;0;8;0;0;8;0{@~~~~~~~~/BBBBBBBB\033\\");
        feed(t, "\033)@");
        feed(t, "\033~");
        unsigned char seq[] = { 0xA0, 0xA1, 0xFF };
        feed(t, seq);
        CHECK(cell(t, 0, 0)->codePoint == 0x00A0);
        CHECK(cell(t, 0, 1)->codePoint == Ubiquitty::VT::drcsCodePointBase + 1);
        CHECK(cell(t, 0, 2)->codePoint == 0x00FF);
    }
}

/// DECDLD Pcn names the first cell to load, and DEC reads 0 and 1 both as the
/// first character of the set. A 94-set's first character is '!', so a
/// download at Pcn 0 must not land on SPACE: the demo that hit this printed
/// its first bitmap in every space of the line. A 96-set holds SPACE, and
/// there 0 is SPACE.
TEST(testDecdldPcnZeroStartsA94SetAtBang)
{
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        Ubiquitty::Modes::setInputEncoding(&t, Ubiquitty::Unicode::Encoding::Latin1); // GR exists only in an 8-bit coding
        feed(t, "\033P1;0;0;8;0;0;8;0{@~~~~~~~~/BBBBBBBB\033\\");
        feed(t, "\033)@\033~");
        unsigned char seq[] = { 0xA0, 0xA1, 0xA2 };
        feed(t, seq);
        CHECK(cell(t, 0, 0)->codePoint == 0x00A0); // SPACE stays SPACE
        CHECK(cell(t, 0, 1)->codePoint == Ubiquitty::VT::drcsCodePointBase + 1);
        CHECK(cell(t, 0, 2)->codePoint != Ubiquitty::VT::drcsCodePointBase + 2); // one bitmap, one cell
    }
    {
        // A 96-set is named with the DRCS intermediate and designated the same way.
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        Ubiquitty::Modes::setInputEncoding(&t, Ubiquitty::Unicode::Encoding::Latin1); // GR exists only in an 8-bit coding
        feed(t, "\033P1;0;0;8;0;0;8;1{ @~~~~~~~~/BBBBBBBB\033\\");
        feed(t, "\033- @\033~");
        unsigned char seq[] = { 0xA0, 0xA1 };
        feed(t, seq);
        CHECK(cell(t, 0, 0)->codePoint == Ubiquitty::VT::drcsCodePointBase);
        CHECK(cell(t, 0, 1)->codePoint != Ubiquitty::VT::drcsCodePointBase + 1);
    }
}

TEST(testS7c1tS8c1tReplyC1Form)
{
    Ubiquitty::Terminal t;
    char outputBuffer[64];
    /// S7C1T (7-bit): reply begins ESC '['.
    Ubiquitty::hardReset(t);
    feed(t, "\033 F"); // S7C1T
    feed(t, "\033[c"); // DA1
    int n = drain(t, outputBuffer);
    CHECK(n >= 2);
    CHECK((unsigned char)outputBuffer[0] == 0x1B && outputBuffer[1] == '[');
    /// S8C1T (8-bit): reply begins with the single CSI byte 0x9B.
    Ubiquitty::hardReset(t);
    feed(t, "\033 G"); // S8C1T
    feed(t, "\033[c"); // DA1
    n = drain(t, outputBuffer);
    CHECK(n >= 1);
    CHECK((unsigned char)outputBuffer[0] == 0x9B);
}

TEST(testControlCharactersInsideCSI)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[H"); // home
    feed(t, "XYZ"); // row 0: X Y Z, cursor at column 3
    // CSI 4 <CR> 2 m: the CR moves the cursor to column 0 mid-sequence,
    // then SGR 42 (green background) dispatches with parameter "42".
    feed(t, "\033[4\r2m");
    feed(t, "Q"); // lands at column 0 (CR worked), background = SGR 42
    CHECK(cell(t, 0, 0)->codePoint == 'Q');
    CHECK(cell(t, 0, 0)->pen.backgroundIndex == 2);
    // The CSI still dispatched, so 'Q' is the only new content on the
    // row's start; Y/Z untouched by the sequence itself.
    CHECK(cell(t, 0, 1)->codePoint == 'Y');
    CHECK(cell(t, 0, 2)->codePoint == 'Z');
}

TEST(testCSIMissEmitsDiag)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    CaptureDiagClient diag;
    t.diagnosticsClient = &diag;
    // "CSI ? 9999 h" is a DECSET with an unrecognised mode number
    // and uses the '?' private prefix. csiPrefixedLookup misses on
    // ('?', 0, 'h') for ps==9999 — well, actually it dispatches via
    // the ? h handler (DECSET). Use a ('?', 0, 'Z') instead which
    // has no handler at all.
    feed(t, "\033[?Z");
    CHECK(diag.csiMiss == 1);
    // (priv='?'=0x3F)<<16 | (int=0)<<8 | final='Z'=0x5A
    CHECK(diag.lastDetail == ((((uint32_t)0x3F) << 16) | ((uint32_t)'Z')));
    // Plain unknown CSI: 'Z' is not a recognised final.
    diag.csiMiss = 0;
    feed(t, "\033[Z");
    // 'Z' is CBT (cursor backward tab) which is recognised; pick a
    // truly unrecognised final like '_'.
    diag.csiMiss = 0;
    feed(t, "\033[_");
    CHECK(diag.csiMiss == 1);
    CHECK(diag.lastDetail == (uint32_t)'_');
    t.diagnosticsClient = nullptr;
}

TEST(testDCSTmuxPassthroughRecursionLimit)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    std::string payload = nestedCompactTmuxPassthrough(Ubiquitty::VT::maxDCSTmuxPassthroughDepth, "A");
    Ubiquitty::processBytes(&t, payload);
    CHECK(cell(t, 0, 0)->codePoint == 'A');
    CHECK(t.screens.active->cursorX == 1);
    CHECK(t.dcsTmuxPassthroughDepth == 0);

    t.dcsTmuxPassthroughDepth = Ubiquitty::VT::maxDCSTmuxPassthroughDepth;
    payload = nestedCompactTmuxPassthrough(1, "B");
    Ubiquitty::processBytes(&t, payload);
    CHECK(cell(t, 0, 1)->codePoint == 0);
    CHECK(t.screens.active->cursorX == 1);
    CHECK(t.dcsTmuxPassthroughDepth == Ubiquitty::VT::maxDCSTmuxPassthroughDepth);
    t.dcsTmuxPassthroughDepth = 0;

    feed(t, "C");
    CHECK(cell(t, 0, 1)->codePoint == 'C');
}

TEST(testDCSPayloadGrowsNoTruncation)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    CaptureDiagClient diag;
    t.diagnosticsClient = &diag;
    // A 600-byte DECRQSS ('$q') payload exceeds the 256-byte baseline.
    // The accumulator must grow to hold every byte (no clip).
    char payload[600];
    memset(payload, 'A', sizeof(payload));
    feed(t, "\033P$q");
    // The length is stated, because `payload` carries no terminator: `feed`
    // takes a string_view, a char array decays to a pointer, and the view's
    // constructor would call strlen and read off the end of the buffer. What
    // the parser then received was decided by the next zero byte on the stack,
    // so this case passed or failed on layout rather than on the accumulator
    // it names -- and a pass proved nothing, since 600 bytes were never
    // reliably fed.
    feed(t, std::string_view(payload, sizeof(payload)));
    // Before the ST terminator: all bytes accumulated, buffer grew.
    CHECK(t.parser.osc.length == 600);
    CHECK(t.parser.osc.capacity >= 600);
    feed(t, "\033\\");
    CHECK(diag.dcsTrunc == 0);
    t.diagnosticsClient = nullptr;
}

// Which ceiling an OSC gets is decided by re-reading its leading digits from the
// accumulator on every appended byte, so how far that read goes is a cost paid
// per byte, not once. Bounding it changes what a very long run of digits
// resolves to, which is what these pin.
TEST(testOSCCeilingResolvesFromABoundedRunOfDigits)
{
    const std::string big(Ubiquitty::VT::maxOSCNonImageSize + 4096, 'A');

    // A clipboard OSC keeps the large ceiling, so its payload is not clipped.
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        feed(t, "\033]52;c;");
        feed(t, big);
        CHECK(t.parser.osc.length > Ubiquitty::VT::maxOSCNonImageSize);
    }
    // Leading zeros are part of the same number, and stay so up to the bound: a
    // Ps written 00000052 still names the clipboard.
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        feed(t, "\033]00000052;c;");
        feed(t, big);
        CHECK(t.parser.osc.length > Ubiquitty::VT::maxOSCNonImageSize);
    }
    // Past the bound the digits are no longer read as a code. Nothing emits a Ps
    // that long, and the alternative is reading the whole payload to find out.
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        feed(t, "\033]000000000052;c;");
        feed(t, big);
        CHECK(t.parser.osc.length == Ubiquitty::VT::maxOSCNonImageSize);
    }
    // A payload of nothing but digits is the shape that made the re-read
    // quadratic: `ps * 10 + 0` stays at zero, so no overflow guard ever fires and
    // the scan ran to the end on every byte. It resolves to the low ceiling.
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        feed(t, "\033]");
        feed(t, std::string(Ubiquitty::VT::maxOSCNonImageSize + 4096, '0'));
        CHECK(t.parser.osc.length == Ubiquitty::VT::maxOSCNonImageSize);
    }
}

TEST(testMaxDCSNonimageBytes)
{
    const std::string big(Ubiquitty::VT::maxOSCNonImageSize + 4096, 'A'); // past the low ceiling

    // Bulk feed exercises the batched run path (appendDCSPayloadRun); no ST, so
    // the accumulator holds the in-flight DCS, clipped at the non-image ceiling.
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        feed(t, "\033P+q"); // XTGETTCAP DCS, no terminator
        feed(t, big);
        CHECK(t.parser.osc.length == Ubiquitty::VT::maxOSCNonImageSize);
        CHECK(t.parser.osc.capacity <= 2 * Ubiquitty::VT::maxOSCNonImageSize); // bounded, not 64 MiB
        CHECK(t.dcsTruncated);
    }
    // Byte-at-a-time feed exercises the per-byte path (actDCSPayloadPut) — same clip.
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        feed(t, "\033P$q"); // DECRQSS DCS
        for (int i = 0; i < Ubiquitty::VT::maxOSCNonImageSize + 4096; ++i) {
            const unsigned char c = 'A';
            feed(t, std::span(&c, 1));
        }
        CHECK(t.parser.osc.length == Ubiquitty::VT::maxOSCNonImageSize);
        CHECK(t.dcsTruncated);
    }
}

TEST(testRawC1CSI)
{
    /// 8-bit CSI (0x9B) on the single-byte legacy path (where 0x80-0x9F are C1);
    /// in UTF-8 it is a continuation byte — see testC1RawGatedOffInUTF8.
    Ubiquitty::Terminal t;
    unsigned char seq[] = { 0x9B, '3', '1', 'm', 'R' };
    Ubiquitty::hardReset(t);
    Ubiquitty::Unicode::encReset(&t.parser.inputEncoding, Ubiquitty::Unicode::Encoding::Latin1);
    feed(t, seq);
    CHECK(cell(t, 0, 0)->codePoint == 'R');
    CHECK(cell(t, 0, 0)->pen.foregroundIndex == 1);
}

TEST(testMultibyteLeadBytesNotStolenAsC1)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::Unicode::encReset(&t.parser.inputEncoding, Ubiquitty::Unicode::Encoding::ShiftJis);
    unsigned char sjis[] = { 0x82, 0xA0 }; // あ U+3042 — 0x82 is a lead byte
    feed(t, sjis);
    CHECK(cell(t, 0, 0)->codePoint == 0x3042);

    Ubiquitty::hardReset(t);
    Ubiquitty::Unicode::encReset(&t.parser.inputEncoding, Ubiquitty::Unicode::Encoding::EucJp);
    unsigned char kana[] = { 0x8E, 0xB1 }; // ｱ U+FF71 — 0x8E is EUC SS2
    feed(t, kana);
    CHECK(cell(t, 0, 0)->codePoint == 0xFF71);
}

TEST(testCSINumericSaturation)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[999999999999999999999999999999C!");
    CHECK(t.screens.active->cursorX == Ubiquitty::defaultColumns - 1);
    CHECK(cell(t, 0, Ubiquitty::defaultColumns - 1)->codePoint == '!');
}

TEST(testPaneLookupRoutesOutput)
{
    Ubiquitty::Terminal primary;
    Ubiquitty::Terminal children[2];
    Ubiquitty::hardReset(primary);
    Ubiquitty::hardReset(children[0]);
    Ubiquitty::hardReset(children[1]);

    // Hook the primary so decoded %output bytes go to the right child.
    primary.integration.paneRouter.lookup = fakePaneLookup;
    primary.integration.paneRouter.context = children;

    /// Engage tmux Control Mode on the primary.
    Ubiquitty::Protocol::Tmux::configure(&primary.integration.tmux, Ubiquitty::Protocol::Tmux::IntegrationMode::Enabled, Ubiquitty::Protocol::Tmux::AutoDetectMode::Automatic);
    feed(primary, "%begin 1 1 0\r\n%end 1 1 0\r\n");

    // %output %0 → children[0] should receive 'A'.
    feed(primary, "%output %0 A\r\n");
    // %output %1 → children[1] should receive 'B'.
    feed(primary, "%output %1 B\r\n");

    CHECK(cell(children[0], 0, 0)->codePoint == 'A');
    CHECK(cell(children[1], 0, 0)->codePoint == 'B');
    // Primary stays untouched — it shouldn't have received either byte.
    CHECK(cell(primary, 0, 0)->codePoint == 0);

    // Pane 7 is unknown (lookup returns nullptr): byte must be dropped, not
    // written to primary. Otherwise the pre-tmux shell screen on the
    // primary gets corrupted with overlapping output as panes are
    // still being asynchronously spawned.
    feed(primary, "%output %7 X\r\n");
    CHECK(cell(primary, 0, 0)->codePoint == 0);
    CHECK(cell(children[0], 0, 1)->codePoint == 0);
    CHECK(cell(children[1], 0, 1)->codePoint == 0);
}

TEST(testPaneLookupMemoizedAcrossRun)
{
    Ubiquitty::Terminal primary;
    Ubiquitty::Terminal children[2];
    Ubiquitty::hardReset(primary);
    Ubiquitty::hardReset(children[0]);
    Ubiquitty::hardReset(children[1]);

    PaneLookupCounter counter = { children, 0, false };
    primary.integration.paneRouter.lookup = countingPaneLookup;
    primary.integration.paneRouter.context = &counter;
    Ubiquitty::Protocol::Tmux::configure(&primary.integration.tmux, Ubiquitty::Protocol::Tmux::IntegrationMode::Enabled, Ubiquitty::Protocol::Tmux::AutoDetectMode::Automatic);
    feed(primary, "%begin 1 1 0\r\n%end 1 1 0\r\n");

    // (1) One frame, five payload bytes to one pane → one lookup, not five.
    counter.count = 0;
    feed(primary, "%output %0 ABCDE\r\n");
    CHECK(counter.count == 1);
    CHECK(cell(children[0], 0, 0)->codePoint == 'A');
    CHECK(cell(children[0], 0, 4)->codePoint == 'E');

    // (2) A pane change mid-pass re-looks-up: two panes in one parse pass → two.
    counter.count = 0;
    feed(primary, "%output %0 A\r\n%output %1 B\r\n");
    CHECK(counter.count == 2);

    // (3) The end-of-pass flush invalidates: the same pane across two passes
    // re-looks-up each pass (the frontend released the child lock/ref).
    counter.count = 0;
    feed(primary, "%output %0 A\r\n");
    feed(primary, "%output %0 A\r\n");
    CHECK(counter.count == 2);

    // (4) A nullptr (pane pending spawn) is never cached — retried on the next run.
    counter.count = 0;
    counter.forceNull = true;
    feed(primary, "%output %0 ABCD\r\n");
    CHECK(counter.count == 1);
    counter.forceNull = false;

    // (5) Same pane across FRAME boundaries within one pass: the memo persists
    // (no flush ran), so two frames for one pane still cost one lookup.
    counter.count = 0;
    feed(primary, "%output %0 AB\r\n%output %0 CD\r\n");
    CHECK(counter.count == 1);

    /// (6) An interleaved unresolved pane kills the memo: resolving a different
    /// pane makes the frontend release the previous pane's lock/pin, so the
    /// return to pane 0 must re-look-up (re-acquire), never reuse the old memo.
    /// Count: 1 (pane 0) + 1 (pane 7 nullptr) + 1 (pane 0 re-acquired).
    Ubiquitty::hardReset(children[0]);
    counter.count = 0;
    feed(primary, "%output %0 AB\r\n%output %7 XY\r\n%output %0 CD\r\n");
    CHECK(counter.count == 3);
    CHECK(cell(children[0], 0, 0)->codePoint == 'A');
    CHECK(cell(children[0], 0, 3)->codePoint == 'D'); // delivery resumed after re-acquire
}

TEST(testPaneLookupRoutesPerByteFeed)
{
    // The single-byte public API (processByte) feeds tmux one char at a time;
    // the decoder then delivers the %output payload through the per-byte
    // callback, which routes through the pane router. A pane the lookup does
    // not know drops its bytes.
    Ubiquitty::Terminal primary;
    Ubiquitty::Terminal children[2];
    Ubiquitty::hardReset(primary);
    Ubiquitty::hardReset(children[0]);
    Ubiquitty::hardReset(children[1]);

    primary.integration.paneRouter.lookup = fakePaneLookup;
    primary.integration.paneRouter.context = children;
    Ubiquitty::Protocol::Tmux::configure(&primary.integration.tmux, Ubiquitty::Protocol::Tmux::IntegrationMode::Enabled, Ubiquitty::Protocol::Tmux::AutoDetectMode::Automatic);

    const std::string begin = "%begin 1 1 0\r\n";
    for (char c : begin)
        Ubiquitty::processByte(&primary, static_cast<uint8_t>(c));

    const std::string output0 = "%output %0 A\r\n";
    for (char c : output0)
        Ubiquitty::processByte(&primary, static_cast<uint8_t>(c));
    const std::string output7 = "%output %7 B\r\n";
    for (char c : output7)
        Ubiquitty::processByte(&primary, static_cast<uint8_t>(c));

    CHECK(cell(children[0], 0, 0)->codePoint == 'A');
    CHECK(cell(primary, 0, 0)->codePoint == 0); // pane 7 unknown → dropped, not written to primary
}

TEST(testPaneRouteDiesOnMidpassLayoutChange)
{
    Ubiquitty::Terminal primary;
    Ubiquitty::Terminal children[2];
    Ubiquitty::hardReset(primary);
    Ubiquitty::hardReset(children[0]);
    Ubiquitty::hardReset(children[1]);

    PaneLookupCounter counter = { children, 0, false };
    primary.integration.paneRouter.lookup = countingPaneLookup;
    primary.integration.paneRouter.context = &counter;
    Ubiquitty::Protocol::Tmux::configure(&primary.integration.tmux, Ubiquitty::Protocol::Tmux::IntegrationMode::Enabled, Ubiquitty::Protocol::Tmux::AutoDetectMode::Automatic);

    Ubiquitty::Protocol::Tmux::TmuxEventClient client;
    client.context = &primary;
    client.layoutChange = [](void* context, int, std::string_view) {
        Ubiquitty::Terminal* primaryPointer = static_cast<Ubiquitty::Terminal*>(context);
        /// The production handler releases the deliver pin/lock, whose flush
        /// invalidates the route memo; model exactly that release side effect.
        Ubiquitty::invalidateTmuxPaneRoute(primaryPointer);
    };
    Ubiquitty::Protocol::Tmux::setEventClient(&primary.integration.tmux, client);
    feed(primary, "%begin 1 1 0\r\n%end 1 1 0\r\n");

    counter.count = 0;
    feed(primary, "%output %0 AB\r\n%layout-change @1 abcd,80x24,0,0,1\r\n%output %0 CD\r\n");
    CHECK(counter.count == 2); // re-looked-up after the mid-pass release
    CHECK(cell(children[0], 0, 0)->codePoint == 'A');
    CHECK(cell(children[0], 0, 3)->codePoint == 'D');
}

TEST(testDelByteIsNoopInGround)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    /// Position at (0,0), feed DEL, then a printable. The printable
    /// must land at column 0 — DEL did not advance the cursor and
    /// did not write a cell.
    static const unsigned char bytes[] = { 0x7F, 'A' };
    feed(t, bytes);
    CHECK(cell(t, 0, 0)->codePoint == 'A');
    CHECK(cell(t, 0, 1)->codePoint == 0);
}

TEST(testDecid8bitC1)
{
    Ubiquitty::Terminal t;
    char da1[64];
    char decid[64];
    Ubiquitty::hardReset(t);
    feed(t, "\033[c");
    int n1 = drain(t, da1);
    CHECK(n1 > 0);

    /// 0x9A is the C1 DECID only on a single-byte legacy path; in UTF-8 it is a
    /// continuation byte (gated off — see testC1RawGatedOffInUTF8).
    Ubiquitty::hardReset(t);
    Ubiquitty::Unicode::encReset(&t.parser.inputEncoding, Ubiquitty::Unicode::Encoding::Latin1);
    unsigned char c1[] = { 0x9A };
    feed(t, c1);
    int n2 = drain(t, decid);
    CHECK(n2 == n1);
    CHECK(memcmp(da1, decid, (size_t)n1) == 0);
    // And nothing was printed to the grid (no U+FFFD from the Print path).
    CHECK(cell(t, 0, 0)->codePoint == 0);
}

TEST(test8bitC1ControlsInReplies)
{
    Ubiquitty::Terminal t;
    char outputBuffer[64];
    int n;
    Ubiquitty::hardReset(t);
    feed(t, "\033[5;7H");

    feed(t, "\033 G"); // S8C1T — enable 8-bit C1
    feed(t, "\033[6n");
    n = drain(t, outputBuffer);
    CHECK(n == 5); // 0x9B + "5;7R", one byte shorter
    CHECK((unsigned char)outputBuffer[0] == 0x9B); // CSI, not ESC [
    CHECK(strcmp(outputBuffer + 1, "5;7R") == 0);

    feed(t, "\033 F"); // S7C1T — back to 7-bit
    feed(t, "\033[6n");
    n = drain(t, outputBuffer);
    CHECK(strcmp(outputBuffer, "\033[5;7R") == 0);
}

TEST(testCSILfStaysOnGenericPath)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    feed(t, "ABCDE");
    feed(t, "\033[4\n2m");
    CHECK(t.screens.active->cursorY == 1);
    CHECK(t.screens.active->cursorX == 5);
    feed(t, "Z");
    CHECK(cell(t, 1, 5)->codePoint == 'Z');
    CHECK(cell(t, 1, 5)->pen.backgroundIndex == 2);
}

/// PM (ESC ^ … ESC \) — same shape as SOS.
TEST(testPmConsumedSilently)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\r");
    feed(t, "\033^private-msg\033\\");
    // PM is a string consumer: the payload reaches no cell.
    CHECK(cell(t, 0, 0)->codePoint == 0);
}

/// SOS via the 8-bit C1 introducer 0x98 — must consume its string
/// exactly like ESC X. Regression for the C1-dispatch leak where 0x98
/// fell through to the default branch and the payload wasRendered.
///
/// 8-bit SOS (0x98) opens a string only on the single-byte legacy path; in
/// UTF-8 it is a continuation byte and its payload prints (gated off — see
/// testC1RawGatedOffInUTF8).
TEST(testSos8bitConsumedSilently)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::Unicode::encReset(&t.parser.inputEncoding, Ubiquitty::Unicode::Encoding::Latin1);
    feed(t, "\r");
    int startX = t.screens.active->cursorX;
    int startY = t.screens.active->cursorY;
    feed(t,
        "\x98"
        "payload-text"
        "\033\\");
    CHECK(cell(t, 0, 0)->codePoint == 0);
    CHECK(t.screens.active->cursorX == startX);
    CHECK(t.screens.active->cursorY == startY);
}

/// PM via the 8-bit C1 introducer 0x9E — same string-consumer shape
/// (single-byte legacy path).
TEST(testPm8bitConsumedSilently)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::Unicode::encReset(&t.parser.inputEncoding, Ubiquitty::Unicode::Encoding::Latin1);
    feed(t, "\r");
    feed(t,
        "\x9e"
        "private-msg"
        "\033\\");
    CHECK(cell(t, 0, 0)->codePoint == 0);
}

/// APC (ESC _ … ESC \) — payloads not prefixed by 'G' must be
/// consumed silently (the 'G' branch is the Kitty graphics handler,
/// exercised elsewhere). Ubiquitty does route APC into a string
/// consumer (parseAPC), so this should pass green.
TEST(testAPCConsumedSilently)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\r");
    feed(t, "\033_anything-non-kitty\033\\");
    // Payload must not have leaked into row 0.
    CHECK(cell(t, 0, 0)->codePoint == 0);
    CHECK(cell(t, 0, 1)->codePoint == 0);
    CHECK(cell(t, 0, 5)->codePoint == 0);
}

// BEL executes inside DEC DCS and PM without ending the string.
TEST(testBelInsideAControlStringRingsAndTheStringGoesOn)
{
    for (const char* introducer : { "\033P", "\033Pzh", "\033^" }) {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        feed(t, "A");
        t.bellPending = false;
        feed(t, introducer);
        feed(t, "hi\007B\033\\C");
        CHECK(t.bellPending);
        CHECK(cell(t, 0, 0)->codePoint == 'A');
        CHECK(cell(t, 0, 1)->codePoint == 'C');
        CHECK(cell(t, 0, 2)->codePoint == 0);
    }
}

TEST(testC1CSI8bit)
{
    Ubiquitty::Terminal t;
    static const unsigned char seq[] = { 0x9B, 'A' };
    Ubiquitty::hardReset(t);
    Ubiquitty::Unicode::encReset(&t.parser.inputEncoding, Ubiquitty::Unicode::Encoding::Latin1);
    feed(t, "\033[5;1H"); // park at row 5
    CHECK(t.screens.active->cursorY == 4);
    feed(t, seq);
    CHECK(t.screens.active->cursorY == 3); // 8-bit CSI A moved the cursor up
}

TEST(testC1St8bitTerminator)
{
    Ubiquitty::Terminal t;
    static const unsigned char seq[] = { 0x1B, ']', '2', ';', 'A', 0x9C, 'X', 0x07 };
    Ubiquitty::hardReset(t);
    feed(t, seq);
    // 0x9C must be treated as payload, not as ST. If 0x9C aborted the
    // OSC, the 'X' would have wasRendered at column 0.
    CHECK(cell(t, 0, 0)->codePoint == 0);
    // And the title accumulated the 0x9C + X as payload.
    CHECK(t.titleChanged == true);
}

TEST(testCSIQuestionAtStartIsPrivatePrefix)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    CHECK(t.modes.appCursorKeys == false);
    feed(t, "\033[?1h"); // DECCKM set
    CHECK(t.modes.appCursorKeys == true);
    // And LNM (ANSI public mode 20) must not have flipped — that would
    // indicate the parser routed the '?'-prefixed parameter to public-mode
    // dispatch.
    CHECK(t.modes.lnm == false);
    feed(t, "\033[?1l"); // DECCKM reset
    CHECK(t.modes.appCursorKeys == false);
}

TEST(testTmuxNoRouterDeliversPerByteThroughSlowDriver)
{
    Ubiquitty::Terminal primary;
    Ubiquitty::hardReset(primary);

    // Tmux control mode without a pane router: %output bytes are decoded and
    // delivered to the primary one byte at a time (no batched lanes), which is
    // the route by which the per-byte CSI parameter handlers run.
    Ubiquitty::Protocol::Tmux::configure(&primary.integration.tmux, Ubiquitty::Protocol::Tmux::IntegrationMode::Enabled, Ubiquitty::Protocol::Tmux::AutoDetectMode::Automatic);
    feed(primary, "%begin 1 1 0\r\n");
    feed(primary, "%output %0 \033[1;31mZ\r\n");

    CHECK(cell(primary, 0, 0)->codePoint == 'Z');
    CHECK(cell(primary, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Bold));
    CHECK(cell(primary, 0, 0)->pen.foregroundIndex == 1); // SGR 31 → red

    // A leading `;` opens a parameter slot from an empty parameter count —
    // the slow driver's other CSI-parameter arm.
    feed(primary, "%output %0 \033[;32mY\r\n");
    CHECK(cell(primary, 0, 1)->codePoint == 'Y');
    CHECK(cell(primary, 0, 1)->pen.foregroundIndex == 2); // SGR 32 → green
}

TEST(testKittyAPCPerByteAppendFailureLatchesOverflow)
{
    // Fill the APC accumulator to exactly maxAPCCapacity with batched runs,
    // then deliver one more byte through the per-byte tmux path: the append is
    // refused (length == limit), which latches the overflow exactly like a
    // larger refusal would. The APC state survives enabling tmux, so the tail
    // byte lands in the still-open APC.
    Ubiquitty::Terminal primary;
    Ubiquitty::hardReset(primary);
    feed(primary, "\033_G;"); // payload "G;" — 'G' is the first payload byte
    constexpr int chunkSize = 1024 * 1024;
    const int fullChunks = Ubiquitty::Kitty::maxAPCCapacity / chunkSize - 1; // 63 chunks = 63 MiB
    std::string chunk(static_cast<std::size_t>(chunkSize), 'a');
    for (int i = 0; i < fullChunks; i++)
        feed(primary, chunk);
    const int remainderBytes = Ubiquitty::Kitty::maxAPCCapacity - 2 - fullChunks * chunkSize; // to exactly maxAPCCapacity
    std::string remainder(static_cast<std::size_t>(remainderBytes), 'a');
    feed(primary, remainder);
    CHECK_EQ(primary.images.kittyAPC.length, Ubiquitty::Kitty::maxAPCCapacity);
    CHECK(primary.images.kittyAPCOverflow == false);

    Ubiquitty::Protocol::Tmux::configure(&primary.integration.tmux, Ubiquitty::Protocol::Tmux::IntegrationMode::Enabled, Ubiquitty::Protocol::Tmux::AutoDetectMode::Automatic);
    feed(primary, "%begin 1 1 0\r\n");
    feed(primary, "%output %0 x"); // one byte past the limit — per-byte append refused
    CHECK(primary.images.kittyAPCOverflow == true);
    CHECK(primary.images.kittyAPC.length == 0);
    feed(primary, "%output %0 \033X"); // ESC pair on a latched APC — dropped
    CHECK(primary.images.kittyAPCOverflow == true);
    feed(primary, R"(%output %0 \033\\)"); // tmux-escaped ST on a latched APC — returns without dispatching
    CHECK(primary.images.kittyAPCOverflow == true);
}

TEST(testTmuxPerByteLinuxConsoleOSC)
{
    // The Linux-console private OSC grammar (ESC ] P/R) is claimed per byte;
    // through the per-byte tmux delivery it exercises the linux-OSC fast path
    // that batched runs never reach.
    Ubiquitty::Terminal primary;
    Ubiquitty::hardReset(primary);
    Ubiquitty::Protocol::Tmux::configure(&primary.integration.tmux, Ubiquitty::Protocol::Tmux::IntegrationMode::Enabled, Ubiquitty::Protocol::Tmux::AutoDetectMode::Automatic);
    feed(primary, "%begin 1 1 0\r\n");

    feed(primary, "%output %0 \033]0;T"); // ordinary title OSC — the Linux claim declines
    CHECK(primary.parser.osc.length == 3); // "0;T" stored by the generic path
    feed(primary, R"(%output %0 \033\\)"); // ST terminates the title
    CHECK(primary.parser.parseState == Ubiquitty::VT::Parser::State::Ground);

    Ubiquitty::Modes::setEmulation(&primary, Ubiquitty::VT::Emulation::Linux);

    const auto resetRed = primary.palette.indexed[1].red;
    feed(primary, "%output %0 \033]P1FF0000"); // set palette entry 1 → red
    CHECK(primary.palette.indexed[1].red == Ubiquitty::channel8To16(0xFF));
    CHECK(primary.palette.indexed[1].green == Ubiquitty::channel8To16(0x00));
    CHECK(primary.parser.parseState == Ubiquitty::VT::Parser::State::Ground);

    feed(primary, "%output %0 \033]R"); // reset palette
    CHECK(primary.palette.indexed[1].red == resetRed);
    CHECK(primary.parser.parseState == Ubiquitty::VT::Parser::State::Ground);
    CHECK(primary.parser.osc.length == 0);
}

TEST(testTmuxPerByteDCSDeliversPayloadAndEscapes)
{
    // DCS payload bytes through the per-byte tmux path: header parameters, a
    // leading `;`, literal ESC pairs inside the payload, and a sixel payload
    // feeding the sixel accumulator — arms the batched lanes never reach.
    Ubiquitty::Terminal primary;
    Ubiquitty::hardReset(primary);
    Ubiquitty::Protocol::Tmux::configure(&primary.integration.tmux, Ubiquitty::Protocol::Tmux::IntegrationMode::Enabled, Ubiquitty::Protocol::Tmux::AutoDetectMode::Automatic);
    feed(primary, "%begin 1 1 0\r\n");

    feed(primary, "%output %0 \033P1$r0q\\033\\\\"); // DECRQSS per-byte
    CHECK(primary.parser.parseState == Ubiquitty::VT::Parser::State::Ground);

    feed(primary, "%output %0 \033P;1$r0q\\033\\\\"); // leading `;` opens the param slot
    CHECK(primary.parser.parseState == Ubiquitty::VT::Parser::State::Ground);

    feed(primary, "%output %0 \033P1$r0q\033Xq\\033\\\\"); // ESC pair stored literally
    CHECK(primary.parser.parseState == Ubiquitty::VT::Parser::State::Ground);

    feed(primary, "%output %0 \033Pq#1\033X\\033\\\\"); // sixel payload per-byte; ESC in sixel is dropped
    CHECK(primary.parser.parseState == Ubiquitty::VT::Parser::State::Ground);
}

TEST(testProcessByteNullGuards)
{
    // The public single-byte entry and the pane-route invalidator tolerate a
    // null terminal — the frontend's teardown races can deliver either.
    Ubiquitty::processByte(nullptr, 'x');
    Ubiquitty::invalidateTmuxPaneRoute(nullptr);
    Ubiquitty::processBytes(nullptr, std::string_view("x", 1));
    CHECK(Ubiquitty::processBytesUpToSyncClear(nullptr, std::string_view("x", 1)) == 0);
}

TEST(testTekEnterSegmentsMidBuffer)
{
    // DECSET 38 (Tek enter) dispatched mid-buffer: the remaining bytes are
    // consumed by the Tek state machine, not the VT lanes.
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[?38hAAA"); // CSI run lane dispatch → Tek segment
    CHECK(t.isTekModeActive == true);
    feed(t, "\033\003"); // ESC ETX exits Tek
    CHECK(t.isTekModeActive == false);

    // A CSI final arriving in a later feed is a bare-final dispatch — the
    // second Tek-entry site.
    Ubiquitty::hardReset(t);
    feed(t, "\033[?38");
    feed(t, "hB");
    CHECK(t.isTekModeActive == true);
    feed(t, "\033\003");
    CHECK(t.isTekModeActive == false);

    // A CSI split across feeds lands on the per-byte driver; the dispatch
    // enters Tek and the next byte opens a string run that the scan loop
    // still tries to batch — the post-run Tek checks.
    Ubiquitty::hardReset(t);
    feed(t, "\033[?38");
    feed(t, "h\033]0;AAA\033\003"); // OSC run after per-byte Tek entry
    CHECK(t.isTekModeActive == false); // ESC ETX ran inside Tek
    CHECK(t.parser.parseState == Ubiquitty::VT::Parser::State::Ground);

    Ubiquitty::hardReset(t);
    feed(t, "\033[?38");
    feed(t, "h\033_GAAA\033\003"); // APC run after per-byte Tek entry
    CHECK(t.isTekModeActive == false);
    CHECK(t.parser.parseState == Ubiquitty::VT::Parser::State::Ground);

    Ubiquitty::hardReset(t);
    feed(t, "\033[?38");
    feed(t, "h\033P0;1;2qAAA\033\003"); // DCS run after per-byte Tek entry
    CHECK(t.isTekModeActive == false);
    CHECK(t.parser.parseState == Ubiquitty::VT::Parser::State::Ground);
}

TEST(testC1StringTerminatorsPerByteMultibyte)
{
    // Under a multibyte input encoding a raw 0x9C is the string terminator
    // (not a UTF-8 continuation), delivered through the per-byte driver.
    Ubiquitty::Terminal primary;
    Ubiquitty::hardReset(primary);
    Ubiquitty::Unicode::encReset(&primary.parser.inputEncoding, Ubiquitty::Unicode::Encoding::EucJp);
    Ubiquitty::Protocol::Tmux::configure(&primary.integration.tmux, Ubiquitty::Protocol::Tmux::IntegrationMode::Enabled, Ubiquitty::Protocol::Tmux::AutoDetectMode::Automatic);
    feed(primary, "%begin 1 1 0\r\n");

    feed(primary, "%output %0 \033]0;T");
    CHECK(primary.parser.parseState == Ubiquitty::VT::Parser::State::OSC);
    feed(primary, "%output %0 \x9C");
    CHECK(primary.parser.parseState == Ubiquitty::VT::Parser::State::Ground);

    feed(primary, "%output %0 \033P1$r0q");
    CHECK(primary.parser.parseState == Ubiquitty::VT::Parser::State::DCSPassthrough);
    feed(primary, "%output %0 \x9C");
    CHECK(primary.parser.parseState == Ubiquitty::VT::Parser::State::Ground);

    feed(primary, "%output %0 \033_Gz");
    CHECK(primary.parser.parseState == Ubiquitty::VT::Parser::State::APC);
    feed(primary, "%output %0 \x9C");
    CHECK(primary.parser.parseState == Ubiquitty::VT::Parser::State::Ground);

    feed(primary, "%output %0 \033^z");
    CHECK(primary.parser.parseState == Ubiquitty::VT::Parser::State::SOSPMAPC);
    feed(primary, "%output %0 \x9C");
    CHECK(primary.parser.parseState == Ubiquitty::VT::Parser::State::Ground);
}

TEST(testUTF8ContinuationRoutingInGroundAndVT52)
{
    // A mid-sequence continuation byte in Ground is a glyph, never a C1 —
    // the routing guard sends it to putCharacter.
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\xC3"); // incomplete lead — utf8.need > 0
    feed(t, "\x80");
    CHECK(cell(t, 0, 0)->codePoint == 0x00C0); // À = C3 80

    // Same guard in VT52 ground.
    Ubiquitty::hardReset(t);
    feed(t, "\033[?2l"); // enter VT52
    feed(t, "\xC3");
    feed(t, "\x80");
    CHECK(cell(t, 0, 0)->codePoint == 0x00C0);
}

TEST(testTranscodeLoopBreakAndSyncStopVariant)
{
    // The multibyte transcode loop breaks when the decoder cannot consume the
    // trailing byte (a lone lead) — the non-stop and stop variants alike.
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::Unicode::encReset(&t.parser.inputEncoding, Ubiquitty::Unicode::Encoding::EucJp);
    feed(t, "\x8E"); // EUC SS2 lead with no trail — nothing emitted
    CHECK(cell(t, 0, 0)->codePoint == 0);

    // The stop variant consumes the lone lead without emitting a glyph
    // (fresh terminal so no prior mid-sequence carries into the call).
    Ubiquitty::Terminal fresh;
    Ubiquitty::hardReset(fresh);
    Ubiquitty::Unicode::encReset(&fresh.parser.inputEncoding, Ubiquitty::Unicode::Encoding::EucJp);
    const int consumed = Ubiquitty::processBytesUpToSyncClear(&fresh, std::string_view("\x8E", 1));
    CHECK(consumed == 1);
    CHECK(cell(fresh, 0, 0)->codePoint == 0);

    // A trailing byte after the clear keeps the stop observable: the frame
    // returns exactly up to (and including) the 2026-clear.
    Ubiquitty::hardReset(t);
    Ubiquitty::Unicode::encReset(&t.parser.inputEncoding, Ubiquitty::Unicode::Encoding::EucJp);
    const char frame[] = "\033[?2026hA\033[?2026lZ";
    const int first = Ubiquitty::processBytesUpToSyncClear(&t, std::string_view(frame, sizeof(frame) - 1));
    CHECK(first < static_cast<int>(sizeof(frame) - 1)); // stopped at the clear
    CHECK(first > 0);
    const int rest = Ubiquitty::processBytesUpToSyncClear(&t, std::string_view(frame + first, sizeof(frame) - 1 - static_cast<std::size_t>(first)));
    CHECK(rest == static_cast<int>(sizeof(frame) - 1) - first);
    CHECK(cell(t, 0, 0)->codePoint == 'A');
}

TEST(testSyncClearWithTmuxDeliversPerByte)
{
    // With tmux integration engaged the sync-clear stop degrades to the
    // per-byte public route; the 2026-clear still ends the window.
    Ubiquitty::Terminal primary;
    Ubiquitty::hardReset(primary);
    Ubiquitty::Protocol::Tmux::configure(&primary.integration.tmux, Ubiquitty::Protocol::Tmux::IntegrationMode::Enabled, Ubiquitty::Protocol::Tmux::AutoDetectMode::Automatic);
    feed(primary, "%begin 1 1 0\r\n");
    const char frame[] = "%output %0 \033[?2026hA\033[?2026lB\r\n";
    const int consumed = Ubiquitty::processBytesUpToSyncClear(&primary, std::string_view(frame, sizeof(frame) - 1));
    CHECK(consumed < static_cast<int>(sizeof(frame) - 1));
    CHECK(consumed > 0);
}

TEST(testSyncClearSessionLogWrites)
{
    // The sync-clear entry writes the consumed slice to the session log.
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    t.sessionLogFile = std::tmpfile();
    CHECK(t.sessionLogFile != nullptr);
    t.sessionLogCaptureEnabled = true;
    const char frame[] = "\033[?2026hA\033[?2026lZ";
    const int consumed = Ubiquitty::processBytesUpToSyncClear(&t, std::string_view(frame, sizeof(frame) - 1));
    CHECK(consumed < static_cast<int>(sizeof(frame) - 1));
    std::fclose(static_cast<FILE*>(t.sessionLogFile));
    t.sessionLogFile = nullptr;
    t.sessionLogCaptureEnabled = false;
}

TEST(testTmuxPerByteDCSPayloadClippedAtMaxOSCNonImageSize)
{
    // A per-byte DCS payload past 64 KiB clips at the non-image ceiling and
    // records the truncation once — the per-byte twin of the batched clip.
    Ubiquitty::Terminal primary;
    Ubiquitty::hardReset(primary);
    Ubiquitty::Protocol::Tmux::configure(&primary.integration.tmux, Ubiquitty::Protocol::Tmux::IntegrationMode::Enabled, Ubiquitty::Protocol::Tmux::AutoDetectMode::Automatic);

    const std::string open = "%begin 1 1 0\r\n%output %0 \033P1$r\r\n";
    for (char c : open)
        Ubiquitty::processByte(&primary, static_cast<uint8_t>(c));

    std::string payload = "%output %0 ";
    payload.append(Ubiquitty::VT::maxOSCNonImageSize + 64, 'a');
    payload.append("\r\n");
    for (char c : payload)
        Ubiquitty::processByte(&primary, static_cast<uint8_t>(c));

    CHECK_EQ(primary.parser.osc.length, Ubiquitty::VT::maxOSCNonImageSize); // clipped at the ceiling
    CHECK(primary.dcsTruncated == true);
    for (char c : std::string("%output %0 \\033\\\\\r\n"))
        Ubiquitty::processByte(&primary, static_cast<uint8_t>(c));
    CHECK(primary.parser.parseState == Ubiquitty::VT::Parser::State::Ground);
}

TEST(testDCSListParse)
{
    Ubiquitty::OSC::DCSKind outputBuffer[8];
    int n = Ubiquitty::OSC::parseDCSList("sixel, DECUDK  decrqss bogus xtgettcap tmux", outputBuffer, 8);
    CHECK(n == 5);
    CHECK(outputBuffer[0] == Ubiquitty::OSC::DCSKind::Sixel);
    CHECK(outputBuffer[1] == Ubiquitty::OSC::DCSKind::DECUDK);
    CHECK(outputBuffer[2] == Ubiquitty::OSC::DCSKind::DECRQSS);
    CHECK(outputBuffer[3] == Ubiquitty::OSC::DCSKind::XTGETTCAP);
    CHECK(outputBuffer[4] == Ubiquitty::OSC::DCSKind::Tmux);
}

// A processBytesUpToSyncClear return that consumed its whole bite leaves the
// deferred scroll-lane run open — the flood pipeline's cadence — and the
// reader-side entry flush (termFlushScrollLane) commits it.
TEST(testTermFlushScrollLaneCommitsOpenRun)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 3, 10);
    t.isScrollLaneEnabled = true;

    feed(t, "1234567890\r\n4567890123\r\n"); // cursor now at the bottom row
    const char bite[] = "y\r\n"; // one lane-eligible linefeed, no sync clear
    const int consumed = Ubiquitty::processBytesUpToSyncClear(&t, std::string_view(bite, sizeof(bite) - 1));
    CHECK(consumed == static_cast<int>(sizeof(bite) - 1)); // input-exhausted return
    CHECK(t.scrollLaneRun.active == true); // the run survives the return
    CHECK(t.scrollbackCount == 1); // the departing row is already in history

    Ubiquitty::termFlushScrollLane(t);
    CHECK(t.scrollLaneRun.active == false); // committed
    CHECK(t.scrollbackCount == 1);

    feed(t, "z"); // the screen still takes text normally
    CHECK(cell(t, 2, 0)->codePoint == 'z');
}

// SUB draws the error character where it stands and advances like a printed
// character, in the current rendition; inside a sequence it first abandons
// the sequence. Cancel (0x18) abandons a sequence and draws nothing.
TEST(testSubstituteDrawsTheErrorCharacterAndCancelDrawsNothing)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "AB\x1A"
            "C");
    CHECK(cell(t, 0, 2)->codePoint == Ubiquitty::VT::substituteCharacter);
    CHECK(cell(t, 0, 3)->codePoint == 'C');
    CHECK(t.screens.active->cursorX == 4);

    Ubiquitty::hardReset(t);
    feed(t, "A\033[1\x1A"
            "C"); // an unfinished CUF
    CHECK(cell(t, 0, 1)->codePoint == Ubiquitty::VT::substituteCharacter);
    CHECK(cell(t, 0, 2)->codePoint == 'C');

    Ubiquitty::hardReset(t);
    feed(t, "\033[1m\x1A");
    CHECK(cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Bold));

    for (const char* opener : { "\033", "\033#", "\033]0;x", "\033Pq", "\033_G" }) {
        Ubiquitty::hardReset(t);
        feed(t, opener);
        feed(t, "\x1A"
                "C");
        CHECK_MSG(cell(t, 0, 0)->codePoint == Ubiquitty::VT::substituteCharacter, opener);
        CHECK_MSG(cell(t, 0, 1)->codePoint == 'C', opener);
    }

    Ubiquitty::hardReset(t);
    feed(t, "A\033[1\x18"
            "C");
    CHECK(cell(t, 0, 1)->codePoint == 'C');
}

// At conformance level 1 SUB draws the VT100's error blotch instead, and
// DECSCL back to a later level restores the reversed question mark.
TEST(testSubstituteFollowsTheConformanceLevel)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[61\"p\x1A");
    CHECK(cell(t, 0, 0)->codePoint == Ubiquitty::VT::vt100SubstituteCharacter);
    feed(t, "\033[65;1\"p\033[H\x1A");
    CHECK(cell(t, 0, 0)->codePoint == Ubiquitty::VT::substituteCharacter);
}

TEST(testSOSContainsControlsUntilStringTerminator)
{
    const std::string_view sequence = "A\033Xhi\007\030\032\033[2;3H\033\033Q\033\\B";
    for (std::size_t split = 0; split <= sequence.size(); ++split) {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        feed(t, sequence.substr(0, split));
        feed(t, sequence.substr(split));
        CHECK(!t.bellPending);
        CHECK(cell(t, 0, 0)->codePoint == 'A');
        CHECK(cell(t, 0, 1)->codePoint == 'B');
        CHECK(t.screens.active->cursorX == 2);
        CHECK(t.screens.active->cursorY == 0);
        CHECK(t.parser.parseState == Ubiquitty::VT::Parser::State::Ground);
    }
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "A\033_hi\007B\033\\C");
    CHECK(!t.bellPending);
    CHECK(cell(t, 0, 1)->codePoint == 'C');
    Ubiquitty::hardReset(t);
    Ubiquitty::Unicode::encReset(&t.parser.inputEncoding, Ubiquitty::Unicode::Encoding::Latin1);
    feed(t, "A\230hi\007\030\032\033[2;3H\234B");
    CHECK(!t.bellPending);
    CHECK(cell(t, 0, 1)->codePoint == 'B');
    CHECK(t.parser.parseState == Ubiquitty::VT::Parser::State::Ground);
}

TEST(cancelCharacterRestoresOverwriteInsertWrapAndScroll)
{
    for (const std::string_view setup : {
             "ABCD\r", "ABCD\033[1;2H\033[4h", "ABCDEF", "ABCDEF\r\n123456",
             "\033[?69h\033[2;5s\033[1;2H1234\033[2;2H5678", "\033[?1049hABCDEF\r\n123456",
             "\033[1mABCD\033[0m\r", "\033#6ABC", "\033[?34h\033[1;6HABCDEF" }) {
        for (const std::string_view graphic : { "X", " ", "\xE7\x95\x8C", "\xF0\x9F\x98\x80" }) {
            Ubiquitty::Terminal t;
            Ubiquitty::hardReset(t);
            Ubiquitty::resize(t, 2, 6);
            feed(t, setup);
            std::array<Ubiquitty::Cell, 12> before;
            std::array<Ubiquitty::RowMeta, 2> metadata;
            for (int row = 0; row < 2; ++row) {
                metadata[row] = t.grid->rowMeta(row);
                for (int column = 0; column < 6; ++column)
                    before[row * 6 + column] = *cell(t, row, column);
            }
            const int x = t.screens.active->cursorX;
            const int y = t.screens.active->cursorY;
            const bool pending = t.screens.active->wrapPending;
            const int history = t.scrollbackCount;
            feed(t, graphic);
            feed(t, "\033");
            feed(t, "T");
            CHECK_MSG(t.screens.active->cursorX == x, std::string(setup).c_str());
            CHECK_EQ(t.screens.active->cursorY, y);
            CHECK_EQ(t.screens.active->wrapPending, pending);
            CHECK_EQ(t.scrollbackCount, history);
            for (int row = 0; row < 2; ++row) {
                CHECK_EQ(t.grid->rowMeta(row).lineAttribute, metadata[row].lineAttribute);
                CHECK_EQ(t.grid->rowMeta(row).has(Ubiquitty::RowFlag::Wrapped), metadata[row].has(Ubiquitty::RowFlag::Wrapped));
                for (int column = 0; column < 6; ++column) {
                    const auto& saved = before[row * 6 + column];
                    const auto& restored = *cell(t, row, column);
                    CHECK_EQ(restored.codePoint, saved.codePoint);
                    CHECK(std::memcmp(&restored.pen, &saved.pen, sizeof(saved.pen)) == 0);
                    CHECK_EQ(restored.linkID(), saved.linkID());
                }
            }
        }
    }
}

TEST(cancelCharacterRestoresEvictedHistoryAndLineIdentity)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 2, 6);
    t.scrollbackLimit = 2;
    feed(t, "old1\r\nold2\r\nABCDEF\r\n123456");
    CHECK_EQ(t.scrollbackCount, 2);
    const auto front = t.lineStore->frontLine();
    const auto head = t.lineStore->headLineNumber();
    const auto epoch = t.historyEpoch;
    const auto rowEpoch = t.absoluteRowEpoch;
    feed(t, "X\033T");
    CHECK_EQ(t.scrollbackCount, 2);
    CHECK(t.lineStore->frontLine().number >= head);
    CHECK(t.historyEpoch != epoch);
    CHECK(t.absoluteRowEpoch != rowEpoch);
    CHECK(t.lineStore->frontLine().continuity == front.continuity);
    CHECK_EQ(t.lineStore->headLineNumber(), t.lineStore->frontLine().number + 2);
    CHECK_EQ(t.lineStore->at(0)->cells[3].codePoint, '1');
    CHECK_EQ(t.lineStore->at(1)->cells[3].codePoint, '2');
    CHECK_EQ(cell(t, 0, 0)->codePoint, 'A');
    CHECK_EQ(cell(t, 1, 0)->codePoint, '1');
}

TEST(cancelCharacterIsIndependentOfInputChunkBoundaries)
{
    for (const std::string_view text : { "ABCDEF", "\xE7\x95\x8C\xE7\x95\x8C", "e\xCC\x81"
                                                                               "f\xCC\x81",
             "\xF0\x9F\x98\x80\xF0\x9F\x98\x80" }) {
        const std::string stream = std::string(text) + "\033T";
        Ubiquitty::Terminal whole;
        Ubiquitty::hardReset(whole);
        feed(whole, stream);
        for (std::size_t split = 0; split <= stream.size(); ++split) {
            Ubiquitty::Terminal divided;
            Ubiquitty::hardReset(divided);
            feed(divided, std::string_view(stream).substr(0, split));
            feed(divided, std::string_view(stream).substr(split));
            CHECK_EQ(divided.screens.active->cursorX, whole.screens.active->cursorX);
            for (int column = 0; column < 8; ++column) {
                CHECK_EQ(cell(divided, 0, column)->codePoint, cell(whole, 0, column)->codePoint);
                CHECK_EQ(cell(divided, 0, column)->combiningLength(), cell(whole, 0, column)->combiningLength());
            }
        }
    }
}

TEST(cancelCharacterAfterCCHFreeChunksMatchesBytewiseInput)
{
    for (const std::string_view graphic : { "X", "XY", "\xC3\xA9", "\xE7\x95\x8C", "e\xCC\x81", "e\xCC\x81"
                                                                                                "f\xCC\x81",
             "\xF0\x9F\x98\x80", "\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x92\xBB" }) {
        for (const std::string_view boundary : { "", "\r", "\n", "\033[m", "\033[38;5;123m", "\033[?25l", "\033[1 q" }) {
            for (const std::string& successor : { std::string(), std::string("T"), std::string("%"), std::string("\033"), std::string("\033[31m"), std::string("\033[31"),
                     "\033[" + std::string(64, '0'), "\033[" + std::string(64, '0') + "m",
                     "\033[" + std::string(65, '0'), "\033[" + std::string(65, '0') + "m",
                     "\033[" + std::string(80, '0'), "\033[" + std::string(80, '0') + "m", std::string("\033(B"),
                     std::string("\033%@"), std::string("\033%G"), std::string("\033]title\033[31m"), std::string("\033Pignored\033[31m") }) {
                const auto prefix = "AB" + std::string(boundary) + std::string(graphic) + successor;
                for (const std::string_view cch : { "\033T", "\xC2\x94" }) {
                    Ubiquitty::Terminal bytewise;
                    Ubiquitty::hardReset(bytewise);
                    Ubiquitty::resize(bytewise, 3, 12, Ubiquitty::Modes::ReflowMode::Fixed);
                    const auto stream = prefix + std::string(cch);
                    for (const char& byte : stream)
                        feed(bytewise, { &byte, 1 });
                    for (std::size_t split = prefix.size(); split <= stream.size(); ++split) {
                        Ubiquitty::Terminal divided;
                        Ubiquitty::hardReset(divided);
                        Ubiquitty::resize(divided, 3, 12, Ubiquitty::Modes::ReflowMode::Fixed);
                        feed(divided, std::string_view(stream).substr(0, split));
                        feed(divided, std::string_view(stream).substr(split));
                        CHECK_EQ(divided.screens.active->cursorX, bytewise.screens.active->cursorX);
                        CHECK_EQ(divided.screens.active->cursorY, bytewise.screens.active->cursorY);
                        CHECK_EQ(divided.screens.active->wrapPending, bytewise.screens.active->wrapPending);
                        CHECK_EQ(divided.scrollbackCount, bytewise.scrollbackCount);
                        for (int row = 0; row < 3; ++row) {
                            for (int column = 0; column < 12; ++column) {
                                const auto* actual = cell(divided, row, column);
                                const auto* expected = cell(bytewise, row, column);
                                CHECK_EQ(actual->codePoint, expected->codePoint);
                                CHECK(std::memcmp(&actual->pen, &expected->pen, sizeof(expected->pen)) == 0);
                                std::array<uint32_t, Ubiquitty::ClusterPool::maxCodePoints> actualCluster {};
                                std::array<uint32_t, Ubiquitty::ClusterPool::maxCodePoints> expectedCluster {};
                                CHECK_EQ(Ubiquitty::cellClusterCodePoints(&divided, actual, actualCluster.data(), actualCluster.size()),
                                    Ubiquitty::cellClusterCodePoints(&bytewise, expected, expectedCluster.data(), expectedCluster.size()));
                                CHECK(actualCluster == expectedCluster);
                            }
                        }
                    }
                }
            }
        }
    }
}

TEST(cancelCharacterAfterAnInterruptedSGRRetainsTheGraphic)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "AB\033[mX\033[31");
    feed(t, "\033");
    feed(t, "T");
    CHECK_EQ(t.screens.active->cursorX, 2);
    CHECK_EQ(cell(t, 0, 2)->codePoint, 0u);
    feed(t, "X\033[31m\033T");
    CHECK_EQ(t.screens.active->cursorX, 3);
    CHECK_EQ(cell(t, 0, 2)->codePoint, 'X');
}

TEST(cancelCharacterAfterSplitControlStringsKeepsTheSamePredecessor)
{
    for (const std::string_view introducer : { "\033]0;", "\033P", "\033_", "\033^", "\033X", "\xC2\x9D"
                                                                                              "0;",
             "\xC2\x90" }) {
        const auto prefix = "X" + std::string(introducer) + "ignored\r\n\033[m";
        const auto stream = prefix + "\033\\\033T";
        Ubiquitty::Terminal divided;
        Ubiquitty::Terminal bytewise;
        for (auto* terminal : { &divided, &bytewise }) {
            Ubiquitty::hardReset(*terminal);
            Ubiquitty::resize(*terminal, 3, 16);
        }
        feed(divided, prefix);
        feed(divided, "\033");
        feed(divided, "\\");
        feed(divided, "\033T");
        for (const char& byte : stream)
            feed(bytewise, { &byte, 1 });
        CHECK_EQ(divided.screens.active->cursorX, bytewise.screens.active->cursorX);
        CHECK_EQ(divided.screens.active->cursorY, bytewise.screens.active->cursorY);
        for (int row = 0; row < 3; ++row) {
            for (int column = 0; column < 16; ++column)
                CHECK_EQ(cell(divided, row, column)->codePoint, cell(bytewise, row, column)->codePoint);
        }
    }
}

TEST(cancelCharacterKeepsDecoderAndREPPredecessorsAcrossChunks)
{
    const auto check = [](std::string_view first, std::string_view second, std::string_view cch, Ubiquitty::Unicode::Encoding encoding) {
        Ubiquitty::Terminal divided;
        Ubiquitty::Terminal bytewise;
        for (auto* terminal : { &divided, &bytewise }) {
            Ubiquitty::hardReset(*terminal);
            Ubiquitty::resize(*terminal, 3, 16);
            Ubiquitty::Unicode::encReset(&terminal->parser.inputEncoding, encoding);
        }
        feed(divided, first);
        feed(divided, second);
        feed(divided, cch);
        const auto stream = std::string(first) + std::string(second) + std::string(cch);
        for (const char& byte : stream)
            feed(bytewise, { &byte, 1 });
        CHECK_EQ(divided.screens.active->cursorX, bytewise.screens.active->cursorX);
        CHECK_EQ(divided.screens.active->cursorY, bytewise.screens.active->cursorY);
        CHECK_EQ(divided.scrollbackCount, bytewise.scrollbackCount);
        for (int row = 0; row < 3; ++row) {
            for (int column = 0; column < 16; ++column)
                CHECK_EQ(cell(divided, row, column)->codePoint, cell(bytewise, row, column)->codePoint);
        }
    };
    const auto utf8 = Ubiquitty::Unicode::Encoding::UTF8;
    check("A\xE7\x95", "\x8C"
                       "B\nX",
        "\033T", utf8);
    check("\033%", "GAB\nX", "\033T", utf8);
    check("\033%", "@AB\nX", "\033T", utf8);
    check("X\033[m", "\033[3b", "\033T", utf8);
    check("", "AB\nX", "\224", Ubiquitty::Unicode::Encoding::Latin1);
}

TEST(cancelCharacterPreservesStatusLineCRInsideSequences)
{
    for (const bool isStatusActive : { false, true }) {
        for (const bool isSetupInSameChunk : { false, true }) {
            for (const bool isCSI : { false, true }) {
                Ubiquitty::Terminal t;
                Ubiquitty::hardReset(t);
                Ubiquitty::resize(t, 2, 8);
                const std::string setup = isStatusActive ? "\033[2$~\033[1$}" : "";
                const std::string body = isCSI ? "X\033[1\r" : "X\033\r";
                if (!isSetupInSameChunk)
                    feed(t, setup);
                feed(t, (isSetupInSameChunk ? setup : "") + body);
                feed(t, isCSI ? "\033T" : "T");
                if (isStatusActive) {
                    CHECK_EQ(Ubiquitty::Rendering::statusCellAt(t, 0).codePoint, 0u);
                    CHECK_EQ(t.statusCursorX, 0);
                } else {
                    CHECK_EQ(cell(t, 0, 0)->codePoint, 'X');
                    CHECK_EQ(t.screens.active->cursorX, 0);
                }
            }
        }
    }
}

TEST(cancelCharacterPrefilterKeepsTheSyncClearBoundary)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 2, 8);
    t.isScrollLaneEnabled = true;
    feed(t, "12345678\r\nabcdefgh\r\n\033[?2026h");
    const std::string_view prefix = "x\r\n\033[?2026l";
    const std::string input = std::string(prefix) + "Y";
    bool didStop = false;
    CHECK_EQ(Ubiquitty::processBytesUpToSyncClear(&t, input, &didStop), static_cast<int>(prefix.size()));
    CHECK(didStop);
    CHECK(!t.scrollLaneRun.active);
    CHECK_EQ(t.screens.active->cursorX, 0);
    CHECK_EQ(cell(t, 1, 0)->codePoint, 0u);
    feed(t, "Y");
    feed(t, "\033T");
    CHECK_EQ(t.screens.active->cursorX, 0);
    CHECK_EQ(cell(t, 1, 0)->codePoint, 0u);
}

TEST(cancelCharacterPrefilterKeepsTheReplyPressureBoundary)
{
    for (const int pending : { Ubiquitty::Terminal::outputReplyPressureBytes - 4, Ubiquitty::Terminal::outputReplyPressureBytes }) {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        std::fill_n(t.outputBuffer.begin(), pending, 'x');
        t.outputLength = pending;
        const std::string_view prefix = "A\033[5n";
        const std::string input = std::string(prefix) + "B";
        bool didStop = false;
        CHECK_EQ(Ubiquitty::processBytesUpToSyncClear(&t, input, &didStop), static_cast<int>(prefix.size()));
        CHECK(!didStop);
        CHECK(t.outputLength >= Ubiquitty::Terminal::outputReplyPressureBytes);
        CHECK_EQ(cell(t, 0, 0)->codePoint, 'A');
        CHECK_EQ(cell(t, 0, 1)->codePoint, 0u);
    }
}

TEST(cancelCharacterAfterBatchedTextMatchesBytewiseInput)
{
    for (const std::string_view unit : { "ABCDEF", "\xE7\x95\x8C", "e\xCC\x81\xCC\x88",
             "\xE2\x9D\xA4\xEF\xB8\x8F", "\xF0\x9F\x98\x80", "\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x92\xBB" }) {
        std::string text;
        for (int index = 0; index < 110; ++index)
            text += unit;
        for (const std::string_view separator : { "", "\xC2\x94", "\177", "\033[", "\033[31mX",
                 "\033]8;;https://example.org\a", "\033]8;;https://example.org\033\\", "\033]8;;",
                 "\033P", "\033_", "\033X", "\033^" }) {
            const auto stream = text + std::string(separator) + "\033T";
            Ubiquitty::Terminal batched;
            Ubiquitty::Terminal bytewise;
            Ubiquitty::hardReset(batched);
            Ubiquitty::hardReset(bytewise);
            Ubiquitty::resize(batched, 3, 17, Ubiquitty::Modes::ReflowMode::Fixed);
            Ubiquitty::resize(bytewise, 3, 17, Ubiquitty::Modes::ReflowMode::Fixed);
            feed(batched, stream);
            for (const char& byte : stream)
                feed(bytewise, { &byte, 1 });
            CHECK_EQ(batched.screens.active->cursorX, bytewise.screens.active->cursorX);
            CHECK_EQ(batched.screens.active->cursorY, bytewise.screens.active->cursorY);
            CHECK_EQ(batched.screens.active->wrapPending, bytewise.screens.active->wrapPending);
            for (int row = 0; row < 3; ++row) {
                for (int column = 0; column < 17; ++column) {
                    const auto* actual = cell(batched, row, column);
                    const auto* expected = cell(bytewise, row, column);
                    CHECK_EQ(actual->codePoint, expected->codePoint);
                    CHECK(actual->pen.attributes == expected->pen.attributes);
                    std::array<uint32_t, Ubiquitty::ClusterPool::maxCodePoints> actualCluster {};
                    std::array<uint32_t, Ubiquitty::ClusterPool::maxCodePoints> expectedCluster {};
                    const int actualLength = Ubiquitty::cellClusterCodePoints(&batched, actual, actualCluster.data(), actualCluster.size());
                    const int expectedLength = Ubiquitty::cellClusterCodePoints(&bytewise, expected, expectedCluster.data(), expectedCluster.size());
                    CHECK_EQ(actualLength, expectedLength);
                    CHECK(actualCluster == expectedCluster);
                }
            }
        }
    }
}

TEST(cancelCharacterAfterALoneGraphicMatchesBytewiseInputForEverySuccessor)
{
    // A lone graphic decides whether to journal itself from the bytes after
    // it, which bytewise input never shows. Complete, incomplete and abandoned
    // successors must leave the same screen either way. The tail puts a word
    // of input past each successor, so short control sequences are decided
    // from the word after their introducer as well as at the input's end.
    for (const std::string_view graphic : { "X", "\xC3\xA9", "\xE7\x95\x8C", "\xF0\x9F\x98\x80" }) {
        for (const std::string_view successor : { "", "\r", "\n", "Y", "\033[m", "\033[31m", "\033[1;2;3;4m",
                 "\033[1;2;3;45m", "\033[48;5;123m", "\033[38;2;10;20;30m", "\033[1 q", "\033[?25l", "\033[1;",
                 "\033[48;5;12", "\033[1;2;3;4;5;6;7;8", "\033[1;\033", "\033[1\0302m", "\033[\177m",
                 "\033]8;;\a", "\033P", "\033(B", "\033", "\xC2\x94", "\xC2\xA0", "\xC2", "\xE7\x95",
                 "\xED\xA0\x80", "\xF4\x90\x80\x80", "\xFF" }) {
            for (const std::string_view tail : { "", "0123456789" }) {
                const auto stream = "AB\033[m" + std::string(graphic) + std::string(successor) + "\033T" + std::string(tail);
                Ubiquitty::Terminal whole;
                Ubiquitty::Terminal bytewise;
                for (auto* terminal : { &whole, &bytewise }) {
                    Ubiquitty::hardReset(*terminal);
                    Ubiquitty::resize(*terminal, 2, 24, Ubiquitty::Modes::ReflowMode::Fixed);
                }
                feed(whole, stream);
                for (const char& byte : stream)
                    feed(bytewise, { &byte, 1 });
                CHECK_EQ(whole.screens.active->cursorX, bytewise.screens.active->cursorX);
                CHECK_EQ(whole.screens.active->cursorY, bytewise.screens.active->cursorY);
                CHECK_EQ(whole.screens.active->wrapPending, bytewise.screens.active->wrapPending);
                for (int row = 0; row < 2; ++row) {
                    for (int column = 0; column < 24; ++column) {
                        const auto* actual = cell(whole, row, column);
                        const auto* expected = cell(bytewise, row, column);
                        CHECK_EQ(actual->codePoint, expected->codePoint);
                        CHECK_EQ(actual->combiningLength(), expected->combiningLength());
                        CHECK(std::memcmp(&actual->pen, &expected->pen, sizeof(expected->pen)) == 0);
                    }
                }
            }
        }
    }
}

TEST(cancelCharacterAfterMalformedUTF8MatchesBytewiseInputForEverySuccessor)
{
    // A malformed sequence's substitute is a graphic CCH can cancel, and it
    // decides whether to journal itself from what follows it, as any graphic
    // does. The successor that matters most is C2 94, CCH spelled in UTF-8,
    // which re-feeds through the same recovery as the byte that broke the
    // sequence. Bytewise input, which never sees a successor, is the reference.
    for (const std::string_view malformed : { "\x80", "\xBF", "\xC3", "\xE2\x82", "\xC0\xAF", "\xE0\x80\xAF", "\xED\xA0\x80",
             "\xF4\x90\x80\x80", "\xFE", "\xFF" }) {
        for (const std::string_view successor : { "", "Y", "\r", "\n", "\xC3\xA9", "\xE7\x95\x8C", "\xC2\x94", "\xC2", "\xE7\x95",
                 "\xFF", "\x80", "\033[31m", "\033", "\033P", "\t" }) {
            for (const std::string_view tail : { "", "0123456789" }) {
                const auto stream = "AB\033[m" + std::string(malformed) + std::string(successor) + "\033T" + std::string(tail);
                Ubiquitty::Terminal whole;
                Ubiquitty::Terminal bytewise;
                for (auto* terminal : { &whole, &bytewise }) {
                    Ubiquitty::hardReset(*terminal);
                    Ubiquitty::resize(*terminal, 2, 24, Ubiquitty::Modes::ReflowMode::Fixed);
                }
                feed(whole, stream);
                for (const char& byte : stream)
                    feed(bytewise, { &byte, 1 });
                CHECK_EQ(whole.screens.active->cursorX, bytewise.screens.active->cursorX);
                CHECK_EQ(whole.screens.active->cursorY, bytewise.screens.active->cursorY);
                for (int row = 0; row < 2; ++row) {
                    for (int column = 0; column < 24; ++column) {
                        const auto* actual = cell(whole, row, column);
                        const auto* expected = cell(bytewise, row, column);
                        CHECK_EQ(actual->codePoint, expected->codePoint);
                    }
                }
            }
        }
    }
}

TEST(cancelCharacterAcrossEverySplitOfTextCarryingPrefilterBytesMatchesBytewiseInput)
{
    // A plain T or %, and a 0x94 that continues a UTF-8 sequence, are text:
    // a chunk holding them still takes the batched path, and every split of
    // the stream around them cancels the same graphic as bytewise input.
    // U+4E14 ends in 0x94.
    const std::string stream = "T%\xE4\xB8\x94X\033T";
    Ubiquitty::Terminal bytewise;
    Ubiquitty::hardReset(bytewise);
    Ubiquitty::resize(bytewise, 2, 24, Ubiquitty::Modes::ReflowMode::Fixed);
    for (const char& byte : stream)
        feed(bytewise, { &byte, 1 });
    CHECK_EQ(cell(bytewise, 0, 0)->codePoint, 'T');
    CHECK_EQ(cell(bytewise, 0, 1)->codePoint, '%');
    CHECK_EQ(cell(bytewise, 0, 2)->codePoint, 0x4E14u);
    CHECK_EQ(cell(bytewise, 0, 4)->codePoint, 0u); // X was cancelled
    for (std::size_t split = 0; split <= stream.size(); ++split) {
        Ubiquitty::Terminal divided;
        Ubiquitty::hardReset(divided);
        Ubiquitty::resize(divided, 2, 24, Ubiquitty::Modes::ReflowMode::Fixed);
        feed(divided, std::string_view(stream).substr(0, split));
        feed(divided, std::string_view(stream).substr(split));
        CHECK_EQ(divided.screens.active->cursorX, bytewise.screens.active->cursorX);
        for (int column = 0; column < 24; ++column)
            CHECK_EQ(cell(divided, 0, column)->codePoint, cell(bytewise, 0, column)->codePoint);
    }
}

TEST(cancelUnicodeWrapPreservesHistoryAcrossInputBoundaries)
{
    const auto prepare = [](Ubiquitty::Terminal& terminal, Ubiquitty::ScreenMode screen) {
        Ubiquitty::hardReset(terminal);
        Ubiquitty::resize(terminal, 2, 4, Ubiquitty::Modes::ReflowMode::Fixed);
        if (screen == Ubiquitty::ScreenMode::Alternate)
            feed(terminal, "\033[?1049h");
        feed(terminal, "1111\r\n2222");
    };
    for (const auto screen : { Ubiquitty::ScreenMode::Primary, Ubiquitty::ScreenMode::Alternate }) {
        for (const std::string_view graphic : { "\xC3\xA9", "\xE7\x95\x8C", "\xF0\x9F\x98\x80", "e\xCC\x81" }) {
            for (const std::string_view successor : { "", "\xE7\x95\x8C", "\xE7", "\xE7\x95", "\xC2\x94", "\xFF", "\033[31m\xE7\x95\x8C" }) {
                const auto stream = std::string(graphic) + std::string(successor) + "\033T";
                Ubiquitty::Terminal bytewise;
                prepare(bytewise, screen);
                for (const char& byte : stream)
                    feed(bytewise, { &byte, 1 });

                // Every graphic begins at a pending bottom-row wrap. Splits
                // inside its successor must retain enough state for CCH to
                // undo the last graphic, including a scroll into history.
                for (std::size_t split = 0; split <= stream.size(); ++split) {
                    Ubiquitty::Terminal divided;
                    prepare(divided, screen);
                    feed(divided, std::string_view(stream).substr(0, split));
                    feed(divided, std::string_view(stream).substr(split));
                    CHECK_EQ(divided.screens.active->cursorX, bytewise.screens.active->cursorX);
                    CHECK_EQ(divided.screens.active->cursorY, bytewise.screens.active->cursorY);
                    CHECK_EQ(divided.screens.active->wrapPending, bytewise.screens.active->wrapPending);
                    CHECK_EQ(divided.scrollbackCount, bytewise.scrollbackCount);
                    CHECK(static_cast<bool>(divided.lineStore) == static_cast<bool>(bytewise.lineStore));
                    if (divided.lineStore && bytewise.lineStore) {
                        CHECK_EQ(divided.lineStore->headLineNumber(), bytewise.lineStore->headLineNumber());
                        CHECK(divided.lineStore->frontLine().continuity == bytewise.lineStore->frontLine().continuity);
                        for (int row = 0; row < divided.lineStore->count(); ++row) {
                            for (int column = 0; column < 4; ++column)
                                CHECK_EQ(divided.lineStore->at(row)->cells[column].codePoint, bytewise.lineStore->at(row)->cells[column].codePoint);
                        }
                    }
                    for (int row = 0; row < 2; ++row) {
                        for (int column = 0; column < 4; ++column) {
                            const auto* actual = cell(divided, row, column);
                            const auto* expected = cell(bytewise, row, column);
                            CHECK_EQ(actual->codePoint, expected->codePoint);
                            CHECK(actual->pen.attributes == expected->pen.attributes);
                            std::array<uint32_t, Ubiquitty::ClusterPool::maxCodePoints> actualCluster {};
                            std::array<uint32_t, Ubiquitty::ClusterPool::maxCodePoints> expectedCluster {};
                            CHECK_EQ(Ubiquitty::cellClusterCodePoints(&divided, actual, actualCluster.data(), actualCluster.size()),
                                Ubiquitty::cellClusterCodePoints(&bytewise, expected, expectedCluster.data(), expectedCluster.size()));
                            CHECK(actualCluster == expectedCluster);
                        }
                    }
                }
            }
        }
    }
}

TEST(cancelCharacterIgnoresAControlPredecessorAndAcceptsC1)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "AB\033[1;1H\033T");
    CHECK_EQ(cell(t, 0, 0)->codePoint, 'A');
    CHECK_EQ(cell(t, 0, 1)->codePoint, 'B');
    feed(t, "\033%@\033[1;3HC\224");
    CHECK_EQ(t.screens.active->cursorX, 2);
    CHECK_EQ(cell(t, 0, 2)->codePoint, 0u);
}

TEST(cancelCharacterAfterStatusCarriageReturnLeavesTheGraphic)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[2$~\033[1$}AB\r\033T");
    CHECK_EQ(t.statusCursorX, 0);
    CHECK_EQ(Ubiquitty::Rendering::statusCellAt(t, 1).codePoint, 'B');
}

TEST(cancelCombiningCharacterRetainsItsBaseAndAcceptsAnotherMark)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "e\xCC\x81\033T");
    CHECK_EQ(cell(t, 0, 0)->codePoint, 'e');
    CHECK_EQ(cell(t, 0, 0)->combiningLength(), 0);
    CHECK_EQ(t.screens.active->cursorX, 1);
    feed(t, "\xCC\x88");
    CHECK_EQ(cell(t, 0, 0)->combiningLength(), 1);
    CHECK_EQ(cell(t, 0, 0)->combining[0], 0x308u);
}

TEST(cancelVariationSelectorRestoresTheNarrowBaseAndOverwrittenNeighbour)
{
    for (const bool shouldSplitSelector : { false, true }) {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        feed(t, "AB\r");
        if (shouldSplitSelector) {
            feed(t, "\xE2\x9D\xA4");
            feed(t, "\xEF\xB8\x8F");
        } else
            feed(t, "\xE2\x9D\xA4\xEF\xB8\x8F");
        feed(t, "\033T");
        CHECK_EQ(cell(t, 0, 0)->codePoint, 0x2764u);
        CHECK_EQ(cell(t, 0, 0)->combiningLength(), 0);
        CHECK_EQ(cell(t, 0, 1)->codePoint, 'B');
        CHECK_EQ(t.screens.active->cursorX, 1);
    }
}

TEST(cancelCharacterRestoresTheSingleShiftForTheNextGraphic)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033*0\033Nq\033Tq");
    CHECK_EQ(cell(t, 0, 0)->codePoint, 0x2500u);
    CHECK_EQ(t.screens.active->cursorX, 1);
}

namespace {

enum class ScrollCSIShape {
    Primary,
    Band
};
enum class ScrollCSIExecution {
    Deferred,
    Classic
};

void prepareScrollCSI(Ubiquitty::Terminal& terminal, ScrollCSIShape shape, ScrollCSIExecution execution)
{
    const bool isBand = shape == ScrollCSIShape::Band;
    const bool isEnabled = execution == ScrollCSIExecution::Deferred;
    Ubiquitty::hardReset(terminal);
    Ubiquitty::resize(terminal, 4, 12);
    terminal.isScrollLaneEnabled = isEnabled && !isBand;
    terminal.isBandScrollLaneEnabled = isEnabled && isBand;
    if (isBand)
        feed(terminal, "\033[?1049h\033[?69h\033[3;10s\033[4;3H");
    else
        feed(terminal, "\033[4;1H");
    const std::string_view line = "old\r\n";
    CHECK_EQ(Ubiquitty::processBytesUpToSyncClear(&terminal, line), static_cast<int>(line.size()));
    CHECK_EQ(terminal.scrollLaneRun.active, isEnabled && !isBand);
    CHECK_EQ(terminal.bandScrollRun.active, isEnabled && isBand);
    CHECK_EQ(terminal.scrollbackCount, isBand ? 0 : 1);
}

void checkScrollCSIState(Ubiquitty::Terminal& actual, Ubiquitty::Terminal& expected)
{
    Ubiquitty::termFlushScrollLane(actual);
    Ubiquitty::termFlushScrollLane(expected);
    CHECK_EQ(actual.screens.active->cursorX, expected.screens.active->cursorX);
    CHECK_EQ(actual.screens.active->cursorY, expected.screens.active->cursorY);
    CHECK_EQ(actual.screens.active->wrapPending, expected.screens.active->wrapPending);
    CHECK_EQ(actual.scrollbackCount, expected.scrollbackCount);
    CHECK_EQ(actual.printMode, expected.printMode);
    CHECK(actual.parser.parseState == expected.parser.parseState);
    CHECK_EQ(actual.parser.parameterCount, expected.parser.parameterCount);
    CHECK(actual.parser.parameters == expected.parser.parameters);
    CHECK(actual.parser.parameterSeparatorWasColon == expected.parser.parameterSeparatorWasColon);
    CHECK_EQ(actual.parser.parameterWasEmpty, expected.parser.parameterWasEmpty);
    CHECK_EQ(actual.parser.didParametersOverflow, expected.parser.didParametersOverflow);
    CHECK_EQ(actual.parser.privatePrefix, expected.parser.privatePrefix);
    CHECK_EQ(actual.parser.intermediate, expected.parser.intermediate);
    CHECK(std::memcmp(&actual.screens.active->pen, &expected.screens.active->pen, sizeof(actual.screens.active->pen)) == 0);
    CHECK_EQ(actual.outputLength, expected.outputLength);
    if (actual.outputLength == expected.outputLength)
        CHECK(std::memcmp(actual.outputBuffer.data(), expected.outputBuffer.data(), static_cast<std::size_t>(actual.outputLength)) == 0);
    for (int row = 0; row < actual.rows; ++row)
        CHECK(actual.screens.active->lineWrapped[row] == expected.screens.active->lineWrapped[row]);
    if (actual.scrollbackCount != expected.scrollbackCount)
        return;
    for (int row = 0; row < actual.scrollbackCount + actual.rows; ++row) {
        for (int column = 0; column < actual.columns; ++column) {
            const auto* actualCell = Ubiquitty::Screen::absoluteCell(&actual, row, column);
            const auto* expectedCell = Ubiquitty::Screen::absoluteCell(&expected, row, column);
            CHECK_EQ(actualCell == nullptr, expectedCell == nullptr);
            if (actualCell && expectedCell)
                CHECK(std::memcmp(actualCell, expectedCell, sizeof(Ubiquitty::Cell)) == 0);
        }
    }
    for (int row = 0; row < actual.scrollbackCount; ++row) {
        const auto* actualLine = Ubiquitty::Screen::scrollbackLine(&actual, row);
        const auto* expectedLine = Ubiquitty::Screen::scrollbackLine(&expected, row);
        REQUIRE(actualLine && expectedLine);
        CHECK_EQ(actualLine->wrapped, expectedLine->wrapped);
        CHECK_EQ(actualLine->lineAttribute, expectedLine->lineAttribute);
        CHECK_EQ(actualLine->mark, expectedLine->mark);
    }
    TEST_ASSERT_DISPLAY_INVARIANTS(actual.grid.get());
}

} // namespace

TEST(activeScrollCSIOnlyKeepsCompleteBlankInvariantSGR)
{
    struct Sequence {
        std::string_view bytes;
        bool keepsLane;
    };
    const std::array sequences {
        Sequence { "\033[m", true },
        Sequence { "\033[1;3m", true },
        Sequence { "\033[39m", true },
        Sequence { "\033[31m", false },
        Sequence { "\033[38;2;17;83;129m", false },
        Sequence { "\033[38:2::17:83:129m", false },
        Sequence { "\033[41m", false },
        Sequence { "\033[3;4H", false },
        Sequence { "\033[31", false },
        Sequence { "\033[", false },
    };
    for (const auto shape : { ScrollCSIShape::Primary, ScrollCSIShape::Band }) {
        const bool isBand = shape == ScrollCSIShape::Band;
        for (const auto& sequence : sequences) {
            Ubiquitty::Terminal actual;
            Ubiquitty::Terminal expected;
            prepareScrollCSI(actual, shape, ScrollCSIExecution::Deferred);
            prepareScrollCSI(expected, shape, ScrollCSIExecution::Classic);
            CHECK_EQ(Ubiquitty::processBytesUpToSyncClear(&actual, sequence.bytes), static_cast<int>(sequence.bytes.size()));
            CHECK_EQ(Ubiquitty::processBytesUpToSyncClear(&expected, sequence.bytes), static_cast<int>(sequence.bytes.size()));
            CHECK_EQ(actual.scrollLaneRun.active, sequence.keepsLane && !isBand);
            CHECK_EQ(actual.bandScrollRun.active, sequence.keepsLane && isBand);
            checkScrollCSIState(actual, expected);
        }
    }
}

TEST(activeScrollCSIParametersMatchEveryInputSplit)
{
    std::string overflow = "\033[";
    for (int parameter = 0; parameter < Ubiquitty::VT::maxParameters + 4; ++parameter)
        overflow += "1;";
    overflow += "31mX";
    const std::array<std::string, 22> sequences {
        "\033[31mX",
        "\033[1;38:2::17:83:129mX",
        "\033[3;4HX",
        "\033[6n",
        "\033[31?mX",
        "\033[?25lX",
        "\033[1 mX",
        "\033[1 2mX",
        "\033[3\0071mX",
        "\033[3\x7f"
        "1mX",
        "\033[3\0301mX",
        "\033[3\0321mX",
        "\033[3\033[32mX",
        "\033[2:3HX",
        "\033[31mQ\033[2b\033T",
        "\033[3\xC2\x9B;4HX",
        "\033[999999999999mX",
        "\033[5iabc\033[4iX",
        "\033[31",
        "\033[;;mX",
        overflow,
        overflow.substr(0, overflow.size() - 2) + "HX",
    };
    for (const auto shape : { ScrollCSIShape::Primary, ScrollCSIShape::Band }) {
        for (const auto& sequence : sequences) {
            for (std::size_t split = 0; split <= sequence.size(); ++split) {
                Ubiquitty::Terminal actual;
                Ubiquitty::Terminal expected;
                prepareScrollCSI(actual, shape, ScrollCSIExecution::Deferred);
                prepareScrollCSI(expected, shape, ScrollCSIExecution::Classic);
                const std::string_view bytes = sequence;
                CHECK_EQ(Ubiquitty::processBytesUpToSyncClear(&actual, bytes.substr(0, split)), static_cast<int>(split));
                CHECK_EQ(Ubiquitty::processBytesUpToSyncClear(&actual, bytes.substr(split)), static_cast<int>(bytes.size() - split));
                for (const char& byte : sequence)
                    feed(expected, { &byte, 1 });
                checkScrollCSIState(actual, expected);
                // A split/incomplete sequence must leave the same accumulator
                // for the following read, not merely the same current grid.
                feed(actual, "mZ\033[6n");
                feed(expected, "mZ\033[6n");
                checkScrollCSIState(actual, expected);
            }
        }
    }
}

TEST(activeScrollCSIPreservesTraceConsumption)
{
    const std::array<std::string_view, 6> sequences {
        "\033[31mX",
        "\033[38:2::17:83:129mX",
        "\033[3;4HX",
        "\033[31?mX",
        "\033[1 mX",
        "\033[31",
    };
    for (const auto shape : { ScrollCSIShape::Primary, ScrollCSIShape::Band }) {
        for (const auto sequence : sequences) {
            Ubiquitty::Terminal actual;
            Ubiquitty::Terminal expected;
            prepareScrollCSI(actual, shape, ScrollCSIExecution::Deferred);
            prepareScrollCSI(expected, shape, ScrollCSIExecution::Classic);
            actual.integration.traceLength = 0;
            expected.integration.traceLength = 0;
            CHECK_EQ(Ubiquitty::processBytesUpToSyncClear(&actual, sequence), static_cast<int>(sequence.size()));
            CHECK_EQ(Ubiquitty::processBytesUpToSyncClear(&expected, sequence), static_cast<int>(sequence.size()));
            CHECK_EQ(actual.integration.traceLength, expected.integration.traceLength);
            CHECK(std::equal(actual.integration.traceBytes.begin(), actual.integration.traceBytes.begin() + actual.integration.traceLength, expected.integration.traceBytes.begin()));
        }
    }
}

TEST(activeScrollCSIPreservesSyncAndReplyPressureStops)
{
    for (const auto shape : { ScrollCSIShape::Primary, ScrollCSIShape::Band }) {
        Ubiquitty::Terminal actual;
        Ubiquitty::Terminal expected;
        prepareScrollCSI(actual, shape, ScrollCSIExecution::Deferred);
        prepareScrollCSI(expected, shape, ScrollCSIExecution::Classic);
        feed(actual, "\033[?2026h");
        feed(expected, "\033[?2026h");
        CHECK_EQ(Ubiquitty::processBytesUpToSyncClear(&actual, "line\r\n"), 6);
        CHECK_EQ(Ubiquitty::processBytesUpToSyncClear(&expected, "line\r\n"), 6);
        CHECK(actual.scrollLaneRun.active || actual.bandScrollRun.active);
        const std::string_view frame = "\033[31mR\033[?2026lTAIL";
        bool didActualStop = false;
        bool didExpectedStop = false;
        const int actualConsumed = Ubiquitty::processBytesUpToSyncClear(&actual, frame, &didActualStop);
        const int expectedConsumed = Ubiquitty::processBytesUpToSyncClear(&expected, frame, &didExpectedStop);
        CHECK_EQ(actualConsumed, static_cast<int>(frame.find("TAIL")));
        CHECK_EQ(actualConsumed, expectedConsumed);
        CHECK(didActualStop && didExpectedStop);
        checkScrollCSIState(actual, expected);

        prepareScrollCSI(actual, shape, ScrollCSIExecution::Deferred);
        prepareScrollCSI(expected, shape, ScrollCSIExecution::Classic);
        actual.outputLength = Ubiquitty::Terminal::outputReplyPressureBytes;
        expected.outputLength = Ubiquitty::Terminal::outputReplyPressureBytes;
        std::fill_n(actual.outputBuffer.data(), actual.outputLength, '?');
        std::fill_n(expected.outputBuffer.data(), expected.outputLength, '?');
        didActualStop = true;
        didExpectedStop = true;
        const std::string_view styledTail = "\033[31mTAIL";
        CHECK_EQ(Ubiquitty::processBytesUpToSyncClear(&actual, styledTail, &didActualStop), 5);
        CHECK_EQ(Ubiquitty::processBytesUpToSyncClear(&expected, styledTail, &didExpectedStop), 5);
        CHECK(!didActualStop && !didExpectedStop);
        checkScrollCSIState(actual, expected);
    }
}

UBIQUITTY_TEST_MAIN()
