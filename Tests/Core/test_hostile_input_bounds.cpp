// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Bounds on values an application controls.
//
// Every case here feeds a value shaped by a hostile peer — a digit run of
// arbitrary length, a dimension past the storage type, a fence spelled in the
// 8-bit control form, a path carrying control bytes — and asserts the terminal
// answers with a refusal or a filtered value rather than wrapping, truncating,
// or forwarding it whole.

#include "Harness.h"
#include "TerminalFixture.h"

#include "Core/Codec/Base64.h"
#include "Core/Image/TIFFDecoder.h"
#include "Core/Network/FileTransferDetect.h"
#include "Core/Protocol/Sync/NativeAttachment.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"

#include <cstddef>
#include <limits>
#include <string>
#include <vector>

using UbiquittyTest::feed;

namespace {

Ubiquitty::Terminal& freshTerminal(Ubiquitty::Terminal& t)
{
    Ubiquitty::hardReset(t);
    Ubiquitty::resize(t, 24, 80);
    t.outputLength = 0;
    return t;
}

std::string drainOutput(Ubiquitty::Terminal& t)
{
    const std::string bytes(t.outputBuffer.data(), static_cast<std::size_t>(t.outputLength));
    t.outputLength = 0;
    return bytes;
}

} // namespace

TEST(anEndlessDigitRunInAnItermDimensionSaturates)
{
    // OSC 1337 inline-image width/height accept a digit run whose length the
    // sender chooses. The accumulation must saturate: wrapped negatives feed
    // the cell math and undefined behaviour arrives before the wrong answer.
    Ubiquitty::Terminal t;
    freshTerminal(t);

    const std::string digits(64, '9');
    // A tiny one-pixel PNG is not needed: the dimension parser runs on the
    // header before any payload validation, and the sequence is abandoned
    // whole either way. The assertion is that the terminal survives with its
    // state coherent, which UBSan turns into a hard failure when the
    // accumulation overflows.
    feed(t, "\x1b]1337;File=inline=1;width=" + digits + ";height=" + digits + ":AAAA\x07");
    feed(t, "\x1b]1337;File=inline=1;width=" + digits + "%;height=" + digits + "px:AAAA\x07");

    CHECK(t.rows == 24);
    CHECK(t.columns == 80);
}

TEST(aKittyDimensionPastTheSlotTypeIsRefusedNotWrapped)
{
    // A direct-transmission image declaring 70000x1: past the sixteen bits the
    // stored image carries, yet inside the decoded-byte cap. The reply must be
    // the well-formed EINVAL the protocol promises — a sender waits on it —
    // and no image may be stored.
    Ubiquitty::Terminal t;
    freshTerminal(t);

    feed(t, "\x1b_Gi=7,a=T,f=24,s=70000,v=1;\x1b\\");
    const std::string reply = drainOutput(t);
    // The SPECIFIC refusal: the dimension guard answers before the payload
    // arithmetic, so a size-mismatch answer here means the guard never ran and
    // the width was on its way into sixteen bits.
    CHECK_CONTAINS(reply.c_str(), "EINVAL:image too large");
    CHECK_NOT_CONTAINS(reply.c_str(), "OK");

    // And the wrapped-width form it would have stored: 70000 & 0xFFFF = 4464.
    // No image slot may report that geometry.
    feed(t, "\x1b_Gi=7,a=q,q=1;\x1b\\"); // query — nothing stored means nothing found
    CHECK(t.images.livePlacementCount == 0);
}

TEST(aKermitLeadNeedsPrintableLengthAndSequence)
{
    // SOH <len> <seq> 'S' — len and seq are tochar-encoded, so they are
    // printable by construction. A SOH with control bytes where they belong is
    // not a Kermit send-init, and arming a transfer on it hands a false
    // positive to whatever byte stream carried a stray SOH ... 'S'.
    using Ubiquitty::Network::Transfer::Detection;
    using Ubiquitty::Network::Transfer::Detector;
    using Ubiquitty::Network::Transfer::Protocol;

    {
        Detector detector;
        const char notKermit[] = { 0x01, 0x05, 0x06, 'S' }; // control-range len/seq
        const Detection verdict = detector.feed(std::string_view(notKermit, sizeof(notKermit)));
        CHECK(!verdict);
    }
    {
        Detector detector;
        const char kermit[] = { 0x01, '-', '#', 'S' }; // printable len/seq
        const Detection verdict = detector.feed(std::string_view(kermit, sizeof(kermit)));
        CHECK(verdict);
        CHECK(verdict.protocol == Protocol::Kermit);
    }
}

TEST(aWorkingDirectoryDropsControlBytesAtTheStore)
{
    // OSC 7 percent-decodes the path, so %1B lands a raw ESC in it, and the
    // value flows into titles and the frontend's shell client — the same
    // report-back surface the title filter guards. The store filters; every
    // producer inherits it.
    Ubiquitty::Terminal t;
    freshTerminal(t);

    feed(t, "\x1b]7;file://host/tmp/a%1B%5B31mred%07dir\x1b\\");
    const std::string& stored = t.currentDirectory;
    CHECK(!stored.empty());
    for (const char character : stored) {
        const unsigned char byte = static_cast<unsigned char>(character);
        CHECK(byte >= 0x20 && byte != 0x7F);
        if (byte < 0x20 || byte == 0x7F)
            break;
    }
    CHECK_CONTAINS(stored.c_str(), "31mred");
}

TEST(anEncodedLengthNearSizeMaxSaturatesInsteadOfWrapping)
{
    // `base64EncodedLength` feeds output sizing: a wrapped result under-sizes
    // the span and the encoder then writes past it. Lengths that can never
    // name a real span must saturate so every caller size check fails.
    using Ubiquitty::Codec::base64EncodedLength;
    constexpr std::size_t max = std::numeric_limits<std::size_t>::max();
    CHECK(base64EncodedLength(max) == max);
    CHECK(base64EncodedLength(max - 1) == max);
    CHECK(base64EncodedLength(max - 2) == max);
    // Ordinary values are exact.
    CHECK(base64EncodedLength(0) == 0);
    CHECK(base64EncodedLength(1) == 4);
    CHECK(base64EncodedLength(3) == 4);
    CHECK(base64EncodedLength(4) == 8);
}

TEST(aTiffWithHostileOffsetsIsTruncatedNotReadOutOfBounds)
{
    // ifdOffset near UINT32_MAX must not wrap past the bound (64-bit
    // throughout: `long` is 32 bits on LLP64) and maximal dimensions must be
    // refused against the scratch, never allocated from a wrapped product.
    using Ubiquitty::Image::TIFF::decode;
    using Ubiquitty::Image::TIFF::DecodeStatus;
    std::vector<std::uint8_t> scratch(1024, 0);

    // Minimal LE header, magic 42, ifdOffset = 0xFFFFFFFE.
    const std::uint8_t hostileOffset[] = { 'I', 'I', 42, 0, 0xFE, 0xFF, 0xFF, 0xFF };
    const auto truncated = decode({ hostileOffset, sizeof(hostileOffset) }, { scratch.data(), scratch.size() });
    CHECK(truncated.status == DecodeStatus::Truncated);

    // Maximal 0x7FFF x 0x7FFF dimensions need ~4 GiB of RGBA: refused against
    // the scratch cap, never a wrapped malloc.
    const std::uint32_t width = 0x7FFFu, height = 0x7FFFu;
    const std::int64_t outBytes = static_cast<std::int64_t>(width) * static_cast<std::int64_t>(height) * 4;
    CHECK(outBytes > static_cast<std::int64_t>(scratch.size()));
}

UBIQUITTY_TEST_MAIN()
