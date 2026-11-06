// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

//
// ReconnectBackoff.h — how long a client waits before it dials a host again.
//
// One schedule for everything that redials on its own: an attached session
// whose transport dropped, and a watch that follows a host. The first few
// attempts come a second apart, because a daemon that restarted or an ssh
// that blinked is back that fast and the person wants what they were looking
// at. Past them the host is not blinking, it is down, and the wait doubles to
// a ceiling: a client asking every second is a client nobody asked to poll.
//

namespace Ubiquitty::Session {

/// The wait before each of the first attempts after a drop.
inline constexpr double reconnectDelaySeconds = 1.0;

/// How many attempts keep that wait before it starts to double.
inline constexpr unsigned reconnectFastAttempts = 3;

/// Where the doubling stops: long enough to stop hammering, short enough
/// that a host coming back is noticed while the person is still watching.
inline constexpr double reconnectMaxDelaySeconds = 30.0;

/// Returns the delay before a reconnect attempt.
///
/// Flat for the first `reconnectFastAttempts`, then doubling to a ceiling. The
/// flat head is the ssh blip that resolves itself; the doubling tail is the
/// host that is genuinely gone, which a fixed timer would ask about forever.
///
/// - Parameter attempt: How many attempts have already failed, counting from
///   1 for the first retry after a drop.
/// - Returns: Seconds to wait.
[[nodiscard]] constexpr double reconnectDelayForAttempt(unsigned attempt)
{
    if (attempt <= reconnectFastAttempts)
        return reconnectDelaySeconds;
    double delay = reconnectDelaySeconds;
    for (unsigned doubled = reconnectFastAttempts; doubled < attempt; ++doubled) {
        delay *= 2.0;
        if (delay >= reconnectMaxDelaySeconds)
            return reconnectMaxDelaySeconds;
    }
    return delay;
}

} // namespace Ubiquitty::Session
