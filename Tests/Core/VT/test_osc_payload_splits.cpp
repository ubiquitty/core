// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// An OSC keeps the same payload however its bytes are split across reads.
//
// How much of an OSC the accumulator keeps is decided by its command: the
// clipboard (52) and iTerm2's images and files (1337) keep up to
// `maxOSCImageSize` bytes, every other command up to `maxOSCNonImageSize`. The
// command is the number the string begins with, and a read can end anywhere in
// it, so each case feeds one stream whole and then cut, and holds what is kept,
// the screen and the replies to the whole stream's.
//
// A cut can only matter where it lands in the number or its separator, or near
// a ceiling. Around the non-image max every such cut is tried, and the cuts
// between them, inside a run of payload, are sampled. A string at the image max
// is 64 MiB, so there a few read patterns each serve the three sizes around it,
// and seeded partitions cover the rest.

#include "Harness.h"
#include "TerminalFixture.h"

#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "Core/VT/Constants.h"
#include "Core/VT/Parser.h"
#include "PAL/TerminalClients.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace UbiquittyTest;

namespace {

struct CapturedClipboard : Ubiquitty::ClipboardClient {
    std::string bytes;
    int writes = 0;

    void writeClipboard(Ubiquitty::ClipboardBuffer, std::span<const char> value) override
    {
        bytes.assign(value.data(), value.size());
        ++writes;
    }
};

/// Counts the accumulator's clip reports, which it makes once per string, when
/// the stored length reaches the command's ceiling.
struct ClipReports : Ubiquitty::DiagnosticsClient {
    int count = 0;
    std::uint32_t lastLength = 0;

    void log(std::string_view tag, std::uint32_t detail) override
    {
        if (tag == "osc-clipped") {
            ++count;
            lastLength = detail;
        }
    }
};

/// What a program can observe after a stream, and the payload of a string the
/// parser is still reading.
struct Observation {
    std::string cells;
    std::string title;
    std::string replies;
    std::string pendingPayload;
    std::string clipboard;
    int clipboardWrites;
    int parserState;
    int row;
    int column;

    friend bool operator==(const Observation&, const Observation&) = default;
};

/// A small terminal whose clipboard records what it is handed and which
/// answers window reports, so OSC 52 and OSC 1337 both have an effect to compare.
class Session {
public:
    Session()
    {
        Ubiquitty::resize(m_terminal, 3, 20, Ubiquitty::Modes::ReflowMode::Fixed);
        Ubiquitty::hardReset(m_terminal);
        m_terminal.clipboardClient = &m_clipboard;
        m_terminal.diagnosticsClient = &m_clipReports;
        m_terminal.isOSC52Allowed = true;
        m_terminal.osc52ClipboardWritePolicy = Ubiquitty::OSC::ClipboardWritePolicy::Allow;
        m_terminal.allowWindowOperations = true;
    }

    void feed(std::string_view bytes) { UbiquittyTest::feed(m_terminal, bytes); }

    /// Feeds `stream` as reads that end at each of `ends`, the last being the
    /// stream's length.
    void feedReads(std::string_view stream, std::span<const std::size_t> ends)
    {
        std::size_t offset = 0;
        for (const std::size_t end : ends) {
            feed(stream.substr(offset, end - offset));
            offset = end;
        }
    }

    Observation observe()
    {
        using State = Ubiquitty::VT::Parser::State;
        const auto state = m_terminal.parser.parseState;
        Observation result {
            .cells = {},
            .title = std::string(m_terminal.titleUTF8.view()),
            .replies = {},
            .pendingPayload = {},
            .clipboard = m_clipboard.bytes,
            .clipboardWrites = m_clipboard.writes,
            .parserState = static_cast<int>(state),
            .row = m_terminal.screens.active->cursorY,
            .column = m_terminal.screens.active->cursorX,
        };
        for (int row = 0; row < m_terminal.rows; ++row) {
            for (int column = 0; column < m_terminal.columns; ++column) {
                const auto* value = cell(m_terminal, row, column);
                result.cells.append(reinterpret_cast<const char*>(value), sizeof(*value));
            }
        }
        std::array<char, 4096> output;
        for (int count = drain(m_terminal, output); count; count = drain(m_terminal, output))
            result.replies.append(output.data(), static_cast<std::size_t>(count));
        if ((state == State::OSC || state == State::OSCEscape) && m_terminal.parser.osc.length)
            result.pendingPayload.assign(m_terminal.parser.osc.data, static_cast<std::size_t>(m_terminal.parser.osc.length));
        return result;
    }

    [[nodiscard]] int accumulatorCapacity() const { return m_terminal.parser.osc.capacity; }
    [[nodiscard]] const ClipReports& clipReports() const { return m_clipReports; }

private:
    CapturedClipboard m_clipboard;
    ClipReports m_clipReports;
    Ubiquitty::Terminal m_terminal;
};

Observation observeWhole(std::string_view stream)
{
    Session session;
    session.feed(stream);
    return session.observe();
}

Observation observeCut(std::string_view stream, std::size_t cut)
{
    Session session;
    session.feed(stream.substr(0, cut));
    session.feed(stream.substr(cut));
    return session.observe();
}

void reportDifference(const char* label, const Observation& actual, const Observation& expected)
{
    std::fprintf(stderr, "%s: kept %zu/%zu cells=%d title=%d replies=%d clipboard=%d/%d state=%d/%d cursor=%d,%d/%d,%d\n",
        label, actual.pendingPayload.size(), expected.pendingPayload.size(), actual.cells == expected.cells,
        actual.title == expected.title, actual.replies == expected.replies, actual.clipboardWrites, expected.clipboardWrites,
        actual.parserState, expected.parserState, actual.row, actual.column, expected.row, expected.column);
}

/// `count` bytes of payload with a letter every 4093 bytes, so a byte kept out
/// of order or lost from the middle shows in a comparison, not only in a length.
std::string fillerOf(std::size_t count)
{
    std::string bytes(count, 'x');
    for (std::size_t index = 0; index < count; index += 4093)
        bytes[index] = static_cast<char>('a' + index % 26);
    return bytes;
}

/// An unterminated OSC whose accumulated string, `command` and the filler
/// after it, is `size` bytes.
std::string unterminatedOSC(std::string_view command, std::size_t size)
{
    return "\033]" + std::string(command) + fillerOf(size - command.size());
}

/// The cuts of a stream `length` bytes long that can change what is kept: each
/// one through the introducer, the number and its separator, those within a
/// few bytes of `boundaryEnd`, where a ceiling would clip, and those at the
/// end. `stride`, when not zero, samples the payload between them.
std::vector<std::size_t> cutsThatCanMatter(std::size_t length, std::size_t numberEnd, std::size_t boundaryEnd, std::size_t stride)
{
    constexpr std::size_t reach = 4;
    std::vector<std::size_t> cuts;
    const auto addRange = [&](std::size_t from, std::size_t to) {
        for (std::size_t cut = from; cut <= std::min(to, length); ++cut)
            cuts.push_back(cut);
    };
    addRange(0, numberEnd + 1);
    addRange(boundaryEnd - std::min(reach, boundaryEnd), boundaryEnd + reach);
    addRange(length - std::min(reach, length), length);
    if (stride) {
        for (std::size_t cut = numberEnd; cut < length; cut += stride)
            cuts.push_back(cut);
    }
    std::ranges::sort(cuts);
    cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
    return cuts;
}

/// Holds an unterminated OSC of each size around `boundary` to keeping exactly
/// the first `min(size, maxKept)` bytes, whole and at every cut that can matter.
void checkMaxPayload(std::string_view command, int boundary, int maxKept, std::size_t stride)
{
    for (const int delta : { -1, 0, 1 }) {
        const auto size = static_cast<std::size_t>(boundary + delta);
        const std::string stream = unterminatedOSC(command, size);
        const auto expected = observeWhole(stream);
        const std::string_view accumulated = std::string_view(stream).substr(2);
        const std::string label = std::string(command) + " size=" + std::to_string(size);
        CHECK_MSG(expected.pendingPayload == accumulated.substr(0, std::min(size, static_cast<std::size_t>(maxKept))), (label + " whole").c_str());
        // The number and its separator follow the introducer, and the boundary
        // falls after the introducer and `boundary` bytes of string.
        const std::size_t numberEnd = 2 + command.find(';') + 1;
        for (const std::size_t cut : cutsThatCanMatter(stream.size(), numberEnd, 2 + static_cast<std::size_t>(boundary), stride)) {
            const auto actual = observeCut(stream, cut);
            const std::string cutLabel = label + " cut=" + std::to_string(cut);
            if (!(actual == expected))
                reportDifference(cutLabel.c_str(), actual, expected);
            CHECK_MSG(actual == expected, cutLabel.c_str());
        }
    }
}

// The image and clipboard max is the accumulator's whole capacity less the
// terminator it always carries. A string that size costs a 64 MiB feed, so each
// read pattern serves the three sizes around the max: the string arrives in one
// read up to a size and then a byte at a time, observed after each, which is
// the stream of each larger size cut where the earlier reads ended. The other
// patterns first end a read inside the number, at each of `numberCuts`.
void checkMaxImagePayload(std::string_view command, std::initializer_list<std::size_t> numberCuts)
{
    constexpr auto maxKept = static_cast<std::size_t>(Ubiquitty::VT::maxOSCImageSize);
    const std::string stream = unterminatedOSC(command, maxKept + 1);
    struct Pattern {
        std::size_t numberCut;
        std::size_t firstSize;
    };
    std::vector<Pattern> patterns { { 0, maxKept - 1 }, { 0, maxKept }, { 0, maxKept + 1 } };
    for (const std::size_t cut : numberCuts)
        patterns.push_back({ cut, maxKept - 1 });
    for (const auto& pattern : patterns) {
        Session session;
        session.feed(std::string_view(stream).substr(0, pattern.numberCut));
        session.feed(std::string_view(stream).substr(pattern.numberCut, 2 + pattern.firstSize - pattern.numberCut));
        for (std::size_t size = pattern.firstSize; size <= maxKept + 1; ++size) {
            if (size > pattern.firstSize)
                session.feed(std::string_view(stream).substr(1 + size, 1));
            const auto observed = session.observe();
            const std::string label = std::string(command) + " size=" + std::to_string(size) + " number cut=" + std::to_string(pattern.numberCut) + " first read=" + std::to_string(pattern.firstSize);
            const bool keepsExpectedBytes = observed.pendingPayload == std::string_view(stream).substr(2, std::min(size, maxKept));
            if (!keepsExpectedBytes)
                std::fprintf(stderr, "%s: kept %zu\n", label.c_str(), observed.pendingPayload.size());
            CHECK_MSG(keepsExpectedBytes && observed.parserState == static_cast<int>(Ubiquitty::VT::Parser::State::OSC), label.c_str());
        }
    }
}

// Under many reads: each seed ends one read inside the command and the rest at
// random up to 128 KiB apart, and names itself in a failure so the partition can
// be replayed. The string is then ended, and the text and report after it must
// land as they do after the whole stream.
//
// The max is a length the accumulator reaches, and reports once when it does.
// One the accumulator could not hold with its terminator would refuse the run's
// allocation instead and leave the rest of each read to the per-byte path, which
// rescans the read for every byte.
void checkMaxImagePayloadUnderSeededReads(std::string_view command)
{
    const auto checkClipReport = [](const Session& session, const std::string& label) {
#ifndef NDEBUG
        CHECK_MSG(session.clipReports().count == 1 && session.clipReports().lastLength == static_cast<std::uint32_t>(Ubiquitty::VT::maxOSCImageSize), (label + " clip report").c_str());
#else
        (void)session;
        (void)label;
#endif
    };
    constexpr std::string_view tail = "\033\\Z\033[6n";
    constexpr auto maxKept = static_cast<std::size_t>(Ubiquitty::VT::maxOSCImageSize);
    const std::string stream = unterminatedOSC(command, maxKept + 4096);
    Session whole;
    whole.feed(stream);
    const auto pending = whole.observe();
    CHECK_MSG(pending.pendingPayload == std::string_view(stream).substr(2, maxKept), (std::string(command) + " whole").c_str());
    CHECK(whole.accumulatorCapacity() <= Ubiquitty::VT::maxOSCBufferSize);
    checkClipReport(whole, std::string(command) + " whole");
    whole.feed(tail);
    const auto completed = whole.observe();
    for (std::uint32_t seed = 1; seed <= 3; ++seed) {
        std::mt19937 random(seed);
        std::vector<std::size_t> ends { 1 + random() % (2 + command.size()) };
        while (ends.back() < stream.size())
            ends.push_back(std::min(stream.size(), ends.back() + 1 + random() % (128 * 1024)));
        Session session;
        session.feedReads(stream, ends);
        const std::string label = std::string(command) + " seed=" + std::to_string(seed) + " first cut=" + std::to_string(ends.front());
        const auto actual = session.observe();
        if (!(actual == pending))
            reportDifference(label.c_str(), actual, pending);
        CHECK_MSG(actual == pending, label.c_str());
        checkClipReport(session, label);
        session.feed(tail);
        CHECK_MSG(session.observe() == completed, (label + " after ST").c_str());
    }
}

} // namespace

// The minimal stream the decoder split sweep found: an OSC 52 one byte past the
// non-image ceiling kept 65,536 bytes when read whole and all 65,537 when a read
// ended after its number.
TEST(imageOSCCommandNumberHasTheSameMaxAcrossTheFirstRead)
{
    for (const int delta : { -1, 0, 1 }) {
        const std::string payload = "52;" + std::string(static_cast<std::size_t>(Ubiquitty::VT::maxOSCNonImageSize + delta - 3), 'x');
        const std::string stream = "\033]" + payload;
        const auto expected = observeWhole(stream);
        CHECK_MSG(expected.pendingPayload == payload, ("whole payload=" + std::to_string(payload.size())).c_str());
        for (std::size_t cut = 0; cut <= 4; ++cut) {
            const auto actual = observeCut(stream, cut);
            if (!(actual == expected))
                std::fprintf(stderr, "OSC 52 payload=%zu split=%zu: retained %zu, whole %zu\n", payload.size(), cut, actual.pendingPayload.size(), expected.pendingPayload.size());
            CHECK_MSG(actual == expected, ("payload=" + std::to_string(payload.size()) + " split=" + std::to_string(cut)).c_str());
        }
    }
}

// The same stream as a program means it: a valid clipboard write whose base64
// runs past the non-image ceiling reaches the clipboard whole, read in one piece
// or cut after its number.
TEST(largeClipboardOSCProducesTheSameWriteAcrossTheCommandNumberSplit)
{
    constexpr std::size_t encodedBytes = 65532;
    const std::string stream = "\033]52;c;" + std::string(encodedBytes, 'A') + "\033\\";
    const std::string decoded(encodedBytes / 4 * 3, '\0');
    for (const std::size_t cut : { 0u, 4u }) {
        const auto observed = observeCut(stream, cut);
        const auto label = "split=" + std::to_string(cut);
        if (observed.clipboardWrites != 1 || observed.clipboard != decoded)
            std::fprintf(stderr, "OSC 52 %s: writes=%d bytes=%zu expected=%zu\n", label.c_str(), observed.clipboardWrites, observed.clipboard.size(), decoded.size());
        CHECK_MSG(observed.clipboardWrites == 1 && observed.clipboard == decoded, label.c_str());
    }
}

// Each command family, as a complete short string between text and a report,
// read whole, cut at every byte, and a byte at a time. The numbers include
// leading zeros, a ninth digit, and two that begin with the digits of 52 and of
// 1337, so a cut inside a number tells a command from its prefix.
TEST(oscCommandFamiliesTakeTheSameEffectAtEveryTwoPartCut)
{
    struct Family {
        std::string_view stream;
        std::string_view clipboard;
        std::string_view title;
        bool hasReplies;
    };
    static constexpr std::array families {
        Family { "A\033]52;c;QUJD\033\\B\033[6n", "ABC", "", true },
        Family { "A\033]00000052;c;QUJD\aB", "ABC", "", false },
        Family { "A\033]1337;ReportCellSize\033\\B\033[6n", "", "", true },
        Family { "A\033]2;title\aB\033[6n", "", "title", true },
        Family { "A\033]13370;unknown\033\\B\033[6n", "", "", true },
        Family { "A\033]520;unknown\aB", "", "", false },
        Family { "A\033]000000052;c;QUJD\033\\B", "", "", false },
    };
    for (const auto& family : families) {
        const auto expected = observeWhole(family.stream);
        const std::string label(family.stream.substr(1, 8));
        if (!family.clipboard.empty())
            CHECK_MSG(expected.clipboard == family.clipboard, (label + " whole").c_str());
        if (!family.title.empty())
            CHECK_MSG(expected.title == family.title, (label + " whole").c_str());
        if (family.hasReplies)
            CHECK_MSG(!expected.replies.empty(), (label + " whole").c_str());
        for (std::size_t cut = 0; cut <= family.stream.size(); ++cut) {
            const auto actual = observeCut(family.stream, cut);
            const std::string cutLabel = label + " cut=" + std::to_string(cut);
            if (!(actual == expected))
                reportDifference(cutLabel.c_str(), actual, expected);
            CHECK_MSG(actual == expected, cutLabel.c_str());
        }
        Session session;
        for (std::size_t index = 0; index < family.stream.size(); ++index)
            session.feed(family.stream.substr(index, 1));
        CHECK_MSG(session.observe() == expected, (label + " byte-at-a-time").c_str());
    }
}

// A title, and a number that begins with 1337's digits, stop at the non-image
// ceiling however they are read.
TEST(titleAndUnknownOSCsStopAtTheNonImageMaxAtEveryCut)
{
    constexpr int maxNonImage = Ubiquitty::VT::maxOSCNonImageSize;
    checkMaxPayload("2;", maxNonImage, maxNonImage, 4093);
    checkMaxPayload("13370;", maxNonImage, maxNonImage, 4093);
}

// The clipboard and an image keep their payload past the non-image ceiling
// however they are read: the reproducer's shape, for both commands and every cut.
TEST(clipboardAndImageOSCsPassTheNonImageMaxAtEveryCut)
{
    constexpr int maxNonImage = Ubiquitty::VT::maxOSCNonImageSize;
    checkMaxPayload("52;c;", maxNonImage, Ubiquitty::VT::maxOSCImageSize, 4093);
    checkMaxPayload("1337;File=", maxNonImage, Ubiquitty::VT::maxOSCImageSize, 4093);
}

TEST(clipboardOSCStopsAtTheImageMaxAtEveryCut)
{
    checkMaxImagePayload("52;c;", { 3 });
}

TEST(imageOSCStopsAtTheImageMaxAtEveryCut)
{
    checkMaxImagePayload("1337;File=", { 4 });
}

TEST(clipboardOSCAtTheImageMaxHoldsUnderSeededReads)
{
    checkMaxImagePayloadUnderSeededReads("52;c;");
}

TEST(imageOSCAtTheImageMaxHoldsUnderSeededReads)
{
    checkMaxImagePayloadUnderSeededReads("1337;File=");
}

UBIQUITTY_TEST_MAIN()
