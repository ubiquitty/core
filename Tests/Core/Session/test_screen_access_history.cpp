// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Session/ScreenAccessHistory.h"
#include "Harness.h"

#include <cstdint>
#include <limits>
#include <string>

using Ubiquitty::Session::ScreenAccessHistory;

TEST(openingAnActivityListDoesNotReviewAReadThatArrivedWhileItWasOpen)
{
    ScreenAccessHistory history;
    CHECK(!history.hasUnread());
    CHECK(history.record({ 10, 1000, "first reader", "/reader" }));
    const auto shown = history.latestCursor();
    CHECK(history.record({ 11, 1100, "second reader", "/other" }));
    history.acknowledgeThrough(shown);
    CHECK(history.hasUnread());
    history.acknowledgeThrough(11);
    CHECK(!history.hasUnread());
    CHECK(history.records().size() == 2);
    CHECK(history.records().back().claimedLabel == "second reader");
}

TEST(replayedReadsStayReviewedAndAFutureAcknowledgmentCannotHideANewRead)
{
    ScreenAccessHistory history;
    CHECK(history.record({ 20, 2000, "reader", "/reader" }));
    history.acknowledgeThrough(std::numeric_limits<std::uint64_t>::max());
    CHECK(!history.record({ 20, 2000, "reader", "/reader" }));
    CHECK(!history.record({ 19, 1900, "reader", "/reader" }));
    CHECK(!history.hasUnread());
    CHECK(history.record({ 21, 2100, "reader", "/reader" }));
    CHECK(history.hasUnread());
}

TEST(accessHistoryBoundsRecordsAndIdentityTextWithoutLosingItsUnreadPosition)
{
    ScreenAccessHistory history;
    for (std::uint64_t cursor = 1; cursor <= 100; ++cursor)
        CHECK(history.record({ cursor, cursor * 1000, "reader", "/reader" }));
    CHECK(history.records().size() == ScreenAccessHistory::maxRecords);
    CHECK(history.records().front().cursor == 69);
    CHECK(history.hasUnread());
    CHECK(!history.record({ 101, 101000, std::string(ScreenAccessHistory::maxIdentityBytes + 1, 'a'), "" }));
    CHECK(!history.record({ 101, 101000, "", std::string(ScreenAccessHistory::maxIdentityBytes + 1, 'b') }));
    CHECK(history.latestCursor() == 100);
    history.acknowledgeThrough(100);
    history.reset();
    CHECK(!history.hasUnread());
    CHECK(history.records().empty());
    CHECK(!history.record({ 0, 0, "", "" }));
    CHECK(history.record({ 1, 1000, "new pane", "" }));
    CHECK(history.hasUnread());
}

UBIQUITTY_TEST_MAIN()
