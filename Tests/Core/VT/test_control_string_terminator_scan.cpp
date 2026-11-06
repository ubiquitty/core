// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// A control string ends where the scan says it ends.
//
// The payload scan reads eight bytes at a time and falls back to a byte walk on
// any word that could hold a terminator. That is a claim about every alignment
// of every terminator, and about payload bytes that look like one to the word
// test and are not — so it is held over all of them rather than at a few
// offsets, which is where a word-at-a-time scan goes wrong.
//
// The observable is the window title: OSC 0 stores its payload verbatim, so a
// scan that stopped one byte early or late shows up as a title of the wrong
// length.

#include "Assertions.h"
#include "Harness.h"
#include "TerminalFixture.h"

#include "Core/Modes/Emulation.h"
#include "Core/Screen/Addressing.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "Core/Unicode/Encodings.h"
#include "Core/VT/ControlStringScan.h"
#include "Core/VT/Parser.h"

#include <cstdint>
#include <cstdio>
#include <string>

namespace {

// Well past two words on either side of the alignment boundary, so every offset
// of a terminator within a word is covered along with the tail the word loop
// cannot reach.
constexpr int maxPayloadLength = 24;

// C1 ST ends a control string only where the input is not UTF-8, which is what
// selects the conditional arm of the word test. The encoding is set directly:
// the gate is the parser's, and reaching it through DOCS would put a second
// question — whether that sequence is honoured here — inside this one.
// The encoding under which 0x9C ends a control string, and one under which it
// is a payload byte.
constexpr Ubiquitty::Unicode::Encoding c1Terminating = Ubiquitty::Unicode::Encoding::Latin1;
constexpr Ubiquitty::Unicode::Encoding c1AsPayload = Ubiquitty::Unicode::Encoding::UTF8;

std::string titleAfter(const std::string& stream, Ubiquitty::Unicode::Encoding encoding)
{
    Ubiquitty::Terminal t;
    Ubiquitty::hardReset(t);
    Ubiquitty::Modes::setInputEncoding(&t, encoding);
    Ubiquitty::processBytes(&t, stream);
    return t.titleUTF8;
}

// How much filler survived, which says where the scan stopped without depending
// on how the encoding rendered the bytes around it.
int fillerIn(const std::string& title)
{
    int count = 0;
    for (const char c : title) {
        if (c == 'A')
            ++count;
    }
    return count;
}

} // namespace

TEST(aControlStringEndsAtEveryTerminatorAtEveryOffset)
{
    // BEL and the string terminators end a control string and hand its payload
    // on. The scan's claim is where they are found, which the stored payload
    // reports.
    struct TerminatorCase {
        const char* name;
        const char* bytes;
        bool needsC1;
    };
    const TerminatorCase dispatching[] = {
        { "BEL", "\x07", false },
        { "ESC ST", "\x1B\\", false },
        { "C1 ST", "\x9C", true },
    };

    int cases = 0;
    for (const TerminatorCase& terminator : dispatching) {
        for (int payloadLength = 0; payloadLength < maxPayloadLength; ++payloadLength) {
            const std::string payload(static_cast<std::size_t>(payloadLength), 'A');
            const std::string stream = "\033]0;" + payload + terminator.bytes;
            char what[64];
            std::snprintf(what, sizeof(what), "%s at payload length %d", terminator.name, payloadLength);
            CHECK_MSG(fillerIn(titleAfter(stream, c1Terminating)) == payloadLength, what);
            if (!terminator.needsC1)
                CHECK_MSG(fillerIn(titleAfter(stream, c1AsPayload)) == payloadLength, what);
            ++cases;
        }
    }
    CHECK_EQ(cases, 3 * maxPayloadLength);
}

TEST(anAbandoningTerminatorEndsTheStringWhereItSits)
{
    // 0x18 (CAN) and 0x1A (SUB) end the scan and abandon the string rather than
    // handing its payload on, so where they were found is reported by what comes after
    // them: the next byte is text, after the error character SUB draws. A scan that ran past one would swallow that
    // byte into a payload nothing ever dispatches, and the screen stays empty.
    struct AbandoningCase {
        const char* name;
        char byte;
    };
    const AbandoningCase abandoning[] = { { "CAN", '\x18' }, { "SUB", '\x1A' } };

    int cases = 0;
    for (const AbandoningCase& terminator : abandoning) {
        for (int payloadLength = 0; payloadLength < maxPayloadLength; ++payloadLength) {
            std::string stream = "\033]0;";
            stream.append(static_cast<std::size_t>(payloadLength), 'A');
            stream += terminator.byte;
            stream += 'Z';

            Ubiquitty::Terminal t;
            Ubiquitty::hardReset(t);
            Ubiquitty::processBytes(&t, stream);
            char what[64];
            std::snprintf(what, sizeof(what), "%s at payload length %d", terminator.name, payloadLength);
            CHECK_MSG(t.titleUTF8.empty(), what);
            const int textColumn = terminator.byte == '\x1A' ? 1 : 0;
            if (textColumn == 1) {
                const Ubiquitty::Cell* drawn = Ubiquitty::Screen::visibleCell(&t, 0, 0, 0);
                CHECK_MSG(drawn != nullptr && drawn->codePoint == Ubiquitty::VT::substituteCharacter, what);
            }
            const Ubiquitty::Cell* first = Ubiquitty::Screen::visibleCell(&t, 0, 0, textColumn);
            CHECK_MSG(first != nullptr && first->codePoint == 'Z', what);
            ++cases;
        }
    }
    CHECK_EQ(cases, 2 * maxPayloadLength);
}

TEST(aPayloadByteThatIsNotATerminatorDoesNotEndTheString)
{
    // 0x1F sits below 0x20, so the word test says a word holding it may hold a
    // terminator — and it is not one. A scan that stopped there truncates the
    // title; one that skipped the whole word past it runs off the end.
    for (int payloadLength = 1; payloadLength < maxPayloadLength; ++payloadLength) {
        for (int position = 0; position < payloadLength; ++position) {
            std::string payload(static_cast<std::size_t>(payloadLength), 'A');
            payload[static_cast<std::size_t>(position)] = '\x1F';
            const std::string stream = "\033]0;" + payload + '\x07';
            const std::string title = titleAfter(stream, c1Terminating);
            CHECK_MSG(fillerIn(title) == payloadLength - 1,
                "a byte below 0x20 that terminates nothing cut the payload short");
        }
    }
}

// The word scan against a byte walk, over the whole space the
// claim covers rather than a corpus of it: every terminator, at every offset of
// a buffer long enough to hold three words and a tail, with C1 termination on
// and off, and with payload bytes chosen to be the ones a word test can confuse
// for a terminator.
TEST(theWordScanFindsWhatAByteWalkFinds)
{
    // A byte walk, kept as the loop the three appenders each ran.
    const auto referenceIndex = [](const char* buffer, int start, int length, bool doesC1Terminate) {
        int i = start;
        while (i < length) {
            const std::uint8_t b = static_cast<std::uint8_t>(buffer[i]);
            if (b == 0x07 || b == 0x1B || b == 0x18 || b == 0x1A || (b == 0x9C && doesC1Terminate))
                break;
            ++i;
        }
        return i;
    };

    // Payload bytes a word test could take for a terminator: the space just
    // above the C0 range, the byte one below the C1 terminator and one above,
    // and ordinary printable content.
    const char fillers[] = { 'A', '\x1F', '\x20', '\x9B', '\x9D', '\x7F' };
    const char terminators[] = { '\x07', '\x18', '\x1A', '\x1B', '\x9C' };
    constexpr int bufferLength = 27; // three words and a three-byte tail

    int comparisons = 0;
    int scansThatFoundNothing = 0;
    for (const char filler : fillers) {
        for (const char terminator : terminators) {
            for (int position = 0; position <= bufferLength; ++position) {
                for (int start = 0; start < 3; ++start) {
                    std::string buffer(static_cast<std::size_t>(bufferLength), filler);
                    // position == bufferLength leaves the buffer without one,
                    // which is the case that must run off the end and say so.
                    if (position < bufferLength)
                        buffer[static_cast<std::size_t>(position)] = terminator;
                    for (const bool doesC1Terminate : { false, true }) {
                        const int expected = referenceIndex(buffer.data(), start, bufferLength, doesC1Terminate);
                        const int actual = Ubiquitty::VT::controlStringTerminatorIndex(
                            buffer.data(), start, bufferLength, doesC1Terminate);
                        CHECK_MSG(expected == actual, "the word scan and the byte walk disagree");
                        if (actual == bufferLength)
                            ++scansThatFoundNothing;
                        ++comparisons;
                    }
                }
            }
        }
    }

    // Guards against a corpus that compares nothing: the walk must run, and it
    // must reach both answers — a space where every scan finds a terminator
    // never exercises the word loop's full stride.
    CHECK_EQ(comparisons, 6 * 5 * (bufferLength + 1) * 3 * 2);
    CHECK_MSG(scansThatFoundNothing > 0, "no case ran off the end, so the terminator-free path went unread");
}

TEST(aWordTheScanRejectsHoldsNoTerminator)
{
    // The word test's one-sided claim, held over every byte value in every lane
    // of a word: rejecting a word that holds a terminator would skip it, and
    // that is the only way this scan can lose one.
    int words = 0;
    for (int lane = 0; lane < 8; ++lane) {
        for (int value = 0; value < 256; ++value) {
            std::uint64_t word = 0x4141414141414141ULL; // printable filler
            word &= ~(static_cast<std::uint64_t>(0xFF) << (lane * 8));
            word |= static_cast<std::uint64_t>(value) << (lane * 8);
            for (const bool doesC1Terminate : { false, true }) {
                const bool mayHold = Ubiquitty::VT::mayHoldControlStringTerminator(word, doesC1Terminate);
                const bool doesHold = Ubiquitty::VT::isControlStringTerminator(
                    static_cast<std::uint8_t>(value), doesC1Terminate);
                CHECK_MSG(!doesHold || mayHold, "a word holding a terminator was rejected");
                ++words;
            }
        }
    }
    CHECK_EQ(words, 8 * 256 * 2);
}

UBIQUITTY_TEST_MAIN()
