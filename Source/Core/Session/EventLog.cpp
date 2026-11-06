// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Source/Core/Session/EventLog.cpp — see EventLog.h.

#include "Core/Session/EventLog.h"

#include "Core/Session/Identity.h"

#include <algorithm>
#include <utility>

namespace Ubiquitty::Session {

namespace {

// Cuts at a code point boundary, so a cut record is still UTF-8.
void truncateText(std::string& text)
{
    if (text.size() <= maxEventTextLength)
        return;
    std::size_t end = maxEventTextLength;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80)
        --end;
    text.resize(end);
}

} // namespace

std::string_view eventKindName(EventKind kind)
{
    switch (kind) {
    case EventKind::Gap:
        return "gap";
    case EventKind::SessionCreated:
        return "session-created";
    case EventKind::SessionEnded:
        return "session-ended";
    case EventKind::PaneFocused:
        return "pane-focused";
    case EventKind::ProgramStatus:
        return "program-status";
    case EventKind::CommandFinished:
        return "command-finished";
    case EventKind::Bell:
        return "bell";
    case EventKind::TitleChanged:
        return "title-changed";
    case EventKind::ForegroundChanged:
        return "foreground-changed";
    case EventKind::ConsentRequested:
        return "consent-requested";
    case EventKind::ConsentGranted:
        return "consent-granted";
    case EventKind::ConsentDenied:
        return "consent-denied";
    case EventKind::CommandStarted:
        return "command-started";
    case EventKind::ScreenRead:
        return "screen-read";
    }
    return "gap";
}

std::string_view eventProgramStateName(EventProgramState state)
{
    switch (state) {
    case EventProgramState::None:
        return "none";
    case EventProgramState::Idle:
        return "idle";
    case EventProgramState::Working:
        return "working";
    case EventProgramState::Done:
        return "done";
    case EventProgramState::Blocked:
        return "blocked";
    case EventProgramState::Error:
        return "error";
    }
    return "none";
}

Codec::JSON::Value eventJSON(const Event& event)
{
    using Codec::JSON::Value;
    Value object = Value::object();
    object.set("cursor", Value::number(static_cast<double>(event.cursor)));
    object.set("kind", Value::string(std::string(eventKindName(event.kind))));
    if (event.sessionID)
        object.set("session", Value::string(Identity::format({ Identity::ObjectKind::Session, event.sessionID })));
    if (event.paneID)
        object.set("pane", Value::string(Identity::format({ Identity::ObjectKind::Pane, event.paneID })));
    switch (event.kind) {
    case EventKind::ScreenRead:
        object.set("claimedLabel", Value::string(event.text));
        object.set("verifiedProcess", Value::string(event.detail));
        object.set("unixMilliseconds", Value::number(static_cast<double>(event.count)));
        break;
    case EventKind::Gap:
        object.set("dropped", Value::number(static_cast<double>(event.count)));
        break;
    case EventKind::CommandFinished:
        if (event.exitStatus == noExitStatus)
            object.set("exitStatus", Value::null());
        else
            object.set("exitStatus", Value::number(event.exitStatus));
        object.set("commandLine", Value::string(event.text));
        object.set("directory", Value::string(event.detail));
        break;
    case EventKind::ProgramStatus:
        object.set("state", Value::string(std::string(eventProgramStateName(event.state))));
        object.set("message", Value::string(event.text));
        object.set("id", Value::string(event.detail));
        break;
    case EventKind::ForegroundChanged:
        object.set("atPrompt", Value::boolean(event.isAtPrompt));
        object.set("process", Value::string(event.detail));
        break;
    case EventKind::TitleChanged:
        object.set("title", Value::string(event.text));
        break;
    case EventKind::ConsentRequested:
    case EventKind::ConsentGranted:
    case EventKind::ConsentDenied:
        object.set("agent", Value::string(event.text));
        object.set("detail", Value::string(event.detail));
        break;
    case EventKind::SessionEnded:
        object.set("reason", Value::string(event.text));
        break;
    case EventKind::SessionCreated:
    case EventKind::PaneFocused:
    case EventKind::Bell:
    case EventKind::CommandStarted:
        break;
    }
    return object;
}

EventLog::EventLog(std::size_t capacity)
    : m_ring(std::max<std::size_t>(capacity, 1))
{
}

bool EventLog::isValidContinuation(const Continuation& state, std::size_t capacity)
{
    if (!capacity || state.ring.size() != capacity
        || state.head >= state.ring.size() || state.count > state.ring.size()
        || !state.nextCursor || state.nextCursor == UINT64_MAX
        || state.count != std::min<std::uint64_t>(state.nextCursor - 1, state.ring.size())
        || (state.count < state.ring.size() && state.head))
        return false;
    const auto first = state.nextCursor - state.count;
    for (std::size_t index = 0; index < state.ring.size(); ++index) {
        const auto& event = state.ring[(state.head + index) % state.ring.size()];
        if (index >= state.count) {
            if (event != Event {})
                return false;
            continue;
        }
        if (event.cursor != first + index || event.kind == EventKind::Gap
            || std::to_underlying(event.kind) > lastEventKind
            || event.state > EventProgramState::Error
            || event.text.size() > maxEventTextLength || event.detail.size() > maxEventTextLength)
            return false;
    }
    return true;
}

bool EventLog::restoreContinuation(Continuation&& state)
{
    if (m_count || m_head || m_nextCursor != 1 || !isValidContinuation(state, m_ring.size()))
        return false;
    m_ring = std::move(state.ring);
    m_head = std::exchange(state.head, 0);
    m_count = std::exchange(state.count, 0);
    m_nextCursor = std::exchange(state.nextCursor, 1);
    return true;
}

const Event& EventLog::at(std::size_t ordinal) const
{
    return m_ring[(m_head + ordinal) % m_ring.size()];
}

const Event& EventLog::append(Event event)
{
    event.cursor = m_nextCursor++;
    truncateText(event.text);
    truncateText(event.detail);
    std::size_t slot;
    if (m_count < m_ring.size()) {
        slot = (m_head + m_count) % m_ring.size();
        ++m_count;
    } else {
        slot = m_head;
        m_head = (m_head + 1) % m_ring.size();
    }
    m_ring[slot] = std::move(event);
    return m_ring[slot];
}

EventLog::Backlog EventLog::since(std::uint64_t cursor, std::size_t maximum) const
{
    Backlog backlog;
    if (!m_count || !maximum || cursor >= latestCursor())
        return backlog;
    const std::uint64_t oldest = at(0).cursor;
    if (cursor + 1 < oldest) {
        backlog.dropped = oldest - cursor - 1;
        backlog.gapCursor = oldest - 1;
    }
    // Cursors are consecutive, so the first record owed sits at a computed
    // ordinal rather than one found by searching.
    const std::uint64_t first = std::max(cursor + 1, oldest);
    const auto count = static_cast<std::size_t>(std::min(m_nextCursor - first, static_cast<std::uint64_t>(maximum)));
    backlog.events.reserve(count);
    for (std::size_t index = 0; index < count; ++index)
        backlog.events.push_back(at(static_cast<std::size_t>(first - oldest) + index));
    return backlog;
}

void holdEvent(std::vector<Event>& held, Event event, std::size_t capacity)
{
    capacity = std::max<std::size_t>(capacity, 2);
    while (held.size() >= capacity) {
        const auto lost = [](const Event& record) { return record.kind == EventKind::Gap ? record.count : 1; };
        Event gap;
        gap.kind = EventKind::Gap;
        gap.count = lost(held[0]) + lost(held[1]);
        gap.cursor = held[1].cursor;
        held[0] = std::move(gap);
        held.erase(held.begin() + 1);
    }
    held.push_back(std::move(event));
}

} // namespace Ubiquitty::Session
