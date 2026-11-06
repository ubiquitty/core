// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <atomic>
#include <cstddef>
#include <limits>

namespace UbiquittyTest {

/// Scopes an allocation failure injected by a test binary's nothrow allocator.
///
/// The allocator is process-wide, so its control has one thread-safe owner.
/// Scopes are nested on the test thread; allocator calls may observe the policy
/// from any thread. A test replaces only the allocation form it exercises.
class ScopedNothrowAllocationRefusal {
public:
    /// Refuses every size by default, or only the positive size named.
    ///
    /// - Parameter size: The allocation size to refuse; the maximum value
    ///   refuses every size, and zero disables refusals within this scope.
    explicit ScopedNothrowAllocationRefusal(std::size_t size = std::numeric_limits<std::size_t>::max())
        : m_previousSize(state().refusedSize.exchange(size))
    {
    }

    /// Restores the enclosing scope's allocation policy.
    ~ScopedNothrowAllocationRefusal() { state().refusedSize.store(m_previousSize); }

    ScopedNothrowAllocationRefusal(const ScopedNothrowAllocationRefusal&) = delete;
    ScopedNothrowAllocationRefusal& operator=(const ScopedNothrowAllocationRefusal&) = delete;

    /// Records whether the active policy refuses this allocation.
    ///
    /// - Parameter size: The number of bytes requested by the allocator.
    /// - Returns: Whether the allocator must return a null pointer.
    [[nodiscard]] static bool refuses(std::size_t size)
    {
        State& control = state();
        ++control.attempts;
        const std::size_t requested = control.refusedSize.load();
        if (!requested || (requested != std::numeric_limits<std::size_t>::max() && requested != size))
            return false;
        ++control.refusals;
        return true;
    }

    /// The number of allocation attempts refused by this process's injector.
    [[nodiscard]] static std::size_t refusalCount() { return state().refusals.load(); }
    /// Every allocation attempt routed through this injector, refused or not.
    [[nodiscard]] static std::size_t attemptCount() { return state().attempts.load(); }

private:
    /// The process-wide policy and count shared by allocator callbacks.
    struct State {
        std::atomic<std::size_t> refusedSize { 0 };
        std::atomic<std::size_t> refusals { 0 };
        std::atomic<std::size_t> attempts { 0 };
    };

    /// Returns the policy owner with a lifetime spanning every test scope.
    ///
    /// - Returns: The process-wide allocator control.
    static State& state()
    {
        static State control;
        return control;
    }

    const std::size_t m_previousSize;
};

} // namespace UbiquittyTest
