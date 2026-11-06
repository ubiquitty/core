// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Two parser controls the byte stream can carry inside text.
//
// DEL is never a final byte: DEC STD 070 3.3.4 treats it as a control
// character with no function, ignored on receipt, so it cannot end an escape
// sequence or be taken for the character an intermediate asks for.
//
// U+0085, received as UTF-8, is NEL: ECMA-48 8.3.86 represents it as 08/05 and
// Unicode 5.8 makes it a newline function. The other C1 code points are ignored
// in a UTF-8 stream, so text that merely contains one cannot start a sequence.

#include "VT100Fixture.h"

#include <string_view>

// DEL between ESC and its final byte leaves the sequence waiting for the final.
TEST(testDeleteBeforeTheFinalByteIsIgnored)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "A\033\x7F"
            "DX"); // ESC DEL D is IND
    CHECK(cell(t, 0, 0)->codePoint == 'A');
    CHECK(cell(t, 1, 1)->codePoint == 'X');
    CHECK(t.screens.active->cursorY == 1);
}

// DEL after an intermediate is ignored too, for the generic intermediate state
// and the character-set designation alike.
TEST(testDeleteAfterAnIntermediateIsIgnored)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033(\x7F"
            "0q"); // ESC ( DEL 0 selects DEC graphics
    CHECK(cell(t, 0, 0)->codePoint == 0x2500u);
    CHECK(t.screens.active->cursorX == 1);

    Ubiquitty::hardReset(t);
    feed(t, "\033#\x7F"
            "8"); // ESC # DEL 8 is DECALN
    CHECK(cell(t, 0, 0)->codePoint == 'E');

    Ubiquitty::hardReset(t);
    feed(t, "\033!\x7F"
            "XI"); // an unassigned two-byte sequence, then text
    CHECK(cell(t, 0, 0)->codePoint == 'I');
}

// DEL does not stand in for the final byte of an escape sequence.
TEST(testDeleteDoesNotEndAnEscapeSequence)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "\033\x7F\x7F[2;3HX"); // ESC DEL DEL [ is still CSI
    CHECK(cell(t, 1, 2)->codePoint == 'X');
}

// U+0085 as UTF-8 moves to the start of the next line, from any column.
TEST(testDecodedNextLineMovesToTheNextLine)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "AB\xC2\x85"
            "C");
    CHECK(cell(t, 0, 0)->codePoint == 'A');
    CHECK(cell(t, 0, 1)->codePoint == 'B');
    CHECK(cell(t, 0, 2)->codePoint == 0);
    CHECK(cell(t, 1, 0)->codePoint == 'C');
    CHECK(t.screens.active->cursorY == 1);
    CHECK(t.screens.active->cursorX == 1);
}

// Split across writes it means the same.
TEST(testDecodedNextLineAcrossWrites)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "AB\xC2");
    feed(t, "\x85"
            "C");
    CHECK(cell(t, 1, 0)->codePoint == 'C');
    CHECK(t.screens.active->cursorY == 1);
}

// A raw 0x85 byte is not UTF-8: it is substituted, not performed.
TEST(testARawC1ByteIsNotNextLine)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "AB\x85"
            "C");
    CHECK(t.screens.active->cursorY == 0);
    CHECK(cell(t, 0, 2)->codePoint == 0xFFFD);
}

// A full reset partway through a write leaves the rest of the write UTF-8: a
// raw 0x9D after RIS is a stray byte, substituted like any other, not an OSC
// introducer that swallows what follows it. The write is split two ways because
// the next write starts UTF-8 afresh, which is how the two ever disagreed.
TEST(testARawC1ByteAfterAResetInTheSameWriteIsNotAControl)
{
    const std::string_view bytes = "\x1B"
                                   "c\x9D"
                                   "o?";
    for (const std::size_t chunk : { bytes.size(), std::size_t { 1 } }) {
        Ubiquitty::Terminal t;
        Ubiquitty::hardReset(t);
        for (std::size_t offset = 0; offset < bytes.size(); offset += chunk)
            Ubiquitty::processBytes(&t, bytes.substr(offset, chunk));
        CHECK(cell(t, 0, 0)->codePoint == 0xFFFD);
        CHECK(cell(t, 0, 1)->codePoint == 'o');
        CHECK(cell(t, 0, 2)->codePoint == '?');
        CHECK(t.screens.active->cursorX == 3);
    }
}

// The other C1 code points stay ignored: U+009B is not CSI and U+0084 is not IND.
TEST(testOtherDecodedC1CodePointsAreIgnored)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    feed(t, "A\xC2\x9B"
            "2;2H");
    CHECK(t.screens.active->cursorY == 0);
    CHECK(cell(t, 0, 1)->codePoint == '2');

    Ubiquitty::hardReset(t);
    feed(t, "A\xC2\x84"
            "B");
    CHECK(t.screens.active->cursorY == 0);
    CHECK(cell(t, 0, 1)->codePoint == 'B');
}

// Inside a long run of multibyte text the batched writer hands U+0085 to the
// per-character path, and the text on either side lands on the right lines.
TEST(testDecodedNextLineInsideAMultibyteRun)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    std::string input;
    for (int i = 0; i < 3; ++i)
        input += "\xC3\xA9";
    input += "\xC2\x85";
    for (int i = 0; i < 3; ++i)
        input += "\xC3\xA9";
    feed(t, input);
    CHECK(cell(t, 0, 2)->codePoint == 0xE9u);
    CHECK(cell(t, 0, 3)->codePoint == 0);
    CHECK(cell(t, 1, 0)->codePoint == 0xE9u);
    CHECK(cell(t, 1, 2)->codePoint == 0xE9u);
    CHECK(t.screens.active->cursorY == 1);
}

UBIQUITTY_TEST_MAIN()
