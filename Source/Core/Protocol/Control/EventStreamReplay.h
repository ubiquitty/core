// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Protocol/Control/EventStream.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace Ubiquitty::Protocol::Control {

/// A connection's bounded replay cursor, never a second copy of the host log.
class EventStreamReplay {
public:
    /// Begins after a saved cursor, or marks the exact current position.
    /// - Parameters:
    ///   - subscription: The validated call's parameters.
    ///   - newest: The log's cursor when the subscription was accepted.
    void begin(EventSubscription subscription, std::uint64_t newest)
    {
        m_isSubscribed = true;
        m_cursor = subscription.isFromLatest ? newest : std::min(subscription.after, newest);
        m_isPositionOwed = subscription.isFromLatest;
    }

    /// Ends replay when the connection loses its subscription authority.
    void stop() { m_isSubscribed = false; }

    [[nodiscard]] bool isSubscribed() const { return m_isSubscribed; }
    [[nodiscard]] std::uint64_t cursor() const { return m_cursor; }
    [[nodiscard]] bool isOwed(std::uint64_t newest) const { return m_isSubscribed && (m_isPositionOwed || m_cursor < newest); }

    /// Offers a bounded pass, retrying a refused send without advancing it.
    /// Disclosure is checked again for every retained record; a withheld
    /// record advances the cursor without claiming that the log lost it.
    /// Callbacks borrow a record only through their synchronous call and must
    /// not reenter this replay object.
    /// - Parameters:
    ///   - log: The authoritative bounded host log.
    ///   - admits: The current subscriber's disclosure predicate.
    ///   - send: Returns true only when the notification queue admitted the record.
    ///   - maximum: The most stored records examined this pass, capped independently.
    /// - Returns: Admitted notifications, including explicit loss/position gaps.
    template<typename Admits, typename Send>
    std::size_t service(const Session::EventLog& log, Admits admits, Send send, std::size_t maximum = maxEventsPerNotify)
    {
        maximum = std::min(maximum, maxEventsPerNotify);
        if (!m_isSubscribed || !maximum)
            return 0;
        std::size_t sent = 0;
        if (m_isPositionOwed) {
            const Session::Event position { .cursor = m_cursor, .kind = Session::EventKind::Gap, .text = {}, .detail = {} };
            if (!send(position))
                return 0;
            m_isPositionOwed = false;
            ++sent;
        }
        const auto backlog = log.since(m_cursor, maximum);
        if (backlog.dropped) {
            const Session::Event gap { .cursor = backlog.gapCursor, .kind = Session::EventKind::Gap, .count = backlog.dropped, .text = {}, .detail = {} };
            if (!send(gap))
                return sent;
            m_cursor = backlog.gapCursor;
            ++sent;
        }
        std::size_t examined = 0;
        for (const auto& event : backlog.events) {
            if (examined++ == maximum)
                break;
            if (admits(event)) {
                if (!send(event))
                    break;
                ++sent;
            }
            m_cursor = event.cursor;
        }
        return sent;
    }

private:
    bool m_isSubscribed = false;
    bool m_isPositionOwed = false;
    std::uint64_t m_cursor = 0;
};

} // namespace Ubiquitty::Protocol::Control
