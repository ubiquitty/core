// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Base/WeakPtr.h"
#include "Core/Session/PaneLink.h"

#include <optional>
#include <utility>

namespace Ubiquitty::Session {

/// Whether a verified frame arrived live or was restored before connecting.
enum class FrameOrigin : std::uint8_t {
    Live,
    Restored,
};

/// The latest coherent picture a pane can present.
struct FrameReady {
    Protocol::Sync::FrameClass frameClass = Protocol::Sync::FrameClass::Structural;
    std::uint64_t generation = 0;
    std::uint64_t lagMicroseconds = 0;
    FrameOrigin origin = FrameOrigin::Live;

    friend bool operator==(const FrameReady&, const FrameReady&) = default;
};

/// Receives a pane's coalesced notifications on its presentation owner thread.
class PaneLinkObserver : public CanMakeWeakPtr {
public:
    virtual ~PaneLinkObserver() { revokeWeakPtrs(); }
    virtual void frameReady(const FrameReady&) { }
    virtual void factsChanged() { }
    virtual void linkStateChanged(const LinkState&) { }
};

/// A bounded presentation-owner-thread mailbox. A local pane stages from a
/// capture taken under its model lock, never from the ingest thread. A callback may
/// replace its observer or destroy the mailbox. Recursive delivery never
/// enters an observer; notifications it stages wait for the next owner turn.
class PaneLinkEvents : public CanMakeWeakPtr {
public:
    ~PaneLinkEvents() { revokeWeakPtrs(); }

    bool observe(PaneLinkObserver& observer)
    {
        if (m_observer.get() == &observer)
            return false;
        m_observer = observer.weakPtr();
        m_lastState.reset();
        m_lastFrame.reset();
        return true;
    }

    void noteFrame(const FrameReady& frame)
    {
        if (m_lastFrame == frame)
            return;
        m_lastFrame = frame;
        m_pendingFrame = frame;
    }

    void noteFacts() { m_hasPendingFacts = true; }

    [[nodiscard]] bool hasObserver() const { return !!m_observer; }

    void noteState(const LinkState& state)
    {
        if (m_lastState && m_lastState->phase == state.phase
            && m_lastState->transport.isSilent == state.transport.isSilent
            && m_lastState->transport.isInputUndelivered == state.transport.isInputUndelivered)
            return;
        m_lastState = state;
        m_pendingState = state;
    }

    void deliver()
    {
        if (m_isDelivering)
            return;
        const auto observer = m_observer;
        if (!observer)
            return;
        const auto frame = std::exchange(m_pendingFrame, {});
        const auto state = std::exchange(m_pendingState, {});
        const bool hasFacts = std::exchange(m_hasPendingFacts, false);
        m_isDelivering = true;
        const auto weakThis = weakPtr();
        if (frame)
            observer->frameReady(*frame);
        if (!weakThis)
            return;
        if (observer && observer == m_observer && hasFacts)
            observer->factsChanged();
        if (!weakThis)
            return;
        if (observer && observer == m_observer && state)
            observer->linkStateChanged(*state);
        if (weakThis)
            m_isDelivering = false;
    }

private:
    WeakPtr<PaneLinkObserver> m_observer;
    std::optional<FrameReady> m_lastFrame;
    std::optional<FrameReady> m_pendingFrame;
    std::optional<LinkState> m_lastState;
    std::optional<LinkState> m_pendingState;
    bool m_hasPendingFacts = false;
    bool m_isDelivering = false;
};

} // namespace Ubiquitty::Session
