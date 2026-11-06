// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <climits>
#include <cstdint>

namespace Ubiquitty {
namespace Host {

/// The poll timeout that reaches a millisecond deadline.
///
/// A deadline is a `std::uint64_t` and `poll` takes an `int`, so the conversion
/// is where a long wait can become a wrong one. A difference past `INT_MAX`
/// truncates to whatever the low bits happen to say — usually a *short* timeout
/// where a long one was due, which spins the loop instead of sleeping it.
///
/// Clamping makes the error late rather than wrong, and late is recoverable: a
/// poll that returns before its deadline recomputes and waits again,
/// where one that wrapped busy-waits until the deadline arrives on its own.
///
/// - Parameters:
///   - deadlineMilliseconds: When the caller wants to be woken.
///   - nowMilliseconds: The clock the deadline is measured against.
/// - Returns: Zero when the deadline has passed, the remainder when it fits,
///   and `INT_MAX` when it does not.
constexpr int wakeAfterMilliseconds(std::uint64_t deadlineMilliseconds, std::uint64_t nowMilliseconds)
{
    if (nowMilliseconds >= deadlineMilliseconds)
        return 0;
    const std::uint64_t remaining = deadlineMilliseconds - nowMilliseconds;
    return remaining > static_cast<std::uint64_t>(INT_MAX) ? INT_MAX : static_cast<int>(remaining);
}

/// The poll timeout that reaches a deadline given in whole seconds.
///
/// The clamp sits on the seconds rather than on the milliseconds they convert
/// to, because the product is where the overflow is: an expiry is
/// `time(nullptr) + ttl` with a ttl an operator names, so a large one leaves a
/// remainder whose product with 1000 does not fit a `std::uint64_t` at all.
///
/// The extra millisecond puts the wake past the boundary rather than on it.
///
/// - Parameters:
///   - deadlineUnix: The second the caller wants to be woken after.
///   - nowUnix: The wall clock the deadline is measured against.
/// - Returns: Zero when the deadline has passed, the remainder in milliseconds
///   when it fits, and `INT_MAX` when it does not.
constexpr int wakeAfterSeconds(std::uint64_t deadlineUnix, std::uint64_t nowUnix)
{
    constexpr std::uint64_t secondsThatFitAWake = static_cast<std::uint64_t>(INT_MAX) / 1000;
    if (nowUnix >= deadlineUnix)
        return 0;
    const std::uint64_t remainingSeconds = deadlineUnix - nowUnix;
    return remainingSeconds >= secondsThatFitAWake ? INT_MAX : static_cast<int>(remainingSeconds * 1000 + 1);
}

/// When a file that trails what it records is next written.
///
/// A change owes a write one interval later, and the changes before that
/// write ride it. A sign that what is recorded is about to change is owed
/// more: a line typed to a shell changes its directory only once the shell
/// has read the line, so a write already owed may come before what the sign
/// announces. A write serves every sign at least an interval before it; a
/// later sign is owed the next write, an interval after the latest such sign.
/// A write is owed only while a change or a sign has gone unserved, so
/// nothing is written for a sign the last write already served.
struct TrailingWrite {
    /// When the next write is due; zero while none is owed.
    std::uint64_t dueMilliseconds = 0;
    /// The latest sign no write has yet served; zero while there is none.
    std::uint64_t latestSignMilliseconds = 0;

    /// Owes a write for a change that has happened.
    ///
    /// - Parameters:
    ///   - nowMilliseconds: The clock.
    ///   - intervalMilliseconds: How long a write trails a change.
    constexpr void oweForChange(std::uint64_t nowMilliseconds, std::uint64_t intervalMilliseconds)
    {
        if (dueMilliseconds == 0)
            dueMilliseconds = nowMilliseconds + intervalMilliseconds;
    }

    /// Owes a write a whole interval after a sign that a change is coming.
    ///
    /// - Parameters:
    ///   - nowMilliseconds: The clock.
    ///   - intervalMilliseconds: How long a write trails a sign.
    constexpr void oweAfterSign(std::uint64_t nowMilliseconds, std::uint64_t intervalMilliseconds)
    {
        latestSignMilliseconds = nowMilliseconds;
        if (dueMilliseconds == 0)
            dueMilliseconds = nowMilliseconds + intervalMilliseconds;
    }

    /// Notes that the file was written: a sign it came too soon after is owed
    /// the next write.
    ///
    /// - Parameters:
    ///   - nowMilliseconds: The clock when the write sampled what it records.
    ///   - intervalMilliseconds: How long a write trails a sign.
    constexpr void noteWritten(std::uint64_t nowMilliseconds, std::uint64_t intervalMilliseconds)
    {
        if (latestSignMilliseconds != 0 && latestSignMilliseconds + intervalMilliseconds > nowMilliseconds) {
            dueMilliseconds = latestSignMilliseconds + intervalMilliseconds;
            return;
        }
        dueMilliseconds = 0;
        latestSignMilliseconds = 0;
    }

    /// Reports whether a write is owed.
    /// - Returns: Whether one is.
    [[nodiscard]] constexpr bool isOwed() const { return dueMilliseconds != 0; }

    /// Reports whether the next write's time has come.
    ///
    /// - Parameter nowMilliseconds: The clock.
    /// - Returns: Whether a write is owed and due.
    [[nodiscard]] constexpr bool isDue(std::uint64_t nowMilliseconds) const { return isOwed() && nowMilliseconds >= dueMilliseconds; }
};

} // namespace Host
} // namespace Ubiquitty
