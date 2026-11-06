// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"
#include "ScopedNothrowAllocationRefusal.h"

#include "Core/Codec/Base64.h"
#include "Core/Codec/JSON.h"
#include "Core/Protocol/Sync/ScreenPlane.h"
#include "Core/Session/BackgroundEvents.h"
#include "Core/Session/BackgroundRefreshState.h"
#include "Core/TerminalOperations.h"

#include <memory>
#include <new>

void* operator new(std::size_t size, const std::nothrow_t&) noexcept
{
    if (UbiquittyTest::ScopedNothrowAllocationRefusal::refuses(size))
        return nullptr;
    return ::operator new(size);
}

void operator delete(void* pointer, const std::nothrow_t&) noexcept { ::operator delete(pointer); }

namespace {

using namespace Ubiquitty;
using Codec::JSON::Value;
using Session::BackgroundEvents;
using Session::BackgroundRefreshState;
const std::string paneID = Session::backgroundPaneID(1, 2);
constexpr std::uint64_t now = 1000000;

std::string base64(std::span<const std::uint8_t> bytes)
{
    std::string result(Codec::base64EncodedLength(bytes.size()), '\0');
    // The codec reads the byte vector's object representation as characters.
    CHECK(Codec::base64Encode(std::span(reinterpret_cast<const char*>(bytes.data()), bytes.size()), result).has_value());
    return result;
}

Value metadata(const OSC::ProgramStatusStore& store = {})
{
    Protocol::Sync::PaneFacts facts;
    facts.rows = 24;
    facts.columns = 80;
    facts.programTitle = "Build host";
    std::vector<std::uint8_t> bytes;
    Protocol::Sync::encodePaneFacts(bytes, facts);
    Value pane = Value::object();
    pane.set("paneID", Value::string(paneID));
    pane.set("sessionID", Value::number(7));
    pane.set("facts", Value::string(base64(bytes)));
    Protocol::Sync::StatusRecords records;
    records.store = store;
    bytes.clear();
    CHECK_EQ(Protocol::Sync::encodeStatusFragment(bytes, records, 0), store.records.size());
    Value statuses = Value::array();
    statuses.push(Value::string(base64(bytes)));
    pane.set("status", std::move(statuses));
    return pane;
}

Value payload(const BackgroundEvents& journal, std::uint64_t after = 0, const OSC::ProgramStatusStore& store = {})
{
    const auto page = journal.since(after, now);
    Value result = Value::object();
    result.set("epoch", Value::string(std::to_string(journal.epoch())));
    result.set("inventory", Value::string("1"));
    result.set("cursor", Value::string(std::to_string(page.cursor)));
    result.set("hasMore", Value::boolean(page.hasMore));
    result.set("gap", Value::boolean(page.hasGap));
    result.set("hostNowUnixMilliseconds", Value::string(std::to_string(now)));
    result.set("nextPaneOffset", Value::number(0));
    result.set("panesOmitted", Value::boolean(false));
    Value events = Value::array();
    for (const auto& event : page.events) {
        events.push(Session::backgroundEventJSON(event));
    }
    result.set("events", std::move(events));
    Value panes = Value::array();
    panes.push(metadata(store));
    result.set("panes", std::move(panes));
    return result;
}

Value replacing(const Value& value, const std::string& key, Value replacement)
{
    Value result = Value::object();
    for (const auto& member : value.members()) {
        result.set(member.first, member.first == key ? replacement : member.second);
    }
    return result;
}

std::string response(BackgroundRefreshState& state, Value result)
{
    bool valid = false;
    const auto request = Codec::JSON::parse(state.makeRequest(), valid);
    CHECK(valid);
    Value envelope = Value::object();
    envelope.set("jsonrpc", Value::string("2.0"));
    envelope.set("id", Value::string(request.stringOr("id")));
    envelope.set("result", std::move(result));
    return Codec::JSON::serialize(envelope);
}

} // namespace

TEST(refreshPersistsPendingDeliveryAndCursorTogether)
{
    BackgroundEvents journal(42);
    journal.appendNotification(paneID, 7, { .title = "Question", .body = "Choose a destination", .identifier = {} }, now);
    BackgroundRefreshState state;
    CHECK(state.applyResponse(response(state, payload(journal))));
    CHECK_EQ(state.pendingNotificationCount(), 1u);
    const auto notification = state.notificationAt(0);
    CHECK_STR_EQ(notification.identifier, "42:1");
    BackgroundRefreshState relaunched;
    CHECK(relaunched.restoreRecord(state.saveRecord()));
    CHECK_EQ(relaunched.pendingNotificationCount(), 0u);
    CHECK(relaunched.applyResponse(response(relaunched, payload(journal, 1))));
    CHECK_EQ(relaunched.pendingNotificationCount(), 1u);
    CHECK_STR_EQ(relaunched.notificationAt(0).identifier, notification.identifier);
    relaunched.markDelivered(notification.identifier);
    BackgroundRefreshState acknowledged;
    CHECK(acknowledged.restoreRecord(relaunched.saveRecord()));
    CHECK_EQ(acknowledged.pendingNotificationCount(), 0u);
    CHECK(acknowledged.applyResponse(response(acknowledged, payload(journal, 1))));
    CHECK_EQ(acknowledged.pendingNotificationCount(), 0u);
}

TEST(refreshRejectsWrongCorrelationAndRepeatedReply)
{
    BackgroundEvents journal(42);
    journal.appendNotification(paneID, 7, { .title = "Build", .body = "Done", .identifier = {} }, now);
    BackgroundRefreshState state;
    const auto first = response(state, payload(journal));
    const auto second = response(state, payload(journal));
    CHECK(!state.applyResponse(first));
    CHECK_EQ(state.pendingNotificationCount(), 0u);
    CHECK(state.applyResponse(second));
    CHECK(!state.applyResponse(second));
    CHECK_EQ(state.pendingNotificationCount(), 1u);
}

TEST(refreshMalformedMetadataDoesNotAdvanceTheCursor)
{
    BackgroundEvents journal(42);
    journal.appendNotification(paneID, 7, { .title = "Build", .body = "Done", .identifier = {} }, now);
    BackgroundRefreshState state;
    auto broken = payload(journal);
    broken.set("panes", Value::array());
    CHECK(!state.applyResponse(response(state, std::move(broken))));
    CHECK_EQ(state.pendingNotificationCount(), 0u);
    CHECK(state.applyResponse(response(state, payload(journal))));
    CHECK_EQ(state.pendingNotificationCount(), 1u);
}

TEST(refreshRejectsOversizedAndCorruptPersistedRecordsWithoutLosingState)
{
    BackgroundEvents journal(42);
    journal.appendNotification(paneID, 7, { .title = "Build", .body = "Done", .identifier = {} }, now);
    BackgroundRefreshState state;
    CHECK(state.applyResponse(response(state, payload(journal))));
    const std::string saved = state.saveRecord();
    CHECK(!state.restoreRecord(saved.substr(0, saved.size() - 1)));
    CHECK(!state.restoreRecord(std::string(BackgroundRefreshState::maxRecordBytes + 1, ' ')));
    CHECK_STR_EQ(state.saveRecord(), saved);
}

TEST(refreshSuppressesPendingQuestionsResolvedBeforePresentation)
{
    BackgroundEvents journal(42);
    OSC::ProgramStatusStore store;
    OSC::ProgramStatus question;
    question.id = "agent";
    question.state = OSC::ProgramState::Blocked;
    question.kind = OSC::ProgramBlockKind::Question;
    question.title = "Which target?";
    store.records.push_back(question);
    journal.updateQuestions(paneID, 7, {}, store.records, now);
    BackgroundRefreshState state;
    CHECK(state.applyResponse(response(state, payload(journal, 0, store))));
    CHECK_EQ(state.pendingNotificationCount(), 1u);
    const auto previous = store.records;
    store.records.front().state = OSC::ProgramState::Working;
    store.records.front().kind = OSC::ProgramBlockKind::None;
    journal.updateQuestions(paneID, 7, previous, store.records, now);
    CHECK(state.applyResponse(response(state, payload(journal, 1, store))));
    CHECK_EQ(state.pendingNotificationCount(), 0u);
    CHECK(!state.paneAt(0).statusText.empty());
}

TEST(refreshHostEpochChangesRetireOldPendingDeliveries)
{
    BackgroundEvents firstHost(42);
    firstHost.appendNotification(paneID, 7, { .title = "Old", .body = "Old", .identifier = {} }, now);
    BackgroundRefreshState state;
    CHECK(state.applyResponse(response(state, payload(firstHost))));
    BackgroundEvents replacementHost(43);
    replacementHost.appendNotification(paneID, 7, { .title = "New", .body = "New", .identifier = {} }, now);
    CHECK(state.applyResponse(response(state, payload(replacementHost))));
    CHECK_EQ(state.pendingNotificationCount(), 1u);
    CHECK_STR_EQ(state.notificationAt(0).identifier, "43:1");
}

TEST(refreshPaginatesRetainedEventsAndReportsGaps)
{
    BackgroundEvents journal(42);
    for (std::size_t index = 0; index < BackgroundEvents::capacity + 10; ++index) {
        journal.appendNotification(paneID, 7, { .title = "Event", .body = std::to_string(index), .identifier = {} }, now);
    }
    BackgroundRefreshState state;
    std::uint64_t after = 0;
    std::size_t received = 0;
    for (std::size_t page = 0; page < 4; ++page) {
        const auto events = journal.since(after, now);
        CHECK(state.applyResponse(response(state, payload(journal, after))));
        CHECK(state.hadRetentionGap());
        received += state.pendingNotificationCount();
        while (state.pendingNotificationCount()) {
            state.markDelivered(state.notificationAt(0).identifier);
        }
        after = events.cursor;
    }
    CHECK_EQ(received, BackgroundEvents::capacity);
}

TEST(refreshRejectsCursorRegression)
{
    BackgroundEvents journal(42);
    journal.appendNotification(paneID, 7, { .title = "Event", .body = "One", .identifier = {} }, now);
    BackgroundRefreshState state;
    CHECK(state.applyResponse(response(state, payload(journal))));
    BackgroundEvents regressed(42);
    CHECK(!state.applyResponse(response(state, payload(regressed))));
    CHECK_EQ(state.pendingNotificationCount(), 1u);
}

TEST(refreshCachesVerifiedCheckpointWithoutAttaching)
{
    auto terminal = std::make_unique<Terminal>();
    processBytes(terminal.get(), "cached picture");
    terminal->titleUTF8 = std::string(123, 't');
    Protocol::Sync::ScreenPublisher publisher;
    CHECK(publisher.capture(*terminal));
    Protocol::Sync::AttachmentBaseline baseline;
    std::vector<std::uint8_t> checkpoint;
    CHECK(publisher.checkpoint(*terminal, 1, checkpoint, baseline));
    const auto digest = Protocol::Sync::digestOf(checkpoint);
    BackgroundEvents journal(42);
    BackgroundRefreshState state;
    CHECK(state.applyResponse(response(state, payload(journal))));
    CHECK(state.hasMore());
    auto result = payload(journal);
    Value screen = Value::object();
    screen.set("paneID", Value::string(paneID));
    screen.set("revision", Value::string(Session::backgroundPaneID(digest.high, digest.low)));
    screen.set("checkpoint", Value::string(base64(checkpoint)));
    result.set("screen", std::move(screen));
    const auto answer = response(state, std::move(result));
    const auto saved = state.saveRecord();
    for (const auto refusedSize : { sizeof(Terminal), terminal->titleUTF8.size() + 1 }) {
        const auto refusals = UbiquittyTest::ScopedNothrowAllocationRefusal::refusalCount();
        {
            UbiquittyTest::ScopedNothrowAllocationRefusal refusal(refusedSize);
            CHECK(!state.applyResponse(answer));
            CHECK_EQ(UbiquittyTest::ScopedNothrowAllocationRefusal::refusalCount(), refusals + 1);
            CHECK_STR_EQ(state.saveRecord(), saved);
        }
    }
    CHECK(state.applyResponse(answer));
    CHECK(!state.hasMore());
    CHECK(state.paneAt(0).cachedScreenText.find("cached picture") != std::string::npos);
    BackgroundRefreshState relaunched;
    CHECK(relaunched.restoreRecord(state.saveRecord()));
    CHECK_STR_EQ(relaunched.paneAt(0).cachedScreenText, state.paneAt(0).cachedScreenText);
}

TEST(refreshRejectsTamperedScreenWithoutDiscardingPendingEvents)
{
    BackgroundEvents journal(42);
    journal.appendNotification(paneID, 7, { .title = "Event", .body = "One", .identifier = {} }, now);
    BackgroundRefreshState state;
    CHECK(state.applyResponse(response(state, payload(journal))));
    const auto saved = state.saveRecord();
    auto result = payload(journal, 1);
    Value screen = Value::object();
    screen.set("paneID", Value::string(paneID));
    screen.set("revision", Value::string(paneID));
    screen.set("checkpoint", Value::string("AAAA"));
    result.set("screen", std::move(screen));
    CHECK(!state.applyResponse(response(state, std::move(result))));
    CHECK_STR_EQ(state.saveRecord(), saved);
}

TEST(refreshHoldsQuestionUntilCurrentMetadataArrives)
{
    BackgroundEvents journal(42);
    OSC::ProgramStatusStore store;
    OSC::ProgramStatus question;
    question.id = "agent";
    question.state = OSC::ProgramState::Blocked;
    question.kind = OSC::ProgramBlockKind::Question;
    question.title = "Which target?";
    store.records.push_back(question);
    journal.updateQuestions(paneID, 7, {}, store.records, now);
    BackgroundRefreshState state;
    auto first = replacing(payload(journal, 0, store), "panes", Value::array());
    first = replacing(first, "nextPaneOffset", Value::number(2));
    CHECK(state.applyResponse(response(state, std::move(first))));
    CHECK_EQ(state.pendingNotificationCount(), 0u);
    BackgroundRefreshState relaunched;
    CHECK(relaunched.restoreRecord(state.saveRecord()));
    CHECK(relaunched.applyResponse(response(relaunched, payload(journal, 1, store))));
    CHECK_EQ(relaunched.pendingNotificationCount(), 1u);
    CHECK_STR_EQ(relaunched.notificationAt(0).title, question.title);
}

TEST(refreshDropsSupersededQuestionWithSameStatusID)
{
    BackgroundEvents journal(42);
    OSC::ProgramStatusStore store;
    OSC::ProgramStatus question;
    question.id = "agent";
    question.state = OSC::ProgramState::Blocked;
    question.kind = OSC::ProgramBlockKind::Question;
    question.title = "Old question";
    store.records.push_back(question);
    journal.updateQuestions(paneID, 7, {}, store.records, now);
    BackgroundRefreshState state;
    CHECK(state.applyResponse(response(state, payload(journal, 0, store))));
    const auto previous = store.records;
    store.records.front().title = "New question";
    journal.updateQuestions(paneID, 7, previous, store.records, now);
    CHECK(state.applyResponse(response(state, payload(journal, 1, store))));
    CHECK_EQ(state.pendingNotificationCount(), 1u);
    CHECK_STR_EQ(state.notificationAt(0).title, "New question");
    CHECK_STR_EQ(state.notificationAt(0).identifier, "42:2");
}

TEST(refreshRetiresDeletedPaneQuestionAfterCompleteInventory)
{
    BackgroundEvents journal(42);
    OSC::ProgramStatusStore store;
    OSC::ProgramStatus question;
    question.id = "agent";
    question.state = OSC::ProgramState::Blocked;
    question.kind = OSC::ProgramBlockKind::Question;
    store.records.push_back(question);
    journal.updateQuestions(paneID, 7, {}, store.records, now);
    BackgroundRefreshState state;
    CHECK(state.applyResponse(response(state, payload(journal, 0, store))));
    state.beginRefresh();
    auto absent = replacing(payload(journal, 1), "panes", Value::array());
    CHECK(state.applyResponse(response(state, replacing(absent, "panesOmitted", Value::boolean(true)))));
    CHECK_EQ(state.paneCount(), 1u);
    CHECK_EQ(state.pendingNotificationCount(), 0u);
    CHECK(state.applyResponse(response(state, absent)));
    CHECK_EQ(state.paneCount(), 0u);
    BackgroundRefreshState restored;
    CHECK(restored.restoreRecord(state.saveRecord()));
    CHECK(restored.applyResponse(response(restored, payload(journal, 1, store))));
    CHECK_EQ(restored.pendingNotificationCount(), 0u);
}

TEST(refreshRejectsInconsistentPersistedEpochAndOversizedInventory)
{
    BackgroundRefreshState state;
    bool valid = false;
    auto record = Codec::JSON::parse(state.saveRecord(), valid);
    CHECK(valid);
    CHECK(!state.restoreRecord(Codec::JSON::serialize(replacing(record, "cursor", Value::string("1")))));
    CHECK(!state.restoreRecord(Codec::JSON::serialize(replacing(record, "epoch", Value::string("0")))));
    Value seen = Value::array();
    for (std::size_t index = 0; index <= BackgroundRefreshState::maxSeenPanes; ++index) {
        seen.push(Value::string(Session::backgroundPaneID(1, index)));
    }
    CHECK(!state.restoreRecord(Codec::JSON::serialize(replacing(record, "seen", std::move(seen)))));
}

TEST(refreshNamedNotificationsReplaceAcrossPagesAndPreserveOccasion)
{
    BackgroundEvents journal(42);
    journal.appendNotification(paneID, 7, { .title = {}, .body = "Old", .identifier = "build", .occasion = NotificationOccasion::Unfocused }, now);
    BackgroundRefreshState state;
    CHECK(state.applyResponse(response(state, payload(journal))));
    const auto identifier = state.notificationAt(0).identifier;
    CHECK(state.notificationAt(0).occasion == NotificationOccasion::Unfocused);
    for (std::size_t index = 0; index < 64; ++index) {
        journal.appendNotification(paneID, 7, { .title = {}, .body = "Intervening", .identifier = {} }, now);
    }
    journal.appendNotification(paneID, 7, { .title = {}, .body = "New", .identifier = "build", .occasion = NotificationOccasion::Invisible }, now);
    CHECK(state.applyResponse(response(state, payload(journal, 1))));
    CHECK_EQ(state.pendingNotificationCount(), 0u);
    CHECK(state.applyResponse(response(state, payload(journal, 65))));
    CHECK_EQ(state.pendingNotificationCount(), 65u);
    const auto updated = state.notificationAt(64);
    CHECK_STR_EQ(updated.identifier, identifier);
    CHECK_STR_EQ(updated.body, "New");
    CHECK(updated.occasion == NotificationOccasion::Invisible);
}

TEST(refreshMakesProgressWhenSuspensionAndNewEventsExceedPendingCapacity)
{
    BackgroundEvents journal(42);
    BackgroundRefreshState state;
    for (std::size_t index = 0; index < 256; ++index) {
        journal.appendNotification(paneID, 7, { .title = {}, .body = "Before suspension", .identifier = {} }, now);
    }
    for (std::uint64_t after : { 0, 64, 128 }) {
        CHECK(state.applyResponse(response(state, payload(journal, after))));
        CHECK_EQ(state.pendingNotificationCount(), 0u);
    }
    BackgroundRefreshState relaunched;
    CHECK(relaunched.restoreRecord(state.saveRecord()));
    for (std::size_t index = 0; index < 256; ++index) {
        journal.appendNotification(paneID, 7, { .title = {}, .body = "While suspended", .identifier = {} }, now);
    }
    for (std::uint64_t after : { 192, 320, 384, 448 }) {
        CHECK(relaunched.applyResponse(response(relaunched, payload(journal, after))));
    }
    CHECK_EQ(relaunched.pendingNotificationCount(), 256u);
    CHECK(relaunched.hadRetentionGap());
    CHECK_STR_EQ(relaunched.notificationAt(0).body, "While suspended");
    CHECK_STR_EQ(relaunched.notificationAt(255).identifier, "42:512");
}

TEST(refreshRestartsAnInterruptedInventoryWithoutKeepingRemovedPanes)
{
    BackgroundEvents journal(42);
    BackgroundRefreshState state;
    CHECK(state.applyResponse(response(state, payload(journal))));
    CHECK_EQ(state.paneCount(), 1u);
    auto first = replacing(payload(journal), "nextPaneOffset", Value::number(33));
    CHECK(state.applyResponse(response(state, std::move(first))));
    BackgroundRefreshState restored;
    CHECK(restored.restoreRecord(state.saveRecord()));
    bool valid = false;
    const auto request = Codec::JSON::parse(restored.makeRequest(), valid);
    REQUIRE(valid);
    CHECK_STR_EQ(request.find("params")->stringOr("inventory"), "1");
    CHECK_EQ(request.find("params")->numberOr("paneOffset"), 33);

    const std::string replacementID = Session::backgroundPaneID(1, 99);
    Value panes = Value::array();
    panes.push(replacing(metadata(), "paneID", Value::string(replacementID)));
    auto restarted = replacing(payload(journal), "inventory", Value::string("2"));
    restarted = replacing(restarted, "panes", std::move(panes));
    CHECK(restored.applyResponse(response(restored, std::move(restarted))));
    CHECK_EQ(restored.paneCount(), 1u);
    CHECK_STR_EQ(restored.paneAt(0).paneID, replacementID);
    const auto finalRequest = Codec::JSON::parse(restored.makeRequest(), valid);
    REQUIRE(valid);
    CHECK_STR_EQ(finalRequest.find("params")->stringOr("inventory"), "2");
    CHECK_EQ(finalRequest.find("params")->numberOr("paneOffset"), 0);
}

UBIQUITTY_TEST_MAIN()
