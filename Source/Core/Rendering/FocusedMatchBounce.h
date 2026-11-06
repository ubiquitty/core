// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>

namespace Ubiquitty::Rendering {

/// The clock behind the focused pill's bounce: how long ago the active
/// search match took focus, in seconds, read off the frame tick.
///
/// One per frame producer, fed the search state's active-match epoch and the
/// frame's monotonic tick each frame. The first frame that sees the epoch
/// move is the match's arrival, and the age counts from that frame's tick.
/// With no search state the age is negative, which every reader takes as a
/// pill at rest; a snapshot tool that pins its tick at zero sees an age of
/// zero, which the bounce curve maps to rest as well.
struct FocusedMatchBounceClock {
    /// The epoch seen on the last frame, and whether any has been seen.
    std::uint32_t lastEpoch = 0;
    bool hasSeenEpoch = false;
    /// The tick of the frame on which the current epoch arrived.
    std::uint32_t focusedAtTicks = 0;

    /// Advances the clock by one frame and returns the pill's age.
    ///
    /// - Parameters:
    ///   - epoch: `ScrollSearch::activeMatchEpoch` for the frame's terminal.
    ///   - nowTicks: The frame's monotonic tick, in the platform's 60 Hz ticks.
    /// - Returns: Seconds since the active match took focus, or a negative
    ///   value when there is no search state.
    float ageSeconds(std::uint32_t epoch, std::uint32_t nowTicks)
    {
        if (!hasSeenEpoch || epoch != lastEpoch) {
            hasSeenEpoch = true;
            lastEpoch = epoch;
            focusedAtTicks = nowTicks;
        }
        if (epoch == 0)
            return -1.0f;
        return static_cast<float>(nowTicks - focusedAtTicks) / 60.0f;
    }
};

} // namespace Ubiquitty::Rendering
