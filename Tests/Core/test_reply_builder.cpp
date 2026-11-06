// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// ReplyBuilder assembles every host-facing answer, and its frame is fixed. The
// rule that matters is what happens at the edge of that frame: a reply which
// does not fit must be refused, not sent short, because the host reads whatever
// follows a partial escape sequence as part of it.

#include "Harness.h"

#include "Core/ReplyBuilder.h"

#include <array>
#include <limits>
#include <string>

namespace {

std::string bytesOf(const Ubiquitty::ReplyBuilder& reply)
{
    return std::string(reply.span().data(), reply.span().size());
}

// The frame, read off the class rather than repeated here.
int frameSize()
{
    Ubiquitty::ReplyBuilder reply;
    for (int i = 0; i < 4096; ++i)
        reply.put('x');
    return reply.length();
}

} // namespace

TEST(theBuilderWritesWhatItWasGiven)
{
    Ubiquitty::ReplyBuilder reply;
    reply.put("\033[");
    reply.putDecimal(12);
    reply.put(';');
    reply.putDecimal(34);
    reply.put('R');

    CHECK(bytesOf(reply) == "\033[12;34R");
    CHECK(reply.length() == 8);
    CHECK(reply.isSendable());
    CHECK(!reply.isTruncated());
}

TEST(decimalCoversItsRange)
{
    Ubiquitty::ReplyBuilder reply;
    reply.putDecimal(0);
    reply.put(',');
    reply.putDecimal(7);
    reply.put(',');
    reply.putDecimal(1024);
    reply.put(',');
    reply.putDecimal(-42);

    CHECK(bytesOf(reply) == "0,7,1024,-42");
}

TEST(decimalRendersTheSmallestInteger)
{
    Ubiquitty::ReplyBuilder reply;
    reply.putDecimal(std::numeric_limits<int>::min());
    CHECK(bytesOf(reply) == std::to_string(std::numeric_limits<int>::min()));
    CHECK(reply.isSendable());
    CHECK(!reply.isTruncated());
}

TEST(decimalMatchesIndependentFormattingAcrossSignsAndDigitBoundaries)
{
    for (int value = -32768; value <= 32768; ++value) {
        Ubiquitty::ReplyBuilder reply;
        reply.putDecimal(value);
        CHECK(bytesOf(reply) == std::to_string(value));
        CHECK(reply.isSendable());
    }
    constexpr std::array values { std::numeric_limits<int>::min() + 1,
        -1000000000, -999999999, 999999999, 1000000000, std::numeric_limits<int>::max() };
    for (int value : values) {
        Ubiquitty::ReplyBuilder reply;
        reply.putDecimal(value);
        CHECK(bytesOf(reply) == std::to_string(value));
        CHECK(reply.isSendable());
    }
}

TEST(emptyViewsDoNotReadADataPointerOrChangeTheFrame)
{
    const int frame = frameSize();
    for (int prefixLength = 0; prefixLength <= frame; ++prefixLength) {
        Ubiquitty::ReplyBuilder reply;
        const std::string prefix(static_cast<std::size_t>(prefixLength), 'p');
        reply.put(std::string_view { prefix });
        reply.put(std::string_view {});
        reply.put("");
        CHECK(bytesOf(reply) == prefix);
        CHECK(!reply.isTruncated());
        CHECK(reply.isSendable() == (prefixLength != 0));
    }
}

TEST(textViewsRespectEveryRemainingFrameBoundary)
{
    const int frame = frameSize();
    for (int prefixLength = 0; prefixLength <= frame; ++prefixLength) {
        for (int runLength = 0; runLength <= frame + 1; ++runLength) {
            Ubiquitty::ReplyBuilder reply;
            const std::string prefix(static_cast<std::size_t>(prefixLength), 'p');
            const std::string run(static_cast<std::size_t>(runLength), 'r');
            reply.put(std::string_view { prefix });
            reply.put(std::string_view { run });
            const bool doesFit = runLength <= frame - prefixLength;
            CHECK(bytesOf(reply) == (doesFit ? prefix + run : prefix));
            CHECK(reply.isTruncated() == !doesFit);
            CHECK(reply.isSendable() == (doesFit && prefixLength + runLength != 0));
        }
    }
}

TEST(literalsRespectEveryRemainingFrameBoundary)
{
    const int frame = frameSize();
    for (int prefixLength = 0; prefixLength <= frame; ++prefixLength) {
        Ubiquitty::ReplyBuilder reply;
        const std::string prefix(static_cast<std::size_t>(prefixLength), 'p');
        reply.put(std::string_view { prefix });
        reply.put("reply");
        const bool doesFit = 5 <= frame - prefixLength;
        CHECK(bytesOf(reply) == (doesFit ? prefix + "reply" : prefix));
        CHECK(reply.isTruncated() == !doesFit);
        CHECK(reply.isSendable() == doesFit);
    }
}

TEST(decimalPreservesItsPrefixAndRefusesEveryPartialNumber)
{
    const int frame = frameSize();
    constexpr std::array values { std::numeric_limits<int>::min(), -100, -1, 0, 1, 100,
        std::numeric_limits<int>::max() };
    for (int value : values) {
        const std::string number = std::to_string(value);
        for (int prefixLength = 0; prefixLength <= frame; ++prefixLength) {
            Ubiquitty::ReplyBuilder reply;
            const std::string prefix(static_cast<std::size_t>(prefixLength), 'p');
            reply.put(std::string_view { prefix });
            reply.putDecimal(value);
            const auto remaining = static_cast<std::size_t>(frame - prefixLength);
            const bool doesFit = number.size() <= remaining;
            CHECK(bytesOf(reply) == prefix + number.substr(0, remaining));
            CHECK(reply.isTruncated() == !doesFit);
            CHECK(reply.isSendable() == doesFit);
        }
    }
}

TEST(aColourBodyIsFourHexDigitsAPart)
{
    Ubiquitty::ReplyBuilder reply;
    reply.putColor(0x0000, 0x1a2b, 0xffff);
    CHECK(bytesOf(reply) == ";rgb:0000/1a2b/ffff");
}

TEST(aRunFillingTheFrameExactlyIsStillSendable)
{
    const int frame = frameSize();
    Ubiquitty::ReplyBuilder reply;
    const std::string exact(static_cast<std::size_t>(frame), 'a');
    reply.put(std::string_view { exact });

    CHECK(reply.length() == frame);
    CHECK(!reply.isTruncated());
    CHECK(reply.isSendable());
    CHECK(bytesOf(reply) == exact);
}

TEST(aRunPastTheFrameIsRefusedRatherThanSentShort)
{
    // The rule the class exists for. A run that does not fit is dropped whole
    // and the reply is marked unsendable, so no caller emits a partial escape
    // sequence for the host to absorb whatever follows into.
    const int frame = frameSize();
    Ubiquitty::ReplyBuilder reply;
    const std::string tooMuch(static_cast<std::size_t>(frame) + 1, 'a');
    reply.put(std::string_view { tooMuch });

    CHECK(reply.isTruncated());
    CHECK(!reply.isSendable());
    CHECK(reply.length() == 0);
}

TEST(aRunThatWouldOverflowLeavesWhatCameBeforeItAlone)
{
    // Dropping the run whole means the bytes already in the frame are not
    // disturbed — the reply is refused on its truncation flag, not by being
    // half-overwritten.
    const int frame = frameSize();
    Ubiquitty::ReplyBuilder reply;
    reply.put("\033[");
    const std::string tooMuch(static_cast<std::size_t>(frame), 'z');
    reply.put(std::string_view { tooMuch });

    CHECK(reply.isTruncated());
    CHECK(!reply.isSendable());
    CHECK(bytesOf(reply) == "\033[");
}

TEST(aByteAtTheEdgeIsTakenOrRefusedOneAtATime)
{
    const int frame = frameSize();
    Ubiquitty::ReplyBuilder reply;
    for (int i = 0; i < frame; ++i)
        reply.put('x');
    CHECK(!reply.isTruncated());
    CHECK(reply.isSendable());

    reply.put('y');
    CHECK(reply.isTruncated());
    CHECK(!reply.isSendable());
    CHECK(reply.length() == frame);
}

TEST(resetStartsTheReplyOver)
{
    Ubiquitty::ReplyBuilder reply;
    reply.put("\033P1$r");
    reply.put("0;1;4m");
    CHECK(reply.length() == 11);

    reply.reset();
    CHECK(reply.length() == 0);
    CHECK(!reply.isSendable());

    reply.put("\033P0$r");
    reply.put("\033\\");
    CHECK(bytesOf(reply) == "\033P0$r\033\\");
    CHECK(reply.isSendable());
}

TEST(resetClearsTheTruncationTheOverflowRecorded)
{
    // A reply that gave up on its body starts the miss over in the same frame,
    // so the truncation that made it give up must not follow it there.
    const int frame = frameSize();
    Ubiquitty::ReplyBuilder reply;
    const std::string tooMuch(static_cast<std::size_t>(frame) + 1, 'a');
    reply.put(std::string_view { tooMuch });
    CHECK(reply.isTruncated());

    reply.reset();
    reply.put("\033P0$r\033\\");
    CHECK(!reply.isTruncated());
    CHECK(reply.isSendable());
}

TEST(anEmptyReplyIsNotSendable)
{
    Ubiquitty::ReplyBuilder reply;
    CHECK(!reply.isSendable());
    CHECK(!reply.isTruncated());
    CHECK(reply.length() == 0);
}

UBIQUITTY_TEST_MAIN()
