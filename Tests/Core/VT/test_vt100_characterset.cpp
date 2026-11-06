// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Character-set designation and the encodings underneath it.
//
// SCS into G0-G3, the locking and single shifts that select between them, the
// national replacement sets, and the soft fonts DECDLD downloads. Alongside
// them the byte-level encodings the parser decodes before any of that applies,
// and the parity sweep that holds the legacy decoders to the UTF-8 one.
//
// Designation and encoding sit together because a byte's meaning is decided by
// both: the same 0xE0 is a Greek letter under one designation and the lead of a
// three-byte sequence under another, and a test that fixes one without the
// other pins half a rule.

#include "Core/Rendering/StatusRow.h"
#include "Core/VT/Internal.h"
#include "Harness.h"

#include "VT100Fixture.h"
#include <cstring>
#include <string>

// Arabic display-time shaping + RTL ordering through getDisplayRow.
// Asserts the shaped presentation forms land in visual (right-to-left)
// order while the LOGICAL store keeps the original codePoints (so
// copy/paste/selection/search are unaffected).
// An Arabic row still shapes after it ages into scrollback. Guards the
// render gate's O(1) needs-shaping flag: the bit must survive capture
// (scrollbackAppend) so getDisplayRow shapes a scrolled-back row exactly
// like the live one. A dropped flag would silently return logical order.
/// CUU/CUD respect the DECSTBM margins independent of origin mode (G1):
/// starting inside the region, a large cursor-up/down stops at the top/bottom
/// margin, not the screen edge; started outside the region it reaches the edge.
/// DEC Technical Character Set — ESC ( > designates the 94-set into G0.
/// One probe per structural group of the table (radical/bracket pieces,
/// Sigma builders, relations, Greek, arrows) plus the two kinds of
/// unassigned position, which a hard set shows as a blank rather than as
/// the ASCII byte that occupies the slot.
/// National Replacement Character Sets (NRCS) — ESC ( <X> swaps in a
/// 96-glyph variant of ASCII where ~10 byte positions render as
/// national characters. We touch one byte per characterSet so every arm
/// of nrcsTranslate's switch sees at least one hit. Each characterSet has
/// MacRoman fixtures spelled out so a regression in the table lights
/// up as a single test failure.
/// SCS designation into G1 / G2 / G3 slots. ESC ) <X> targets G1,
/// ESC * <X> targets G2, ESC + <X> targets G3. parseCharacterSet clamps
/// the slot index defensively.
/// 8-bit single shifts (G3): 0x8E (SS2) / 0x8F (SS3) are the C1 forms of
/// ESC N / ESC O — the next GL byte resolves through G2 / G3 for one character.
/// They fire only with no UTF-8 sequence in flight, so a 0x8E arriving as a
/// UTF-8 continuation byte is decoded, never treated as a single shift.
/// In UTF-8 mode a raw 8-bit C1 byte (0x80-0x9F) is not performed as CSI / OSC /
/// SS2 / DECID / SOS. Followed by text it is ignored, leaving no U+FFFD, so the
/// bytes that would have been its "payload" print as text; at the end of a
/// write it is a stray byte, U+FFFD.
/// An undefined C1 byte (no assigned function) is ignored on the single-byte
/// legacy input path too — ECMA-48 says undefined C1 controls are ignored in an
/// 8-bit environment, so no stray U+FFFD glyph on a Latin-1 stream.
/// Can (0x18) / SUB (0x1A) abort an in-progress OSC/DCS/APC string (G8),
/// matching the Escape state: the partial string is discarded and the parser
/// returns to Ground, so a following byte prints normally instead of being
/// swallowed as string payload.
/// SOS (ESC X) and PM (ESC ^) strings must not end on BEL: BEL-as-ST is an
/// xterm extension scoped to OSC. BEL inside an SOS or PM rings and the string
/// goes on, so a following byte
/// stays swallowed until a real ST (ESC \) / CAN / SUB. Ending on BEL would return to Ground
/// early and print the post-BEL bytes.
/// Legacy-encoding transcode parity: feeding the same non-UTF-8 stream whole,
/// in odd-sized chunks, and one byte at a time must land the identical Terminal
/// state — the batched run decode inside processBytes is an internal
/// optimization only. The stream mixes ASCII runs, high-byte runs, SGR,
/// newlines, strings terminated by raw 0x9C (C1 ST), and stray ground-state
/// 0x9C, over 10 KB+ so the grid scrolls and run boundaries land everywhere.
struct LegacyParitySnapshot {
    std::vector<Ubiquitty::Cell> cells;
    int cursorX;
    int cursorY;
    Ubiquitty::Pen pen;
    std::string title;
};

static LegacyParitySnapshot legacyParitySnapshot(Ubiquitty::Terminal& t)
{
    LegacyParitySnapshot s;
    s.cells.reserve(static_cast<size_t>(t.rows) * static_cast<size_t>(t.columns));
    for (int r = 0; r < t.rows; r++)
        for (int c = 0; c < t.columns; c++)
            s.cells.push_back(*cell(t, r, c));
    s.cursorX = t.screens.active->cursorX;
    s.cursorY = t.screens.active->cursorY;
    s.pen = t.screens.active->pen;
    s.title = t.titleUTF8;
    return s;
}

static bool legacyParityMatches(const LegacyParitySnapshot& a, const LegacyParitySnapshot& b)
{
    if (a.cursorX != b.cursorX || a.cursorY != b.cursorY)
        return false;
    if (memcmp(&a.pen, &b.pen, sizeof(Ubiquitty::Pen)) != 0)
        return false;
    if (a.title != b.title)
        return false;
    if (a.cells.size() != b.cells.size())
        return false;
    return memcmp(a.cells.data(), b.cells.data(), a.cells.size() * sizeof(Ubiquitty::Cell)) == 0;
}

static std::string buildLegacyParityStream()
{
    std::string s;
    unsigned seed = 0x12345u;
    auto next = [&seed] {
        seed = seed * 1664525u + 1013904223u;
        return seed >> 16;
    };
    while (s.size() < 12000) {
        switch (next() % 10) {
        case 0:
        case 1:
        case 2: { // ASCII word run
            int len = 3 + (int)(next() % 40);
            for (int k = 0; k < len; k++)
                s += (char)('a' + next() % 26);
            s += ' ';
            break;
        }
        case 3:
        case 4:
        case 5: { // high-byte run through the legacy right half
            int len = 1 + (int)(next() % 24);
            for (int k = 0; k < len; k++)
                s += (char)(0xA0 + next() % 0x60);
            break;
        }
        case 6: // SGR color
            s += "\033[3";
            s += (char)('0' + next() % 8);
            s += 'm';
            break;
        case 7:
            s += "\r\n";
            break;
        case 8: // OSC title with a high payload byte, ended by raw 0x9C ST
            s += "\033]0;t";
            s += (char)(0xC0 + next() % 0x20);
            s += (char)0x9C;
            break;
        case 9: // stray ground-state 0x9C: C1 ST with no open string — routed to the parser and swallowed under a single-byte encoding
            s += (char)0x9C;
            break;
        }
    }
    return s;
}

static LegacyParitySnapshot legacyParityFeed(Ubiquitty::Unicode::Encoding encoding, const std::string& stream, int chunk)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::Modes::setInputEncoding(&t, encoding);
    int n = (int)stream.size();
    for (int i = 0; i < n; i += chunk)
        Ubiquitty::processBytes(&t, std::string_view(stream.data() + i, static_cast<std::size_t>((n - i < chunk) ? (n - i) : chunk)));
    return legacyParitySnapshot(t);
}

// A designated set reaches the same glyph whether a byte arrives alone or
// inside a run. The batched writer carries the set's translation rather than
// giving up the batch, so the two routes have to agree cell for cell, and the
// run length is what picks between them: a lone byte goes per-character, two
// or more go through the batch. Equality alone would hold vacuously if neither
// route translated, so each set also pins one glyph outright.
//
// Whether the batch is TAKEN is the benchmark's question, not this one; what
// stands here is that taking it changes no cell.
namespace {

void checkRunMatchesPerCharacter(const char* designator, const char* text)
{
    const int count = static_cast<int>(std::strlen(text));
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, designator);

    // Row 0 per-character: a one-byte feed cannot form a run.
    for (int index = 0; index < count; ++index) {
        const char single[2] = { text[index], '\0' };
        feed(t, single);
    }
    feed(t, "\r\n");
    // Row 1 in one feed: the batch.
    feed(t, text);

    for (int index = 0; index < count; ++index)
        CHECK(cell(t, 0, index)->codePoint == cell(t, 1, index)->codePoint);
}

} // namespace

// DECNRCM set. A national set designates with it set or reset.
static constexpr const char* nationalReplacementMode = "\033[?42h";

TEST(testDesignatedSetRunsMatchPerCharacter)
{
    checkRunMatchesPerCharacter("\033(0", "lqwqkxmnjtuv`afg~_");
    checkRunMatchesPerCharacter("\033(A", "#@[\\]{|}~ABC");
    checkRunMatchesPerCharacter("\033(R", "#@[\\]{|}~abc");
    checkRunMatchesPerCharacter("\033(K", "@[\\]{|}~xyz");
    checkRunMatchesPerCharacter("\033(>", "abcdefghijklmnop");
    checkRunMatchesPerCharacter("\033(B", "plain ASCII stays itself");

    // The glyphs themselves, so the equality above cannot pass by translating
    // on neither route.
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033(0");
    feed(t, "lqk");
    CHECK(cell(t, 0, 0)->codePoint == 0x250C); // l
    CHECK(cell(t, 0, 1)->codePoint == 0x2500); // q
    CHECK(cell(t, 0, 2)->codePoint == 0x2510); // k
    feed(t, "\033(B");
    feed(t, "\r\nlqk");
    CHECK(cell(t, 1, 0)->codePoint == 'l');
}

TEST(testDesignatedSetRunsAcrossRowEdges)
{
    // The batched writer has three destination lanes — a fresh row, a repaint
    // of identical content, and a row still holding a deferred scroll blank.
    // A designated set has to land the same glyphs down all three.
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033(0");

    std::string full;
    for (int column = 0; column < t.columns; ++column)
        full.push_back('q');

    feed(t, full.c_str()); // fills the row exactly to the right margin
    CHECK(cell(t, 0, 0)->codePoint == 0x2500);
    CHECK(cell(t, 0, t.columns - 1)->codePoint == 0x2500);

    feed(t, "\033[H");
    feed(t, full.c_str()); // the identical-repaint lane
    CHECK(cell(t, 0, t.columns - 1)->codePoint == 0x2500);

    // Scroll so the destination row carries a deferred blank, then write it.
    feed(t, "\033[2J\033[H");
    for (int line = 0; line < t.rows + 2; ++line)
        feed(t, "\r\n");
    feed(t, full.c_str());
    CHECK(cell(t, t.rows - 1, 0)->codePoint == 0x2500);
    CHECK(cell(t, t.rows - 1, t.columns - 1)->codePoint == 0x2500);
}

TEST(testInvokedNinetySixSetRunsMatchPerCharacter)
{
    // A 96-set invoked into GL is the case the batch has to decline: ISO 8859-1
    // puts SOFT HYPHEN at 0x2D, and a zero-width code point breaks the
    // one-byte-one-cell rule the run writer advances on exactly as a wide one
    // would. Declining is invisible from here — the cells still have to match
    // the per-character route, which is what makes the refusal safe rather than
    // merely cautious.
    checkRunMatchesPerCharacter("\033-A\016", "abc-def-ghi");

    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033-A\016"); // designate ISO 8859-1 into G1, invoke it into GL
    feed(t, "a-b");
    CHECK(cell(t, 0, 1)->codePoint == 0x00AD); // the position that forces the refusal
}

TEST(testVT52ModeEnterExit)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    CHECK(t.modes.vt52Mode == false);
    feed(t, "\033[?2l");
    CHECK(t.modes.vt52Mode == true);
    // ESC < inside VT52 returns to ANSI.
    feed(t, "\033<");
    CHECK(t.modes.vt52Mode == false);
}

TEST(testVT52CursorMotion)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[?2l"); // enter VT52
    feed(t, "\033H"); // home
    CHECK(t.screens.active->cursorX == 0);
    CHECK(t.screens.active->cursorY == 0);
    feed(t, "\033C"); // right
    CHECK(t.screens.active->cursorX == 1);
    feed(t, "\033B"); // down
    CHECK(t.screens.active->cursorY == 1);
    feed(t, "\033D"); // left
    CHECK(t.screens.active->cursorX == 0);
    feed(t, "\033A"); // up
    CHECK(t.screens.active->cursorY == 0);
}

TEST(testVT52DirectCursorAddress)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[?2l"); // enter VT52
    // ESC Y <row + 0x20> <column + 0x20> — row 5 column 10 = 0x25 0x2A.
    unsigned char addr[] = { 0x1B, 'Y', 0x20 + 5, 0x20 + 10 };
    feed(t, addr);
    CHECK(t.screens.active->cursorY == 5);
    CHECK(t.screens.active->cursorX == 10);
}

TEST(testVT52ConsecutiveCursorAddress)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[?2l"); // enter VT52
    unsigned char seq[] = {
        0x1B,
        'Y',
        0x20 + 2,
        0x20 + 3, // row 2 col 3
        0x1B,
        'Y',
        0x20 + 4,
        0x20 + 6, // row 4 col 6
    };
    feed(t, seq);
    CHECK(t.screens.active->cursorY == 4);
    CHECK(t.screens.active->cursorX == 6);
    // A print after the second address lands at (4,6); no coordinate
    // byte leaked as text onto row 0.
    feed(t, "X");
    CHECK(cell(t, 4, 6)->codePoint == 'X');
    CHECK(cell(t, 0, 0)->codePoint == 0);
}

TEST(testVT52EraseAfterCursorAddress)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[?2l"); // enter VT52
    feed(t, "ABC");
    CHECK(cell(t, 0, 0)->codePoint == 'A');
    unsigned char home[] = { 0x1B, 'Y', 0x20, 0x20 }; // ESC Y 0,0
    feed(t, home);
    feed(t, "\033J"); // erase to end of screen
    CHECK(cell(t, 0, 0)->codePoint == 0);
}

TEST(testVT52GraphicsMode)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[?2l"); // enter VT52
    feed(t, "\033Fq"); // graphics on, 'q' -> ─
    CHECK(cell(t, 0, 0)->codePoint == 0x2500);
    feed(t, "\033Gq"); // graphics off, plain 'q'
    CHECK(cell(t, 0, 1)->codePoint == 'q');
}

TEST(testLegacyEmulationFromName)
{
    CHECK(Ubiquitty::Modes::emulationFromName("linux") == Ubiquitty::VT::Emulation::Linux);
    CHECK(Ubiquitty::Modes::emulationFromName("wyse") == Ubiquitty::VT::Emulation::Wyse);
    CHECK(Ubiquitty::Modes::emulationFromName("televideo") == Ubiquitty::VT::Emulation::Televideo);
    CHECK(Ubiquitty::Modes::emulationFromName("adm") == Ubiquitty::VT::Emulation::Adm);
    CHECK(Ubiquitty::Modes::emulationFromName("sco") == Ubiquitty::VT::Emulation::Sco);
    /// Unknown token returns std::nullopt.
    CHECK(!Ubiquitty::Modes::emulationFromName("bogus").has_value());
    /// Default (no setEmulation) is VT.
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    CHECK(Ubiquitty::Modes::emulation(&t) == Ubiquitty::VT::Emulation::VT);
}

TEST(testLegacyWyseCursorAddress)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::Modes::setEmulation(&t, Ubiquitty::VT::Emulation::Wyse);
    // ESC = <row + 0x20> <col + 0x20> — row 5 col 10.
    unsigned char addr[] = { 0x1B, '=', 0x20 + 5, 0x20 + 10 };
    feed(t, addr);
    CHECK(t.screens.active->cursorY == 5);
    CHECK(t.screens.active->cursorX == 10);
    // A print lands at the addressed cell; no coordinate byte leaked as text.
    feed(t, "Z");
    CHECK(cell(t, 5, 10)->codePoint == 'Z');
    CHECK(cell(t, 0, 0)->codePoint == 0);
}

TEST(testLegacyWyseConsecutiveAddress)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::Modes::setEmulation(&t, Ubiquitty::VT::Emulation::Televideo);
    unsigned char seq[] = {
        0x1B,
        '=',
        0x20 + 2,
        0x20 + 3,
        0x1B,
        '=',
        0x20 + 4,
        0x20 + 6,
    };
    feed(t, seq);
    CHECK(t.screens.active->cursorY == 4);
    CHECK(t.screens.active->cursorX == 6);
    feed(t, "Q");
    CHECK(cell(t, 4, 6)->codePoint == 'Q');
    CHECK(cell(t, 0, 0)->codePoint == 0);
}

TEST(testLegacyWyseAttribute)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::Modes::setEmulation(&t, Ubiquitty::VT::Emulation::Wyse);
    // ESC G n: low nibble is the attribute mask (bit1 = reverse = '2').
    feed(t, "\033G2"); // reverse
    feed(t, "R");
    CHECK(cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Reverse));
    feed(t, "\033G8"); // underline (bit3); ESC G replaces, clears reverse
    feed(t, "U");
    CHECK(cell(t, 0, 1)->pen.attributes.contains(Ubiquitty::VT::Attribute::Underline));
    CHECK(!cell(t, 0, 1)->pen.attributes.contains(Ubiquitty::VT::Attribute::Reverse));
    feed(t, "\033G0"); // normal — mask 0 clears all
    feed(t, "N");
    CHECK(cell(t, 0, 2)->pen.attributes.isEmpty());
}

TEST(testLegacyControls)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::Modes::setEmulation(&t, Ubiquitty::VT::Emulation::Adm);
    feed(t, "AB\r"); // print AB, CR back to column 0
    CHECK(t.screens.active->cursorX == 0);
    CHECK(t.screens.active->cursorY == 0);
    feed(t, "\n"); // LF down one row
    CHECK(t.screens.active->cursorY == 1);
    feed(t, "XY\b"); // print XY at row 1, BS back one
    CHECK(t.screens.active->cursorX == 1);
    // ADM-3A home: ESC ; (and ESC H) -> (0,0).
    feed(t, "\033;");
    CHECK(t.screens.active->cursorX == 0);
    CHECK(t.screens.active->cursorY == 0);
}

TEST(testLegacyLinuxPaletteSet)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::Modes::setEmulation(&t, Ubiquitty::VT::Emulation::Linux);
    // Set palette entry 1 to 0xAA 0xBB 0xCC.
    feed(t, "\033]P1aabbcc");
    CHECK(Ubiquitty::channel16To8(t.palette.indexed[1].red) == 0xAA);
    CHECK(Ubiquitty::channel16To8(t.palette.indexed[1].green) == 0xBB);
    CHECK(Ubiquitty::channel16To8(t.palette.indexed[1].blue) == 0xCC);
    // The sequence completes without a terminator: text after it prints
    // normally on the grid, not into the OSC buffer.
    feed(t, "hi");
    CHECK(cell(t, 0, 0)->codePoint == 'h');
    CHECK(cell(t, 0, 1)->codePoint == 'i');
}

TEST(testLegacyLinuxPaletteReset)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::Modes::setEmulation(&t, Ubiquitty::VT::Emulation::Linux);
    feed(t, "\033]P200ff00"); // entry 2 -> green
    CHECK(Ubiquitty::channel16To8(t.palette.indexed[2].green) == 0xFF);
    feed(t, "\033]R"); // reset to defaults
    CHECK(Ubiquitty::channel16To8(t.palette.indexed[2].red) == Ubiquitty::defaultAnsi16[2][0]);
    CHECK(Ubiquitty::channel16To8(t.palette.indexed[2].green) == Ubiquitty::defaultAnsi16[2][1]);
    // Text after ESC ] R prints normally.
    feed(t, "ok");
    CHECK(cell(t, 0, 0)->codePoint == 'o');
}

TEST(testLegacyVTDefaultUnchanged)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    // No setEmulation: stays VT. ESC = is DECKPAM (application keypad),
    // not a cursor address — the cursor must not jump.
    feed(t, "\033=");
    feed(t, "A");
    CHECK(cell(t, 0, 0)->codePoint == 'A');
    CHECK(t.screens.active->cursorY == 0);
    CHECK(t.screens.active->cursorX == 1);
}

TEST(testAnswerback)
{
    Ubiquitty::Terminal t;
    char outputBuffer[80];
    Ubiquitty::hardReset(t);
    feed(t, "\005"); // ENQ, no answerback set
    CHECK(drain(t, outputBuffer) == 0);

    Ubiquitty::setAnswerback(&t, "UB-1");
    feed(t, "\005");
    int n = drain(t, outputBuffer);
    CHECK(n == 4);
    CHECK(strcmp(outputBuffer, "UB-1") == 0);

    // Long configured answerback strings are retained without a fixed-buffer cap.
    char big[128];
    memset(big, 'A', sizeof(big));
    big[sizeof(big) - 1] = '\0';
    Ubiquitty::setAnswerback(&t, big);
    feed(t, "\005");
    n = drain(t, outputBuffer);
    CHECK(n == 79);
}

TEST(testVT52Identify)
{
    Ubiquitty::Terminal t;
    char outputBuffer[16];
    Ubiquitty::hardReset(t);
    feed(t, "\033[?2l");
    feed(t, "\033Z");
    int n = drain(t, outputBuffer);
    CHECK(n == 3);
    CHECK(outputBuffer[0] == 0x1B && outputBuffer[1] == '/' && outputBuffer[2] == 'Z');
}

TEST(testDecSpecialGraphics)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033(0lqk\r\nx x\r\nmqj\033(B");
    CHECK(cell(t, 0, 0)->codePoint == 0x250C); // ┌
    CHECK(cell(t, 0, 1)->codePoint == 0x2500); // ─
    CHECK(cell(t, 0, 2)->codePoint == 0x2510); // ┐
    CHECK(cell(t, 1, 0)->codePoint == 0x2502); // │
    CHECK(cell(t, 1, 2)->codePoint == 0x2502);
    CHECK(cell(t, 2, 0)->codePoint == 0x2514); // └
    CHECK(cell(t, 2, 1)->codePoint == 0x2500);
    CHECK(cell(t, 2, 2)->codePoint == 0x2518); // ┘
}

TEST(testDecTechnicalCharacterSet)
{
    Ubiquitty::Terminal t;
    struct Probe {
        uint8_t in;
        uint16_t want;
    };
    const Probe probes[] = {
        { 0x21, 0x23B7 }, // ⎷ radical symbol bottom
        { 0x24, 0x2320 }, // ⌠ top half integral
        { 0x2B, 0x239B }, // ⎛ left parenthesis upper hook
        { 0x31, 0x23B2 }, // ⎲ summation top
        { 0x37, 0x27E9 }, // ⟩ sigma vertex (nearest encoded shape)
        { 0x38, 0x0020 }, // unassigned → blank, not '8'
        { 0x3C, 0x2264 }, // ≤
        { 0x44, 0x0394 }, // Δ
        { 0x56, 0x221A }, // √
        { 0x61, 0x03B1 }, // α
        { 0x6D, 0x0020 }, // unassigned → blank, not 'm'
        { 0x76, 0x0192 }, // ƒ
        { 0x7E, 0x2193 }, // ↓
    };
    for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
        const Probe& p = probes[i];
        Ubiquitty::hardReset(t);
        char buffer[16];
        snprintf(buffer, sizeof(buffer), "\033(>%c\033(B", p.in);
        feed(t, buffer);
        if (cell(t, 0, 0)->codePoint != p.want) {
            printf("FAIL %s: ESC ( > %02x → %04x (want %04x)\n", __func__, p.in, cell(t, 0, 0)->codePoint, p.want);
            failures++;
        }
    }
    /// ESC ) > designates G1; SO invokes it.
    Ubiquitty::hardReset(t);
    unsigned char seq[] = { 0x1B, ')', '>', 0x0E, 0x61, 0x0F };
    feed(t, seq);
    CHECK(cell(t, 0, 0)->codePoint == 0x03B1); // α through G1
    /// ESC * > into G2, reached for exactly one character by SS2.
    Ubiquitty::hardReset(t);
    unsigned char seq2[] = { 0x1B, '*', '>', 0x1B, 'N', 0x61, 0x61 };
    feed(t, seq2);
    CHECK(cell(t, 0, 0)->codePoint == 0x03B1);
    CHECK(cell(t, 0, 1)->codePoint == 'a'); // single shift expired
    /// ESC ( B restores US-ASCII.
    Ubiquitty::hardReset(t);
    feed(t, "\033(>\033(Ba");
    CHECK(cell(t, 0, 0)->codePoint == 'a');
}

TEST(testNRCSCharacterSetTranslations)
{
    Ubiquitty::Terminal t;
    /// (designator, source byte, expected Unicode codePoint)
    struct Probe {
        const char* selection;
        uint8_t in;
        uint16_t want;
    };
    const Probe probes[] = {
        { "(R", 0x23, 0x00A3 }, // French # → £
        { "(f", 0x40, 0x00E0 }, // French alias f, @ → à
        { "(K", 0x40, 0x00A7 }, // German @ → §
        { "(Y", 0x40, 0x00A7 }, // Italian @ → §
        { "(E", 0x5B, 0x00C6 }, // Norwegian/Danish [ → Æ
        { "(6", 0x7D, 0x00E5 }, // Norwegian alias 6, } → å
        { "(H", 0x40, 0x00C9 }, // Swedish @ → É
        { "(7", 0x5B, 0x00C4 }, // Swedish alias 7, [ → Ä
        { "(C", 0x7B, 0x00E4 }, // Finnish { → ä
        { "(5", 0x7C, 0x00F6 }, // Finnish alias 5, | → ö
        { "(4", 0x5B, 0x0133 }, // Dutch [ → ij ligature
        { "(=", 0x23, 0x00F9 }, // Swiss # → ù
        { "(Z", 0x5C, 0x00D1 }, // Spanish \\ → Ñ
        { "(Q", 0x5B, 0x00E2 }, // French-Canadian [ → â
        { "(9", 0x7E, 0x00FB }, // French-Canadian alias 9, ~ → û
    };
    for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
        const Probe& p = probes[i];
        Ubiquitty::hardReset(t);
        // ESC <selection> selects, then send the probe byte, then ESC ( B
        // returns to US-ASCII so subsequent tests don't carry state.
        char buffer[16];
        snprintf(buffer, sizeof(buffer), "\033%s%c\033(B", p.selection, p.in);
        feed(t, buffer);
        if (cell(t, 0, 0)->codePoint != p.want) {
            printf("FAIL %s: ESC %s %02x → %04x (want %04x)\n", __func__, p.selection, p.in, cell(t, 0, 0)->codePoint, p.want);
            failures++;
        }
        /// And the pass-through path: a byte outside the substitution
        /// table renders as itself even while the characterSet is active.
        Ubiquitty::hardReset(t);
        snprintf(buffer, sizeof(buffer),
            "\033%s."
            "\033(B",
            p.selection);
        feed(t, buffer);
        CHECK(cell(t, 0, 0)->codePoint == '.');
    }
}

TEST(testNRCSG1G2G3Designation)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    // ESC ) 0 — select DEC graphics into G1 — then SO (0x0E) shifts
    // G1 into active.
    unsigned char seq[] = { 0x1B, ')', '0', 0x0E, 'l', 0x0F };
    feed(t, seq);
    CHECK(cell(t, 0, 0)->codePoint == 0x250C); // ┌
    /// ESC * 0 — into G2 — and ESC N single-shift selects G2 for
    /// exactly the next character.
    Ubiquitty::hardReset(t);
    unsigned char seq2[] = { 0x1B, '*', '0', 0x1B, 'N', 'l', 'l' };
    feed(t, seq2);
    CHECK(cell(t, 0, 0)->codePoint == 0x250C); // SS2 took effect
    CHECK(cell(t, 0, 1)->codePoint == 'l'); // second 'l' back to G0 ASCII
    /// ESC + 0 — into G3 — and ESC O single-shift selects G3.
    Ubiquitty::hardReset(t);
    unsigned char seq3[] = { 0x1B, '+', '0', 0x1B, 'O', 'l' };
    feed(t, seq3);
    CHECK(cell(t, 0, 0)->codePoint == 0x250C);
}

TEST(testC18bitSingleShift)
{
    using Ubiquitty::Unicode::Encoding;
    Ubiquitty::Terminal t;
    /// Raw 8-bit C1 is honored only on a single-byte legacy input path, where
    /// 0x80-0x9F are the C1 range; in UTF-8 they are continuation bytes (see
    /// testC1RawGatedOffInUTF8). Latin-1 exercises the C1 path.
    Ubiquitty::hardReset(t);
    Ubiquitty::Unicode::encReset(&t.parser.inputEncoding, Encoding::Latin1);
    // ESC * 0 — DEC graphics into G2 — then 0x8E (SS2) selects G2 for the next
    // byte: 'l' -> ┌; the following 'l' is back to G0 ASCII (one-shot).
    unsigned char ss2[] = { 0x1B, '*', '0', 0x8E, 'l', 'l' };
    feed(t, ss2);
    CHECK(cell(t, 0, 0)->codePoint == 0x250C); // 8-bit SS2 took effect
    CHECK(cell(t, 0, 1)->codePoint == 'l'); // one-shot expired
    /// ESC + 0 — DEC graphics into G3 — then 0x8F (SS3) selects G3.
    Ubiquitty::hardReset(t);
    Ubiquitty::Unicode::encReset(&t.parser.inputEncoding, Encoding::Latin1);
    unsigned char ss3[] = { 0x1B, '+', '0', 0x8F, 'l' };
    feed(t, ss3);
    CHECK(cell(t, 0, 0)->codePoint == 0x250C); // 8-bit SS3 took effect
    /// Regression: 0x8E as a UTF-8 continuation byte is not SS2. 0xC8 0x8E
    /// decodes to U+020E; the 0x8E must complete the character and arm no
    /// shift, so the following 'x' stays plain ASCII. (hardReset preserves the
    /// input encoding, so restore UTF-8 after the Latin-1 sub-cases above.)
    Ubiquitty::hardReset(t);
    Ubiquitty::Unicode::encReset(&t.parser.inputEncoding, Encoding::UTF8);
    unsigned char cont[] = { 0xC8, 0x8E, 'x' };
    feed(t, cont);
    CHECK(cell(t, 0, 0)->codePoint == 0x20E); // Ȏ, not a single shift
    CHECK(cell(t, 0, 1)->codePoint == 'x'); // not G-shifted
}

TEST(testC1RawGatedOffInUTF8)
{
    /// Raw CSI 0x9B: must not start a control sequence. "31m" prints literally
    /// and the pen keeps the default foreground (no SGR 31 applied).
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    unsigned char csi[] = { 0x9B, '3', '1', 'm', 'R' };
    feed(t, csi);
    CHECK(cell(t, 0, 0)->codePoint == 0xFFFD);
    CHECK(cell(t, 0, 1)->codePoint == '3');
    CHECK(cell(t, 0, 4)->codePoint == 'R');
    CHECK(cell(t, 0, 4)->pen.foregroundIndex != 1); // SGR 31 never applied
    CHECK(t.screens.active->cursorX == 5);

    /// Raw SS2 0x8E: not a single shift; 'l' stays ASCII (no G2 graphics).
    Ubiquitty::hardReset(t);
    unsigned char ss2[] = { 0x1B, '*', '0', 0x8E, 'l' };
    feed(t, ss2);
    CHECK(cell(t, 0, 0)->codePoint == 0xFFFD);
    CHECK(cell(t, 0, 1)->codePoint == 'l'); // not ┌ (0x250C)

    /// Raw DECID 0x9A: no primary-DA reply, just a replacement glyph.
    Ubiquitty::hardReset(t);
    char reply[32];
    unsigned char decid[] = { 0x9A };
    feed(t, std::span(decid, 1));
    CHECK(drain(t, reply) == 0); // no DA1
    CHECK(cell(t, 0, 0)->codePoint == 0xFFFD); // nothing after it: a stray byte

    /// Raw SOS 0x98: no string state; the "payload" prints as text.
    Ubiquitty::hardReset(t);
    feed(t,
        "\x98"
        "hi");
    CHECK(cell(t, 0, 0)->codePoint == 0xFFFD);
    CHECK(cell(t, 0, 1)->codePoint == 'h');
    CHECK(cell(t, 0, 2)->codePoint == 'i');
}

TEST(testC1UndefinedIgnoredOnLegacyPath)
{
    using Ubiquitty::Unicode::Encoding;
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::Unicode::encReset(&t.parser.inputEncoding, Encoding::Latin1);
    unsigned char seq[] = { 0x81, 'A' }; // 0x81 has no C1 function
    feed(t, seq);
    CHECK(cell(t, 0, 0)->codePoint == 'A'); // 0x81 ignored; 'A' at column 0

    /// UTF-8: the same byte is malformed input and uses the replacement policy.
    Ubiquitty::hardReset(t);
    Ubiquitty::Unicode::encReset(&t.parser.inputEncoding, Encoding::UTF8);
    unsigned char u[] = { 0x81, 'A' };
    feed(t, u);
    CHECK(cell(t, 0, 0)->codePoint == 0xFFFD);
    CHECK(cell(t, 0, 1)->codePoint == 'A');
}

TEST(testString8bitStRespectsUTF8Mode)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    unsigned char utf8OSC[] = { 0x1B, ']', '0', ';', 'A', 0x9C, 'B', 0x07 };
    feed(t, utf8OSC);
    CHECK(t.titleChanged == true);
    CHECK(cell(t, 0, 0)->codePoint == 0);

    Ubiquitty::hardReset(t);
    Ubiquitty::Unicode::encReset(&t.parser.inputEncoding, Ubiquitty::Unicode::Encoding::Latin1);
    unsigned char latinOSC[] = { 0x1B, ']', '0', ';', 0xDC, 'A', 0x9C, 'B' };
    feed(t, latinOSC);
    CHECK(t.titleChanged == true);
    CHECK(cell(t, 0, 0)->codePoint == 'B');

    Ubiquitty::hardReset(t);
    Ubiquitty::Unicode::encReset(&t.parser.inputEncoding, Ubiquitty::Unicode::Encoding::Latin1);
    unsigned char syncLatinOSC[] = { 0x1B, ']', '0', ';', 0xDC, 'A', 0x9C, 'H' };
    int consumed = Ubiquitty::processBytesUpToSyncClear(&t, std::string_view(reinterpret_cast<const char*>(syncLatinOSC), sizeof(syncLatinOSC)));
    CHECK(consumed == (int)sizeof(syncLatinOSC));
    CHECK(t.titleChanged == true);
    CHECK(cell(t, 0, 0)->codePoint == 'H');

    /// Under a legacy (non-UTF-8) encoding, a mid-buffer DECRST 2026 sync-clear
    /// must STOP processBytesUpToSyncClear at the boundary so the synchronized
    /// frame publishes (no tearing). A decode loop that swallows the inner stop
    /// and reports the whole buffer consumed tears the frame it was asked to
    /// hold, and the encoding is what decides which loop runs.
    Ubiquitty::hardReset(t);
    Ubiquitty::Unicode::encReset(&t.parser.inputEncoding, Ubiquitty::Unicode::Encoding::Latin1);
    unsigned char syncClearLatin[] = {
        0x1B,
        '[',
        '?',
        '2',
        '0',
        '2',
        '6',
        'h', // DECSET 2026 begin sync
        'A',
        0x1B,
        '[',
        '?',
        '2',
        '0',
        '2',
        '6',
        'l', // DECRST 2026 -> sync-clear
        'B',
    };
    int syncN = (int)sizeof(syncClearLatin);
    int syncStop = Ubiquitty::processBytesUpToSyncClear(&t, std::string_view(reinterpret_cast<const char*>(syncClearLatin), static_cast<std::size_t>(syncN)));
    CHECK(syncStop < syncN); // boundary surfaced under a legacy encoding too
    CHECK(syncStop == syncN - 1); // stops right after the 'l'; 'B' remains
    CHECK(t.modes.syncOutput == false); // sync ended

    Ubiquitty::hardReset(t);
    Ubiquitty::Unicode::encReset(&t.parser.inputEncoding, Ubiquitty::Unicode::Encoding::Latin1);
    unsigned char latinAPC[] = { 0x1B, '_', 'G', 'a', '=', 'T', 0x9C, 'C' };
    feed(t, latinAPC);
    CHECK(cell(t, 0, 0)->codePoint == 'C');

    Ubiquitty::hardReset(t);
    Ubiquitty::Unicode::encReset(&t.parser.inputEncoding, Ubiquitty::Unicode::Encoding::Latin1);
    unsigned char latinDCS[] = { 0x1B, 'P', 't', 'x', 0x9C, 'D' };
    feed(t, latinDCS);
    CHECK(cell(t, 0, 0)->codePoint == 'D');

    Ubiquitty::hardReset(t);
    unsigned char oscEscCAN[] = { 0x1B, ']', '0', ';', 'x', 0x1B, 0x18, 'E' };
    feed(t, oscEscCAN);
    CHECK(cell(t, 0, 0)->codePoint == 'E');

    Ubiquitty::hardReset(t);
    unsigned char dcsEscSUB[] = { 0x1B, 'P', 't', 'x', 0x1B, 0x1A, 'F' };
    feed(t, dcsEscSUB);
    CHECK(cell(t, 0, 0)->codePoint == Ubiquitty::VT::substituteCharacter);
    CHECK(cell(t, 0, 1)->codePoint == 'F');

    Ubiquitty::hardReset(t);
    unsigned char apcEscCAN[] = { 0x1B, '_', 'G', 'a', '=', 'T', 0x1B, 0x18, 'G' };
    feed(t, apcEscCAN);
    CHECK(cell(t, 0, 0)->codePoint == 'G');
}

TEST(testLegacyEncodingBatchParity)
{
    const std::string stream = buildLegacyParityStream();
    const Ubiquitty::Unicode::Encoding encodings[] = {
        Ubiquitty::Unicode::Encoding::Latin1,
        Ubiquitty::Unicode::Encoding::Koi8R,
    };
    for (Ubiquitty::Unicode::Encoding encoding : encodings) {
        LegacyParitySnapshot perByte = legacyParityFeed(encoding, stream, 1);
        LegacyParitySnapshot chunked = legacyParityFeed(encoding, stream, 37);
        LegacyParitySnapshot whole = legacyParityFeed(encoding, stream, (int)stream.size());
        CHECK(legacyParityMatches(perByte, chunked));
        CHECK(legacyParityMatches(perByte, whole));
    }

    /// Sync-clear stop point: with high bytes queued behind a mid-buffer
    /// DECRST 2026, processBytesUpToSyncClear still stops exactly after the
    /// clearing 'l' and leaves the tail unconsumed.
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::Modes::setInputEncoding(&t, Ubiquitty::Unicode::Encoding::Latin1);
    unsigned char syncHighBytes[] = {
        0x1B,
        '[',
        '?',
        '2',
        '0',
        '2',
        '6',
        'h',
        0xE9,
        0xFC,
        'a',
        0xC0,
        0x1B,
        '[',
        '?',
        '2',
        '0',
        '2',
        '6',
        'l',
        0xDF,
        'b',
    };
    int syncLength = (int)sizeof(syncHighBytes);
    int stop = Ubiquitty::processBytesUpToSyncClear(&t, std::string_view(reinterpret_cast<const char*>(syncHighBytes), static_cast<std::size_t>(syncLength)));
    CHECK(stop == syncLength - 2);
    CHECK(t.modes.syncOutput == false);
    CHECK(cell(t, 0, 0)->codePoint == 0xE9);
    CHECK(cell(t, 0, 3)->codePoint == 0xC0);
    CHECK(cell(t, 0, 4)->codePoint == 0); // tail unconsumed
}

TEST(testG1ShiftOutGraphics)
{
    Ubiquitty::Terminal t;
    unsigned char seq[] = { 0x1B, ')', '0', 0x0E, 'l', 'q', 'k', 0x0F };
    Ubiquitty::hardReset(t);
    feed(t, seq);
    CHECK(cell(t, 0, 0)->codePoint == 0x250C);
    CHECK(cell(t, 0, 1)->codePoint == 0x2500);
    CHECK(cell(t, 0, 2)->codePoint == 0x2510);
}

TEST(test96setLatin1G1Ls1r)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::Modes::setInputEncoding(&t, Ubiquitty::Unicode::Encoding::Latin1); // GR exists only in an 8-bit coding
    // ESC - A   (designate Latin-1 → G1)
    // ESC ~     (LS1R: invoke G1 into GR)
    unsigned char seq[] = { 0x1B, '-', 'A', 0x1B, '~', 0xA0, 0xA9, 0xC0, 0xE9, 0xFF };
    feed(t, seq);
    CHECK(cell(t, 0, 0)->codePoint == 0x00A0); // NBSP
    CHECK(cell(t, 0, 1)->codePoint == 0x00A9); // ©
    CHECK(cell(t, 0, 2)->codePoint == 0x00C0); // À
    CHECK(cell(t, 0, 3)->codePoint == 0x00E9); // é
    CHECK(cell(t, 0, 4)->codePoint == 0x00FF); // ÿ
    // GL (0x20..0x7F) is untouched by the GR invocation: ASCII stays ASCII.
    feed(t, "Z");
    CHECK(cell(t, 0, 5)->codePoint == 'Z');
}

TEST(test96setDecSupplementalG2Ls2r)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::Modes::setInputEncoding(&t, Ubiquitty::Unicode::Encoding::Latin1); // GR exists only in an 8-bit coding
    // ESC . <   (designate DEC Supplemental → G2)
    // ESC }     (LS2R: invoke G2 into GR)
    unsigned char seq[] = { 0x1B, '.', '<', 0x1B, '}', 0xA8, 0xC0, 0xD7, 0xDD, 0xE9, 0xF7, 0xFD };
    feed(t, seq);
    CHECK(cell(t, 0, 0)->codePoint == 0x00A4); // ¤ (DEC moves it to A8)
    CHECK(cell(t, 0, 1)->codePoint == 0x00C0); // À (== Latin-1)
    CHECK(cell(t, 0, 2)->codePoint == 0x0152); // Œ (DEC, not ×)
    CHECK(cell(t, 0, 3)->codePoint == 0x0178); // Ÿ (DEC, not Ý)
    CHECK(cell(t, 0, 4)->codePoint == 0x00E9); // é (== Latin-1)
    CHECK(cell(t, 0, 5)->codePoint == 0x0153); // œ (DEC, not ÷)
    CHECK(cell(t, 0, 6)->codePoint == 0x00FF); // ÿ (DEC, not ý)
}

TEST(test96setG3Ls3r)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::Modes::setInputEncoding(&t, Ubiquitty::Unicode::Encoding::Latin1); // GR exists only in an 8-bit coding
    unsigned char seq[] = { 0x1B, '/', 'A', 0x1B, '|', 0xE0, 0xE7 };
    feed(t, seq);
    CHECK(cell(t, 0, 0)->codePoint == 0x00E0); // à
    CHECK(cell(t, 0, 1)->codePoint == 0x00E7); // ç
}

TEST(testLs2Ls3LockIntoGl)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    unsigned char seq[] = { 0x1B, '*', '0', 0x1B, 'n', 'l', 'q' };
    feed(t, seq);
    CHECK(cell(t, 0, 0)->codePoint == 0x250C); // ┌ via G2 in GL (LS2)
    CHECK(cell(t, 0, 1)->codePoint == 0x2500); // ─ — still G2 (locking)
}

TEST(testSingleShiftOntoGrByte)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::Modes::setInputEncoding(&t, Ubiquitty::Unicode::Encoding::Latin1); // GR exists only in an 8-bit coding
    // ESC - A → Latin-1 into... no: - designates G1. Use ESC . A for G2.
    // ESC . A  (Latin-1 → G2)
    // ESC N    (SS2: G2 for the next char only)
    // 0xE9     (resolves via G2 = Latin-1 → é)
    unsigned char seq[] = { 0x1B, '.', 'A', 0x1B, 'N', 0xE9 };
    feed(t, seq);
    CHECK(cell(t, 0, 0)->codePoint == 0x00E9); // é via SS2/G2
    // The single-shift is one-shot: it is consumed, so a SUBSEQUENT GR byte
    // with no GR set invoked goes back to the UTF-8 / replacement path.
    CHECK(t.singleShift == 0);
}

TEST(testSingleShiftSs3Gl94set)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    unsigned char seq[] = { 0x1B, '+', '0', 0x1B, 'O', 'l', 'l' };
    feed(t, seq);
    CHECK(cell(t, 0, 0)->codePoint == 0x250C); // SS3 took effect
    CHECK(cell(t, 0, 1)->codePoint == 'l'); // back to G0 ASCII
}

TEST(testDecSupplemental94setGl)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    unsigned char seq[] = { 0x1B, '(', '<', 'W', '@', 'w' };
    feed(t, seq);
    CHECK(cell(t, 0, 0)->codePoint == 0x0152); // 'W' (0x57→0xD7) → Œ
    CHECK(cell(t, 0, 1)->codePoint == 0x00C0); // '@' (0x40→0xC0) → À
    CHECK(cell(t, 0, 2)->codePoint == 0x0153); // 'w' (0x77→0xF7) → œ
}

TEST(testGrDefaultInertKeepsUTF8)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    unsigned char seq[] = { 0xA9 }; // lone continuation byte: invalid UTF-8
    feed(t, seq);
    CHECK(cell(t, 0, 0)->codePoint == 0xFFFD); // replacement, not ©
    /// And real UTF-8 still decodes: U+00A9 © = 0xC2 0xA9.
    Ubiquitty::hardReset(t);
    unsigned char utf8seq[] = { 0xC2, 0xA9 };
    feed(t, utf8seq);
    CHECK(cell(t, 0, 0)->codePoint == 0x00A9);
}

TEST(testUTF8ContinuationsAreNotC1Controls)
{
    Ubiquitty::Terminal t;
    unsigned char a_ogonek[] = { 0xC4, 0x84, 'X' };
    unsigned char right_quote[] = { 0xE2, 0x80, 0x9D, 'Y' };

    Ubiquitty::hardReset(t);
    feed(t, a_ogonek);
    CHECK(t.screens.active->cursorY == 0);
    CHECK(cell(t, 0, 0)->codePoint == 0x0104); // Ą — canonical Unicode
    CHECK(cell(t, 0, 1)->codePoint == 'X');

    Ubiquitty::hardReset(t);
    feed(t, right_quote);
    CHECK(t.screens.active->cursorY == 0);
    CHECK(cell(t, 0, 0)->codePoint == 0x201D); // ” — canonical Unicode
    CHECK(cell(t, 0, 1)->codePoint == 'Y');
}

// A control byte closes an incomplete UTF-8 sequence, and the truncated
// character is substituted before the control acts.
//
// The control still does its work — the line feed moves down, the escape
// sequence parses, the bell rings — and the reader sees one U+FFFD where a
// character was cut short. Unicode's recommended practice (§3.9) is one
// replacement per maximal subpart, and dropping the lead in silence showed one
// character fewer than the stream contained, which is what a showcase of
// invalid encodings rendered as an empty cell.
TEST(testControlsCloseAPartialUTF8Sequence)
{
    Ubiquitty::Terminal t;
    unsigned char lf_seq[] = { 0xE2, 0x0A, 'A' };
    unsigned char esc_seq[] = { 0xE2, 0x1B, '[', '3', '1', 'm', 'R' };
    unsigned char bel_seq[] = { 0xE2, 0x07, 'B' };

    // The replacement lands on the row the sequence was cut short on, then the
    // line feed moves down. It keeps the column, as it does after any character,
    // so 'A' follows in column one.
    Ubiquitty::hardReset(t);
    feed(t, lf_seq);
    CHECK(cell(t, 0, 0)->codePoint == 0xFFFD);
    CHECK(t.screens.active->cursorY == 1);
    CHECK(cell(t, 1, 1)->codePoint == 'A');

    // The escape still selects the colour, and 'R' follows the replacement.
    Ubiquitty::hardReset(t);
    feed(t, esc_seq);
    CHECK(cell(t, 0, 0)->codePoint == 0xFFFD);
    CHECK(cell(t, 0, 1)->codePoint == 'R');
    CHECK(cell(t, 0, 1)->pen.foregroundIndex == 1);

    // The bell still rings.
    Ubiquitty::hardReset(t);
    CHECK(!t.bellPending);
    feed(t, bel_seq);
    CHECK(t.bellPending);
    CHECK(cell(t, 0, 0)->codePoint == 0xFFFD);
    CHECK(cell(t, 0, 1)->codePoint == 'B');
    t.bellPending = false;
    CHECK(!t.bellPending);
}

TEST(testRawC1CSIUnderLegacyEncoding)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::Unicode::encReset(&t.parser.inputEncoding, Ubiquitty::Unicode::Encoding::Latin1);
    feed(t, "X");
    CHECK(cell(t, 0, 0)->codePoint == 'X');

    // 0x9B '2' 'J' = the 8-bit form of ED 2 (erase whole display).
    unsigned char ed2[] = { 0x9B, '2', 'J' };
    feed(t, ed2);
    CHECK(cell(t, 0, 0)->codePoint == 0); // erased, not "X2J" as literal text
}

TEST(testRawC1MidRunSplitsLegacyBatch)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::Unicode::encReset(&t.parser.inputEncoding, Ubiquitty::Unicode::Encoding::Latin1);
    unsigned char run[] = { 'X', 0x9B, '2', 'J', 0xE9 }; // X, 8-bit ED 2, é
    feed(t, run);
    CHECK(cell(t, 0, 0)->codePoint == 0); // ED 2 fired mid-run: X erased
    CHECK(cell(t, 0, 1)->codePoint == 0x00E9); // post-C1 bytes still decode
}

TEST(testRawC1ControlsUnderLegacyEncoding)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::Unicode::encReset(&t.parser.inputEncoding, Ubiquitty::Unicode::Encoding::Latin1);

    // 0x85 = NEL (next line): cursor to column 0 of the next row, not the
    // literal codePoint 0x85 (which Latin-1 would otherwise decode to).
    unsigned char nel[] = { 'A', 0x85, 'B' };
    feed(t, nel);
    CHECK(cell(t, 0, 0)->codePoint == 'A');
    CHECK(t.screens.active->cursorY == 1);
    CHECK(cell(t, 1, 0)->codePoint == 'B');

    /// 0x9D ... 0x9C = the 8-bit OSC ... ST form, setting the title.
    Ubiquitty::hardReset(t);
    Ubiquitty::Unicode::encReset(&t.parser.inputEncoding, Ubiquitty::Unicode::Encoding::Latin1);
    unsigned char osc[] = { 0x9D, '0', ';', 'T', 0x9C, 'C' };
    feed(t, osc);
    CHECK(t.titleChanged == true);
    CHECK(cell(t, 0, 0)->codePoint == 'C');
}

TEST(testG2G3SingleShifts)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    // Designate DEC special graphics as G2 (ESC * 0) and then SS2 `l`
    // — which resolves to U+250C through DEC special.
    feed(t, "\033*0\033Nl");
    CHECK(cell(t, 0, 0)->codePoint == 0x250C);

    // The next character must not use G2 (single shift was consumed).
    feed(t, "x");
    CHECK(cell(t, 0, 1)->codePoint == 'x');
    CHECK(!cell(t, 0, 1)->pen.attributes.contains(Ubiquitty::VT::Attribute::ForceEmojiPresentation));

    /// G3 designation + SS3 (ESC O) works the same way.
    Ubiquitty::hardReset(t);
    feed(t, "\033+0\033Oq");
    CHECK(cell(t, 0, 0)->codePoint == 0x2500);
}

TEST(testUkNationalSet)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    // G0=UK
    feed(t, "\033(A#A");
    CHECK(cell(t, 0, 0)->codePoint == 0xA3); // £ in MacRoman
    CHECK(cell(t, 0, 1)->codePoint == 'A');
    // switch back to US-ASCII
    feed(t, "\033(B#");
    CHECK(cell(t, 0, 2)->codePoint == '#');

    /// G2=UK reachable via SS2 single shift.
    Ubiquitty::hardReset(t);
    feed(t, "\033*A\033N#");
    CHECK(cell(t, 0, 0)->codePoint == 0xA3);
}

TEST(testNRCSFrench)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033(R#A");
    CHECK(cell(t, 0, 0)->codePoint == 0x00A3); // £
    CHECK(cell(t, 0, 1)->codePoint == 'A'); // ASCII passthrough

    /// All ten French substitutions, written into a fresh row.
    Ubiquitty::hardReset(t);
    feed(t, "\033(R#@[\\]{|}~");
    CHECK(cell(t, 0, 0)->codePoint == 0x00A3); // # → £
    CHECK(cell(t, 0, 1)->codePoint == 0x00E0); // @ → à
    CHECK(cell(t, 0, 2)->codePoint == 0x00B0); // [ → °
    CHECK(cell(t, 0, 3)->codePoint == 0x00E7); // \ → ç
    CHECK(cell(t, 0, 4)->codePoint == 0x00A7); // ] → §
    CHECK(cell(t, 0, 5)->codePoint == 0x00E9); // { → é
    CHECK(cell(t, 0, 6)->codePoint == 0x00F9); // | → ù
    CHECK(cell(t, 0, 7)->codePoint == 0x00E8); // } → è
    CHECK(cell(t, 0, 8)->codePoint == 0x00A8); // ~ → ¨

    /// The 'f' designator is the long-form alias for 'R'.
    Ubiquitty::hardReset(t);
    feed(t, "\033(f@");
    CHECK(cell(t, 0, 0)->codePoint == 0x00E0); // à
}

TEST(testNRCSGerman)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033(K[\\]~");
    CHECK(cell(t, 0, 0)->codePoint == 0x00C4); // [ → Ä
    CHECK(cell(t, 0, 1)->codePoint == 0x00D6); // \ → Ö
    CHECK(cell(t, 0, 2)->codePoint == 0x00DC); // ] → Ü
    CHECK(cell(t, 0, 3)->codePoint == 0x00DF); // ~ → ß

    /// Lowercase row: {|}.
    Ubiquitty::hardReset(t);
    feed(t, "\033(K{|}");
    CHECK(cell(t, 0, 0)->codePoint == 0x00E4); // { → ä
    CHECK(cell(t, 0, 1)->codePoint == 0x00F6); // | → ö
    CHECK(cell(t, 0, 2)->codePoint == 0x00FC); // } → ü
}

TEST(testNRCSItalian)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033(Y[]");
    CHECK(cell(t, 0, 0)->codePoint == 0x00B0); // [ → °
    CHECK(cell(t, 0, 1)->codePoint == 0x00E9); // ] → é

    /// Complete substitution row.
    Ubiquitty::hardReset(t);
    feed(t, "\033(Y#@\\`{|}~");
    CHECK(cell(t, 0, 0)->codePoint == 0x00A3); // # → £
    CHECK(cell(t, 0, 1)->codePoint == 0x00A7); // @ → §
    CHECK(cell(t, 0, 2)->codePoint == 0x00E7); // \ → ç
    CHECK(cell(t, 0, 3)->codePoint == 0x00F9); // ` → ù
    CHECK(cell(t, 0, 4)->codePoint == 0x00E0); // { → à
    CHECK(cell(t, 0, 5)->codePoint == 0x00F2); // | → ò
    CHECK(cell(t, 0, 6)->codePoint == 0x00E8); // } → è
    CHECK(cell(t, 0, 7)->codePoint == 0x00EC); // ~ → ì
}

TEST(testNRCSNordan)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033(E[\\]");
    CHECK(cell(t, 0, 0)->codePoint == 0x00C6); // [ → Æ
    CHECK(cell(t, 0, 1)->codePoint == 0x00D8); // \ → Ø
    CHECK(cell(t, 0, 2)->codePoint == 0x00C5); // ] → Å

    Ubiquitty::hardReset(t);
    feed(t, "\033(6{|}");
    CHECK(cell(t, 0, 0)->codePoint == 0x00E6); // { → æ
    CHECK(cell(t, 0, 1)->codePoint == 0x00F8); // | → ø
    CHECK(cell(t, 0, 2)->codePoint == 0x00E5); // } → å
}

TEST(testNRCSSwedishFinnish)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033(H[\\]");
    CHECK(cell(t, 0, 0)->codePoint == 0x00C4); // [ → Ä
    CHECK(cell(t, 0, 1)->codePoint == 0x00D6); // \ → Ö
    CHECK(cell(t, 0, 2)->codePoint == 0x00C5); // ] → Å

    /// Designator '5' selects Finnish; the table is identical to Swedish,
    /// so the same substitution must apply.
    Ubiquitty::hardReset(t);
    feed(t, "\033(5{|}");
    CHECK(cell(t, 0, 0)->codePoint == 0x00E4); // { → ä
    CHECK(cell(t, 0, 1)->codePoint == 0x00F6); // | → ö
    CHECK(cell(t, 0, 2)->codePoint == 0x00E5); // } → å
}

TEST(testNRCSSpanish)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033(Z[\\]");
    CHECK(cell(t, 0, 0)->codePoint == 0x00A1); // [ → ¡
    CHECK(cell(t, 0, 1)->codePoint == 0x00D1); // \ → Ñ
    CHECK(cell(t, 0, 2)->codePoint == 0x00BF); // ] → ¿
}

TEST(testNRCSSwiss)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033(=#@_");
    CHECK(cell(t, 0, 0)->codePoint == 0x00F9); // # → ù
    CHECK(cell(t, 0, 1)->codePoint == 0x00E0); // @ → à
    CHECK(cell(t, 0, 2)->codePoint == 0x00E8); // _ → è
}

TEST(testNRCSFrenchCanadian)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033(Q@[\\");
    CHECK(cell(t, 0, 0)->codePoint == 0x00E0); // @ → à
    CHECK(cell(t, 0, 1)->codePoint == 0x00E2); // [ → â
    CHECK(cell(t, 0, 2)->codePoint == 0x00E7); // \ → ç

    Ubiquitty::hardReset(t);
    feed(t, "\033(9{|}");
    CHECK(cell(t, 0, 0)->codePoint == 0x00E9); // { → é
    CHECK(cell(t, 0, 1)->codePoint == 0x00F9); // | → ù
    CHECK(cell(t, 0, 2)->codePoint == 0x00E8); // } → è
}

TEST(testNRCSRoundTripToASCII)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033(R#X\033(B#Y");
    CHECK(cell(t, 0, 0)->codePoint == 0x00A3); // # under French → £
    CHECK(cell(t, 0, 1)->codePoint == 'X');
    CHECK(cell(t, 0, 2)->codePoint == '#'); // # back under ASCII → '#'
    CHECK(cell(t, 0, 3)->codePoint == 'Y');
}

TEST(testDelByteIsNoopInVT52Ground)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033[?2l"); // leave ANSI → VT52 mode
    static const unsigned char bytes[] = { 0x7F, 'A' };
    feed(t, bytes);
    CHECK(cell(t, 0, 0)->codePoint == 'A');
    CHECK(cell(t, 0, 1)->codePoint == 0);
}

TEST(testNRCSDesignatesG1)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    // G1 = French, SO (0x0E) locks shift to G1.
    feed(t, "\033)R\016#");
    CHECK(cell(t, 0, 0)->codePoint == 0xA3); // £
    // SI (0x0F) returns to G0 (still ASCII).
    feed(t, "\017#");
    CHECK(cell(t, 0, 1)->codePoint == '#');
}

TEST(testNRCSSingleShiftG2)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    // G2=French, SS2 then '#' — emits £ once.
    feed(t, "\033*R\033N#");
    CHECK(cell(t, 0, 0)->codePoint == 0xA3); // £ via G2
    // Subsequent character must not be single-shifted (SS2 consumed).
    feed(t, "#");
    CHECK(cell(t, 0, 1)->codePoint == '#');
}

TEST(testNRCSDECSCPreservesCharacterSet)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    // Designate G0=French; '#' → £.
    feed(t, "\033(R");
    feed(t, "#");
    CHECK(cell(t, 0, 0)->codePoint == 0xA3);

    // DECSC (ESC 7) snapshots the characterSet state.
    feed(t, "\0337");

    // Designate G0=US-ASCII; '#' → '#'.
    feed(t, "\033(B#");
    CHECK(cell(t, 0, 1)->codePoint == '#');

    // DECRC (ESC 8) restores characterSet state — French is back.
    feed(t, "\0338");
    feed(t, "#");
    // Cursor was restored to its DECSC position (column 1) so the cell
    // we wrote at lands there — but the previous '#' is overwritten.
    CHECK(cell(t, 0, 1)->codePoint == 0xA3);
}

TEST(test_nrcs_f_alias_matches_R)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033(f#");
    CHECK(cell(t, 0, 0)->codePoint == 0xA3); // £ via French/f
    Ubiquitty::hardReset(t);
    feed(t, "\033(R#");
    CHECK(cell(t, 0, 0)->codePoint == 0xA3); // £ via French/R
}

/// DECSC / DECRC — G0 characterSet designation restored across an
/// intervening switch to US-ASCII. We probe behaviourally: after
/// restore, designating G0 to US-ASCII and rendering '#' should still
/// show '#'; while DEC special graphics is active, 'l' renders as the
/// box-drawing top-left glyph (grTL).
// A 96-set invoked into GR leaves UTF-8 alone. Under the UTF-8 encoding the
// bytes 0x80..0xFF are parts of encoded characters, so box drawing written as
// UTF-8 (E2 94 80 ...) keeps decoding while LS1R is in force, after G1 is
// designated back to ASCII with GR still pointing at it, and on the per-byte
// route as well as the batched one. The 94-set path through SO/SI is untouched.
TEST(test96SetInGRLeavesUTF8BoxDrawingIntact)
{
    const unsigned char boxRun[] = { 0xE2, 0x94, 0x8C, 0xE2, 0x94, 0x80, 0xE2, 0x94, 0x90 };
    const auto checkBoxRow = [](Ubiquitty::Terminal& t, int row) {
        CHECK(cell(t, row, 0)->codePoint == 0x250C);
        CHECK(cell(t, row, 1)->codePoint == 0x2500);
        CHECK(cell(t, row, 2)->codePoint == 0x2510);
    };

    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033-A\033~"); // Latin-1 into G1, G1 into GR
    CHECK(t.characterSetGR == 1);
    Ubiquitty::processBytes(&t, std::string_view(reinterpret_cast<const char*>(boxRun), sizeof boxRun));
    checkBoxRow(t, 0);

    feed(t, "\r\n\033-<"); // DEC Supplemental, one byte at a time
    for (unsigned char byte : boxRun)
        Ubiquitty::processBytes(&t, std::string_view(reinterpret_cast<const char*>(&byte), 1));
    checkBoxRow(t, 1);

    feed(t, "\r\n\033)B"); // G1 back to ASCII, GR still invokes it
    Ubiquitty::processBytes(&t, std::string_view(reinterpret_cast<const char*>(boxRun), sizeof boxRun));
    checkBoxRow(t, 2);

    feed(t, "\r\n\033)0\016lqk\017"); // DEC Special Graphics through G1 in GL
    checkBoxRow(t, 3);
    feed(t, "\r\n\303\200"); // U+00C0 as UTF-8, not two GR bytes
    CHECK(cell(t, 4, 0)->codePoint == 0x00C0);
    CHECK(cell(t, 4, 1)->codePoint == 0);
}

// Under a single-byte encoding the same invocation maps 0xA0..0xFF through the
// invoked set, including when the invocation and the GR bytes share one write:
// the encoded-input path decodes a run before parsing it, so the run has to end
// where an escape sequence could change what a GR byte means.
TEST(test96SetInGRMapsRawBytesUnderAnEightBitEncoding)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::Modes::setInputEncoding(&t, Ubiquitty::Unicode::Encoding::Latin1);
    feed(t, "\033-<\033~\327\367x\033-A\327\r\n");
    CHECK(cell(t, 0, 0)->codePoint == 0x0152); // DEC Supplemental 0xD7
    CHECK(cell(t, 0, 1)->codePoint == 0x0153); // DEC Supplemental 0xF7
    CHECK(cell(t, 0, 2)->codePoint == 'x');
    CHECK(cell(t, 0, 3)->codePoint == 0x00D7); // ISO 8859-1 0xD7
    // With GR at its default a single shift claims the next GR byte only, and
    // the byte after it is the encoding's own right half again.
    Ubiquitty::hardReset(t);
    Ubiquitty::Modes::setInputEncoding(&t, Ubiquitty::Unicode::Encoding::Latin1);
    feed(t, "\033.<\033N\327\327");
    CHECK(cell(t, 0, 0)->codePoint == 0x0152);
    CHECK(cell(t, 0, 1)->codePoint == 0x00D7);
    CHECK(t.singleShift == 0);
}

// DECSC saves G0..G3, the GL and GR invocations; DECRC puts every one back.
TEST(testDECSCDECRCRestoresDesignationsAndShifts)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033)0\033.<\033+A\016\033}"); // G1 special, G2 DEC Supplemental, G3 Latin-1, SO, LS2R
    const auto savedG = t.characterSetG;
    feed(t, "\0337");
    feed(t, "\017\033)B\033.B\033+B\033(0\033~");
    CHECK(t.characterSetActive == 0);
    CHECK(t.characterSetGR == 1);
    feed(t, "\0338");
    CHECK(t.characterSetG == savedG);
    CHECK(t.characterSetActive == 1);
    CHECK(t.characterSetGR == 2);
    feed(t, "\033[1;1Hq");
    CHECK(cell(t, 0, 0)->codePoint == 0x2500); // G1 in GL again
}

// RIS and DECSTR return G0..G3 to ASCII and both invocations to their defaults.
TEST(testRISAndDECSTRResetCharacterSets)
{
    const auto checkDefaults = [](const Ubiquitty::Terminal& t) {
        for (int slot : t.characterSetG)
            CHECK(slot == Ubiquitty::VT::characterSetUSASCII);
        CHECK(t.characterSetActive == 0);
        CHECK(t.characterSetGR == 0);
        CHECK(t.singleShift == 0);
    };
    const char* dirty = "\033(0\033)0\033.A\033/<\016\033|\033N";

    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, dirty);
    feed(t, "\033c"); // RIS
    checkDefaults(t);
    feed(t, dirty);
    feed(t, "\033[!p"); // DECSTR
    checkDefaults(t);
}

TEST(testDECSCDECRCSavesCharacterSetDesignation)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033(0"); // G0 = DEC special graphics
    int g0_before = t.characterSetG[0];
    feed(t, "\0337"); // DECSC
    feed(t, "\033(B"); // G0 = US-ASCII
    CHECK(t.characterSetG[0] != g0_before);
    feed(t, "\0338"); // DECRC
    CHECK(t.characterSetG[0] == g0_before);
    // And behaviourally: 'l' should now resolve to the box-drawing
    // codePoint again (U+250C), not the literal letter.
    feed(t, "\033[1;1Hl");
    CHECK(cell(t, 0, 0)->codePoint == 0x250C);
}

TEST(testInvalidUTF8FffdDefault)
{
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        // Lone 0xFF — never a valid UTF-8 byte.
        const unsigned char bad[] = { 0xFF };
        feed(t, std::span(bad, 1));
        CHECK(cell(t, 0, 0)->codePoint == 0xFFFD);
        CHECK(t.screens.active->cursorX == 1); // U+FFFD has width 1
        CHECK(cell(t, 0, 1)->codePoint == 0); // next cell untouched
    }
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        // Truncated lead: 0xE2 expects two continuations; 'A' breaks it. The
        // abandoned sequence -> U+FFFD, then the 'A' decodes cleanly.
        const unsigned char bad[] = { 0xE2, 0x41 };
        feed(t, std::span(bad, 2));
        CHECK(cell(t, 0, 0)->codePoint == 0xFFFD);
        CHECK(cell(t, 0, 1)->codePoint == 'A');
        CHECK(t.screens.active->cursorX == 2);
    }
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        // Overlong encoding of '/' (0x2F) as a 2-byte form: C0 AF. The lead
        // 0xC0 is itself an invalid lead (below 0xC2) -> U+FFFD; 0xAF is then
        // a lone continuation -> a second U+FFFD.
        const unsigned char bad[] = { 0xC0, 0xAF };
        feed(t, std::span(bad, 2));
        CHECK(cell(t, 0, 0)->codePoint == 0xFFFD);
        CHECK(cell(t, 0, 1)->codePoint == 0xFFFD);
        CHECK(t.screens.active->cursorX == 2);
    }
}

TEST(testInvalidUTF8Drop)
{
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        Ubiquitty::Modes::setInvalidUTF8Policy(&t, Ubiquitty::Unicode::InvalidUTF8Policy::Drop);
        const unsigned char bad[] = { 0xFF };
        feed(t, std::span(bad, 1));
        CHECK(cell(t, 0, 0)->codePoint == 0); // nothing emitted
        CHECK(t.screens.active->cursorX == 0); // cursor unmoved
    }
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        Ubiquitty::Modes::setInvalidUTF8Policy(&t, Ubiquitty::Unicode::InvalidUTF8Policy::Drop);
        // Abandoned lead is dropped; the breaking 'A' still prints at col 0.
        const unsigned char bad[] = { 0xE2, 0x41 };
        feed(t, std::span(bad, 2));
        CHECK(cell(t, 0, 0)->codePoint == 'A');
        CHECK(t.screens.active->cursorX == 1);
    }
}

TEST(testInvalidUTF8Latin1)
{
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        Ubiquitty::Modes::setInvalidUTF8Policy(&t, Ubiquitty::Unicode::InvalidUTF8Policy::Latin1);
        const unsigned char bad[] = { 0xFF };
        feed(t, std::span(bad, 1));
        CHECK(cell(t, 0, 0)->codePoint == 0x00FF); // ÿ
        CHECK(t.screens.active->cursorX == 1);
    }
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        Ubiquitty::Modes::setInvalidUTF8Policy(&t, Ubiquitty::Unicode::InvalidUTF8Policy::Latin1);
        // 0x80 is a lone continuation byte -> Latin-1 U+0080.
        const unsigned char bad[] = { 0x80 };
        feed(t, std::span(bad, 1));
        CHECK(cell(t, 0, 0)->codePoint == 0x0080);
    }
    {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        Ubiquitty::Modes::setInvalidUTF8Policy(&t, Ubiquitty::Unicode::InvalidUTF8Policy::Latin1);
        // Aborted sequence: the LEAD 0xC3 (Ã) is reinterpreted as Latin-1, not
        // the breaking byte; then 'A' decodes cleanly. Output: Ã then A.
        const unsigned char bad[] = { 0xC3, 0x41 };
        feed(t, std::span(bad, 2));
        CHECK(cell(t, 0, 0)->codePoint == 0x00C3); // Ã (lead byte as Latin-1)
        CHECK(cell(t, 0, 1)->codePoint == 'A');
        CHECK(t.screens.active->cursorX == 2);
    }
}

TEST(testInvalidUTF8PolicyLeavesValidUntouched)
{
    const Ubiquitty::Unicode::InvalidUTF8Policy modes[] = {
        Ubiquitty::Unicode::InvalidUTF8Policy::FFFD,
        Ubiquitty::Unicode::InvalidUTF8Policy::Drop,
        Ubiquitty::Unicode::InvalidUTF8Policy::Latin1,
    };
    for (auto mode : modes) {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        Ubiquitty::Modes::setInvalidUTF8Policy(&t, mode);
        feed(t, "\xC3\xA9"); // é (U+00E9), a valid 2-byte sequence
        CHECK(cell(t, 0, 0)->codePoint == 0x00E9);
        CHECK(t.screens.active->cursorX == 1);
    }
}

TEST(testDECDLDBasicGlyph)
{
    Ubiquitty::Terminal t;
    // Pfn=1 Pcn=0 Pe=0 Pcmw=8 Pw=0 Pt=0 Pcmh=8 Pss=0 (94-set, 8x8 matrix),
    // designator '@', one solid 8x8 glyph: band 0 (rows 0-5) all-on = '~'x8, '/'
    // to the next band, band 1 (rows 6-7 on, bits 0-1 = 0x03 -> 'B') = 'B'x8.
    //
    // Pcn 0 and 1 both name the set's FIRST character, and a 94-set's first is
    // '!' — cell 1. SPACE is not a member of a 94-set, so a glyph loaded at
    // cell 0 would print on every space in the run.
    feed(t, "\033P1;0;0;8;0;0;8;0{@~~~~~~~~/BBBBBBBB\033\\");
    CHECK(Ubiquitty::ensureSoftFont(t).valid);
    CHECK(Ubiquitty::ensureSoftFont(t).matrixWidth == 8);
    CHECK(Ubiquitty::ensureSoftFont(t).matrixHeight == 8);
    CHECK(!Ubiquitty::ensureSoftFont(t).set96);
    CHECK(Ubiquitty::ensureSoftFont(t).designFinal == '@');
    CHECK(Ubiquitty::ensureSoftFont(t).designIntermediate == 0);
    CHECK(Ubiquitty::ensureSoftFont(t).glyphSet[1]);
    for (int row = 0; row < 8; ++row)
        CHECK(Ubiquitty::ensureSoftFont(t).glyph[1][row] == 0xFF); // all 8 columns on, every row
    CHECK(!Ubiquitty::ensureSoftFont(t).glyphSet[0]); // SPACE is no member of a 94-set
    CHECK(!Ubiquitty::ensureSoftFont(t).glyphSet[2]); // untouched cells stay unset
}

TEST(testDECDLDDesignatorAndMultiglyph)
{
    Ubiquitty::Terminal t;
    // Designator ' @' (intermediate space 0x20 + final '@'). Pcn=2 -> first glyph
    // at cell 2. 4x6 matrix, 96-set. Glyph for cell 2: column 0 on, rows 0-5
    // ('~' then '?'x3 = empty columns). ';' -> cell 3: row 0 on across 4 columns
    // ('@'x4 = bit 0 of each column).
    feed(t, "\033P0;2;0;4;0;0;6;1{ @~???;@@@@\033\\");
    CHECK(Ubiquitty::ensureSoftFont(t).valid);
    CHECK(Ubiquitty::ensureSoftFont(t).set96); // Pss=1
    CHECK(Ubiquitty::ensureSoftFont(t).matrixWidth == 4);
    CHECK(Ubiquitty::ensureSoftFont(t).matrixHeight == 6);
    CHECK(Ubiquitty::ensureSoftFont(t).designIntermediate == 0x20);
    CHECK(Ubiquitty::ensureSoftFont(t).designFinal == '@');
    CHECK(!Ubiquitty::ensureSoftFont(t).glyphSet[1]); // cells before Pcn untouched
    CHECK(Ubiquitty::ensureSoftFont(t).glyphSet[2]);
    CHECK(Ubiquitty::ensureSoftFont(t).glyphSet[3]);
    for (int row = 0; row < 6; ++row)
        CHECK(Ubiquitty::ensureSoftFont(t).glyph[2][row] == 0x01); // cell 2: column 0 only
    CHECK(Ubiquitty::ensureSoftFont(t).glyph[3][0] == 0x0F); // cell 3: row 0, columns 0-3
    for (int row = 1; row < 6; ++row)
        CHECK(Ubiquitty::ensureSoftFont(t).glyph[3][row] == 0x00);
}

TEST(testDECDLDBoundsAndReset)
{
    Ubiquitty::Terminal t;
    feed(t, "\033P1;0;0;8;0;0;8;0{@~~~~~~~~/BBBBBBBB\033\\");
    CHECK(Ubiquitty::ensureSoftFont(t).valid);

    // Pcmw=99 exceeds the cap -> the whole download is rejected before the erase,
    // so the existing set is untouched.
    feed(t, "\033P1;0;0;99;0;0;8;0{A~~~~\033\\");
    CHECK(Ubiquitty::ensureSoftFont(t).valid);
    CHECK(Ubiquitty::ensureSoftFont(t).designFinal == '@'); // still the first set
    CHECK(Ubiquitty::ensureSoftFont(t).matrixWidth == 8);

    // matrixWidth=4 but 8 column-bytes supplied: only columns 0-3 are written.
    Ubiquitty::Terminal u;
    feed(u, "\033P1;0;0;4;0;0;6;0{B~~~~~~~~\033\\");
    CHECK(Ubiquitty::ensureSoftFont(u).valid);
    CHECK(Ubiquitty::ensureSoftFont(u).matrixWidth == 4);
    for (int row = 0; row < 6; ++row)
        CHECK(Ubiquitty::ensureSoftFont(u).glyph[1][row] == 0x0F); // 4 columns, rows 0-5

    // RIS (hard reset) reconstructs the Terminal — the downloaded set is gone.
    Ubiquitty::hardReset(u);
    CHECK(!Ubiquitty::ensureSoftFont(u).valid);
    CHECK(!Ubiquitty::ensureSoftFont(u).glyphSet[1]);
}

TEST(testDECDLDPe1ReloadAndValidate)
{
    Ubiquitty::Terminal t;
    feed(t, "\033P1;0;0;8;0;0;8;0{@~~~~~~~~/BBBBBBBB\033\\"); // solid 8x8 at the set's first character
    CHECK(Ubiquitty::ensureSoftFont(t).glyph[1][0] == 0xFF);

    // Pe=1 reload of that character with a single top-left dot must REPLACE the
    // solid block: designator '@', glyph '@' (col 0 / row 0) then 7 empty columns.
    feed(t, "\033P1;0;1;8;0;0;8;0{@@???????\033\\");
    CHECK(Ubiquitty::ensureSoftFont(t).glyph[1][0] == 0x01); // only col 0, row 0
    for (int row = 1; row < 8; ++row)
        CHECK(Ubiquitty::ensureSoftFont(t).glyph[1][row] == 0x00); // old rows 1-7 erased, not merged

    // Pe=1 command with an empty payload (no valid designator) must be rejected
    // without touching the live set's geometry or glyphs.
    feed(t, "\033P0;0;1;4;0;0;6;0{\033\\");
    CHECK(Ubiquitty::ensureSoftFont(t).matrixWidth == 8); // unchanged — rejected command's Pcmw=4 ignored
    CHECK(Ubiquitty::ensureSoftFont(t).matrixHeight == 8); // unchanged
    CHECK(Ubiquitty::ensureSoftFont(t).designFinal == '@');
    CHECK(Ubiquitty::ensureSoftFont(t).glyph[1][0] == 0x01); // still the dot
}

TEST(testDRCSSCSDesignationRendersSentinel)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    // Solid 8x8 glyph at cell 1 (Pcn=1 -> GL byte 0x21 '!'), designator '@',
    // no intermediate, 94-set.
    feed(t, "\033P1;1;0;8;0;0;8;0{@~~~~~~~~/BBBBBBBB\033\\");
    CHECK(Ubiquitty::ensureSoftFont(t).valid);
    CHECK(Ubiquitty::ensureSoftFont(t).glyphSet[1]);

    // ESC ( @ — '@' is no hard 94-set final, so the soft designator matches and
    // G0 becomes the DRCS set.
    feed(t, "\033(@");
    feed(t, "!"); // GL 0x21 -> glyph index 1 (defined)
    CHECK(cell(t, 0, 0)->codePoint == Ubiquitty::VT::drcsCodePointBase + 1);

    // An undefined slot ('A' = 0x41 -> index 0x21) falls back to the raw byte.
    feed(t, "A");
    CHECK(cell(t, 0, 1)->codePoint == 'A');

    // Reselecting ASCII (ESC ( B) restores normal rendering.
    feed(t, "\033(B!");
    CHECK(cell(t, 0, 2)->codePoint == '!');
}

TEST(testDRCSSCSIntermediateDesignation)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    // Designator ' @' (intermediate 0x20 + final '@'), glyph at cell 1.
    feed(t, "\033P1;1;0;8;0;0;8;0{ @~~~~~~~~\033\\");
    CHECK(Ubiquitty::ensureSoftFont(t).designIntermediate == 0x20);
    CHECK(Ubiquitty::ensureSoftFont(t).glyphSet[1]);

    // A no-intermediate ESC ( @ must not match (its intermediate is 0, the set's
    // is 0x20): G0 stays ASCII, so '!' prints as itself.
    feed(t, "\033(@!");
    CHECK(cell(t, 0, 0)->codePoint == '!');

    // ESC ) SP @ designates the soft set into G1; SO (0x0E) invokes G1 into GL.
    feed(t, "\033) @");
    feed(t, "\016"); // So
    feed(t, "!");
    CHECK(cell(t, 0, 1)->codePoint == Ubiquitty::VT::drcsCodePointBase + 1);
    feed(t, "\017"); // SI — back to G0 (ASCII)
    feed(t, "!");
    CHECK(cell(t, 0, 2)->codePoint == '!');
}

TEST(testDRCSSoftresetUndesignatesKeepsFont)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033P1;1;0;8;0;0;8;0{@~~~~~~~~\033\\");
    feed(t, "\033(@!");
    CHECK(cell(t, 0, 0)->codePoint == Ubiquitty::VT::drcsCodePointBase + 1);

    feed(t, "\033[!p"); // DECSTR
    CHECK(Ubiquitty::ensureSoftFont(t).valid); // font itself survives a soft reset
    feed(t, "!"); // G0 is ASCII again after DECSTR
    CHECK(cell(t, 0, 1)->codePoint == '!');

    // Re-designating the still-resident set renders DRCS again.
    feed(t, "\033(@!");
    CHECK(cell(t, 0, 2)->codePoint == Ubiquitty::VT::drcsCodePointBase + 1);
}

// Legacy erase finals (Televideo / Wyse): ESC T clears to end of line, ESC Y
// clears to end of screen, ESC * / + / : clear the whole screen and home.
TEST(testLegacyEraseFinals)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::Modes::setEmulation(&t, Ubiquitty::VT::Emulation::Wyse);
    Ubiquitty::resize(t, 5, 10);

    feed(t, "abc"); // row 0 columns 0..2
    feed(t, "\033Y"); // clear to end of screen — nothing above the cursor row
    CHECK(cell(t, 0, 0)->codePoint == 'a'); // row 0 survives (EOS from cursor row's first column)
    feed(t, "DEF\033T"); // ESC T: clear to end of line — keeps D/E/F, clears past
    CHECK(cell(t, 0, 3)->codePoint == 'D');
    CHECK(cell(t, 0, 4)->codePoint == 'E');
    CHECK(cell(t, 0, 5)->codePoint == 'F');
    CHECK(cell(t, 0, 6)->codePoint == 0);

    feed(t, "xy\r\n"); // rows 0-1 filled, cursor at row 1 col 2
    feed(t, "\033*"); // clear screen + home
    CHECK(t.screens.active->cursorX == 0);
    CHECK(t.screens.active->cursorY == 0);
    CHECK(cell(t, 0, 0)->codePoint == 0);

    feed(t, "z\033+"); // ESC + also clears + homes
    CHECK(t.screens.active->cursorX == 0);
    CHECK(t.screens.active->cursorY == 0);
    CHECK(cell(t, 0, 0)->codePoint == 0);

    feed(t, "w\033:"); // ESC : same
    CHECK(t.screens.active->cursorX == 0);
    CHECK(t.screens.active->cursorY == 0);
    CHECK(cell(t, 0, 0)->codePoint == 0);
}

// An unmapped legacy escape final is dropped, never printed, and the state
// machine returns to ground so the next byte prints normally.
TEST(testLegacyUnmappedFinalIsDropped)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::Modes::setEmulation(&t, Ubiquitty::VT::Emulation::Wyse);

    feed(t, "\033X"); // unknown final
    CHECK(cell(t, 0, 0)->codePoint == 0);
    feed(t, "ok");
    CHECK(cell(t, 0, 0)->codePoint == 'o');
    CHECK(cell(t, 0, 1)->codePoint == 'k');
}

// ESC G attribute bits: bit0 blink, bit1 reverse, bit2 dim, bit3 underline.
TEST(testLegacyAttributeBlinkAndDimBits)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::Modes::setEmulation(&t, Ubiquitty::VT::Emulation::Wyse);

    feed(t, "\033G1");
    feed(t, "B");
    CHECK(cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Blink));

    feed(t, "\033G4");
    feed(t, "D");
    CHECK(cell(t, 0, 1)->pen.attributes.contains(Ubiquitty::VT::Attribute::Dim));

    feed(t, "\033G\x0F"); // all four mask bits
    feed(t, "A");
    CHECK(cell(t, 0, 2)->pen.attributes.contains(Ubiquitty::VT::Attribute::Blink));
    CHECK(cell(t, 0, 2)->pen.attributes.contains(Ubiquitty::VT::Attribute::Reverse));
    CHECK(cell(t, 0, 2)->pen.attributes.contains(Ubiquitty::VT::Attribute::Dim));
    CHECK(cell(t, 0, 2)->pen.attributes.contains(Ubiquitty::VT::Attribute::Underline));
}

// ESC = addressing clamps out-of-range biased row/column bytes to the page.
TEST(testLegacyAddressClampsToPage)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::Modes::setEmulation(&t, Ubiquitty::VT::Emulation::Wyse);
    Ubiquitty::resize(t, 5, 10);

    unsigned char below[] = { 0x1B, '=', 0x20 - 1, 0x20 - 1 }; // row/col = -1
    feed(t, below);
    CHECK(t.screens.active->cursorY == 0);
    CHECK(t.screens.active->cursorX == 0);

    unsigned char beyond[] = { 0x1B, '=', 0x20 + 99, 0x20 + 99 }; // row/col = 99
    feed(t, beyond);
    CHECK(t.screens.active->cursorY == 4);
    CHECK(t.screens.active->cursorX == 9);
}

// VT52 ESC Y addressing clamps the same way.
TEST(testVT52AddressClampsToPage)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 5, 10);
    feed(t, "\033[?2l"); // enter VT52

    unsigned char below[] = { 0x1B, 'Y', 0x20 - 1, 0x20 - 1 };
    feed(t, below);
    CHECK(t.screens.active->cursorY == 0);
    CHECK(t.screens.active->cursorX == 0);

    unsigned char beyond[] = { 0x1B, 'Y', 0x20 + 99, 0x20 + 99 };
    feed(t, beyond);
    CHECK(t.screens.active->cursorY == 4);
    CHECK(t.screens.active->cursorX == 9);
}

// 96-set designations (ESC - / . / /): the right-half sets, only valid for
// G1..G3. A is Latin-1, B Latin-2, < DEC Supplemental.
TEST(test96SetDesignations)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    feed(t, "\033-B");
    CHECK(t.characterSetG[1] == Ubiquitty::VT::characterSetLatin2);
    feed(t, "\033-A");
    CHECK(t.characterSetG[1] == Ubiquitty::VT::characterSetLatin1);
    feed(t, "\033-<");
    CHECK(t.characterSetG[1] == Ubiquitty::VT::characterSetDECSupplemental);
    feed(t, "\033.B");
    CHECK(t.characterSetG[2] == Ubiquitty::VT::characterSetLatin2);
    feed(t, "\033/B");
    CHECK(t.characterSetG[3] == Ubiquitty::VT::characterSetLatin2);
    // G0 stays 94-set-only: a 96-set designation leaves it untouched.
    CHECK(t.characterSetG[0] == Ubiquitty::VT::characterSetUSASCII);
}

// The ESC-with-intermediate dispatcher: hash finals (DECALN / line attributes),
// the space intermediate (8-bit C1 transmit mode), the SCS designation arms,
// and an unknown final's swallow.
TEST(testEscapeIntermediateDispatch)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 5, 10);

    feed(t, "\033#8"); // DECALN — every cell an 'E', margins reset, cursor home
    CHECK(cell(t, 4, 9)->codePoint == 'E');
    CHECK(t.screens.active->cursorX == 0);
    CHECK(t.screens.active->cursorY == 0);

    feed(t, "\033#3"); // double-height top
    CHECK(t.screens.active->lineAttribute[0] == Ubiquitty::lineAttributeDoubleHeightTop);
    feed(t, "\033#5"); // normal again
    CHECK(t.screens.active->lineAttribute[0] == Ubiquitty::lineAttributeNormal);

    feed(t, "\033 F"); // ESC <space> F: 8-bit C1 transmit OFF
    CHECK(t.transmit8bitC1 == false);
    feed(t, "\033 G");
    CHECK(t.transmit8bitC1 == true);

    feed(t, "\033(0"); // G0 = DEC special graphics
    CHECK(t.characterSetG[0] == Ubiquitty::VT::characterSetDECSpecial);
    feed(t, "\033)0");
    CHECK(t.characterSetG[1] == Ubiquitty::VT::characterSetDECSpecial);
    feed(t, "\033*0");
    CHECK(t.characterSetG[2] == Ubiquitty::VT::characterSetDECSpecial);
    feed(t, "\033+0");
    CHECK(t.characterSetG[3] == Ubiquitty::VT::characterSetDECSpecial);

    feed(t, "\033(X"); // unknown 94-set final — swallowed, ground unchanged
    feed(t, "\033(B"); // G0 back to ASCII for the printable check
    feed(t, "ok");
    CHECK(cell(t, 0, 0)->codePoint == 'o');
    CHECK(cell(t, 0, 1)->codePoint == 'k');
}

// DECDLD soft-font download: a valid set downloads, its SCS designator
// invokes it, and printed GL bytes resolve to DRCS sentinels. Out-of-range
// matrix dims reject the whole download; a set past the cell cap is truncated.
TEST(testDecSoftFontDownloadUseAndLimits)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);

    feed(t, "\033P1;1;0;8;0;0;8;0{@~~~~~~~~/BBBBBBBB\033\\");
    feed(t, "\033(@!");
    CHECK(cell(t, 0, 0)->codePoint == Ubiquitty::VT::drcsCodePointBase + 1);

    // A 999-wide matrix is refused: the live set survives untouched.
    const uint32_t before = Ubiquitty::ensureSoftFont(t).serial;
    feed(t, "\033P1;1;0;999;0;0;8;0{@~~~~~~~~\033\\");
    CHECK(Ubiquitty::ensureSoftFont(t).serial == before);

    // More cells than the 96-cell cap: the tail is dropped, the font stays.
    std::string flood = "\033P1;0;0;8;0;0;8;0{@";
    for (int i = 0; i < 200; i++)
        flood += "~;";
    flood += "\033\\";
    feed(t, flood.c_str());
    CHECK(Ubiquitty::ensureSoftFont(t).valid);
    feed(t, "\033(@!");
    CHECK(cell(t, 0, 1)->codePoint == Ubiquitty::VT::drcsCodePointBase + 1);
}

// The SetFont capability policy gates DECDLD: a denied origin's download is
// consumed and dropped, so no soft font takes identity.
TEST(testDecSoftFontPolicyDenied)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    fakeCapabilityPolicy.wasCalled = false;
    fakeCapabilityPolicy.decision = Ubiquitty::CapabilityDecision::Deny;
    t.capabilityPolicy = &fakeCapabilityPolicy;

    feed(t, "\033P1;1;0;8;0;0;8;0{@~~~~~~~~\033\\");
    CHECK(fakeCapabilityPolicy.wasCalled);
    CHECK(fakeCapabilityPolicy.lastCapability == Ubiquitty::Capability::SetFont);
    CHECK(Ubiquitty::ensureSoftFont(t).valid == false);

    t.capabilityPolicy = nullptr;
}

// Legacy C0 controls on a non-ANSI grammar: BEL arms the bell, HT advances to
// the next 8-column stop (clamped to the last column).
TEST(testLegacyBellAndHorizontalTab)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::Modes::setEmulation(&t, Ubiquitty::VT::Emulation::Wyse);
    Ubiquitty::resize(t, 5, 10);

    feed(t, "\x07");
    CHECK(t.bellPending);

    feed(t, "\x09"); // HT: 0 -> 8
    CHECK(t.screens.active->cursorX == 8);
    feed(t, "\x09"); // already past the next stop: clamped to the last column
    CHECK(t.screens.active->cursorX == 9);
}

// ESC G is Wyse/Televideo-only; on ADM the final is unmapped and dropped, so
// the attribute byte that followed it prints as plain text.
TEST(testESCGUnderADMEmulationIsDropped)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::Modes::setEmulation(&t, Ubiquitty::VT::Emulation::Adm);

    feed(t, "\033G1"); // ESC G dropped; the '1' is no attribute byte here
    CHECK(cell(t, 0, 0)->codePoint == '1');
    CHECK(!cell(t, 0, 0)->pen.attributes.contains(Ubiquitty::VT::Attribute::Blink));
    feed(t, "x");
    CHECK(cell(t, 0, 1)->codePoint == 'x');
}

// DECDLD's glyph store is not part of a Terminal that never downloaded one.
TEST(testSoftFontStoreIsAbsentUntilDownloaded)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    CHECK_MSG(Ubiquitty::softFontOrNull(t) == nullptr, "no DECDLD, no store");
    CHECK_MSG(!Ubiquitty::hasValidSoftFont(t), "an absent store is not a valid soft font");

    // A designation with nothing downloaded resolves to no DRCS.
    feed(t, "\033( @");
    feed(t, "A");
    CHECK(Ubiquitty::softFontOrNull(t) == nullptr);

    feed(t, "\033P1;1;0;8;0;0;8;0{@????????\033\\");
    CHECK_MSG(Ubiquitty::softFontOrNull(t) != nullptr, "a download allocates the store");

    Ubiquitty::hardReset(t);
    CHECK(Ubiquitty::softFontOrNull(t) == nullptr);
}

// A national set designates with DECNRCM reset, at every level, as it does
// with the mode set: DEC STD 070 SCS lets an emulator "recognize all NRC sets
// regardless of the current keyboard dialect", and the VT510 RM supports all
// DEC character sets in VT500 mode. A set Core does not have (the VT100's
// alternate ROM, the VT382's JIS-Roman, Portuguese, Greek) designates ASCII;
// a final that names no set, or a second intermediate, is consumed and
// changes nothing (xterm charproc.c, xtermDecodeSCS; VTPrsTbl.c, scstable).
TEST(testNationalSetsDesignateWithDECNRCMResetAndUnknownSetsDesignateASCII)
{
    struct Probe {
        const char* designation;
        uint32_t want;
    };
    const Probe national[] = {
        { "\033(A#", 0x00A3 }, // United Kingdom: £
        { "\033(R@", 0x00E0 }, // French: à
        { "\033(K[", 0x00C4 }, // German: Ä
        { "\033)K\016[", 0x00C4 }, // German in G1, shifted into GL
    };
    Ubiquitty::Terminal t;
    for (const char* level : { "", "\033[61\"p", "\033[62\"p", "\033[64;1\"p" }) {
        for (const Probe& probe : national) {
            Ubiquitty::hardReset(t);
            feed(t, level);
            feed(t, probe.designation);
            CHECK(!t.modes.nrcMode);
            CHECK_MSG(cell(t, 0, 0)->codePoint == probe.want, probe.designation);
        }
    }
    Ubiquitty::hardReset(t);
    feed(t, nationalReplacementMode);
    feed(t, "\033(K[");
    CHECK(cell(t, 0, 0)->codePoint == 0x00C4);
    for (const char* designation : { "\033(1", "\033(J", "\033(%6", "\033(\"?" }) {
        Ubiquitty::hardReset(t);
        feed(t, "\033(0");
        feed(t, designation);
        feed(t, "[q");
        CHECK(cell(t, 0, 0)->codePoint == '[');
        CHECK(cell(t, 0, 1)->codePoint == 'q');
    }
    for (const char* designation : { "\033(D", "\033(3", "\033(!B", "\033(%9" }) {
        Ubiquitty::hardReset(t);
        feed(t, "\033(0");
        feed(t, designation);
        feed(t, "q");
        CHECK(cell(t, 0, 0)->codePoint == 0x2500);
    }
    Ubiquitty::hardReset(t);
    feed(t, "\033)0\033-Z\033-3\016q");
    CHECK(cell(t, 0, 0)->codePoint == 0x2500);
}

// ESC ( % 5 designates DEC Supplemental Graphics as a 94-set; in GL 07/07
// is Œ (xterm map_DEC_Supp_Graphic).
TEST(testDECSupplementalGraphicsByItsPercentFiveDesignation)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033(%5WA}");
    CHECK(cell(t, 0, 0)->codePoint == 0x0152);
    CHECK(cell(t, 0, 1)->codePoint == 0x00C1);
    CHECK(cell(t, 0, 2)->codePoint == 0x00FF);
}

// The VT510's ISO 8859 96-sets: designated into G1 and shifted into GL, a
// byte reads as its right-half position (VT510 RM, SCS: B Latin-2, F Greek,
// H Hebrew, L Latin/Cyrillic, M Latin-5). Below VT500 level the designation is
// not recognized and the set stays as it was (xterm CASE_GSETS5).
TEST(testISO96SetsDesignateAtVT500Level)
{
    struct Probe {
        const char* designation;
        char byte;
        uint32_t want;
    };
    const Probe probes[] = {
        { "\033-B", '1', 0x0105 }, // 0xB1 ą
        { "\033-F", 'a', 0x03B1 }, // 0xE1 α
        { "\033-H", '`', 0x05D0 }, // 0xE0 א
        { "\033-L", '0', 0x0410 }, // 0xB0 А
        { "\033-M", 'q', 0x00F1 }, // 0xF1 ñ
        { "\033-M", 'P', 0x011E }, // 0xD0 Ğ
    };
    Ubiquitty::Terminal t;
    for (const Probe& probe : probes) {
        Ubiquitty::hardReset(t);
        feed(t, "\033)0");
        feed(t, probe.designation);
        const char text[] = { 0x0E, probe.byte, 0 };
        feed(t, text);
        CHECK_MSG(cell(t, 0, 0)->codePoint == probe.want, probe.designation);
    }
    Ubiquitty::hardReset(t);
    feed(t, "\033[64;1\"p\033)0\033-M\016q");
    CHECK(cell(t, 0, 0)->codePoint == 0x2500);
}

TEST(hebrewEncodingAndNationalReplacementModesAreInverses)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::hardReset(terminal);
    feed(terminal, "\033[?36$p\033[?42$p\033[?36l\033[?42$p\033[?42l\033[?36$p\033[?42h\033[?36$p");
    std::array<char, 128> reply {};
    const auto length = drain(terminal, reply);
    CHECK(std::string_view(reply.data(), length) == "\033[?36;1$y\033[?42;2$y\033[?42;1$y\033[?36;1$y\033[?36;2$y");
}

TEST(nationalReplacementModeIsIndependentOnTheStatusLine)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::hardReset(terminal);
    feed(terminal, "\033[?42h\033[2$~\033[1$}\033[?42$p");
    std::array<char, 128> reply {};
    auto length = drain(terminal, reply);
    CHECK(std::string_view(reply.data(), length) == "\033[?42;2$y");
    CHECK(!Ubiquitty::VT::isNationalReplacementEnabled(terminal));
    CHECK(terminal.modes.nrcMode);
    feed(terminal, "\033[?36l\033[0$}\033[?36h\033[?42$p\033[1$}\033[?42$p");
    length = drain(terminal, reply);
    CHECK(std::string_view(reply.data(), length) == "\033[?42;2$y\033[?42;1$y");
    CHECK(Ubiquitty::VT::isNationalReplacementEnabled(terminal));
    CHECK(!terminal.modes.nrcMode);
    CHECK(terminal.isStatusNationalReplacementEnabled);
    Ubiquitty::hardReset(terminal);
    CHECK(!terminal.modes.nrcMode);
    CHECK(!terminal.isStatusNationalReplacementEnabled);
}

// The setting selects nothing on either display: a national set designates
// on the status line as on the main display, with the setting reset or set.
TEST(nationalDesignationsTakeEffectOnEitherDisplayWhateverItsSetting)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::hardReset(terminal);
    feed(terminal, "\033[2$~\033[1$}\033[?36h\033(K[\033[0$}\033[?36h\033(K[");
    CHECK(Ubiquitty::Rendering::statusCellAt(terminal, 0).codePoint == 0xC4);
    CHECK(cell(terminal, 0, 0)->codePoint == 0xC4);
    feed(terminal, "\033[1$}\033[?36l\033(A#\033[0$}\033[?36l#");
    CHECK(Ubiquitty::Rendering::statusCellAt(terminal, 1).codePoint == 0xA3);
    CHECK(cell(terminal, 0, 1)->codePoint == 0xA3);
}

TEST(hebrewEncodingModeSaveRestoreKeepsTheNationalReplacementAlias)
{
    Ubiquitty::Terminal terminal;
    Ubiquitty::hardReset(terminal);
    feed(terminal, "\033[?36l\033[?36s\033[?42l\033[?36r");
    CHECK(terminal.modes.nrcMode);
    feed(terminal, "\033[?42s\033[?36h");
    CHECK(!terminal.modes.nrcMode);
    feed(terminal, "\033[?42r");
    CHECK(terminal.modes.nrcMode);
}

UBIQUITTY_TEST_MAIN()

TEST(testCodingSystemSwitchAppliesInsideAWriteAndAcrossEverySplit)
{
    const std::string input = "a\033%@\033)0\033~\361\370\033%G\303\261\033%@\361\033%8\303\251";
    constexpr std::array<uint32_t, 6> expected { 'a', 0x2500, 0x2502, 0x00F1, 0x2500, 0x00E9 };
    for (const bool shouldStopAtSyncClear : { false, true }) {
        for (size_t chunkSize = 1; chunkSize <= input.size(); ++chunkSize) {
            Ubiquitty::Terminal t;
            Ubiquitty::hardReset(t);
            for (size_t offset = 0; offset < input.size(); offset += chunkSize) {
                const auto chunk = std::string_view(input).substr(offset, chunkSize);
                if (shouldStopAtSyncClear)
                    CHECK(Ubiquitty::processBytesUpToSyncClear(&t, chunk) == static_cast<int>(chunk.size()));
                else
                    Ubiquitty::processBytes(&t, chunk);
            }
            for (size_t column = 0; column < expected.size(); ++column)
                CHECK(cell(t, 0, static_cast<int>(column))->codePoint == expected[column]);
            CHECK(t.screens.active->cursorX == static_cast<int>(expected.size()));
            CHECK(t.parser.inputEncoding.enc == Ubiquitty::Unicode::Encoding::UTF8);
        }
    }
}

TEST(testCodingSystemSwitchRetainsTheSyncClearBoundary)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    const std::string frame = "\033[?2026h\033%@\351\033%G\303\251\033[?2026l";
    const std::string input = frame + "tail";
    bool stopped = false;
    CHECK(Ubiquitty::processBytesUpToSyncClear(&t, input, &stopped) == static_cast<int>(frame.size()));
    CHECK(stopped);
    CHECK(cell(t, 0, 0)->codePoint == 0x00E9);
    CHECK(cell(t, 0, 1)->codePoint == 0x00E9);
    CHECK(cell(t, 0, 2)->codePoint == 0);
    feed(t, "tail");
    CHECK(cell(t, 0, 2)->codePoint == 't');
}

TEST(testCodingSystemSelectorRejectsExtraIntermediatesAndCancellation)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033%/@\033%Z\033%\030@\303\251");
    CHECK(t.parser.inputEncoding.enc == Ubiquitty::Unicode::Encoding::UTF8);
    CHECK(cell(t, 0, 0)->codePoint == '@');
    CHECK(cell(t, 0, 1)->codePoint == 0x00E9);
    feed(t, "\033%\007@\351");
    CHECK(t.parser.inputEncoding.enc == Ubiquitty::Unicode::Encoding::Latin1);
    CHECK(cell(t, 0, 2)->codePoint == 0x00E9);
    feed(t, "\033%/G\351");
    CHECK(t.parser.inputEncoding.enc == Ubiquitty::Unicode::Encoding::Latin1);
    CHECK(cell(t, 0, 3)->codePoint == 0x00E9);
}
