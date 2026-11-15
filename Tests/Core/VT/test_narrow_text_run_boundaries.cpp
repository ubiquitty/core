// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"
#include "TerminalFixture.h"

#include "Core/Cell.h"
#include "Core/Unicode/UTF8Encoder.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace {

std::string narrowText(int count)
{
    // Plain Latin, punctuation and symbols with short ASCII gaps. Some widen
    // under the terminal's ambiguous-width policy, exercising the fallback.
    constexpr std::array<std::uint32_t, 8> codePoints { 0xE9, 0xF1, 0x2013, 0x3B1, 0x20, 0xF8, 0x2500, 0x1FB00 };
    std::string text;
    for (int index = 0; index < count; ++index)
        Ubiquitty::Unicode::appendUTF8(text, codePoints[static_cast<std::size_t>(index) % codePoints.size()]);
    return text;
}

void checkSameTerminal(const Ubiquitty::Terminal& actual, const Ubiquitty::Terminal& expected)
{
    CHECK_EQ(actual.screens.active->cursorX, expected.screens.active->cursorX);
    CHECK_EQ(actual.screens.active->cursorY, expected.screens.active->cursorY);
    CHECK_EQ(actual.screens.active->wrapPending, expected.screens.active->wrapPending);
    CHECK_EQ(actual.scrollbackCount, expected.scrollbackCount);
    for (int row = 0; row < actual.rows; ++row) {
        CHECK_EQ(actual.screens.active->lineWrapped[row], expected.screens.active->lineWrapped[row]);
        for (int column = 0; column < actual.columns; ++column) {
            const auto* actualCell = UbiquittyTest::cell(actual, row, column);
            const auto* expectedCell = UbiquittyTest::cell(expected, row, column);
            CHECK(actualCell && expectedCell);
            CHECK_EQ(actualCell->codePoint, expectedCell->codePoint);
            CHECK_EQ(actualCell->linkID(), expectedCell->linkID());
            CHECK(std::memcmp(&actualCell->pen, &expectedCell->pen, sizeof(actualCell->pen)) == 0);
            std::array<std::uint32_t, Ubiquitty::clusterPoolMaximumCodePoints + 1> actualCluster;
            std::array<std::uint32_t, Ubiquitty::clusterPoolMaximumCodePoints + 1> expectedCluster;
            const int actualLength = Ubiquitty::cellClusterCodePoints(&actual, actualCell, actualCluster.data(), static_cast<int>(actualCluster.size()));
            const int expectedLength = Ubiquitty::cellClusterCodePoints(&expected, expectedCell, expectedCluster.data(), static_cast<int>(expectedCluster.size()));
            CHECK_EQ(actualLength, expectedLength);
            CHECK(std::equal(actualCluster.begin(), actualCluster.begin() + actualLength, expectedCluster.begin()));
        }
    }
    CHECK(UbiquittyTest::firstOrphanedWideHalf(actual).empty());
}

} // namespace

TEST(narrowTextRunsPreserveWrapsClustersAndWideEdges)
{
    const std::string row = narrowText(64);
    const std::string prefix = narrowText(17);
    // A mark after a full run, cancellation and repeat at its tail, then a
    // mixed-width run and an overwrite starting on a wide continuation.
    const std::string text = row + "\xCC\x81\r\n" + row + "\x1BT\x1B[2b\r\n"
        + prefix + "漢字" + prefix + "\r\n"
        + "\x1B[10;1H漢字漢字\x1B[10;2H" + prefix;
    for (const auto ambiguousWidth : { Ubiquitty::Unicode::Width::AmbiguousWidth::Narrow, Ubiquitty::Unicode::Width::AmbiguousWidth::Wide }) {
        for (const int columns : { 63, 64, 65, 80 }) {
            for (const char* screen : { "", "\x1B[?1049h" }) {
                const std::string setup = std::string(screen) + "\x1B[38;2;17;83;129;4m\x1B]8;;https://example.test/narrow\x1B\\";
                Ubiquitty::Terminal reference;
                Ubiquitty::hardReset(reference);
                Ubiquitty::resize(reference, 16, columns);
                reference.ambiguousWidth = ambiguousWidth;
                UbiquittyTest::feed(reference, setup);
                for (const char& byte : text)
                    UbiquittyTest::feed(reference, { &byte, 1 });
                for (const int chunkSize : { 3, 7, 31, 127, static_cast<int>(text.size()) }) {
                    Ubiquitty::Terminal candidate;
                    Ubiquitty::hardReset(candidate);
                    Ubiquitty::resize(candidate, 16, columns);
                    candidate.ambiguousWidth = ambiguousWidth;
                    UbiquittyTest::feed(candidate, setup);
                    for (std::size_t offset = 0; offset < text.size(); offset += static_cast<std::size_t>(chunkSize))
                        UbiquittyTest::feed(candidate, std::string_view(text).substr(offset, static_cast<std::size_t>(chunkSize)));
                    checkSameTerminal(candidate, reference);
                }
            }
        }
    }
}

UBIQUITTY_TEST_MAIN()
