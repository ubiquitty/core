// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Base/ThreadSafety.h"
#include "PAL/PAL.h"

#include <atomic>
#include <cassert>
#include <cstdint>
#include <mutex>
#include <thread>

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

/// What Clang's thread-safety analysis knows a thread holds while it holds a
/// pane's model lock.
///
/// One immutable object stands for every ``ModelLock``. The lock is reached
/// through a session's own member, the active-lock pointer an adopted session
/// repoints, the drain host's pointer and a tmux pane's handle, and the
/// analysis counts each spelling as a different capability. Every acquisition
/// of any model lock grants this role instead, so "the model lock is held" is
/// one fact wherever it is checked. The role does not tell panes apart.
class UBIQUITTY_CAPABILITY("model lock") ModelLockRole final { };

/// The capability every model-lock acquisition grants, and every model member
/// and model-reading function names.
inline constexpr ModelLockRole modelLockRole {};

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
///
/// Every acquisition grants ``modelLockRole`` to the thread-safety analysis and
/// every release revokes it. The bodies are unchecked: they hold the lock
/// through `std::mutex`, which the analysis does not see.
///
/// Every build records the holding thread, so ``assertHeld()``, where Core is
/// built with assertions, checks at run time what a callback reached through
/// a function pointer cannot prove to the analysis.
class ModelLock {
public:
    ModelLock() = default;
    ModelLock(const ModelLock&) = delete;
    ModelLock& operator=(const ModelLock&) = delete;

    /// Takes the lock on any thread but the drain's, counting the wait when the
    /// lock is held so the drain's next release hands the lock over.
    void lock() UBIQUITTY_ACQUIRES(modelLockRole) UBIQUITTY_NO_THREAD_SAFETY_ANALYSIS
    {
        if (!m_mutex.try_lock()) {
            m_waiterCount.fetch_add(1, std::memory_order_seq_cst);
            m_mutex.lock();
            m_waiterCount.fetch_sub(1, std::memory_order_relaxed);
        }
        noteHolder();
        noteAcquired();
        enterAsReader();
    }

    /// Takes the lock if no thread holds it, never waiting and never counted.
    ///
    /// Spelled as the standard Lockable requirement spells it, so
    /// `std::unique_lock` with `std::try_to_lock` takes this lock.
    ///
    /// - Returns: Whether the lock was taken.
    [[nodiscard]] bool try_lock() UBIQUITTY_TRY_ACQUIRES(true, modelLockRole) UBIQUITTY_NO_THREAD_SAFETY_ANALYSIS
    {
        if (!m_mutex.try_lock())
            return false;
        noteHolder();
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
    void unlock() UBIQUITTY_RELEASES(modelLockRole) UBIQUITTY_NO_THREAD_SAFETY_ANALYSIS
    {
        noteReleased();
        m_mutex.unlock();
    }

    /// Takes the lock for the draining thread, which never counts as a waiter.
    void lockForDrain() UBIQUITTY_ACQUIRES(modelLockRole) UBIQUITTY_NO_THREAD_SAFETY_ANALYSIS
    {
        m_mutex.lock();
        noteHolder();
    }

    /// Releases the drain's hold, handing the lock to a counted waiter first.
    ///
    /// The handoff ends when any other thread has acquired the lock since the
    /// release, or when the band runs out. It probes without a system call for
    /// ``modelLockHandoffProbeMicroseconds`` and yields after that.
    ///
    /// - Returns: What the release did, and the microseconds it spent.
    ModelLockRelease unlockForDrain() UBIQUITTY_RELEASES(modelLockRole) UBIQUITTY_NO_THREAD_SAFETY_ANALYSIS
    {
        const std::uint32_t acquisitions = m_acquisitionCount.load(std::memory_order_relaxed);
        noteReleased();
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

#ifdef UBIQUITTY_ENABLE_TEST_HOOKS
    /// The acquisitions by threads other than the drain's so far, for a test
    /// that requires a call to take the lock not once.
    ///
    /// - Returns: The count, which only grows, wrapping at 2^32.
    [[nodiscard]] std::uint32_t acquisitionCountForTest() const noexcept { return m_acquisitionCount.load(std::memory_order_acquire); }
#endif

    /// Returns on the thread that holds this lock; where Core is built with
    /// assertions, stops any other.
    ///
    /// For a callback the lock's holder reaches through a function pointer or
    /// an interface the analysis does not annotate: the analysis takes the
    /// lock as held after the call, and the assertion checks it. Defined out
    /// of line, so a binary holds one definition and checks as the Core it
    /// links was built, whatever a frontend's own translation units define.
    void assertHeld() const UBIQUITTY_ASSERTS_CAPABILITY(modelLockRole);

private:
    /// Records the calling thread as the holder, in every build.
    ///
    /// Not only where assertions are on: these are inline, so a binary can
    /// run one translation unit's copy of the acquisition and another's of
    /// ``assertHeld()``, compiled with and without NDEBUG — an archive built
    /// optimised under a frontend whose C++ interop keeps assertions. The
    /// holder is kept whichever copy took the lock, and only the check
    /// depends on NDEBUG.
    ///
    /// Relaxed, because only the holder's own thread compares its identity
    /// with it: it reads its own store, and a thread that does not hold the
    /// lock never reads its own identity there.
    void noteHolder() { m_holder.store(std::this_thread::get_id(), std::memory_order_relaxed); }

    /// Clears the holder before the lock is released, in every build.
    void noteReleased() { m_holder.store(std::thread::id(), std::memory_order_relaxed); }

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
    /// The thread holding the lock, recorded in every build.
    std::atomic<std::thread::id> m_holder {};
};

/// A scope holding a model lock, taken through ``ModelLock/lock()`` and
/// released when the scope ends.
///
/// The guard the thread-safety analysis credits with ``modelLockRole``. A
/// `std::scoped_lock` over a ``ModelLock`` takes the lock but grants no role,
/// so a function that holds one and then touches the model does not compile
/// under Clang's analysis.
class UBIQUITTY_SCOPED_CAPABILITY ModelLockHold final {
public:
    /// Takes the lock as ``ModelLock/lock()`` does, counted as a waiter when
    /// it is held.
    ///
    /// - Parameter lock: The pane's model lock, outliving the scope.
    explicit ModelLockHold(ModelLock& lock) UBIQUITTY_ACQUIRES(modelLockRole)
        : m_lock(lock)
    {
        m_lock.lock();
    }

    /// Adopts a hold the caller already took, such as a successful
    /// ``ModelLock/try_lock()``, and releases it when the scope ends.
    ///
    /// - Parameter lock: The pane's model lock, held by this thread.
    ModelLockHold(ModelLock& lock, std::adopt_lock_t) UBIQUITTY_REQUIRES(modelLockRole)
        : m_lock(lock)
    {
        m_lock.assertHeld();
    }

    /// Releases the lock the scope holds.
    ~ModelLockHold() UBIQUITTY_RELEASES() { m_lock.unlock(); }

    ModelLockHold(const ModelLockHold&) = delete;
    ModelLockHold& operator=(const ModelLockHold&) = delete;

private:
    ModelLock& m_lock;
};

} // namespace Ubiquitty
