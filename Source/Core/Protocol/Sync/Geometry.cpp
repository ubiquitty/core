// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/Protocol/Sync/Geometry.h"

#include <algorithm>
#include <limits>

namespace Ubiquitty::Protocol::Sync {

namespace {

bool isGeometryContinuationValid(const GeometryArbiter::Continuation& state)
{
    const auto isGeometryValid = [](const std::optional<PaneGeometry>& value) {
        return !value || (value->rows && value->columns);
    };
    if (state.handCount > GeometryArbiter::maxHands || state.placement > PanePlacement::BesideOthers
        || !isGeometryValid(state.pin) || !isGeometryValid(state.applied)
        || (state.pinnedBy && *state.pinnedBy == ID128 {})
        || (state.override && (state.override->attachmentID == ID128 {} || !isGeometryValid(state.override->geometry)))
        || (state.appliedAt && (!state.applied || *state.appliedAt > UINT64_MAX - state.policy.coalesceMicroseconds)))
        return false;
    for (unsigned index = 0; index < state.handCount; ++index) {
        const auto& hand = state.hands[index];
        if (hand.attachmentID == ID128 {} || hand.standing > GeometryStanding::Bystander
            || !isGeometryValid(hand.viewport) || !isGeometryValid(hand.windowViewport)
            || (!hand.viewport && hand.viewportAt) || (!hand.windowViewport && hand.windowViewportAt)
            || (hand.typedAt && *hand.typedAt > hand.heardAt) || hand.viewportAt > hand.heardAt || hand.windowViewportAt > hand.heardAt)
            return false;
        for (unsigned previous = 0; previous < index; ++previous) {
            if (state.hands[previous].attachmentID == hand.attachmentID)
                return false;
        }
    }
    return true;
}

} // namespace

std::optional<GeometryArbiter::Continuation> GeometryArbiter::continuation() const
{
    if (m_hands.size() > maxHands)
        return std::nullopt;
    Continuation state;
    state.policy = m_policy;
    state.handCount = static_cast<std::uint8_t>(m_hands.size());
    for (std::size_t index = 0; index < m_hands.size(); ++index) {
        const auto& hand = m_hands[index];
        state.hands[index] = { hand.attachmentID, hand.standing, hand.viewport, hand.windowViewport,
            hand.typedAt, hand.viewportAt, hand.windowViewportAt, hand.heardAt };
    }
    state.pin = m_pin;
    state.pinnedBy = m_pinnedBy;
    if (m_override)
        state.override = Continuation::Override { m_override->attachmentID, m_override->geometry };
    state.applied = m_applied;
    state.appliedAt = m_appliedAt;
    state.placement = m_placement;
    return isGeometryContinuationValid(state) ? std::optional { state } : std::nullopt;
}

bool GeometryArbiter::restoreContinuation(const Continuation& state)
{
    if (state.policy.coalesceMicroseconds != m_policy.coalesceMicroseconds || !isGeometryContinuationValid(state))
        return false;
    std::vector<Hand> hands;
    hands.reserve(state.handCount);
    for (unsigned index = 0; index < state.handCount; ++index) {
        const auto& hand = state.hands[index];
        hands.push_back({ hand.attachmentID, hand.standing, hand.viewport, hand.windowViewport,
            hand.typedAt, hand.viewportAt, hand.windowViewportAt, hand.heardAt });
    }
    m_hands = std::move(hands);
    m_pin = state.pin;
    m_pinnedBy = state.pinnedBy;
    m_override = state.override ? std::optional { Override { state.override->attachmentID, state.override->geometry } } : std::nullopt;
    m_applied = state.applied;
    m_appliedAt = state.appliedAt;
    m_placement = state.placement;
    return true;
}

GeometryArbiter::GeometryArbiter(const GeometryPolicy& policy)
    : m_policy(policy)
{
}

void GeometryArbiter::seed(PaneGeometry geometry)
{
    m_applied = geometry;
}

void GeometryArbiter::noteTyping(ID128 attachmentID, GeometryStanding standing, std::uint64_t now)
{
    Hand& typed = hand(attachmentID, standing, now);
    if (standing != GeometryStanding::Driver)
        return;
    typed.typedAt = now;
    // A driver typing takes the size back from an override.
    m_override.reset();
}

void GeometryArbiter::noteViewport(ID128 attachmentID, GeometryStanding standing, PaneGeometry viewport, std::uint64_t now)
{
    if (viewport.rows == 0 || viewport.columns == 0)
        return;
    Hand& reported = hand(attachmentID, standing, now);
    // A driver's new size takes it back from an override; the same size
    // again, as a focus change resends it, does not.
    if (standing == GeometryStanding::Driver && reported.viewport != viewport)
        m_override.reset();
    reported.viewport = viewport;
    reported.viewportAt = now;
}

void GeometryArbiter::noteWindowViewport(ID128 attachmentID, GeometryStanding standing, PaneGeometry viewport, std::uint64_t now)
{
    if (viewport.rows == 0 || viewport.columns == 0)
        return;
    Hand& reported = hand(attachmentID, standing, now);
    // The size it already reported says nothing new, whenever it is said
    // again. A client's `Facts` carry its window's size beside everything
    // else they report, so a focus change or a theme resends the size as it
    // stood; counted as a new report, that would outrank the size the client
    // has since given the pane itself, and a window gaining the focus would
    // resize its pane to what the window was when it connected.
    if (reported.windowViewport == viewport)
        return;
    // A window's size is no word on a pane laid out inside it, so it takes
    // nothing back from an override there.
    if (standing == GeometryStanding::Driver && m_placement == PanePlacement::WholeWindow)
        m_override.reset();
    reported.windowViewport = viewport;
    reported.windowViewportAt = now;
}

void GeometryArbiter::forget(ID128 attachmentID)
{
    std::erase_if(m_hands, [&](const Hand& hand) { return hand.attachmentID == attachmentID; });
}

void GeometryArbiter::pin(PaneGeometry geometry, std::optional<ID128> by)
{
    m_pin = geometry;
    m_pinnedBy = by;
}

void GeometryArbiter::unpin()
{
    m_pin.reset();
}

void GeometryArbiter::overrideSize(ID128 attachmentID, PaneGeometry geometry, std::uint64_t now)
{
    if (geometry.rows == 0 || geometry.columns == 0)
        return;
    // An override says nothing of who the asker is: a hand already known
    // keeps its standing, and a new one stands by.
    if (std::ranges::find(m_hands, attachmentID, &Hand::attachmentID) == m_hands.end())
        (void)hand(attachmentID, GeometryStanding::Bystander, now);
    m_override = Override { .attachmentID = attachmentID, .geometry = geometry };
}

void GeometryArbiter::rekey(ID128 from, ID128 to)
{
    if (from == to)
        return;
    if (m_pinnedBy == from)
        m_pinnedBy = to;
    const auto source = std::ranges::find(m_hands, from, &Hand::attachmentID);
    if (source == m_hands.end())
        return;
    const auto existing = std::ranges::find(m_hands, to, &Hand::attachmentID);
    if (existing == m_hands.end()) {
        source->attachmentID = to;
    } else {
        // Both keys heard from: the newer of each fact stands.
        if (source->viewport && source->viewportAt >= existing->viewportAt) {
            existing->viewport = source->viewport;
            existing->viewportAt = source->viewportAt;
        }
        if (source->typedAt && (!existing->typedAt || *source->typedAt > *existing->typedAt))
            existing->typedAt = source->typedAt;
        if (source->windowViewport && source->windowViewportAt >= existing->windowViewportAt) {
            existing->windowViewport = source->windowViewport;
            existing->windowViewportAt = source->windowViewportAt;
        }
        existing->heardAt = std::max(existing->heardAt, source->heardAt);
        m_hands.erase(source);
    }
    if (m_override && m_override->attachmentID == from)
        m_override->attachmentID = to;
}

std::optional<PaneGeometry> GeometryArbiter::due(std::uint64_t now)
{
    const std::optional<PaneGeometry> wanted = target();
    if (!wanted || wanted == m_applied)
        return std::nullopt;
    // One size per window: the first change applies at once, and what
    // follows inside the window waits for its end, the newest winning.
    if (m_appliedAt && now < *m_appliedAt + m_policy.coalesceMicroseconds)
        return std::nullopt;
    m_applied = wanted;
    m_appliedAt = now;
    return wanted;
}

std::optional<PaneGeometry> GeometryArbiter::dueBeforeInput(ID128 attachmentID, std::uint64_t now)
{
    const Hand* setter = m_pin || m_override ? nullptr : owner();
    const std::optional<Shown> reported = setter ? shown(*setter) : std::nullopt;
    const bool hasResized = reported && setter->attachmentID == attachmentID && reported->geometry != m_applied && (!m_appliedAt || reported->at > *m_appliedAt);
    if (!hasResized)
        return due(now);
    m_applied = reported->geometry;
    m_appliedAt = now;
    return m_applied;
}

std::optional<std::uint64_t> GeometryArbiter::nextDueMicroseconds() const
{
    const std::optional<PaneGeometry> wanted = target();
    if (!wanted || wanted == m_applied)
        return std::nullopt;
    return m_appliedAt ? *m_appliedAt + m_policy.coalesceMicroseconds : 0;
}

GeometryStatus GeometryArbiter::status() const
{
    if (m_pin)
        return { .owner = std::nullopt, .isPinned = true, .isOverridden = false, .pinnedBy = m_pinnedBy };
    if (m_override)
        return { .owner = m_override->attachmentID, .isPinned = false, .isOverridden = true, .pinnedBy = std::nullopt };
    const Hand* setter = owner();
    return { .owner = setter ? std::optional { setter->attachmentID } : std::nullopt, .isPinned = false, .isOverridden = false, .pinnedBy = std::nullopt };
}

std::optional<ID128> GeometryArbiter::drivingOwner() const
{
    const Hand* setter = owner();
    return setter ? std::optional { setter->attachmentID } : std::nullopt;
}

std::vector<ID128> GeometryArbiter::typistsNewestFirst() const
{
    std::vector<const Hand*> typists;
    for (const Hand& hand : m_hands) {
        if (hand.standing == GeometryStanding::Driver && hand.typedAt)
            typists.push_back(&hand);
    }
    std::ranges::sort(typists, std::ranges::greater {}, [](const Hand* hand) { return *hand->typedAt; });
    std::vector<ID128> ordered;
    ordered.reserve(typists.size());
    for (const Hand* hand : typists)
        ordered.push_back(hand->attachmentID);
    return ordered;
}

GeometryArbiter::Hand& GeometryArbiter::hand(ID128 attachmentID, GeometryStanding standing, std::uint64_t now)
{
    auto found = std::ranges::find(m_hands, attachmentID, &Hand::attachmentID);
    if (found == m_hands.end()) {
        // Bounded: a pane shown by more attachments than this forgets the one
        // heard from longest ago, never the one whose size applies, so a
        // crowd of attachments cannot take the size by displacing its owner.
        if (m_hands.size() >= maxHands) {
            const Hand* kept = owner();
            const auto oldest = std::ranges::min_element(m_hands, {}, [&](const Hand& candidate) {
                return &candidate == kept ? std::numeric_limits<std::uint64_t>::max() : candidate.heardAt;
            });
            m_hands.erase(oldest);
        }
        m_hands.push_back({ .attachmentID = attachmentID, .standing = standing, .viewport = std::nullopt, .windowViewport = std::nullopt, .typedAt = std::nullopt, .viewportAt = 0, .windowViewportAt = 0, .heardAt = now });
        found = m_hands.end() - 1;
    }
    found->standing = standing;
    found->heardAt = now;
    return *found;
}

std::optional<GeometryArbiter::Shown> GeometryArbiter::shown(const Hand& hand) const
{
    const bool hasWindow = hand.windowViewport && m_placement == PanePlacement::WholeWindow;
    if (hand.viewport && (!hasWindow || hand.viewportAt >= hand.windowViewportAt))
        return Shown { .geometry = *hand.viewport, .at = hand.viewportAt };
    if (hasWindow)
        return Shown { .geometry = *hand.windowViewport, .at = hand.windowViewportAt };
    return std::nullopt;
}

const GeometryArbiter::Hand* GeometryArbiter::owner() const
{
    const Hand* typist = nullptr;
    const Hand* reporter = nullptr;
    std::uint64_t reportedAt = 0;
    for (const Hand& hand : m_hands) {
        if (hand.standing != GeometryStanding::Driver)
            continue;
        const std::optional<Shown> size = shown(hand);
        if (!size)
            continue;
        if (hand.typedAt && (!typist || *hand.typedAt > *typist->typedAt))
            typist = &hand;
        if (!reporter || size->at > reportedAt) {
            reporter = &hand;
            reportedAt = size->at;
        }
    }
    // The newest typing decides; until anyone types, the newest viewport.
    return typist ? typist : reporter;
}

std::optional<PaneGeometry> GeometryArbiter::target() const
{
    if (m_pin)
        return m_pin;
    if (m_override)
        return m_override->geometry;
    const Hand* setter = owner();
    const std::optional<Shown> size = setter ? shown(*setter) : std::nullopt;
    return size ? std::optional { size->geometry } : std::nullopt;
}

} // namespace Ubiquitty::Protocol::Sync
