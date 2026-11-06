// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Kitty/Graphics.h"
#include "Core/TerminalOperations.h"
#include "Core/VT/Constants.h"
#include "Core/VT/Parser.h"
#include "Harness.h"
#include "StreamPartitions.h"
#include "TerminalFixture.h"

#include <array>
#include <string>
#include <vector>

using namespace UbiquittyTest;

namespace {

struct Observation {
    std::string cells;
    std::string title;
    std::string replies;
    std::string osc;
    std::string apc;
    std::string sixel;
    int parserState;
    int row;
    int column;
    bool hasTruncatedDCS;
    bool hasOverflowedAPC;
    bool hasOverflowedSixel;

    friend bool operator==(const Observation&, const Observation&) = default;
};

Observation observe(Ubiquitty::Terminal& terminal)
{
    Observation result { .cells = {}, .title = std::string(terminal.titleUTF8.view()), .replies = {}, .osc = {}, .apc = {}, .sixel = {}, .parserState = static_cast<int>(terminal.parser.parseState), .row = terminal.screens.active->cursorY, .column = terminal.screens.active->cursorX, .hasTruncatedDCS = terminal.dcsTruncated, .hasOverflowedAPC = terminal.images.kittyAPCOverflow, .hasOverflowedSixel = terminal.images.sixelAccumulatorOverflow };
    for (int row = 0; row < terminal.rows; ++row) {
        for (int column = 0; column < terminal.columns; ++column) {
            const auto* value = cell(terminal, row, column);
            result.cells.append(reinterpret_cast<const char*>(value), sizeof(*value));
        }
    }
    std::array<char, 4096> output;
    for (int count = drain(terminal, output); count; count = drain(terminal, output))
        result.replies.append(output.data(), static_cast<std::size_t>(count));
    using State = Ubiquitty::VT::Parser::State;
    const auto state = terminal.parser.parseState;
    const bool hasPendingString = state == State::OSC || state == State::OSCEscape || state == State::DCSPassthrough || state == State::DCSEscape;
    if (hasPendingString && terminal.parser.osc.length)
        result.osc.assign(terminal.parser.osc.data, static_cast<std::size_t>(terminal.parser.osc.length));
    if (!terminal.images.kittyAPCOverflow && terminal.images.kittyAPC.length)
        result.apc.assign(terminal.images.kittyAPC.data, static_cast<std::size_t>(terminal.images.kittyAPC.length));
    if (terminal.isSixelDCS && !terminal.images.sixelAccumulatorOverflow && terminal.images.sixelAccumulatorLength)
        result.sixel.assign(reinterpret_cast<const char*>(terminal.images.sixelAccumulatorBuffer), static_cast<std::size_t>(terminal.images.sixelAccumulatorLength));
    return result;
}

void prepare(Ubiquitty::Terminal& terminal)
{
    Ubiquitty::resize(terminal, 3, 20, Ubiquitty::Modes::ReflowMode::Fixed);
    Ubiquitty::hardReset(terminal);
}

bool checkStream(std::string_view stream, PartitionCoverage coverage)
{
    Ubiquitty::Terminal whole;
    prepare(whole);
    feed(whole, stream);
    const auto expected = observe(whole);
    return sweepStreamPartitions(stream.size(), coverage, [&](auto ends, const std::string& label) {
        Ubiquitty::Terminal terminal;
        prepare(terminal);
        feedStreamPartition(std::span(stream.data(), stream.size()), ends, [&](auto piece) {
            feed(terminal, std::string_view(piece.data(), piece.size()));
        });
        const auto actual = observe(terminal);
        const bool matches = actual == expected;
        if (!matches) {
            std::fprintf(stderr, "control string prefix=%02x/%02x/%02x: cells=%d title=%d replies=%d osc=%d apc=%d state=%d/%d cursor=%d,%d/%d,%d flags=%d,%d/%d,%d\n",
                static_cast<unsigned char>(stream[1]), static_cast<unsigned char>(stream[2]), static_cast<unsigned char>(stream[3]),
                actual.cells == expected.cells, actual.title == expected.title, actual.replies == expected.replies,
                actual.osc == expected.osc, actual.apc == expected.apc, actual.parserState, expected.parserState,
                actual.row, actual.column, expected.row, expected.column,
                actual.hasTruncatedDCS, actual.hasOverflowedAPC, expected.hasTruncatedDCS, expected.hasOverflowedAPC);
        }
        CHECK_MSG(matches, ("bytes=" + std::to_string(stream.size()) + " " + label).c_str());
        return matches;
    });
}

void checkAccumulatorBounds(std::string_view prefix)
{
    for (const int bound : { Ubiquitty::VT::oscBufferInitialCapacity, Ubiquitty::VT::maxOSCNonImageSize }) {
        for (const int delta : { -1, 0, 1 }) {
            std::string stream(prefix);
            const auto count = static_cast<std::size_t>(bound + delta) - (prefix.size() - 2);
            for (std::size_t index = 0; index < count; ++index)
                stream.push_back(static_cast<char>('a' + (index * 7 + index / 251) % 26));
            // Inspect only a live accumulator: dispatched OSC storage may
            // already have been released before its next entry resets length.
            CHECK(checkStream(stream, PartitionCoverage::Boundaries));
            stream += "\033\\Z\033[6n";
            CHECK(checkStream(stream, PartitionCoverage::Boundaries));
        }
    }
}

void checkLargeAccumulator(std::string_view prefix, int capacity, std::size_t storedPrefixBytes)
{
    std::string stream(prefix);
    stream.append(static_cast<std::size_t>(capacity + 1) - storedPrefixBytes, 'x');
    for (std::size_t index = prefix.size(); index < stream.size(); index += 4096)
        stream[index] = static_cast<char>('a' + index % 26);
    Ubiquitty::Terminal whole;
    prepare(whole);
    feed(whole, stream);
    const auto pending = observe(whole);
    constexpr std::string_view tail = "\033\\Z\033[6n";
    feed(whole, tail);
    const auto completed = observe(whole);
    for (const int delta : { -1, 0, 1, 2 }) {
        std::vector<std::size_t> ends;
        if (delta <= 1) {
            ends = { static_cast<std::size_t>(capacity + delta) + 2, stream.size() };
        } else {
            std::mt19937 random(4201);
            for (std::size_t end = 0; end < stream.size();) {
                end += std::min(stream.size() - end, 4096 + static_cast<std::size_t>(random()) % 65536);
                ends.push_back(end);
            }
        }
        Ubiquitty::Terminal terminal;
        prepare(terminal);
        feedStreamPartition(std::span<const char>(stream), ends, [&](auto piece) { feed(terminal, std::string_view(piece.data(), piece.size())); });
        const auto label = delta == 2 ? "seed=4201" : "split=" + std::to_string(ends.front());
        const auto actual = observe(terminal);
        if (!(actual == pending))
            std::fprintf(stderr, "large accumulator %s: retained OSC %zu/%zu APC %zu/%zu state %d/%d overflow %d/%d\n", label.c_str(),
                actual.osc.size(), pending.osc.size(), actual.apc.size(), pending.apc.size(), actual.parserState, pending.parserState,
                actual.hasOverflowedAPC, pending.hasOverflowedAPC);
        CHECK_MSG(actual == pending, label.c_str());
        feed(terminal, tail);
        CHECK_MSG(observe(terminal) == completed, label.c_str());
    }
}

} // namespace

TEST(oscStringsPreserveTextAndRepliesAtEverySplit)
{
    for (const std::string_view stream : {
             "A\033]2;title \xC3\xA9\aB\033[6n",
             "A\033]2;title \xF0\x9F\x98\x80\033\\B\033[6n",
             "\033]8;id=one;https://example.test/a\033\\\033[31mURL\033]8;;\033\\Z",
             "\033]4;1;rgb:12/34/56\033\\\033]4;1;?\aZ",
             "\033]2;cancelled\030Z\033[6n",
             "\033]2;abandoned\032Z\033[6n" })
        CHECK(checkStream(stream, PartitionCoverage::Exhaustive));
}

TEST(dcsStringsPreserveQueriesAndCancellationAtEverySplit)
{
    for (const std::string_view stream : {
             "\033[1;31m\033P$qm\033\\Z\033[6n",
             "\033P+q544e;436f\033\\Z\033[6n",
             "\033P$qm\030Z\033[6n",
             "\033P$qm\032Z\033[6n",
             "\033Pignored\033Xstill ignored\033\\Z\033[6n" })
        CHECK(checkStream(stream, PartitionCoverage::Exhaustive));
}

TEST(apcStringsPreserveRepliesAndCancellationAtEverySplit)
{
    for (const std::string_view stream : {
             "\033_Ga=q,f=32,s=1,v=1,i=7;AAAA/w==\033\\Z\033[6n",
             "\033_Ga=q,f=32,s=1,v=1,i=9;AQID/w==\033\\\033_Ga=d,d=a\033\\Z",
             "\033_Ga=q,f=32,s=1,v=1,i=7;AAAA\030Z\033[6n",
             "\033_Ga=q,f=32,s=1,v=1,i=7;AAAA\032Z\033[6n" })
        CHECK(checkStream(stream, PartitionCoverage::Exhaustive));
}

TEST(oscAccumulatorPreservesBytesAroundItsGrowthAndNonImageBounds)
{
    checkAccumulatorBounds("\033]2;");
}

TEST(dcsAccumulatorPreservesBytesAroundItsGrowthAndTruncationBounds)
{
    checkAccumulatorBounds("\033P$q");
}

TEST(apcAccumulatorPreservesBytesAroundItsGrowthAndRetentionBounds)
{
    checkAccumulatorBounds("\033_G");
}

TEST(apcAtItsAbsoluteBoundPreservesRefusalAndFollowingText)
{
    checkLargeAccumulator("\033_G", Ubiquitty::Kitty::maxAPCCapacity, 1);
}

TEST(sixelDCSAtItsAbsoluteBoundPreservesRefusalAndFollowingText)
{
    checkLargeAccumulator("\033Pq", Ubiquitty::VT::maxSixelAccumulatorCapacity, 0);
}

UBIQUITTY_TEST_MAIN()
