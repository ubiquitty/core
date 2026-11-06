// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "PAL/FileDescriptor.h"

#include <cstdint>
#include <memory>
#include <span>

// Waiting until transports are ready, and interrupting that wait from another
// thread. Level-triggered — "which of these are ready now" — because that is the
// question every backend can answer, including Windows through WSAPoll.

namespace Ubiquitty::PAL {

/// What a caller waits for, and what a wait reports.
///
/// A flag set rather than a plain enumeration: a transport can be readable and
/// writable in the same wake, and `Error`, `HangUp` and `Invalid` arrive without
/// being asked for.
enum class Ready : uint8_t {
    None = 0,
    Readable = 1 << 0,
    Writable = 1 << 1,
    Error = 1 << 2,
    HangUp = 1 << 3,
    /// The handle is not open.
    ///
    /// Reported so a loop can fail instead of spinning: a closed handle is ready
    /// forever, so a wait that dropped this returns immediately on every
    /// iteration and burns a core with no diagnostic.
    Invalid = 1 << 4,
};

[[nodiscard]] constexpr Ready operator|(Ready a, Ready b)
{
    return static_cast<Ready>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}

[[nodiscard]] constexpr Ready operator&(Ready a, Ready b)
{
    return static_cast<Ready>(static_cast<uint8_t>(a) & static_cast<uint8_t>(b));
}

[[nodiscard]] constexpr bool contains(Ready set, Ready flags) { return (set & flags) != Ready::None; }

/// One transport's registration, and the result of the most recent wait.
struct Watch {
    FileDescriptor* handle = nullptr;
    Ready interest = Ready::Readable;
    /// Overwritten by each ``waitForReadiness``; ignored on input.
    Ready result = Ready::None;
};

/// Waits until at least one watch is ready or the timeout elapses.
///
/// The caller owns the array and the handles in it, so no backend keeps
/// registration state between calls and a loop holds its watch set in whatever
/// container it already has.
///
/// - Parameters:
///   - watches: Registrations to wait on. Each entry's `result` is overwritten.
///   - timeoutMilliseconds: Negative waits indefinitely; zero polls and returns.
/// - Returns: The number of entries with a non-empty `result`, 0 on timeout, or a
///   negative ``StreamStatus`` on failure. A signal that interrupts the wait
///   reports 0, so callers loop on the timeout they already handle instead of
///   naming `EINTR`.
[[nodiscard]] int waitForReadiness(std::span<Watch> watches, int timeoutMilliseconds);

/// Interrupts a ``waitForReadiness`` running on another thread.
///
/// A loop parked until a transport is ready still has to notice work submitted
/// from elsewhere — outbound bytes, or a request to stop. Watching a waker
/// alongside the transports is what turns that submission into a wake.
class Waker {
public:
    virtual ~Waker() = default;
    Waker(const Waker&) = delete;
    Waker& operator=(const Waker&) = delete;

    /// Wakes the loop. Safe from any thread, and coalescing: repeated calls
    /// before the loop runs cost one wake.
    virtual void wake() = 0;

    /// Clears delivered wakes.
    ///
    /// Call on the loop's own thread whenever its watch reports
    /// ``Ready/Readable``. A wake left undrained keeps the handle readable, and
    /// the next wait returns immediately.
    virtual void drain() = 0;

    /// The handle to register in a ``Watch``.
    [[nodiscard]] virtual FileDescriptor& handle() = 0;

protected:
    Waker() = default;
};

/// Creates a waker, or nullptr when the platform has none.
///
/// Null rather than a silent no-op: a loop whose wakes did nothing would park
/// until its next timeout, turning a missing capability into latency no
/// diagnostic explains.
[[nodiscard]] std::unique_ptr<Waker> makeWaker();

} // namespace Ubiquitty::PAL
