// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Session/BackgroundEvents.h"

#include "Core/Unicode/UTF8Repair.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <utility>

namespace Ubiquitty::Session {

namespace {

using Codec::JSON::Value;

} // namespace

std::string backgroundEventText(std::string_view text, std::size_t capacity)
{
    const std::size_t prefixLength = std::min(text.size(), capacity);
    // Include the rest of a possible four-byte scalar at the capacity boundary.
    std::string repaired(text.substr(0, prefixLength + std::min<std::size_t>(3, text.size() - prefixLength)));
    (void)Unicode::repairUTF8(repaired);
    repaired.resize(Unicode::wellFormedUTF8PrefixLength(std::string_view(repaired).substr(0, capacity)));
    return repaired;
}

std::optional<NotificationOccasion> backgroundNotificationOccasion(std::string_view text)
{
    if (text == "always")
        return NotificationOccasion::Always;
    if (text == "unfocused")
        return NotificationOccasion::Unfocused;
    if (text == "invisible")
        return NotificationOccasion::Invisible;
    return std::nullopt;
}

std::string_view backgroundNotificationOccasionName(NotificationOccasion occasion)
{
    switch (occasion) {
    case NotificationOccasion::Always:
        return "always";
    case NotificationOccasion::Unfocused:
        return "unfocused";
    case NotificationOccasion::Invisible:
        return "invisible";
    }
    return {};
}

namespace {

bool validPaneID(std::string_view value)
{
    return value.size() == 32 && std::ranges::all_of(value, [](char character) {
        return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
    });
}

bool isQuestion(const OSC::ProgramStatus& record)
{
    return record.state == OSC::ProgramState::Blocked
        && (record.kind == OSC::ProgramBlockKind::Question || record.kind == OSC::ProgramBlockKind::Permission || record.kind == OSC::ProgramBlockKind::Auth);
}

bool isExpired(const BackgroundEvent& event, std::uint64_t now)
{
    return now >= event.createdUnixMilliseconds && now - event.createdUnixMilliseconds >= BackgroundEvents::lifetimeMilliseconds;
}

} // namespace

bool isValidBackgroundPaneID(std::string_view text)
{
    return validPaneID(text);
}

std::optional<std::uint64_t> backgroundSequence(std::string_view text)
{
    if (text.empty() || text.size() > 20 || !std::ranges::all_of(text, [](char character) { return character >= '0' && character <= '9'; })) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc {} || parsed.ptr != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

std::string backgroundPaneID(std::uint64_t high, std::uint64_t low)
{
    std::array<char, 32> digits {};
    constexpr std::string_view alphabet = "0123456789abcdef";
    for (std::size_t index = 0; index < 16; ++index) {
        digits[index] = alphabet[(high >> (60 - 4 * index)) & 0xf];
        digits[index + 16] = alphabet[(low >> (60 - 4 * index)) & 0xf];
    }
    return { digits.begin(), digits.end() };
}

Value backgroundEventJSON(const BackgroundEvent& event)
{
    Value result = Value::object();
    result.set("sequence", Value::string(std::to_string(event.sequence)));
    result.set("createdUnixMilliseconds", Value::string(std::to_string(event.createdUnixMilliseconds)));
    result.set("paneID", Value::string(event.paneID));
    result.set("sessionID", Value::number(event.sessionID));
    result.set("kind", Value::string(event.statusID.empty() ? "notification" : "question"));
    result.set("title", Value::string(event.title));
    result.set("body", Value::string(event.body));
    result.set("statusID", Value::string(event.statusID));
    result.set("notificationID", Value::string(event.notificationID));
    result.set("occasion", Value::string(std::string(backgroundNotificationOccasionName(event.occasion))));
    return result;
}

void BackgroundEvents::append(BackgroundEvent event)
{
    if (!validPaneID(event.paneID) || !event.sessionID || m_latest == std::numeric_limits<std::uint64_t>::max()) {
        return;
    }
    event.sequence = ++m_latest;
    if (m_events.size() == capacity) {
        m_droppedThrough = m_events.front().sequence;
        m_events.erase(m_events.begin());
    }
    m_events.push_back(std::move(event));
    ++m_revision;
}

void BackgroundEvents::appendNotification(std::string paneID, std::uint32_t sessionID, const NotificationRequest& request, std::uint64_t now)
{
    if (!validPaneID(paneID) || !sessionID || request.identifier.size() > notificationIDCapacity
        || std::to_underlying(request.occasion) > std::to_underlying(NotificationOccasion::Invisible)) {
        return;
    }
    if (!request.identifier.empty()) {
        for (BackgroundEvent& event : m_events) {
            if (event.isCurrent && event.paneID == paneID && event.statusID.empty() && event.notificationID == request.identifier) {
                event.isCurrent = false;
                ++m_revision;
            }
        }
    }
    append({ .createdUnixMilliseconds = now, .paneID = std::move(paneID), .sessionID = sessionID, .title = backgroundEventText(request.title, titleCapacity), .body = backgroundEventText(request.body, bodyCapacity), .statusID = {}, .notificationID = std::string(request.identifier), .occasion = request.occasion });
}

void BackgroundEvents::updateQuestions(std::string_view paneID, std::uint32_t sessionID, std::span<const OSC::ProgramStatus> previous, std::span<const OSC::ProgramStatus> records, std::uint64_t now)
{
    for (BackgroundEvent& event : m_events) {
        if (event.paneID != paneID || event.statusID.empty() || !event.isCurrent) {
            continue;
        }
        const auto current = std::ranges::find_if(records, [&](const auto& record) { return record.id.view() == event.statusID; });
        if (current == records.end() || !isQuestion(*current) || backgroundEventText(current->title, titleCapacity) != event.title || backgroundEventText(current->message, bodyCapacity) != event.body) {
            event.isCurrent = false;
            ++m_revision;
        }
    }
    for (const OSC::ProgramStatus& record : records) {
        if (!isQuestion(record) || record.id.empty() || record.id.size() > statusCapacity) {
            continue;
        }
        const auto earlier = std::ranges::find(previous, record.id, &OSC::ProgramStatus::id);
        const bool isUnchanged = earlier != previous.end() && isQuestion(*earlier) && earlier->kind == record.kind
            && backgroundEventText(earlier->title, titleCapacity) == backgroundEventText(record.title, titleCapacity)
            && backgroundEventText(earlier->message, bodyCapacity) == backgroundEventText(record.message, bodyCapacity);
        if (!isUnchanged) {
            for (BackgroundEvent& event : m_events) {
                if (event.isCurrent && event.paneID == paneID && event.statusID == record.id) {
                    event.isCurrent = false;
                    ++m_revision;
                }
            }
            append({ .createdUnixMilliseconds = now, .paneID = std::string(paneID), .sessionID = sessionID, .title = backgroundEventText(record.title, titleCapacity), .body = backgroundEventText(record.message, bodyCapacity), .statusID = record.id, .notificationID = {} });
        }
    }
}

void BackgroundEvents::removePane(std::string_view paneID)
{
    updateQuestions(paneID, 0, {}, {}, 0);
}

BackgroundEvents::Page BackgroundEvents::since(std::uint64_t after, std::uint64_t now) const
{
    Page page;
    std::size_t pageBytes = 0;
    page.hasGap = after < m_droppedThrough || after > m_latest;
    page.cursor = std::min(after, m_latest);
    for (const BackgroundEvent& event : m_events) {
        if (event.sequence <= page.cursor) {
            continue;
        }
        if (event.isCurrent && !isExpired(event, now)) {
            const std::size_t eventBytes = Codec::JSON::serialize(backgroundEventJSON(event)).size();
            if (page.events.size() == pageCapacity || pageBytes + eventBytes > 256 * 1024) {
                page.hasMore = true;
                break;
            }
            page.events.push_back(event);
            pageBytes += eventBytes;
        }
        page.cursor = event.sequence;
    }
    if (!page.hasMore) {
        page.cursor = m_latest;
    }
    return page;
}

Value BackgroundEvents::persistentState() const
{
    Value result = Value::object();
    result.set("version", Value::number(1));
    result.set("epoch", Value::string(std::to_string(m_epoch)));
    result.set("latest", Value::string(std::to_string(m_latest)));
    result.set("droppedThrough", Value::string(std::to_string(m_droppedThrough)));
    Value events = Value::array();
    for (const BackgroundEvent& event : m_events) {
        Value stored = backgroundEventJSON(event);
        stored.set("current", Value::boolean(event.isCurrent));
        events.push(std::move(stored));
    }
    result.set("events", std::move(events));
    return result;
}

bool BackgroundEvents::restore(const Value& state)
{
    const auto epoch = backgroundSequence(state.stringOr("epoch"));
    const auto latest = backgroundSequence(state.stringOr("latest"));
    const auto dropped = backgroundSequence(state.stringOr("droppedThrough"));
    const Value* events = state.find("events");
    if (state.numberOr("version") != 1 || !epoch || !*epoch || !latest || !dropped || *dropped > *latest || !events || !events->isArray() || events->items().size() > capacity) {
        return false;
    }
    std::vector<BackgroundEvent> restored;
    std::uint64_t previous = *dropped;
    for (const Value& value : events->items()) {
        const auto sequence = backgroundSequence(value.stringOr("sequence"));
        const auto created = backgroundSequence(value.stringOr("createdUnixMilliseconds"));
        const double session = value.numberOr("sessionID");
        const Value* current = value.find("current");
        BackgroundEvent event { .sequence = sequence.value_or(0), .createdUnixMilliseconds = created.value_or(0), .paneID = value.stringOr("paneID"), .title = value.stringOr("title"), .body = value.stringOr("body"), .statusID = value.stringOr("statusID"), .notificationID = value.stringOr("notificationID"), .isCurrent = value.boolOr("current") };
        const auto occasion = backgroundNotificationOccasion(value.stringOr("occasion"));
        if (!sequence || *sequence <= previous || *sequence > *latest || !created || !validPaneID(event.paneID) || !current || !current->isBool()
            || !std::isfinite(session) || session < 1 || session > UINT32_MAX || std::floor(session) != session
            || !occasion
            || event.notificationID.size() > notificationIDCapacity || (!event.statusID.empty() && !event.notificationID.empty())
            || event.title.size() > titleCapacity || event.body.size() > bodyCapacity || event.statusID.size() > statusCapacity
            || value.stringOr("kind") != (event.statusID.empty() ? "notification" : "question")) {
            return false;
        }
        event.occasion = *occasion;
        event.sessionID = static_cast<std::uint32_t>(session);
        previous = *sequence;
        restored.push_back(std::move(event));
    }
    if (previous != *latest) {
        return false;
    }
    m_epoch = *epoch;
    m_latest = *latest;
    m_droppedThrough = *dropped;
    m_events = std::move(restored);
    ++m_revision;
    return true;
}

bool BackgroundEvents::isValidContinuation(const Continuation& state)
{
    if (!state.epoch || state.events.size() > capacity
        || state.droppedThrough > state.latest || state.revision == UINT64_MAX)
        return false;
    std::uint64_t previous = state.droppedThrough;
    for (const auto& event : state.events) {
        if (event.sequence <= previous || event.sequence > state.latest
            || !validPaneID(event.paneID) || !event.sessionID
            || event.title.size() > titleCapacity || event.body.size() > bodyCapacity
            || event.statusID.size() > statusCapacity || event.notificationID.size() > notificationIDCapacity
            || (!event.statusID.empty() && !event.notificationID.empty())
            || event.occasion > NotificationOccasion::Invisible)
            return false;
        previous = event.sequence;
    }
    if (previous != state.latest)
        return false;
    return true;
}

bool BackgroundEvents::restoreContinuation(Continuation&& state)
{
    if (m_latest || m_revision || m_droppedThrough || !m_events.empty()
        || state.epoch != m_epoch || !isValidContinuation(state))
        return false;
    m_latest = std::exchange(state.latest, 0);
    m_droppedThrough = std::exchange(state.droppedThrough, 0);
    m_revision = std::exchange(state.revision, 0);
    m_events = std::move(state.events);
    return true;
}

} // namespace Ubiquitty::Session
