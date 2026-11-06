// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Session/BackgroundRefreshState.h"

#include "Core/App/ProgramActivity.h"
#include "Core/Codec/Base64.h"
#include "Core/Codec/JSON.h"
#include "Core/Protocol/Sync/Connection.h"
#include "Core/Protocol/Sync/Envelope.h"
#include "Core/Protocol/Sync/Hash.h"
#include "Core/Protocol/Sync/ScreenPlane.h"
#include "Core/Screen/Addressing.h"
#include "Core/Screen/Scrollback.h"
#include "Core/Session/BackgroundEvents.h"
#include "Core/Terminal.h"
#include "Core/Unicode/UTF8Repair.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <utility>

namespace Ubiquitty::Session {

namespace {

using Codec::JSON::Value;
using Protocol::Sync::StatusAssembly;

bool uniqueMembers(const Value& value)
{
    if (!value.isObject()) {
        return false;
    }
    std::set<std::string_view> keys;
    for (const auto& member : value.members()) {
        if (!keys.insert(member.first).second) {
            return false;
        }
    }
    return true;
}

bool stringField(const Value& value, const std::string& key, std::string& output, std::size_t limit)
{
    const auto* field = value.find(key);
    if (!field || !field->isString() || field->asString().size() > limit
        || Unicode::wellFormedUTF8PrefixLength(field->asString()) != field->asString().size()) {
        return false;
    }
    output = field->asString();
    return true;
}

bool unsignedField(const Value& value, const std::string& key, std::uint64_t& output)
{
    std::string text;
    if (!stringField(value, key, text, 20)) {
        return false;
    }
    const auto number = backgroundSequence(text);
    if (!number) {
        return false;
    }
    output = *number;
    return true;
}

std::optional<std::uint32_t> integerField(const Value& value, const std::string& key, std::uint32_t maximum)
{
    const auto* field = value.find(key);
    if (!field || !field->isNumber() || !std::isfinite(field->asNumber()) || field->asNumber() < 0
        || field->asNumber() > maximum || std::floor(field->asNumber()) != field->asNumber()) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(field->asNumber());
}

std::string encodedBytes(std::span<const std::uint8_t> bytes)
{
    std::string encoded(Codec::base64EncodedLength(bytes.size()), '\0');
    // Base64 reads the object representation of the byte span as characters.
    const auto characters = std::span(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (!Codec::base64Encode(characters, encoded)) {
        return {};
    }
    return encoded;
}

std::optional<std::vector<std::uint8_t>> decodedBytes(const Value& field, std::size_t limit)
{
    if (!field.isString() || field.asString().size() > Codec::base64EncodedLength(limit)) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> bytes((field.asString().size() / 4 + 1) * 3);
    // The codec writes character bytes into the vector's object representation.
    const auto decoded = Codec::base64Decode(field.asString(), std::span(reinterpret_cast<char*>(bytes.data()), bytes.size()));
    if (!decoded || decoded->size() > limit) {
        return std::nullopt;
    }
    bytes.resize(decoded->size());
    return bytes;
}

std::string checkpointRevision(std::span<const std::uint8_t> bytes)
{
    const auto digest = Protocol::Sync::digestOf(bytes);
    return backgroundPaneID(digest.high, digest.low);
}

bool applyScreen(BackgroundRefreshPane& pane, const Value& screen)
{
    std::string paneID;
    std::string revision;
    const auto* encoded = screen.find("checkpoint");
    if (!uniqueMembers(screen) || !stringField(screen, "paneID", paneID, 32) || paneID != pane.paneID
        || !stringField(screen, "revision", revision, 32) || !isValidBackgroundPaneID(revision) || !encoded) {
        return false;
    }
    auto bytes = decodedBytes(*encoded, BackgroundRefreshState::maxCheckpointBytes);
    if (!bytes || bytes->size() < 17 || checkpointRevision(*bytes) != revision) {
        return false;
    }
    const auto rows = static_cast<std::uint32_t>((*bytes)[12]) | (static_cast<std::uint32_t>((*bytes)[13]) << 8);
    const auto columns = static_cast<std::uint32_t>((*bytes)[14]) | (static_cast<std::uint32_t>((*bytes)[15]) << 8);
    if (!rows || !columns || rows * columns > 32768) {
        return false;
    }
    auto terminal = std::unique_ptr<Terminal>(new (std::nothrow) Terminal());
    if (!terminal) {
        return false;
    }
    Protocol::Sync::ScreenReplica replica;
    const auto policy = Protocol::Sync::measuredClientPolicy();
    if (Protocol::Sync::applyCheckpoint(*terminal, *bytes, replica,
            { .maxDecodedScreenCellBytes = policy.maxDecodedScreenCellBytes, .maxStorageCost = policy.maxCheckpointStorageCost })
        != Protocol::Sync::ApplyOutcome::Applied) {
        return false;
    }
    std::string preview;
    const int first = Screen::scrollbackCount(terminal.get());
    for (int row = 0; row < terminal->rows; ++row) {
        const std::string line = Screen::absoluteRowText(*terminal, first + row, terminal->columns);
        if (preview.size() + line.size() + 1 > 64 * 1024) {
            break;
        }
        preview += line;
        preview += '\n';
    }
    pane.cachedScreenText = std::move(preview);
    pane.checkpoint = std::move(*bytes);
    pane.screenRevision = std::move(revision);
    return true;
}

std::optional<BackgroundRefreshPane> decodePane(const Value& value)
{
    BackgroundRefreshPane pane;
    const auto sessionID = integerField(value, "sessionID", std::numeric_limits<std::uint32_t>::max());
    const auto* facts = value.find("facts");
    const auto* statuses = value.find("status");
    if (!uniqueMembers(value) || !stringField(value, "paneID", pane.paneID, 32) || !isValidBackgroundPaneID(pane.paneID)
        || !sessionID || !*sessionID || !facts || !statuses || !statuses->isArray() || statuses->items().empty() || statuses->items().size() > 256) {
        return std::nullopt;
    }
    const auto factBytes = decodedBytes(*facts, BackgroundRefreshState::maxResponseBytes);
    const auto decodedFacts = factBytes ? Protocol::Sync::decodePaneFacts(*factBytes) : std::nullopt;
    if (!decodedFacts) {
        return std::nullopt;
    }
    StatusAssembly assembly;
    auto outcome = StatusAssembly::Outcome::Partial;
    for (const auto& encoded : statuses->items()) {
        const auto bytes = decodedBytes(encoded, BackgroundRefreshState::maxResponseBytes);
        auto fragment = bytes ? Protocol::Sync::decodeStatusFragment(*bytes) : std::nullopt;
        if (!fragment || outcome == StatusAssembly::Outcome::Whole) {
            return std::nullopt;
        }
        outcome = assembly.accept(std::move(*fragment));
        if (outcome == StatusAssembly::Outcome::Refused) {
            return std::nullopt;
        }
    }
    if (outcome != StatusAssembly::Outcome::Whole) {
        return std::nullopt;
    }
    pane.sessionID = *sessionID;
    pane.facts = std::move(*decodedFacts);
    pane.records = assembly.take();
    pane.title = App::visibleActivityText(pane.facts.programTitle.empty() ? pane.facts.processChain : pane.facts.programTitle);
    pane.statusText = App::programActivityLabel(pane.records.store);
    if (pane.statusText.empty() && pane.facts.indicator) {
        pane.statusText = App::visibleActivityText(pane.facts.indicator->label);
    }
    if (const auto* screen = value.find("screen"); screen && !applyScreen(pane, *screen)) {
        return std::nullopt;
    }
    return pane;
}

Value encodePane(const BackgroundRefreshPane& pane)
{
    Value value = Value::object();
    value.set("paneID", Value::string(pane.paneID));
    value.set("sessionID", Value::number(pane.sessionID));
    std::vector<std::uint8_t> bytes;
    Protocol::Sync::encodePaneFacts(bytes, pane.facts);
    value.set("facts", Value::string(encodedBytes(bytes)));
    Value statuses = Value::array();
    std::size_t first = 0;
    do {
        bytes.clear();
        first = Protocol::Sync::encodeStatusFragment(bytes, pane.records, first);
        statuses.push(Value::string(encodedBytes(bytes)));
    } while (first < pane.records.store.records.size());
    value.set("status", std::move(statuses));
    if (!pane.checkpoint.empty()) {
        Value screen = Value::object();
        screen.set("paneID", Value::string(pane.paneID));
        screen.set("revision", Value::string(pane.screenRevision));
        screen.set("checkpoint", Value::string(encodedBytes(pane.checkpoint)));
        value.set("screen", std::move(screen));
    }
    return value;
}

std::optional<BackgroundRefreshNotification> decodeEvent(const Value& value, const std::string& epoch)
{
    BackgroundRefreshNotification event;
    const auto occasion = backgroundNotificationOccasion(value.stringOr("occasion"));
    const auto sessionID = integerField(value, "sessionID", std::numeric_limits<std::uint32_t>::max());
    if (!uniqueMembers(value) || !unsignedField(value, "sequence", event.sequence) || !event.sequence
        || !stringField(value, "paneID", event.paneID, 32) || !isValidBackgroundPaneID(event.paneID)
        || !sessionID || !*sessionID || !stringField(value, "kind", event.kind, 12)
        || (event.kind != "notification" && event.kind != "question")
        || !stringField(value, "title", event.title, BackgroundEvents::titleCapacity) || !stringField(value, "body", event.body, BackgroundEvents::bodyCapacity)
        || !stringField(value, "statusID", event.statusID, BackgroundEvents::statusCapacity)
        || !stringField(value, "notificationID", event.notificationID, 32) || !occasion
        || (event.kind == "question" && (event.statusID.empty() || !event.notificationID.empty()))
        || (event.kind == "notification" && !event.statusID.empty())
        || !unsignedField(value, "createdUnixMilliseconds", event.createdUnixMilliseconds)) {
        return std::nullopt;
    }
    event.sessionID = *sessionID;
    event.identifier = epoch + ":" + std::to_string(event.sequence);
    if (!event.notificationID.empty()) {
        event.identifier = epoch + ":" + event.paneID + ":program:" + event.notificationID;
    }
    event.occasion = *occasion;
    event.title = App::visibleActivityText(event.title);
    event.body = App::visibleActivityText(event.body);
    return event;
}

Value encodeEvent(const BackgroundRefreshNotification& event)
{
    Value value = Value::object();
    value.set("sequence", Value::string(std::to_string(event.sequence)));
    value.set("paneID", Value::string(event.paneID));
    value.set("sessionID", Value::number(event.sessionID));
    value.set("kind", Value::string(event.kind));
    value.set("title", Value::string(event.title));
    value.set("body", Value::string(event.body));
    value.set("statusID", Value::string(event.statusID));
    value.set("notificationID", Value::string(event.notificationID));
    value.set("occasion", Value::string(std::string(backgroundNotificationOccasionName(event.occasion))));
    value.set("createdUnixMilliseconds", Value::string(std::to_string(event.createdUnixMilliseconds)));
    return value;
}

bool isResolved(const BackgroundRefreshNotification& event, const BackgroundRefreshPane& pane)
{
    if (event.kind != "question") {
        return false;
    }
    const auto& records = pane.records.store.records;
    const auto record = std::ranges::find_if(records, [&](const auto& candidate) { return candidate.id.view() == event.statusID; });
    return record == records.end() || record->state != OSC::ProgramState::Blocked
        || (record->kind != OSC::ProgramBlockKind::Question && record->kind != OSC::ProgramBlockKind::Permission && record->kind != OSC::ProgramBlockKind::Auth)
        || App::visibleActivityText(backgroundEventText(record->title, BackgroundEvents::titleCapacity)) != event.title
        || App::visibleActivityText(backgroundEventText(record->message, BackgroundEvents::bodyCapacity)) != event.body;
}

} // namespace

void BackgroundRefreshState::beginRefresh()
{
    m_didAttemptScreen = false;
    m_requestID.clear();
    m_error.clear();
    m_hadRetentionGap = false;
    for (auto& event : m_pending) {
        event.isReady = false;
    }
}

std::string BackgroundRefreshState::makeRequest(const std::string& screenPaneID)
{
    m_requestID = "refresh-" + std::to_string(++m_requestNumber);
    m_requestedScreenPane.clear();
    Value parameters = Value::object();
    if (!m_epoch.empty()) {
        parameters.set("epoch", Value::string(m_epoch));
    }
    parameters.set("after", Value::string(std::to_string(m_cursor)));
    parameters.set("paneOffset", Value::number(static_cast<double>(m_nextPaneOffset)));
    if (!m_inventory.empty()) {
        parameters.set("inventory", Value::string(m_inventory));
    }
    if (!m_didAttemptScreen) {
        const std::string chosen = screenPaneID.empty() && !m_panes.empty() ? m_panes.front().paneID : screenPaneID;
        if (isValidBackgroundPaneID(chosen)) {
            m_requestedScreenPane = chosen;
            parameters.set("screenPane", Value::string(chosen));
            const auto found = std::ranges::find(m_panes, chosen, &BackgroundRefreshPane::paneID);
            if (found != m_panes.end() && !found->screenRevision.empty()) {
                parameters.set("screenRevision", Value::string(found->screenRevision));
            }
        }
    }
    Value request = Value::object();
    request.set("jsonrpc", Value::string("2.0"));
    request.set("id", Value::string(m_requestID));
    request.set("method", Value::string("host.refresh"));
    request.set("params", std::move(parameters));
    return Codec::JSON::serialize(request);
}

bool BackgroundRefreshState::applyResponse(const std::string& response)
{
    const auto refuse = [this](const char* reason) {
        m_error = reason;
        return false;
    };
    if (response.size() > maxResponseBytes || m_requestID.empty()) {
        return refuse("The background refresh response is too large or unsolicited.");
    }
    bool valid = false;
    const Value envelope = Codec::JSON::parse(response, valid);
    if (!valid || !uniqueMembers(envelope) || envelope.stringOr("jsonrpc") != "2.0" || envelope.stringOr("id") != m_requestID) {
        return refuse("The host returned an invalid background refresh response.");
    }
    const auto* result = envelope.find("result");
    if (!result || envelope.find("error") || !uniqueMembers(*result)) {
        return refuse("The host could not provide background refresh data.");
    }
    std::string epoch;
    std::string inventory;
    std::uint64_t cursor = 0;
    std::uint64_t now = 0;
    const auto offset = integerField(*result, "nextPaneOffset", 100001);
    const auto* more = result->find("hasMore");
    const auto* gap = result->find("gap");
    const auto* events = result->find("events");
    const auto* panes = result->find("panes");
    const auto* omitted = result->find("panesOmitted");
    if (!stringField(*result, "epoch", epoch, 20) || !backgroundSequence(epoch) || epoch == "0"
        || !stringField(*result, "inventory", inventory, 20) || !backgroundSequence(inventory) || inventory == "0"
        || !unsignedField(*result, "cursor", cursor) || !unsignedField(*result, "hostNowUnixMilliseconds", now)
        || !offset || !more || !more->isBool() || !gap || !gap->isBool()
        || !events || !events->isArray() || events->items().size() > 64
        || !panes || !panes->isArray() || panes->items().size() > 32 || !omitted || !omitted->isBool()) {
        return refuse("The host returned malformed background refresh data.");
    }
    BackgroundRefreshState next = *this;
    if (epoch != m_epoch) {
        next.m_epoch = epoch;
        next.m_cursor = 0;
        next.m_pending.clear();
        next.m_panes.clear();
        next.m_seenPanes.clear();
        next.m_isMetadataIncomplete = false;
    }
    if (inventory != next.m_inventory) {
        next.m_seenPanes.clear();
        next.m_isMetadataIncomplete = false;
        next.m_inventory = inventory;
    }
    if (cursor < next.m_cursor || (more->asBool() && cursor == next.m_cursor)) {
        return refuse("The background refresh cursor did not advance.");
    }
    auto lastSequence = next.m_cursor;
    for (const Value& value : events->items()) {
        auto event = decodeEvent(value, epoch);
        if (!event || event->sequence <= lastSequence || event->sequence > cursor) {
            return refuse("The host returned an invalid event sequence.");
        }
        lastSequence = event->sequence;
        if (event->createdUnixMilliseconds <= now && now - event->createdUnixMilliseconds < BackgroundEvents::lifetimeMilliseconds) {
            if (!event->notificationID.empty()) {
                std::erase_if(next.m_pending, [&event](const auto& pending) { return pending.identifier == event->identifier; });
            }
            next.m_pending.push_back(std::move(*event));
        }
    }
    std::vector<std::string> pageIDs;
    for (const Value& value : panes->items()) {
        auto pane = decodePane(value);
        if (!pane || std::ranges::find(pageIDs, pane->paneID) != pageIDs.end()) {
            return refuse("The host returned invalid pane metadata.");
        }
        pageIDs.push_back(pane->paneID);
        if (next.m_seenPanes.size() < maxSeenPanes) {
            next.m_seenPanes.insert(pane->paneID);
        } else {
            next.m_isMetadataIncomplete = true;
        }
        std::erase_if(next.m_pending, [&pane](const auto& event) { return event.paneID == pane->paneID && isResolved(event, *pane); });
        for (auto& event : next.m_pending) {
            if (event.kind == "question" && event.paneID == pane->paneID) {
                event.isReady = true;
            }
        }
        auto previous = std::ranges::find(next.m_panes, pane->paneID, &BackgroundRefreshPane::paneID);
        if (previous != next.m_panes.end()) {
            pane->checkpoint = std::move(previous->checkpoint);
            pane->screenRevision = std::move(previous->screenRevision);
            pane->cachedScreenText = std::move(previous->cachedScreenText);
            *previous = std::move(*pane);
        } else {
            next.m_panes.push_back(std::move(*pane));
        }
    }
    next.m_isMetadataIncomplete |= omitted->asBool();
    if (!*offset) {
        if (!next.m_isMetadataIncomplete) {
            std::erase_if(next.m_panes, [&next](const auto& pane) { return !next.m_seenPanes.contains(pane.paneID); });
            std::erase_if(next.m_pending, [&next](const auto& event) { return event.kind == "question" && !next.m_seenPanes.contains(event.paneID); });
        }
        next.m_seenPanes.clear();
        next.m_isMetadataIncomplete = false;
    }
    if (const auto* screen = result->find("screen")) {
        const auto pane = std::ranges::find(next.m_panes, m_requestedScreenPane, &BackgroundRefreshPane::paneID);
        if (m_requestedScreenPane.empty() || pane == next.m_panes.end() || !applyScreen(*pane, *screen)) {
            return refuse("The host returned an invalid screen checkpoint.");
        }
    }
    std::erase_if(next.m_pending, [&next, now, &pageIDs](const auto& event) {
        if (event.createdUnixMilliseconds > now || now - event.createdUnixMilliseconds >= BackgroundEvents::lifetimeMilliseconds) {
            return true;
        }
        if (std::ranges::find(pageIDs, event.paneID) == pageIDs.end()) {
            return false;
        }
        const auto pane = std::ranges::find(next.m_panes, event.paneID, &BackgroundRefreshPane::paneID);
        return pane != next.m_panes.end() && isResolved(event, *pane);
    });
    for (auto& event : next.m_pending) {
        if (event.kind == "notification") {
            event.isReady = !more->asBool();
        }
    }
    if (next.m_pending.size() > maxPendingNotifications) {
        const auto excess = static_cast<std::ptrdiff_t>(next.m_pending.size() - maxPendingNotifications);
        next.m_pending.erase(next.m_pending.begin(), next.m_pending.begin() + excess);
        next.m_hadRetentionGap = true;
    }
    next.m_cursor = cursor;
    next.m_hostNowUnixMilliseconds = now;
    next.m_nextPaneOffset = *offset;
    next.m_hasMoreEvents = more->asBool();
    next.m_hadRetentionGap |= gap->asBool();
    next.m_didAttemptScreen = m_didAttemptScreen || !m_requestedScreenPane.empty();
    next.m_requestID.clear();
    next.m_error.clear();
    while (next.m_panes.size() > maxCachedPanes || next.saveRecord().size() > maxRecordBytes) {
        if (next.m_panes.empty()) {
            return refuse("The background refresh cache exceeded its limit.");
        }
        next.m_panes.erase(next.m_panes.begin());
    }
    *this = std::move(next);
    return true;
}

std::string BackgroundRefreshState::saveRecord() const
{
    Value record = Value::object();
    record.set("format", Value::number(1));
    record.set("wire", Value::number(Protocol::Sync::wireVersion));
    record.set("epoch", Value::string(m_epoch));
    record.set("inventory", Value::string(m_inventory));
    record.set("cursor", Value::string(std::to_string(m_cursor)));
    record.set("hostNowUnixMilliseconds", Value::string(std::to_string(m_hostNowUnixMilliseconds)));
    record.set("nextPaneOffset", Value::number(static_cast<double>(m_nextPaneOffset)));
    record.set("metadataIncomplete", Value::boolean(m_isMetadataIncomplete));
    Value seen = Value::array();
    for (const auto& paneID : m_seenPanes) {
        seen.push(Value::string(paneID));
    }
    record.set("seen", std::move(seen));
    Value events = Value::array();
    for (const auto& event : m_pending) {
        events.push(encodeEvent(event));
    }
    record.set("events", std::move(events));
    Value panes = Value::array();
    for (const auto& pane : m_panes) {
        panes.push(encodePane(pane));
    }
    record.set("panes", std::move(panes));
    return Codec::JSON::serialize(record);
}

bool BackgroundRefreshState::restoreRecord(const std::string& bytes)
{
    if (bytes.empty() || bytes.size() > maxRecordBytes) {
        return false;
    }
    bool valid = false;
    const Value record = Codec::JSON::parse(bytes, valid);
    const auto offset = integerField(record, "nextPaneOffset", 100001);
    const auto* events = record.find("events");
    const auto* panes = record.find("panes");
    const auto* seen = record.find("seen");
    const auto* incomplete = record.find("metadataIncomplete");
    BackgroundRefreshState restored;
    if (!valid || !uniqueMembers(record) || record.numberOr("format") != 1 || record.numberOr("wire") != Protocol::Sync::wireVersion
        || !stringField(record, "epoch", restored.m_epoch, 20) || (!restored.m_epoch.empty() && (!backgroundSequence(restored.m_epoch) || !*backgroundSequence(restored.m_epoch)))
        || !stringField(record, "inventory", restored.m_inventory, 20)
        || (!restored.m_inventory.empty() && (!backgroundSequence(restored.m_inventory) || !*backgroundSequence(restored.m_inventory)))
        || !unsignedField(record, "cursor", restored.m_cursor) || !unsignedField(record, "hostNowUnixMilliseconds", restored.m_hostNowUnixMilliseconds)
        || !offset || !events || !events->isArray() || events->items().size() > maxPendingNotifications
        || !panes || !panes->isArray() || panes->items().size() > maxCachedPanes
        || !seen || !seen->isArray() || seen->items().size() > maxSeenPanes || !incomplete || !incomplete->isBool()) {
        return false;
    }
    if (restored.m_epoch.empty() && (!restored.m_inventory.empty() || restored.m_cursor || restored.m_hostNowUnixMilliseconds || *offset || !events->items().empty() || !panes->items().empty() || !seen->items().empty())) {
        return false;
    }
    if (!restored.m_epoch.empty() && restored.m_inventory.empty()) {
        return false;
    }
    restored.m_nextPaneOffset = *offset;
    restored.m_isMetadataIncomplete = incomplete->asBool();
    std::uint64_t previousSequence = 0;
    for (const auto& value : events->items()) {
        auto event = decodeEvent(value, restored.m_epoch);
        if (!event || event->sequence > restored.m_cursor || event->sequence <= previousSequence) {
            return false;
        }
        previousSequence = event->sequence;
        restored.m_pending.push_back(std::move(*event));
    }
    for (const auto& value : panes->items()) {
        auto pane = decodePane(value);
        if (!pane || std::ranges::find(restored.m_panes, pane->paneID, &BackgroundRefreshPane::paneID) != restored.m_panes.end()) {
            return false;
        }
        restored.m_panes.push_back(std::move(*pane));
    }
    for (const auto& value : seen->items()) {
        if (!value.isString() || !isValidBackgroundPaneID(value.asString()) || !restored.m_seenPanes.insert(value.asString()).second) {
            return false;
        }
    }
    *this = std::move(restored);
    return true;
}

void BackgroundRefreshState::markDelivered(const std::string& identifier)
{
    std::erase_if(m_pending, [&identifier](const auto& event) { return event.identifier == identifier; });
}

bool BackgroundRefreshState::hasMore() const
{
    return m_hasMoreEvents || m_nextPaneOffset || (!m_didAttemptScreen && !m_panes.empty());
}

BackgroundRefreshNotification BackgroundRefreshState::notificationAt(std::size_t index) const
{
    for (const auto& event : m_pending) {
        if (event.isReady) {
            if (!index) {
                return event;
            }
            --index;
        }
    }
    return {};
}

std::size_t BackgroundRefreshState::pendingNotificationCount() const
{
    return static_cast<std::size_t>(std::ranges::count(m_pending, true, &BackgroundRefreshNotification::isReady));
}

BackgroundRefreshPane BackgroundRefreshState::paneAt(std::size_t index) const
{
    return index < m_panes.size() ? m_panes[index] : BackgroundRefreshPane {};
}

} // namespace Ubiquitty::Session
