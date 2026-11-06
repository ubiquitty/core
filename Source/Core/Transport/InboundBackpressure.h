// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Core/Transport/Channel.h"

#include <cstddef>

namespace Ubiquitty::Transport {

/// When a consumer stops the carrier, and when it starts it again.
///
/// Every consumer of a channel stages what it was handed and processes it elsewhere, so every one of
/// them needs the same two numbers. They live here rather than in each consumer because two consumers
/// choosing separately is two answers to how much a slow session may buffer, and the one that chose
/// larger is the one that decides.
///
/// Two marks rather than one: a single threshold makes a consumer sitting on it suspend and resume on
/// every chunk, which costs a poll re-registration per chunk and delivers the bytes anyway. The gap is
/// what turns that into one suspend and one resume per burst.
class InboundBackpressure {
public:
    /// Stop reading once this much is staged and unprocessed.
    ///
    /// Large enough that an ordinary burst never reaches it — a flood of terminal output arrives in
    /// chunks far smaller — and small enough that a session which stops draining holds megabytes
    /// rather than growing without bound.
    static constexpr std::size_t suspendAboveBytes = 4u * 1024u * 1024u;

    /// Start reading again once the staged bytes fall back to this.
    ///
    /// A quarter of the stop mark, so a consumer that resumes has real room rather than one chunk's
    /// worth before it stops again.
    static constexpr std::size_t resumeAtOrBelowBytes = suspendAboveBytes / 4;

    /// Tells the channel to stop or start, from the depth the consumer is holding.
    ///
    /// Call after staging and again after draining, with whatever is still unprocessed. Suspend and
    /// resume are idempotent, so a consumer that calls this on every chunk asks the channel to change
    /// state only when the depth actually crosses a mark.
    ///
    /// - Parameters:
    ///   - channel: The carrier to pace. A null channel is a no-op, which is what a consumer that has
    ///     already torn its transport down passes.
    ///   - stagedBytes: What the consumer holds and has not processed.
    void applyTo(Channel* channel, std::size_t stagedBytes)
    {
        if (!channel)
            return;
        if (!m_isSuspended && stagedBytes > suspendAboveBytes) {
            m_isSuspended = true;
            channel->suspendReading();
            return;
        }
        if (m_isSuspended && stagedBytes <= resumeAtOrBelowBytes) {
            m_isSuspended = false;
            channel->resumeReading();
        }
    }

    /// Whether the carrier is currently held.
    ///
    /// - Returns: Whether this asked the channel to stop and has not asked it to start again.
    [[nodiscard]] bool isSuspended() const { return m_isSuspended; }

private:
    bool m_isSuspended = false;
};

} // namespace Ubiquitty::Transport
