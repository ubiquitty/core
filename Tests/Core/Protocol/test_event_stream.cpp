// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

#include "Core/Protocol/Control/EventStream.h"
#include "Core/Protocol/Control/EventStreamReplay.h"
#include "Core/Protocol/Control/MethodRegistry.h"
#include "Core/Protocol/Sync/Messages.h"
#include "Core/Protocol/Sync/Wire.h"

#include <algorithm>
#include <array>
#include <limits>
#include <span>
#include <string>
#include <vector>

using namespace Ubiquitty;
using namespace Ubiquitty::Protocol::Control;
using Codec::JSON::Value;

namespace {

Session::Event exampleEvent(std::uint64_t cursor = 9)
{
    return { .cursor = cursor, .kind = Session::EventKind::CommandFinished, .sessionID = 7, .paneID = 8, .exitStatus = -17, .state = Session::EventProgramState::Blocked, .isAtPrompt = true, .count = std::numeric_limits<std::uint64_t>::max(), .text = std::string("a\0b", 3), .detail = "/work/\xCE\xBB" };
}

Value replacing(const Value& object, std::string_view key, Value value)
{
    auto replaced = Value::object();
    for (const auto& [name, field] : object.members()) {
        if (name != key)
            replaced.set(name, field);
    }
    replaced.set(std::string(key), std::move(value));
    return replaced;
}

/// The record after an `Events` body's kind.
std::vector<std::uint8_t> recordOf(const std::optional<std::vector<std::uint8_t>>& body)
{
    const auto head = body ? Protocol::Sync::decodeNotifyHead(*body) : std::nullopt;
    CHECK(head && head->kind == std::to_underlying(Protocol::Sync::NotifyKind::Events));
    return head ? std::vector<std::uint8_t>(head->record.begin(), head->record.end()) : std::vector<std::uint8_t> {};
}

/// One record's fields, written by hand so any one of them can be wrong.
struct RawRecord {
    std::uint64_t cursor = 9;
    std::uint8_t kind = std::to_underlying(Session::EventKind::CommandFinished);
    std::uint32_t session = 7;
    std::uint32_t pane = 8;
    std::int64_t exitStatus = -17;
    std::uint8_t state = std::to_underlying(Session::EventProgramState::Blocked);
    std::uint8_t atPrompt = 1;
    std::uint64_t count = 3;
    std::string text = "make";
    std::string detail = "/work";
};

std::vector<std::uint8_t> batch(const std::vector<RawRecord>& records)
{
    std::vector<std::uint8_t> bytes;
    Protocol::Sync::WireWriter writer(bytes);
    writer.varint(records.size());
    for (const auto& record : records) {
        writer.u64(record.cursor);
        writer.u8(record.kind);
        writer.u32(record.session);
        writer.u32(record.pane);
        writer.svarint(record.exitStatus);
        writer.u8(record.state);
        writer.u8(record.atPrompt);
        writer.u64(record.count);
        writer.utf8(record.text);
        writer.utf8(record.detail);
    }
    return bytes;
}

TEST(eventSubscriptionsNeverRoundOrAmbiguouslySpellTheirCursor)
{
    for (const std::uint64_t cursor : { std::uint64_t { 0 }, std::uint64_t { 1 } << 53, std::numeric_limits<std::uint64_t>::max() }) {
        const EventSubscription subscription { cursor, false };
        const auto request = eventSubscriptionRequest(subscription, "events.7");
        bool isJSON = false;
        const auto decoded = Codec::JSON::parse(request, isJSON);
        REQUIRE(isJSON);
        CHECK(decoded.stringOr("method") == "events.subscribe");
        CHECK(decoded.stringOr("id") == "events.7");
        REQUIRE(decoded.find("params"));
        CHECK(eventSubscription(*decoded.find("params")) == subscription);
    }
    CHECK(eventSubscriptionRequest({ 1, true }, "events.7").empty());
    CHECK(eventSubscriptionRequest({}, "").empty());
    CHECK(eventSubscriptionRequest({}, std::string(Protocol::Sync::maxFrameBodyBytes + 1, 'x')).empty());
    CHECK(eventSubscriptionRequest({}, std::string(Protocol::Sync::maxFrameBodyBytes / 6, '\x01')).empty());
    auto parameters = Value::object();
    parameters.set("fromLatest", Value::boolean(false));
    for (const auto spelling : { "", "+1", "-1", "01", " 1", "1 ", "1x", "18446744073709551616" }) {
        parameters = replacing(parameters, "after", Value::string(spelling));
        CHECK(!eventSubscription(parameters));
    }
    parameters = replacing(parameters, "after", Value::number(1));
    CHECK(!eventSubscription(parameters));
    parameters = replacing(parameters, "after", Value::string("0"));
    parameters = replacing(parameters, "fromLatest", Value::boolean(true));
    CHECK(eventSubscription(parameters) == (EventSubscription { 0, true }));
    parameters.set("extra", Value::null());
    CHECK(!eventSubscription(parameters));
}

TEST(aViewerEventSubscriptionStillNeedsControlAuthority)
{
    const auto* method = findSyncMethod("events.subscribe");
    REQUIRE(method);
    CHECK(method->scope == Protocol::Sync::Scope::Control);
    CHECK(method->target == CallTarget::Connection);
    auto parameters = Value::object();
    parameters.set("after", Value::string("0"));
    parameters.set("fromLatest", Value::boolean(false));
    CallerAuthority caller { .role = Protocol::Sync::Role::Viewer, .scopes = { Protocol::Sync::Scope::Observe }, .origin = CapabilityOrigin::Remote };
    const auto refusal = admitCall(method, parameters, caller);
    REQUIRE(refusal);
    CHECK(refusal->code == ErrorCode::Forbidden);
    caller.scopes.add(Protocol::Sync::Scope::Control);
    CHECK(!admitCall(method, parameters, caller));
    caller.role = Protocol::Sync::Role::Agent;
    CHECK(!admitCall(method, parameters, caller));
}

TEST(aFutureEventCursorNeverWrapsIntoReplay)
{
    Session::EventLog log(4);
    log.append(exampleEvent());
    log.append(exampleEvent());
    const auto future = log.since(std::numeric_limits<std::uint64_t>::max());
    CHECK(future.events.empty());
    CHECK_EQ(future.dropped, 0u);
}

TEST(everyEventFieldSurvivesItsRecordWithExactWideCounters)
{
    std::array<Session::Event, Session::lastEventKind + 1> events;
    for (std::size_t index = 0; index < events.size(); ++index) {
        events[index] = exampleEvent((std::uint64_t { 1 } << 53) + index + 17);
        events[index].kind = static_cast<Session::EventKind>(index);
        events[index].exitStatus = std::numeric_limits<std::int32_t>::min() + static_cast<std::int32_t>(index);
    }
    const auto decoded = decodeEvents(recordOf(eventsNotify(events)));
    REQUIRE(decoded);
    CHECK(std::ranges::equal(*decoded, events));
    const std::array final { exampleEvent(std::numeric_limits<std::uint64_t>::max()) };
    const auto atLimit = decodeEvents(recordOf(eventsNotify(final)));
    REQUIRE(atLimit);
    CHECK(atLimit->front() == final.front());
}

TEST(anEventBatchRefusesWholeBeforeExposingMalformedRecords)
{
    REQUIRE(decodeEvents(batch({ RawRecord {} })));
    const auto refuses = [](RawRecord record) { CHECK(!decodeEvents(batch({ std::move(record) }))); };
    RawRecord record;
    record.kind = Session::lastEventKind + 1;
    refuses(record);
    record = {};
    record.state = std::to_underlying(Session::EventProgramState::Error) + 1;
    refuses(record);
    record = {};
    record.exitStatus = std::int64_t { std::numeric_limits<std::int32_t>::max() } + 1;
    refuses(record);
    record.exitStatus = std::int64_t { std::numeric_limits<std::int32_t>::min() } - 1;
    refuses(record);
    record = {};
    record.atPrompt = 2;
    refuses(record);
    record = {};
    record.cursor = 0;
    refuses(record);
    record.kind = std::to_underlying(Session::EventKind::Gap);
    CHECK(decodeEvents(batch({ record })));
    for (const auto field : { 0, 1 }) {
        record = {};
        (field ? record.detail : record.text) = std::string(Session::maxEventTextLength + 1, 'x');
        refuses(record);
    }
    CHECK(!decodeEvents(batch({ RawRecord {}, RawRecord {} })));
    std::vector<RawRecord> tooMany(maxEventsPerNotify + 1);
    for (std::size_t index = 0; index < tooMany.size(); ++index)
        tooMany[index].cursor = index + 1;
    CHECK(!decodeEvents(batch(tooMany)));
    tooMany.pop_back();
    CHECK(decodeEvents(batch(tooMany)));
    CHECK(!decodeEvents(batch({})));
    const auto exact = batch({ RawRecord {} });
    for (std::size_t cut = 0; cut < exact.size(); ++cut)
        CHECK(!decodeEvents(std::span(exact).first(cut)));
    auto extended = exact;
    extended.push_back(0);
    CHECK(!decodeEvents(extended));
}

TEST(eventPresentationTextIsRepairedAndBoundedAndTheLargestBatchFitsAFrame)
{
    auto event = exampleEvent();
    event.text = std::string(Session::maxEventTextLength, '\xFF');
    event.detail = std::string(Session::maxEventTextLength, '\x01');
    const std::array events { event };
    const auto decoded = decodeEvents(recordOf(eventsNotify(events)));
    REQUIRE(decoded);
    REQUIRE(decoded->size() == 1);
    CHECK(decoded->front().text.size() <= Session::maxEventTextLength);
    CHECK(Unicode::wellFormedUTF8PrefixLength(decoded->front().text) == decoded->front().text.size());
    CHECK(decoded->front().detail == event.detail);
    CHECK(decoded->front().cursor == event.cursor && decoded->front().count == event.count);
    std::array<Session::Event, maxEventsPerNotify> largest;
    for (std::size_t index = 0; index < largest.size(); ++index) {
        largest[index] = event;
        largest[index].cursor += index;
        largest[index].text = event.detail;
    }
    const auto body = eventsNotify(largest);
    REQUIRE(body);
    CHECK(body->size() <= Protocol::Sync::maxFrameBodyBytes);
    CHECK(!eventsNotify({}));
}

TEST(aBlockedEventNotifyRetriesWithoutSkippingOrDuplicatingItsCursor)
{
    Session::EventLog log(8);
    for (int index = 0; index < 3; ++index)
        log.append(exampleEvent());
    EventStreamReplay replay;
    replay.begin({}, log.latestCursor());
    std::vector<std::uint64_t> received;
    std::size_t capacity = 0;
    const auto send = [&](const Session::Event& event) {
        if (!capacity)
            return false;
        --capacity;
        received.push_back(event.cursor);
        return true;
    };
    const auto admits = [](const Session::Event&) { return true; };
    CHECK_EQ(replay.service(log, admits, send), 0u);
    CHECK_EQ(replay.cursor(), 0u);
    capacity = 1;
    CHECK_EQ(replay.service(log, admits, send), 1u);
    CHECK_EQ(replay.cursor(), 1u);
    capacity = 2;
    CHECK_EQ(replay.service(log, admits, send), 2u);
    CHECK_EQ(replay.cursor(), 3u);
    CHECK_EQ(replay.service(log, admits, send), 0u);
    CHECK((received == std::vector<std::uint64_t> { 1, 2, 3 }));
}

TEST(eventReplayFiltersOtherPanesWithoutReportingThemAsLost)
{
    Session::EventLog log(8);
    for (int index = 0; index < 4; ++index) {
        auto event = exampleEvent();
        event.sessionID = index % 2 == 0 ? 7 : 8;
        log.append(std::move(event));
    }
    EventStreamReplay replay;
    replay.begin({}, log.latestCursor());
    std::vector<Session::Event> received;
    const auto admits = [](const Session::Event& event) { return event.sessionID == 7; };
    const auto send = [&](const Session::Event& event) { received.push_back(event); return true; };
    CHECK_EQ(replay.service(log, admits, send, 2), 1u);
    CHECK_EQ(replay.cursor(), 2u);
    CHECK(replay.isOwed(log.latestCursor()));
    CHECK_EQ(replay.service(log, admits, send, 2), 1u);
    CHECK_EQ(replay.cursor(), 4u);
    CHECK(!replay.isOwed(log.latestCursor()));
    REQUIRE(received.size() == 2);
    CHECK(received[0].cursor == 1 && received[1].cursor == 3);
    CHECK(std::ranges::all_of(received, [](const auto& event) { return event.sessionID == 7 && event.kind != Session::EventKind::Gap; }));
}

TEST(eventReplayReportsLogLossOnlyAfterItsGapWasAdmitted)
{
    Session::EventLog log(3);
    for (int index = 0; index < 7; ++index)
        log.append(exampleEvent());
    EventStreamReplay replay;
    replay.begin({}, log.latestCursor());
    const auto admits = [](const Session::Event&) { return true; };
    CHECK_EQ(replay.service(log, admits, [](const Session::Event&) { return false; }), 0u);
    CHECK_EQ(replay.cursor(), 0u);
    log.append(exampleEvent());
    std::vector<Session::Event> received;
    CHECK_EQ(replay.service(log, admits, [&](const Session::Event& event) { received.push_back(event); return true; }), 4u);
    REQUIRE(received.size() == 4);
    CHECK(received[0].kind == Session::EventKind::Gap && received[0].cursor == 5 && received[0].count == 5);
    CHECK(received[1].cursor == 6 && received[2].cursor == 7 && received[3].cursor == 8);
    CHECK_EQ(replay.cursor(), 8u);
    CHECK_EQ(replay.service(log, admits, [](const Session::Event&) { return true; }), 0u);
}

TEST(fromLatestMarksOneExactPositionBeforeNewEvents)
{
    Session::EventLog log(8);
    for (int index = 0; index < 3; ++index)
        log.append(exampleEvent());
    EventStreamReplay replay;
    replay.begin({ .after = 0, .isFromLatest = true }, log.latestCursor());
    log.append(exampleEvent());
    const auto admits = [](const Session::Event&) { return true; };
    CHECK_EQ(replay.service(log, admits, [](const Session::Event&) { return false; }), 0u);
    CHECK_EQ(replay.cursor(), 3u);
    std::vector<Session::Event> received;
    std::size_t capacity = 1;
    const auto send = [&](const Session::Event& event) {
        if (!capacity)
            return false;
        --capacity;
        received.push_back(event);
        return true;
    };
    CHECK_EQ(replay.service(log, admits, send), 1u);
    CHECK_EQ(replay.cursor(), 3u);
    capacity = 1;
    CHECK_EQ(replay.service(log, admits, send), 1u);
    REQUIRE(received.size() == 2);
    CHECK(received[0].kind == Session::EventKind::Gap && received[0].cursor == 3 && received[0].count == 0);
    CHECK(received[1].kind == Session::EventKind::CommandFinished && received[1].cursor == 4);
    replay.stop();
    log.append(exampleEvent());
    CHECK_EQ(replay.service(log, admits, [](const Session::Event&) { return true; }), 0u);
    CHECK_EQ(replay.cursor(), 4u);
}

TEST(oneReplayPassExaminesOnlyItsIndependentRecordBudget)
{
    Session::EventLog log(64);
    for (int index = 0; index < 64; ++index)
        log.append(exampleEvent());
    EventStreamReplay replay;
    replay.begin({}, log.latestCursor());
    std::size_t examined = 0;
    std::size_t sent = 0;
    const auto admits = [&](const Session::Event&) { ++examined; return true; };
    const auto send = [&](const Session::Event&) { ++sent; return true; };
    CHECK_EQ(replay.service(log, admits, send, std::numeric_limits<std::size_t>::max()), maxEventsPerNotify);
    CHECK_EQ(examined, maxEventsPerNotify);
    CHECK_EQ(sent, maxEventsPerNotify);
    CHECK_EQ(replay.cursor(), maxEventsPerNotify);
    CHECK_EQ(replay.service(log, admits, send, 0), 0u);
    CHECK_EQ(replay.cursor(), maxEventsPerNotify);
    CHECK_EQ(log.since(replay.cursor(), 1).events.size(), 1u);
}

} // namespace

UBIQUITTY_TEST_MAIN()
