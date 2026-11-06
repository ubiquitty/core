// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// What `Detector::feed` reports does not depend on how its input is chunked, nor
// on how it arranges the window it scans.
//
// It runs on the ingest path, once per ring bite, over every byte the terminal
// receives, and it reads input an attacker supplies. Any cheaper arrangement of
// its scan owes the same answer as joining the carry to the whole burst and
// scanning that, so the reference below is that arrangement, held against the
// live one rather than against a description of it.
//
// Chunking is where the risk sits. A preamble split across a bite is what the
// carry exists for, so each stream is fed at every chunk size that can split one,
// and both the detection the two report and the chunk they report it on are
// compared — a detector that agrees on the verdict but fires a bite late has
// still changed when a transfer is offered.

#include "Assertions.h"
#include "Harness.h"

#include "Core/Network/FileTransferDetect.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace Ubiquitty::Network::Transfer;

namespace {

// The joined-window detector, written out here rather than called through the
// production entry point. A reference that shares the code under test cannot
// fail with it: these matchers are the rules as they stand — protocol order over
// the whole window, only the first ZMODEM lead considered, the direction reading
// everything before the header — expressed over one contiguous string.
struct JoiningDetector {
    std::string carry;

    static Detection zmodem(std::string_view window)
    {
        const std::string_view lead = "**\x18";
        const std::size_t at = window.find(lead);
        if (at == std::string_view::npos)
            return {};
        const std::size_t formatIndex = at + lead.size();
        if (formatIndex >= window.size())
            return {};
        const char format = window[formatIndex];
        if (format != '\x41' && format != '\x42' && format != '\x43')
            return {};
        Detection detection;
        detection.protocol = Protocol::ZModem;
        const std::string_view before = window.substr(0, at);
        detection.direction = (before.find("rz\r") != std::string_view::npos
                                  || before.find("rz\n") != std::string_view::npos)
            ? Direction::Receive
            : Direction::Send;
        return detection;
    }

    static Detection kermit(std::string_view window)
    {
        for (std::size_t i = 0; i + 3 < window.size(); ++i) {
            if (window[i] != 0x01)
                continue;
            // len and seq are tochar-encoded, printable by construction — the
            // same rule the production matcher states, spelled independently.
            const unsigned char length = static_cast<unsigned char>(window[i + 1]);
            const unsigned char sequence = static_cast<unsigned char>(window[i + 2]);
            if (length < 0x20 || length > 0x7E || sequence < 0x20 || sequence > 0x7E)
                continue;
            if (window[i + 3] == 'S') {
                Detection detection;
                detection.protocol = Protocol::Kermit;
                detection.direction = Direction::Receive;
                return detection;
            }
        }
        return {};
    }

    Detection feed(std::string_view bytes)
    {
        if (bytes.empty())
            return {};
        std::string window;
        window.reserve(carry.size() + bytes.size());
        window.append(carry);
        window.append(bytes.data(), bytes.size());

        Detection detection = zmodem(window);
        if (!detection)
            detection = kermit(window);
        if (detection) {
            carry.clear();
            return detection;
        }
        if (window.size() > maxPreambleScan)
            carry.assign(window.end() - static_cast<std::ptrdiff_t>(maxPreambleScan), window.end());
        else
            carry = std::move(window);
        return {};
    }
};

// What a caller can observe of a run: which chunk fired, and what it said.
struct Outcome {
    int firedAtChunk = -1;
    int protocol = -1;
    int direction = -1;
};

template<class D>
Outcome runStream(D& detector, const std::string& stream, std::size_t chunk)
{
    Outcome outcome;
    int index = 0;
    for (std::size_t offset = 0; offset < stream.size(); offset += chunk, ++index) {
        const std::size_t count = std::min(chunk, stream.size() - offset);
        const Detection detection = detector.feed(std::string_view(stream.data() + offset, count));
        if (detection) {
            outcome.firedAtChunk = index;
            outcome.protocol = static_cast<int>(detection.protocol);
            outcome.direction = static_cast<int>(detection.direction);
            return outcome;
        }
    }
    return outcome;
}

// xorshift32: deterministic, so a failure reproduces from the seed.
struct Sequence {
    std::uint32_t state;
    std::uint32_t next()
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    }
    int upTo(int bound) { return bound <= 0 ? 0 : static_cast<int>(next() % static_cast<std::uint32_t>(bound)); }
};

// Filler that is not itself a preamble, with the bytes the matchers hunt for
// (the start-of-header byte, '*', cancel, 'r', 'z') present often enough to
// exercise their near-misses.
std::string filler(Sequence& sequence, int length)
{
    static const char alphabet[] = { 'a', ' ', 'r', 'z', '*', '\x18', '\x01', 'S', '\r', 'x' };
    std::string out;
    out.reserve(static_cast<std::size_t>(length));
    for (int i = 0; i < length; ++i)
        out.push_back(alphabet[sequence.upTo(static_cast<int>(sizeof(alphabet)))]);
    return out;
}

} // namespace

TEST(theInPlaceDetectorSeesWhatTheJoiningOneSaw)
{
    Sequence sequence { 0xC2B2AE35u };
    int cases = 0;
    int firedCases = 0;

    static const char* const preambles[] = {
        "**\x18"
        "B00000000000000", // ZMODEM, ZHEX
        "**\x18"
        "A0", // ZMODEM, ZBIN
        "**\x18"
        "C0", // ZMODEM, ZBIN32
        "\x01\x23\x20S", // Kermit send-init
        "**\x18Q", // a lead with no real format byte: not a preamble
        "", // nothing to find
    };

    for (int iteration = 0; iteration < 400; ++iteration) {
        const char* preamble = preambles[sequence.upTo(6)];
        const bool isReceiveAnnounced = (sequence.next() & 1u) != 0;

        std::string stream = filler(sequence, sequence.upTo(300));
        if (isReceiveAnnounced)
            stream += "rz\r";
        stream += filler(sequence, sequence.upTo(120));
        stream += preamble;
        stream += filler(sequence, sequence.upTo(200));

        // Every chunk size that can split the carry window, plus a few larger.
        for (std::size_t chunk : { 1u, 2u, 3u, 7u, 16u, 31u, 63u, 64u, 65u, 127u, 256u, 1024u }) {
            Detector inPlace;
            JoiningDetector joining;
            const Outcome a = runStream(inPlace, stream, chunk);
            const Outcome b = runStream(joining, stream, chunk);
            CHECK_EQ(a.firedAtChunk, b.firedAtChunk);
            CHECK_EQ(a.protocol, b.protocol);
            CHECK_EQ(a.direction, b.direction);
            if (a.firedAtChunk >= 0)
                ++firedCases;
            ++cases;
        }
    }

    // Guards a corpus that compares nothing: the runs must happen, and enough of
    // them must actually detect something for the agreement to mean anything.
    CHECK_EQ(cases, 4800);
    CHECK(firedCases > 1500);
}

UBIQUITTY_TEST_MAIN()
