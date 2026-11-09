// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "PAL/PAL.h"

#include <atomic>
#include <cstdint>
#include <mutex>

namespace Ubiquitty {

/// Bound on a drain's handoff of the model lock to a waiting thread: long
/// enough to cover a woken waiter's scheduling latency, short enough that a
/// waiter the scheduler does not run cannot stall parsing.
inline constexpr std::uint32_t modelLockHandoffMicroseconds = 150;

/// The load-only probe band in front of the handoff's yield tail: a waiter on
/// another core usually takes the lock within a few microseconds, so the
/// common handoff costs no system call; only a waiter sharing the drain's core
/// needs the yield.
inline constexpr std::uint32_t modelLockHandoffProbeMicroseconds = 20;

/// What a drain's release of the model lock did for the threads waiting on it.
enum class ModelLockHandoff : std::uint8_t {
    /// No thread was waiting.
    Unwanted,
    /// A waiting thread took the lock inside the handoff band.
    Taken,
    /// The band ran out before any waiting thread took the lock.
    TimedOut,
};

/// A drain's release of the model lock, and what it cost the drain.
struct ModelLockRelease {
    /// What the release did for the threads waiting on the lock.
    ModelLockHandoff handoff = ModelLockHandoff::Unwanted;
    /// Microseconds the drain spent handing the lock off.
    std::uint32_t waitedMicroseconds = 0;
};

/// The lock over one terminal model.
///
/// One thread drains bytes into the model, taking and releasing this lock for
/// every bite with no gap between a release and the next acquisition. Every
/// other thread that reads or writes the model takes it too. A mutex hands
/// nothing off on release, and a first-fit one (Darwin's default) lets the
/// releasing thread take it straight back ahead of the thread it woke, so a
/// waiter behind a gap-free bite train waits for the train to stop.
///
/// The fairness therefore lives here, on the one lock every party takes: an
/// acquisition through ``lock()`` that finds the lock held counts itself as a
/// waiter before it blocks, and the drain's ``unlockForDrain()`` hands the lock
/// to any counted waiter before it may take the lock again. Which wrapper a
/// waiter locked through, and which object owns the model, make no difference:
/// the count is on the lock the drain releases.
///
/// The draining thread takes the lock through ``lockForDrain()``, which is never
/// counted, so its own re-acquisition is not a waiter it hands off to. A waiter
/// the scheduler does not run within ``modelLockHandoffMicroseconds`` costs the
/// drain that band and no more; it stays counted, so the next release hands off
/// again.
///
/// Every acquisition but the drain's runs the owner's reader entry once it
/// holds the lock: the drain may leave work its bites defer, and a reader
/// must find it done.
///
/// The uncontended cost is one try-acquire, one store and the reader entry for
/// a waiter, and one load before and one after the release for the drain.
class ModelLock {
public:
    ModelLock() = default;
    ModelLock(const ModelLock&) = delete;
    ModelLock& operator=(const ModelLock&) = delete;

    /// Takes the lock on any thread but the drain's, counting the wait when the
    /// lock is held so the drain's next release hands the lock over.
    void lock()
    {
        if (!m_mutex.try_lock()) {
            m_waiterCount.fetch_add(1, std::memory_order_seq_cst);
            m_mutex.lock();
            m_waiterCount.fetch_sub(1, std::memory_order_relaxed);
        }
        noteAcquired();
        enterAsReader();
    }

    /// Takes the lock if no thread holds it, never waiting and never counted.
    ///
    /// Spelled as the standard Lockable requirement spells it, so
    /// `std::unique_lock` with `std::try_to_lock` takes this lock.
    ///
    /// - Returns: Whether the lock was taken.
    [[nodiscard]] bool try_lock()
    {
        if (!m_mutex.try_lock())
            return false;
        noteAcquired();
        enterAsReader();
        return true;
    }

    /// Installs the step every acquisition but the drain's runs once it holds
    /// the lock, before its holder reads the model.
    ///
    /// Set by the model's owner before the lock is shared, and kept for the
    /// lock's life.
    ///
    /// - Parameters:
    ///   - entry: The step, called under the lock with `context`.
    ///   - context: The step's state, outliving every acquisition.
    void setReaderEntry(void (*entry)(void*), void* context)
    {
        m_readerEntry = entry;
        m_readerEntryContext = context;
    }

    /// Releases a hold taken by ``lock()`` or ``try_lock()``.
    void unlock() { m_mutex.unlock(); }

    /// Takes the lock for the draining thread, which never counts as a waiter.
    void lockForDrain() { m_mutex.lock(); }

    /// Releases the drain's hold, handing the lock to a counted waiter first.
    ///
    /// The handoff ends when any other thread has acquired the lock since the
    /// release, or when the band runs out. It probes without a system call for
    /// ``modelLockHandoffProbeMicroseconds`` and yields after that.
    ///
    /// - Returns: What the release did, and the microseconds it spent.
    ModelLockRelease unlockForDrain()
    {
        const std::uint32_t acquisitions = m_acquisitionCount.load(std::memory_order_relaxed);
        m_mutex.unlock();
        if (!m_waiterCount.load(std::memory_order_seq_cst))
            return {};
        const std::uint32_t start = PAL::monotonicMicros();
        std::uint32_t waited = 0;
        while (m_acquisitionCount.load(std::memory_order_acquire) == acquisitions) {
            waited = PAL::monotonicMicros() - start;
            if (waited >= modelLockHandoffMicroseconds)
                return { ModelLockHandoff::TimedOut, waited };
            if (waited >= modelLockHandoffProbeMicroseconds)
                PAL::schedulerYield();
        }
        return { ModelLockHandoff::Taken, waited };
    }

private:
    /// Counts one acquisition by a thread other than the drain.
    ///
    /// Written only by such a holder, so a load and a store under the mutex
    /// count every one; atomic because the releasing drain reads the count
    /// without the mutex while it waits for the handoff.
    void noteAcquired() { m_acquisitionCount.store(m_acquisitionCount.load(std::memory_order_relaxed) + 1, std::memory_order_release); }

    /// Runs the owner's reader entry, if one is installed.
    void enterAsReader()
    {
        if (m_readerEntry)
            m_readerEntry(m_readerEntryContext);
    }

    std::mutex m_mutex;
    void (*m_readerEntry)(void*) = nullptr;
    void* m_readerEntryContext = nullptr;
    std::atomic<std::uint32_t> m_waiterCount { 0 };
    std::atomic<std::uint32_t> m_acquisitionCount { 0 };
};

} // namespace Ubiquitty
