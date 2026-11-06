// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
//
// Source/Platform/POSIX/QUICTimestamp.h — the timestamps one ngtcp2 connection
// is given.
//
// ngtcp2 asserts that every timestamp a connection receives is at least the
// last one (`conn->log.last_ts <= ts`), and the assertion aborts the process. On
// the daemon that ends every session it holds, not only the connection that
// tripped it. So each connection takes its timestamps from one object that
// reads one clock and never answers lower than it answered before.
//
// The clock is the raw monotonic one. Darwin's CLOCK_MONOTONIC is derived from
// the wall clock less the boot time, at microsecond resolution, and moves when
// the wall clock is adjusted; CLOCK_MONOTONIC_RAW is the hardware counter and
// keeps counting across sleep, which the idle and loss timers need. Elsewhere
// CLOCK_MONOTONIC is already the hardware counter. The clamp holds the rule
// whatever the clock does.

#include <time.h>

#include <cstdint>
#include <functional>
#include <utility>

namespace Ubiquitty::POSIX {

/// Reads the clock QUIC connections run on, in nanoseconds.
///
/// - Returns: Nanoseconds since an arbitrary fixed point.
[[nodiscard]] inline std::uint64_t quicClockNanoseconds()
{
#if defined(__APPLE__)
    return clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW);
#else
    struct timespec now;
    ::clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<std::uint64_t>(now.tv_sec) * 1000000000ull + static_cast<std::uint64_t>(now.tv_nsec);
#endif
}

/// Non-decreasing timestamps for one ngtcp2 connection.
///
/// Owned by the connection and read only by the thread that drives it, so every
/// call into that connection, from any call site, takes its timestamp here.
class QUICTimestamp {
public:
    /// Where readings come from when not the QUIC clock.
    using Source = std::function<std::uint64_t()>;

    /// Answers the current time, never lower than any earlier answer.
    ///
    /// - Returns: Nanoseconds on the QUIC clock, clamped to the last answer.
    [[nodiscard]] std::uint64_t now()
    {
        const std::uint64_t reading = m_source ? m_source() : quicClockNanoseconds();
        if (reading > m_last)
            m_last = reading;
        return m_last;
    }

    /// Replaces the clock this connection reads.
    ///
    /// For tests that drive the call paths with a clock that steps backwards.
    ///
    /// - Parameter source: The replacement; an empty one restores the QUIC clock.
    void setSourceForTesting(Source source) { m_source = std::move(source); }

private:
    Source m_source;
    std::uint64_t m_last = 0;
};

} // namespace Ubiquitty::POSIX
