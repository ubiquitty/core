// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// What reaches the screen when a UTF-8 stream is malformed.
//
// Unicode's recommended practice (§3.9, "U+FFFD Substitution of Maximal
// Subparts") is one U+FFFD per maximal subpart of an ill-formed sequence: a
// truncated sequence contributes one for the part that could still have been
// valid, and a byte that cannot begin or continue anything contributes its own.
// A terminal that silently drops instead shows a shorter line than the stream
// describes, and one that emits per byte shows a longer one — either way the
// cells no longer correspond to what arrived.

#include "Harness.h"

#include "Core/Modes/Emulation.h"
#include "Core/Screen/Addressing.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <string>
#include <string_view>

namespace {

void feed(Ubiquitty::Terminal& t, std::string_view bytes)
{
    Ubiquitty::processBytes(&t, bytes);
}

// How many U+FFFD cells the first row holds, and how many cells in total carry
// anything — the two numbers a reader of the screen would count.
int replacementCount(const Ubiquitty::Terminal& t)
{
    int count = 0;
    for (int column = 0; column < t.columns; ++column) {
        const Ubiquitty::Cell* cell = Ubiquitty::Screen::visibleCell(&t, 0, 0, column);
        if (cell && cell->codePoint == 0xFFFD)
            ++count;
    }
    return count;
}

} // namespace

// A byte that can neither begin nor continue a sequence is one maximal subpart,
// so it contributes exactly one replacement.
TEST(testALoneContinuationIsOneReplacement)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, std::string_view("\xA9", 1));
    CHECK(replacementCount(t) == 1);
}

// PTY read boundaries carry no encoding information. Each malformed byte uses
// the selected policy even when printable text follows it in the same read.
TEST(testStrayContinuationsAreIndependentOfWriteBoundaries)
{
    Ubiquitty::Terminal t;
    using Ubiquitty::Unicode::InvalidUTF8Policy;
    for (const auto policy : { InvalidUTF8Policy::FFFD, InvalidUTF8Policy::Drop, InvalidUTF8Policy::Latin1 }) {
        for (unsigned byte = 0x80; byte <= 0xBF; ++byte) {
            std::string input = "A";
            input += static_cast<char>(byte);
            input += "2;2H";
            for (size_t split = 0; split <= input.size(); ++split) {
                Ubiquitty::hardReset(t);
                Ubiquitty::Modes::setInvalidUTF8Policy(&t, policy);
                feed(t, std::string_view(input).substr(0, split));
                feed(t, std::string_view(input).substr(split));
                // Latin-1's soft hyphen is a format character, with no cell.
                const bool isDiscarded = policy == InvalidUTF8Policy::Drop || (policy == InvalidUTF8Policy::Latin1 && byte == 0xAD);
                const int textColumn = isDiscarded ? 1 : 2;
                CHECK(replacementCount(t) == (policy == InvalidUTF8Policy::FFFD ? 1 : 0));
                CHECK(Ubiquitty::Screen::visibleCell(&t, 0, 0, 0)->codePoint == 'A');
                if (!isDiscarded)
                    CHECK_MSG(Ubiquitty::Screen::visibleCell(&t, 0, 0, 1)->codePoint == (policy == InvalidUTF8Policy::FFFD ? 0xFFFD : byte), std::to_string(byte).c_str());
                CHECK(Ubiquitty::Screen::visibleCell(&t, 0, 0, textColumn)->codePoint == '2');
                CHECK(t.screens.active->cursorX == textColumn + 4);
                CHECK(t.screens.active->cursorY == 0);
            }
        }
    }
}

TEST(testAByteThatIsNeverValidIsOneReplacement)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, std::string_view("\xFF", 1));
    CHECK(replacementCount(t) == 1);
}

// A two-byte lead with nothing after it is a maximal subpart: the stream ended
// (or turned to something that cannot continue it) while it was still
// incomplete, and the reader is owed one replacement for it.
TEST(testATruncatedTwoByteSequenceIsOneReplacement)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    // A lead, then an ASCII byte that cannot continue it.
    feed(t, std::string_view("\xC3.", 2));
    CHECK(replacementCount(t) == 1);
}

// Likewise a three-byte sequence cut short after its first continuation: the
// two bytes that arrived form one maximal subpart, not two.
TEST(testATruncatedThreeByteSequenceIsOneReplacement)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, std::string_view("\xE2\x82.", 3));
    CHECK(replacementCount(t) == 1);
}

// An overlong encoding is rejected at its lead — 0xC0 can never begin a
// well-formed sequence — so the continuation that follows is a maximal subpart
// of its own. Two bytes, two subparts, two replacements.
TEST(testAnOverlongEncodingIsTwoReplacements)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, std::string_view("\xC0\xAF", 2));
    CHECK(replacementCount(t) == 2);
}

// A control byte cannot continue a sequence either. The replacement is owed
// before the control acts, so the cells stay in the order the stream described.
TEST(testAControlByteTerminatesATruncatedSequence)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, std::string_view("\xC3\r\n", 3));
    CHECK(replacementCount(t) == 1);
}

// An escape sequence likewise: the truncated character is owed its replacement
// before the sequence is parsed, and the sequence still parses.
TEST(testAnEscapeTerminatesATruncatedSequence)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, std::string_view("\xE2\x82\033[31mX", 8));
    CHECK(replacementCount(t) == 1);
}

// A surrogate is ill-formed from its second byte on: ED may be followed only by
// 80..9F. Unicode 3.9.6 (Table 3-9) therefore substitutes the lead alone, then
// each byte after it, and the text that follows is not swallowed.
TEST(testASurrogateIsOneReplacementPerByte)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "A\xED\xA0\x80"
            "B"); // U+D800
    CHECK(replacementCount(t) == 3);
    CHECK(Ubiquitty::Screen::visibleCell(&t, 0, 0, 4)->codePoint == 'B');
    CHECK(t.screens.active->cursorX == 5);
}

// A non-shortest form is ill-formed from its second byte on (Table 3-8): E0 may
// be followed only by A0..BF, F0 by 90..BF.
TEST(testANonShortestFormIsOneReplacementPerByte)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "A\xE0\x80\xBF"
            "B"); // overlong U+003F
    CHECK(replacementCount(t) == 3);
    CHECK(Ubiquitty::Screen::visibleCell(&t, 0, 0, 4)->codePoint == 'B');
    Ubiquitty::hardReset(t);
    feed(t, "\xF0\x80\x80\xAF"); // overlong U+002F in four bytes
    CHECK(replacementCount(t) == 4);
    CHECK(t.screens.active->cursorX == 4);
}

// A value past U+10FFFF is ill-formed from the byte that exceeds F4's limit.
TEST(testAValuePastTheLastScalarIsOneReplacementPerByte)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\xF4\x90\xBF\xBF");
    CHECK(replacementCount(t) == 4);
    CHECK(t.screens.active->cursorX == 4);
}

// The bytes either side of each limit are well formed, so the new bounds refuse
// no character that was valid.
TEST(testTheBoundsOfEachRestrictedLeadStayValid)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\xE0\xA0\x80\xED\x9F\xBF\xF0\x90\x80\x80\xF4\x8F\xBF\xBF");
    CHECK(replacementCount(t) == 0);
    CHECK(Ubiquitty::Screen::visibleCell(&t, 0, 0, 0)->codePoint == 0x0800);
    CHECK(Ubiquitty::Screen::visibleCell(&t, 0, 0, 1)->codePoint == 0xD7FF);
    CHECK(Ubiquitty::Screen::visibleCell(&t, 0, 0, 2)->codePoint == 0x10000);
    CHECK(Ubiquitty::Screen::visibleCell(&t, 0, 0, 3)->codePoint == 0x10FFFF);
}

// A byte that breaks a sequence may begin the next one: after E0 the byte E2
// is not a continuation, so E0 is one replacement and E2 82 AC is the euro sign.
TEST(testTheByteThatBreaksASequenceBeginsTheNextOne)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\xE0\xE2\x82\xAC");
    CHECK(replacementCount(t) == 1);
    CHECK(Ubiquitty::Screen::visibleCell(&t, 0, 0, 1)->codePoint == 0x20AC);
}

// The valid case, so none of the above is passing because everything emits a
// replacement: a well-formed sequence emits its character and nothing else.
TEST(testAValidSequenceEmitsNoReplacement)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\xE2\x82\xAC"); // U+20AC EURO SIGN
    CHECK(replacementCount(t) == 0);
    const Ubiquitty::Cell* cell = Ubiquitty::Screen::visibleCell(&t, 0, 0, 0);
    CHECK(cell != nullptr && cell->codePoint == 0x20AC);
}

UBIQUITTY_TEST_MAIN()
