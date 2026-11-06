// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// test_event_log — the host's event stream: cursors never repeat, a subscriber
// resuming from a cursor gets exactly what followed it, and a subscriber the
// bounded log outran is told how many records it lost rather than handed a
// stream with a silent hole.

#include "Harness.h"

#include "Core/Codec/JSON.h"
#include "Core/Session/EventLog.h"

#include <string>
#include <vector>

namespace {

using Ubiquitty::Session::Event;
using Ubiquitty::Session::EventKind;
using Ubiquitty::Session::EventLog;
using Ubiquitty::Session::EventProgramState;

Event bell(std::uint32_t session)
{
    Event event;
    event.kind = EventKind::Bell;
    event.sessionID = session;
    return event;
}

} // namespace

TEST(cursorsIncreaseFromOneAndAreNeverReused)
{
    EventLog log(4);
    CHECK_EQ(log.latestCursor(), 0u);
    CHECK_EQ(log.append(bell(1)).cursor, 1u);
    CHECK_EQ(log.append(bell(1)).cursor, 2u);
    for (int i = 0; i < 10; ++i)
        log.append(bell(2));
    CHECK_EQ(log.latestCursor(), 12u);
    CHECK_EQ(log.count(), 4u);
}

TEST(aResumedSubscriberGetsExactlyWhatFollowedItsCursor)
{
    EventLog log(16);
    for (std::uint32_t session = 1; session <= 5; ++session)
        log.append(bell(session));
    const auto backlog = log.since(2);
    CHECK_EQ(backlog.dropped, 0u);
    CHECK_EQ(backlog.events.size(), 3u);
    if (backlog.events.size() == 3) {
        CHECK_EQ(backlog.events[0].cursor, 3u);
        CHECK_EQ(backlog.events[0].sessionID, 3u);
        CHECK_EQ(backlog.events[2].cursor, 5u);
    }
    CHECK_MSG(log.since(5).events.empty(), "a subscriber at the newest cursor is owed nothing");
    CHECK_MSG(log.since(0).events.size() == 5 && log.since(0).dropped == 0, "cursor zero replays the whole log without a gap");
}

TEST(aSubscriberTheLogOutranIsToldHowManyRecordsItLost)
{
    EventLog log(3);
    for (std::uint32_t session = 1; session <= 7; ++session)
        log.append(bell(session));
    // The log holds cursors 5, 6 and 7; a subscriber at 2 missed 3 and 4.
    const auto backlog = log.since(2);
    CHECK_EQ(backlog.dropped, 2u);
    CHECK_EQ(backlog.gapCursor, 4u);
    CHECK_EQ(backlog.events.size(), 3u);
    if (!backlog.events.empty())
        CHECK_EQ(backlog.events.front().cursor, 5u);
}

TEST(aRecordsTextIsBoundedAtACodePointBoundary)
{
    EventLog log(2);
    Event event;
    event.kind = EventKind::TitleChanged;
    // Three-byte code points, so the byte cap falls inside one of them.
    for (std::size_t i = 0; i < Ubiquitty::Session::maxEventTextLength; ++i)
        event.text += "\xE2\x82\xAC";
    const Event& stored = log.append(event);
    CHECK(stored.text.size() <= Ubiquitty::Session::maxEventTextLength);
    CHECK_EQ(stored.text.size() % 3, 0u);
}

TEST(anEventIsWrittenWithTypedIdentitiesAndOnlyTheFieldsItsKindHas)
{
    Event event;
    event.kind = EventKind::CommandFinished;
    event.sessionID = 3;
    event.paneID = 9;
    event.exitStatus = 2;
    event.text = "make test";
    event.cursor = 41;
    CHECK_STR_EQ(Ubiquitty::Codec::JSON::serialize(Ubiquitty::Session::eventJSON(event)),
        R"({"cursor":41,"kind":"command-finished","session":"session:3","pane":"pane:9","exitStatus":2,"commandLine":"make test","directory":""})");

    Event status;
    status.kind = EventKind::ProgramStatus;
    status.sessionID = 3;
    status.state = EventProgramState::Blocked;
    status.text = "Continue?";
    status.detail = "build";
    status.cursor = 42;
    CHECK_STR_EQ(Ubiquitty::Codec::JSON::serialize(Ubiquitty::Session::eventJSON(status)),
        R"({"cursor":42,"kind":"program-status","session":"session:3","state":"blocked","message":"Continue?","id":"build"})");

    // A session is labelled after it exists, so its creation carries no label.
    Event created;
    created.kind = EventKind::SessionCreated;
    created.sessionID = 3;
    created.paneID = 9;
    created.cursor = 1;
    CHECK_STR_EQ(Ubiquitty::Codec::JSON::serialize(Ubiquitty::Session::eventJSON(created)), R"({"cursor":1,"kind":"session-created","session":"session:3","pane":"pane:9"})");

    Event started;
    started.kind = EventKind::CommandStarted;
    started.sessionID = 3;
    started.paneID = 9;
    started.cursor = 43;
    CHECK_STR_EQ(Ubiquitty::Codec::JSON::serialize(Ubiquitty::Session::eventJSON(started)), R"({"cursor":43,"kind":"command-started","session":"session:3","pane":"pane:9"})");
}

TEST(aReaderThatHoldsMoreThanItsBoundIsToldWhatItDropped)
{
    // What a subscriber the log outran is sent: the gap, then every record the
    // log still holds. A reader whose bound is the log's capacity holds one
    // record fewer than that, and the one it gives up must not be the gap.
    std::vector<Event> held;
    Event gap;
    gap.kind = EventKind::Gap;
    gap.cursor = 5;
    gap.count = 5;
    Ubiquitty::Session::holdEvent(held, gap, 4);
    for (std::uint64_t cursor = 6; cursor <= 9; ++cursor) {
        Event event = bell(1);
        event.cursor = cursor;
        Ubiquitty::Session::holdEvent(held, event, 4);
    }
    REQUIRE(held.size() == 4u);
    CHECK_MSG(held[0].kind == EventKind::Gap, "the oldest record held says records were lost");
    CHECK_EQ(held[0].count, 6u);
    CHECK_EQ(held[0].cursor, 6u);
    CHECK_EQ(held[1].cursor, 7u);
    CHECK_EQ(held[3].cursor, 9u);
}

TEST(aReaderThatDropsWhatItHeldReportsAGapOfItsOwn)
{
    std::vector<Event> held;
    for (std::uint64_t cursor = 1; cursor <= 6; ++cursor) {
        Event event = bell(1);
        event.cursor = cursor;
        Ubiquitty::Session::holdEvent(held, event, 4);
    }
    REQUIRE(held.size() == 4u);
    CHECK(held[0].kind == EventKind::Gap);
    CHECK_EQ(held[0].count, 3u);
    CHECK_EQ(held[0].cursor, 3u);
    CHECK_EQ(held[1].cursor, 4u);
    CHECK_EQ(held[3].cursor, 6u);

    // The mark of where a stream that skipped the backlog begins counts
    // nothing, so folding a record into it counts that record alone.
    std::vector<Event> fromLatest;
    Event position;
    position.kind = EventKind::Gap;
    position.cursor = 10;
    Ubiquitty::Session::holdEvent(fromLatest, position, 2);
    for (std::uint64_t cursor = 11; cursor <= 12; ++cursor) {
        Event event = bell(1);
        event.cursor = cursor;
        Ubiquitty::Session::holdEvent(fromLatest, event, 2);
    }
    REQUIRE(fromLatest.size() == 2u);
    CHECK(fromLatest[0].kind == EventKind::Gap);
    CHECK_EQ(fromLatest[0].count, 1u);
    CHECK_EQ(fromLatest[0].cursor, 11u);
    CHECK_EQ(fromLatest[1].cursor, 12u);
}

TEST(anUnknownWireKindIsRefused)
{
    CHECK(Ubiquitty::Session::eventKindFromRaw(12) == EventKind::CommandStarted);
    CHECK(Ubiquitty::Session::eventKindFromRaw(13) == EventKind::ScreenRead);
    CHECK_MSG(!Ubiquitty::Session::eventKindFromRaw(14), "a kind this build does not know is not read as one it does");
}

UBIQUITTY_TEST_MAIN()
