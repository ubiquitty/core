// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

#include "Core/Session/BackgroundEvents.h"

#include <array>
#include <string>

namespace {

using namespace Ubiquitty::Session;
using namespace Ubiquitty::OSC;
const std::string pane = backgroundPaneID(1, 2);

ProgramStatus question(std::string message = "Choose a target")
{
    return { .id = "agent/question", .app = {}, .title = "Agent", .message = std::move(message), .state = ProgramState::Blocked, .kind = ProgramBlockKind::Question };
}

} // namespace

TEST(notificationReplayIsIndependentPerDeviceAndFetchDoesNotConsume)
{
    BackgroundEvents journal(42);
    journal.appendNotification(pane, 2, { .title = "Build", .body = "Finished", .identifier = {} }, 100);
    const auto first = journal.since(0, 101);
    CHECK_EQ(first.events.size(), 1u);
    CHECK_EQ(first.cursor, 1u);
    CHECK(journal.since(0, 102).events == first.events);
    CHECK(journal.since(first.cursor, 103).events.empty());
}

TEST(questionChangesSupersedeAndResolutionWithdrawsPendingAlerts)
{
    BackgroundEvents journal(1);
    std::array records { question() };
    journal.updateQuestions(pane, 2, {}, records, 100);
    journal.updateQuestions(pane, 2, records, records, 101);
    CHECK_EQ(journal.since(0, 102).cursor, 1u);
    auto previous = records;
    records[0].message = "Choose a branch";
    journal.updateQuestions(pane, 2, previous, records, 103);
    auto page = journal.since(0, 104);
    CHECK_EQ(page.events.size(), 1u);
    CHECK_EQ(page.cursor, 2u);
    CHECK_STR_EQ(page.events[0].body, "Choose a branch");
    previous = records;
    records[0].state = ProgramState::Working;
    records[0].kind = ProgramBlockKind::None;
    journal.updateQuestions(pane, 2, previous, records, 105);
    CHECK(journal.since(0, 106).events.empty());
    CHECK_EQ(journal.since(0, 106).cursor, 2u);
}

TEST(paneRemovalWithdrawsQuestionsAndRetainsInformationalNotifications)
{
    BackgroundEvents journal(1);
    journal.updateQuestions(pane, 2, {}, std::array { question() }, 100);
    journal.appendNotification(pane, 2, { .title = "Done", .body = "Tests passed", .identifier = {} }, 101);
    journal.removePane(pane);
    const auto page = journal.since(0, 102);
    CHECK_EQ(page.events.size(), 1u);
    CHECK(page.events[0].statusID.empty());
    CHECK_EQ(page.cursor, 2u);
}

TEST(retentionReportsLossAndPagesNeverSkipUndeliveredEvents)
{
    BackgroundEvents journal(1);
    for (std::size_t index = 0; index < BackgroundEvents::capacity + 3; ++index) {
        journal.appendNotification(pane, 2, { .title = "event", .body = std::to_string(index), .identifier = {} }, 100);
    }
    auto page = journal.since(0, 101);
    CHECK(page.hasGap);
    CHECK(page.hasMore);
    CHECK_EQ(page.events.front().sequence, 4u);
    std::size_t count = page.events.size();
    while (page.hasMore) {
        const auto previous = page.cursor;
        page = journal.since(previous, 101);
        CHECK_EQ(page.events.front().sequence, previous + 1);
        count += page.events.size();
    }
    CHECK_EQ(count, BackgroundEvents::capacity);
    CHECK_EQ(page.cursor, BackgroundEvents::capacity + 3);
}

TEST(expiredEventsAdvanceReceiptWithoutPostingAndClockRollbackDoesNotExpire)
{
    BackgroundEvents journal(1);
    journal.appendNotification(pane, 2, { .title = "title", .body = "body", .identifier = {} }, 100);
    CHECK_EQ(journal.since(0, 99).events.size(), 1u);
    const auto expired = journal.since(0, 100 + BackgroundEvents::lifetimeMilliseconds);
    CHECK(expired.events.empty());
    CHECK_EQ(expired.cursor, 1u);
}

TEST(persistencePreservesEpochCursorsAndWithdrawnQuestions)
{
    BackgroundEvents journal(123);
    journal.updateQuestions(pane, 2, {}, std::array { question() }, 100);
    journal.removePane(pane);
    journal.appendNotification(pane, 2, { .title = "title", .body = "body", .identifier = {} }, 101);
    BackgroundEvents restored(999);
    CHECK(restored.restore(journal.persistentState()));
    CHECK_EQ(restored.epoch(), 123u);
    CHECK(restored.since(0, 102).events == journal.since(0, 102).events);
    restored.appendNotification(pane, 2, { .title = "next", .body = "body", .identifier = {} }, 103);
    CHECK_EQ(restored.since(2, 104).events.front().sequence, 3u);
    CHECK(!restored.restore(Ubiquitty::Codec::JSON::Value::object()));
    CHECK_EQ(restored.epoch(), 123u);
    CHECK_EQ(restored.since(2, 104).cursor, 3u);
}

TEST(untrustedCursorAndEscapedTextStayBounded)
{
    CHECK(!backgroundSequence("-1"));
    CHECK(!backgroundSequence("1x"));
    CHECK(!backgroundSequence("18446744073709551616"));
    CHECK_EQ(backgroundSequence("18446744073709551615").value(), UINT64_MAX);
    BackgroundEvents journal(1);
    for (int index = 0; index < 256; ++index) {
        journal.appendNotification(pane, 2, { .title = std::string(192, '\x01'), .body = std::string(2048, '\x01'), .identifier = {} }, 100);
    }
    const auto page = journal.since(0, 101);
    CHECK(page.hasMore);
    CHECK(page.events.size() < BackgroundEvents::pageCapacity);
    CHECK(Ubiquitty::Codec::JSON::serialize(journal.persistentState()).size() <= BackgroundEvents::maxFileBytes);
}

TEST(evictedQuestionIsNotReissuedUntilItsAuthoritativeStateChanges)
{
    BackgroundEvents journal(1);
    const std::array blocked { question() };
    journal.updateQuestions(pane, 2, {}, blocked, 100);
    for (std::size_t index = 0; index < BackgroundEvents::capacity; ++index) {
        journal.appendNotification(pane, 2, { .title = "event", .body = std::to_string(index), .identifier = {} }, 101);
    }
    const auto before = journal.since(BackgroundEvents::capacity + 1, 102);
    journal.updateQuestions(pane, 2, blocked, blocked, 103);
    CHECK_EQ(journal.since(before.cursor, 104).cursor, before.cursor);
    CHECK(journal.since(before.cursor, 104).events.empty());

    auto resolved = blocked;
    resolved[0].state = ProgramState::Working;
    resolved[0].kind = ProgramBlockKind::None;
    journal.updateQuestions(pane, 2, blocked, resolved, 105);
    journal.updateQuestions(pane, 2, resolved, blocked, 106);
    const auto next = journal.since(before.cursor, 107);
    CHECK_EQ(next.events.size(), 1u);
    CHECK_EQ(next.cursor, before.cursor + 1);
    CHECK_STR_EQ(next.events.front().statusID, "agent/question");
}

TEST(namedNotificationsReplaceWithinTheirPaneAndPreserveOccasion)
{
    BackgroundEvents journal(1);
    journal.appendNotification(pane, 2, { .title = "Old", .body = {}, .identifier = "job", .occasion = Ubiquitty::NotificationOccasion::Invisible }, 100);
    journal.appendNotification(backgroundPaneID(3, 4), 3, { .title = "Other pane", .body = {}, .identifier = "job" }, 101);
    journal.appendNotification(pane, 2, { .title = "New", .body = {}, .identifier = "job", .occasion = Ubiquitty::NotificationOccasion::Unfocused }, 102);
    const auto page = journal.since(0, 103);
    CHECK_EQ(page.events.size(), 2u);
    CHECK_STR_EQ(page.events.back().title, "New");
    CHECK_STR_EQ(page.events.back().notificationID, "job");
    CHECK(page.events.back().occasion == Ubiquitty::NotificationOccasion::Unfocused);
    BackgroundEvents restored(999);
    CHECK(restored.restore(journal.persistentState()));
    CHECK(restored.since(0, 104).events == page.events);
}

UBIQUITTY_TEST_MAIN()
