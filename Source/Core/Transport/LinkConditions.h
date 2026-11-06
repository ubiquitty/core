// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>

namespace Ubiquitty::Transport {

/// What a network is doing to a session, as numbers a test can set.
///
/// One value type for both directions of one carrier. A harness sets it, changes it mid-session and
/// sets it back; nothing here is read by production code, but it lives beside the carrier it
/// conditions because the semantics below are properties of the transport seam rather than of any
/// one test.
///
/// The three impairments a terminal session actually notices are latency (a keystroke's echo),
/// stalling (the link that stops passing bytes without saying so) and the cut. Bandwidth matters
/// only for the large frames — a pushed paste, a full snapshot after a reflow.
struct LinkConditions {
    /// One-way delay applied to every byte, in milliseconds.
    ///
    /// Applied per direction, so a round trip costs twice this.
    std::uint32_t latencyMilliseconds = 0;

    /// Extra delay, uniform in `[0, jitterMilliseconds]`, drawn per chunk.
    ///
    /// A reliable stream cannot reorder, so this delays a chunk and never lets it overtake the one
    /// before it: see ``ConditionedChannel``'s release-time invariant. Jitter that reordered the
    /// stream would corrupt frames and prove a decoder bug that no real network can cause.
    std::uint32_t jitterMilliseconds = 0;

    /// Throughput ceiling in bytes per second; 0 means unmetered.
    ///
    /// Spent from a token bucket, so a burst arriving under the ceiling is not delayed and one over
    /// it is spread rather than dropped.
    std::uint64_t bytesPerSecond = 0;

    /// Percentage of DATAGRAMS discarded, 0 to 100.
    ///
    /// Datagrams only: the reliable stream is retransmitted by the carrier
    /// underneath, so loss there is latency, not absence. A conditioner that dropped stream bytes
    /// would be modelling a corruption no TCP or QUIC stream delivers.
    std::uint32_t datagramLossPercent = 0;

    /// Whether the link is passing bytes at all.
    ///
    /// A blackout holds both directions and does not close: this is the half-open link — a phone
    /// that walked out of range, a NAT binding that expired, a laptop lid — where both peers still
    /// believe they are connected. It is the condition a keep-alive exists to notice, and the one a
    /// carrier's own close callback never reports. Ending it delivers what queued, which is the link
    /// coming back rather than a reconnect.
    bool blackout = false;

    /// Whether a fresh conditioner is unimpaired.
    [[nodiscard]] constexpr bool isClear() const
    {
        return latencyMilliseconds == 0 && jitterMilliseconds == 0 && bytesPerSecond == 0
            && datagramLossPercent == 0 && !blackout;
    }
};

} // namespace Ubiquitty::Transport
