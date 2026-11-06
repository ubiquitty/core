// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_sync_divergence.cpp — a divergence retains an offline diagnostic fixture
// (Docs/sync.md §6.2).
//
// The properties held:
//
// - A diagnostic client keeps the checkpoint at the last verified state and
//   every update after it; when an update's digest does not match, the
//   fixture it hands over replays offline to the same divergence and names the
//   row that was corrupted, the row the host names from the Resync too.
// - A release client keeps nothing for it, however many updates arrive.

#include "Harness.h"
#include "SyncFixture.h"
#include "SyncLink.h"

#include "Core/Protocol/Sync/Connection.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

using namespace Ubiquitty::Protocol::Sync;
using UbiquittyTest::SyncLink;

namespace {

/// A client policy that keeps a divergence fixture.
ClientPolicy diagnosticPolicy()
{
    ClientPolicy policy = UbiquittyTest::syncClientPolicy;
    policy.shouldRetainDivergenceFixture = true;
    return policy;
}

} // namespace

// An op corrupted on the wire leaves a fixture whose replay diverges at that
// update and names the corrupted row.
TEST(aDigestMismatchLeavesAReplayableFixture)
{
    SyncLink link(0, UbiquittyTest::syncDefaultFacts(), std::nullopt, ClientKind::Human, std::nullopt, diagnosticPolicy());
    const std::uint32_t pane = link.addPane(24, 80, "$ ");
    for (int line = 0; line < 5; ++line) {
        link.write(pane, "line " + std::to_string(line) + "\r\n");
        link.step();
    }
    // Row 7 gets text the wire then flips one byte of.
    constexpr int corruptedRow = 7;
    constexpr std::string_view marker = "corrupt this row";
    bool isCorrupted = false;
    link.setStreamFilter([&](std::vector<std::uint8_t>& stream) {
        const auto found = std::ranges::search(stream, marker);
        if (!isCorrupted && !found.empty()) {
            found.front() = 'C';
            isCorrupted = true;
        }
    });
    link.write(pane, "\x1b[" + std::to_string(corruptedRow + 1) + ";1H" + std::string(marker));
    link.step();
    REQUIRE(isCorrupted);
    CHECK_EQ(link.client().counters().digestMismatches, std::uint64_t { 1 });

    std::optional<DivergenceFixture> fixture = link.client().takeDivergenceFixture(pane);
    REQUIRE(fixture.has_value());
    CHECK(fixture->hasDiverged);
    CHECK(!fixture->isIncomplete);
    const std::optional<DivergenceFixture> read = UbiquittyTest::readSyncFixture(UbiquittyTest::writeSyncFixture(*fixture));
    REQUIRE(read.has_value());
    const UbiquittyTest::SyncReplay replay = UbiquittyTest::replaySyncFixture(*read);
    CHECK(replay.isCheckpointApplied);
    REQUIRE(replay.divergedUpdate.has_value());
    CHECK_EQ(*replay.divergedUpdate, read->updates.size() - 1);
    std::vector<std::uint64_t> hostRows;
    for (int row = 0; row < 24; ++row)
        hostRows.push_back(link.source(pane).publisher().rows(0).hash(row));
    CHECK_EQ(UbiquittyTest::firstMismatchRow(replay.builtRowHashes, hostRows), corruptedRow);

    link.setStreamFilter(nullptr);
    link.stepUntilQuiet();
    CHECK_EQ(link.host().counters().lastResyncFirstRow, corruptedRow);
    CHECK_STR_EQ(link.disagreement(pane), "");
}

// A release client keeps no fixture: a thousand updates hold no bytes for one.
TEST(aReleaseBuildRetainsNothing)
{
    SyncLink link(0, UbiquittyTest::syncDefaultFacts());
    const std::uint32_t pane = link.addPane(24, 80, "$ ");
    for (int update = 0; update < 1'000; ++update) {
        link.write(pane, "x");
        link.step();
    }
    CHECK(link.client().counters().updatesApplied >= 500);
    CHECK_EQ(link.client().divergenceFixtureBytes(), std::size_t { 0 });
    CHECK(!link.client().takeDivergenceFixture(pane).has_value());

    // The diagnostic client keeps them, bounded by the buffered-update cap.
    SyncLink diagnostic(0, UbiquittyTest::syncDefaultFacts(), std::nullopt, ClientKind::Human, std::nullopt, diagnosticPolicy());
    const std::uint32_t kept = diagnostic.addPane(24, 80, "$ ");
    for (int update = 0; update < 1'000; ++update) {
        diagnostic.write(kept, "x");
        diagnostic.step();
    }
    CHECK(diagnostic.client().divergenceFixtureBytes() > 0);
    CHECK(diagnostic.client().divergenceFixtureBytes() <= UbiquittyTest::syncClientPolicy.maxBufferedUpdateBytes + (std::size_t { 1 } << 20));
}

UBIQUITTY_TEST_MAIN()
