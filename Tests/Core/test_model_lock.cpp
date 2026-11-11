// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// ModelLock hands itself to every thread waiting on it when the drain releases
// it. The drain re-takes the lock for every bite with no gap, and a mutex lets
// the releasing thread take it straight back ahead of the waiter it woke, so a
// waiter that the lock did not count would wait for the bite train to stop.
// These cases wait the way most of the session's members do, through a plain
// std::lock_guard. They check the handoff by what the drain's releases report
// and by the order in which two waiters are served. A wait's length is the
// scheduler's as much as the lock's: a busy machine leaves a waiter it woke
// unscheduled for tens of milliseconds, as long as a waiter no release hands
// the lock to. So only the median wait is bounded, which a few descheduled
// waits do not move, and the longest is printed.
//
// This test links Source/Platform/POSIX/SystemPAL.cpp (see Tests/CMakeLists.txt)
// because the handoff spins on PAL::monotonicMicros and yields through
// PAL::schedulerYield; a fake clock would turn the band into an iteration count.

#include "Core/ModelLock.h"
#include "Harness.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <mutex>
#include <ranges>
#include <thread>
#include <vector>

namespace Ubiquitty::PAL {

uint32_t monotonicMicros()
{
    const auto since = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::microseconds>(since).count() & 0xFFFFFFFFull);
}

} // namespace Ubiquitty::PAL

// Defined in ModelLockArchiveCopy.cpp, compiled as a frontend's Core archive is.
namespace Ubiquitty::Testing {

void lockAsAnOptimisedArchiveDoes(ModelLock& lock) UBIQUITTY_ACQUIRES(modelLockRole);
void unlockAsAnOptimisedArchiveDoes(ModelLock& lock) UBIQUITTY_RELEASES(modelLockRole);

} // namespace Ubiquitty::Testing

namespace {

using Ubiquitty::ModelLock;
using Ubiquitty::ModelLockHandoff;
using Ubiquitty::ModelLockRelease;
using Clock = std::chrono::steady_clock;

// The parse a bite stands for, spun rather than slept: a drain parsing a flood
// holds a core, and a sleeping one would leave the gap this test must not have.
constexpr auto biteHold = std::chrono::microseconds(20);
// How long a waiter keeps the lock once it has it: a render capture's order.
constexpr auto waiterHold = std::chrono::microseconds(50);
constexpr int trialsPerWaiter = 200;
// The bound the median wait must meet: a handoff costs one bite plus the
// waiter's wake, far below this.
constexpr auto waitBound = std::chrono::milliseconds(5);

void spinFor(Clock::duration span)
{
    const auto until = Clock::now() + span;
    while (Clock::now() < until) {
    }
}

// A drain over the lock: takes it, parses a bite, releases it and takes it back
// at once, until stopped.
class GapFreeDrain {
public:
    explicit GapFreeDrain(ModelLock& lock)
        : m_thread([this, &lock] {
            while (!m_isStopping.load(std::memory_order_relaxed)) {
                lock.lockForDrain();
                spinFor(biteHold);
                if (lock.unlockForDrain().handoff == ModelLockHandoff::Taken)
                    m_handoffCount.fetch_add(1, std::memory_order_relaxed);
                m_biteCount.fetch_add(1, std::memory_order_relaxed);
            }
        })
    {
    }

    ~GapFreeDrain() { stop(); }
    GapFreeDrain(const GapFreeDrain&) = delete;
    GapFreeDrain& operator=(const GapFreeDrain&) = delete;

    void stop()
    {
        m_isStopping.store(true, std::memory_order_relaxed);
        if (m_thread.joinable())
            m_thread.join();
    }

    [[nodiscard]] std::uint64_t biteCount() const { return m_biteCount.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t handoffCount() const { return m_handoffCount.load(std::memory_order_relaxed); }

private:
    std::atomic<bool> m_isStopping { false };
    std::atomic<std::uint64_t> m_biteCount { 0 };
    std::atomic<std::uint64_t> m_handoffCount { 0 };
    std::thread m_thread;
};

// Waits for the lock through a plain std::lock_guard, `trialsPerWaiter` times,
// and returns each wait in microseconds.
std::vector<double> plainWaits(ModelLock& lock)
{
    std::vector<double> waits;
    waits.reserve(trialsPerWaiter);
    for (int trial = 0; trial < trialsPerWaiter; ++trial) {
        std::this_thread::sleep_for(std::chrono::microseconds(500 + (trial % 7) * 150));
        const auto asked = Clock::now();
        const std::lock_guard hold(lock);
        waits.push_back(std::chrono::duration<double, std::micro>(Clock::now() - asked).count());
        spinFor(waiterHold);
    }
    return waits;
}

struct WaitSummary {
    double median = 0;
    double longest = 0;
};

WaitSummary summarize(std::vector<double> waits)
{
    if (waits.empty())
        return {};
    std::ranges::sort(waits);
    return { waits[waits.size() / 2], waits.back() };
}

constexpr double boundMicros = std::chrono::duration<double, std::micro>(waitBound).count();

// A frontend whose C++ interop keeps assertions runs over a Core archive built
// with NDEBUG: the lock is taken by the archive's copy of the acquisition, and
// a callback's holder check is the frontend's. The holder is kept in every
// build, so the check holds on the thread that took the lock.
TEST(aLockTakenWhereAssertionsAreOffSatisfiesACheckWhereTheyAreOn)
{
    Ubiquitty::ModelLock lock;
    Ubiquitty::Testing::lockAsAnOptimisedArchiveDoes(lock);
    lock.assertHeld();
    Ubiquitty::Testing::unlockAsAnOptimisedArchiveDoes(lock);
    if (lock.try_lock())
        lock.unlock();
    else
        CHECK_MSG(false, "the lock was released");
}

TEST(aPlainWaiterBehindAGapFreeDrainIsHandedTheLockEveryTime)
{
    ModelLock lock;
    GapFreeDrain drain(lock);
    const WaitSummary waits = summarize(plainWaits(lock));
    drain.stop();

    char message[192];
    std::snprintf(message, sizeof(message), "%d waits: median %.1fus, longest %.1fus, bound %.0fus; %llu bites, %llu handoffs",
        trialsPerWaiter, waits.median, waits.longest, boundMicros,
        static_cast<unsigned long long>(drain.biteCount()), static_cast<unsigned long long>(drain.handoffCount()));
    std::printf("%s\n", message);
    // The drain kept biting through the whole run, so every wait was against a
    // live train rather than an idle lock, and its releases handed the lock to
    // a waiter that locked through std::lock_guard.
    CHECK_MSG(drain.biteCount() > static_cast<std::uint64_t>(trialsPerWaiter), message);
    CHECK_MSG(drain.handoffCount() > 0, message);
    CHECK_MSG(waits.median < boundMicros, message);
}

// Two waiters at once: neither one's acquisition may end the other's claim on
// the next handoff.
TEST(waitersOnTwoThreadsAreEachHandedTheLock)
{
    ModelLock lock;
    GapFreeDrain drain(lock);
    std::vector<double> otherWaits;
    std::thread other([&] { otherWaits = plainWaits(lock); });
    const WaitSummary own = summarize(plainWaits(lock));
    other.join();
    drain.stop();
    const WaitSummary theirs = summarize(otherWaits);

    char message[192];
    std::snprintf(message, sizeof(message), "longest waits %.1fus and %.1fus, medians %.1fus and %.1fus, bound %.0fus; %llu handoffs",
        own.longest, theirs.longest, own.median, theirs.median, boundMicros, static_cast<unsigned long long>(drain.handoffCount()));
    std::printf("%s\n", message);
    CHECK_MSG(drain.handoffCount() > 0, message);
    CHECK_MSG(own.median < boundMicros, message);
    CHECK_MSG(theirs.median < boundMicros, message);
}

// What the drain's next release did once one of two waiters had been handed
// the lock and let it go.
enum class SecondTurn : std::uint8_t {
    // The second waiter was still counted: the release offered it the lock.
    Offered,
    // The release found no waiter although the second one was still waiting.
    Unwanted,
    // The second waiter took the lock before the drain could take it back, so
    // this case saw no release while it waited.
    Unobserved,
};

// Trials of the claim case in which the drain takes the lock back while the
// second waiter still waits: the scenario the case examines. A loaded machine
// often serves the second waiter first, so trials run until this many form.
constexpr int claimTrials = 8;
// How long the claim case may try for its trials to form.
constexpr auto claimTrialBound = std::chrono::seconds(30);

// Two threads wait behind the drain, and the drain hands the lock to whichever
// takes it. Once that one lets go and the drain takes the lock back, the other
// is still waiting, and the drain's next release must find it counted.
SecondTurn secondWaitersTurn()
{
    ModelLock lock;
    lock.lockForDrain();
    std::atomic<int> askingCount { 0 };
    std::atomic<int> holder { -1 };
    std::array<std::atomic<bool>, 2> mayLetGo {};
    const auto wait = [&](int waiter) {
        askingCount.fetch_add(1, std::memory_order_acq_rel);
        const std::lock_guard hold(lock);
        holder.store(waiter, std::memory_order_release);
        while (!mayLetGo[static_cast<std::size_t>(waiter)].load(std::memory_order_acquire)) {
        }
        holder.store(-1, std::memory_order_release);
    };
    std::thread first(wait, 0);
    std::thread second(wait, 1);
    while (askingCount.load(std::memory_order_acquire) < 2) {
        std::this_thread::yield();
    }
    // Both count themselves before they block; the release comes only after.
    while (lock.waiterCountForTest() < 2) {
        std::this_thread::yield();
    }
    (void)lock.unlockForDrain();
    int firstHolder = -1;
    while ((firstHolder = holder.load(std::memory_order_acquire)) < 0) {
        std::this_thread::yield();
    }
    // A try, as the drain's re-take after a timed-out band is: the other
    // waiter, woken by the release, may take the lock first.
    mayLetGo[static_cast<std::size_t>(firstHolder)].store(true, std::memory_order_release);
    SecondTurn turn = SecondTurn::Unobserved;
    for (;;) {
        if (lock.try_lock()) {
            const ModelLockRelease release = lock.unlockForDrain();
            turn = release.handoff == ModelLockHandoff::Unwanted ? SecondTurn::Unwanted : SecondTurn::Offered;
            break;
        }
        if (holder.load(std::memory_order_acquire) == 1 - firstHolder)
            break;
    }
    mayLetGo[static_cast<std::size_t>(1 - firstHolder)].store(true, std::memory_order_release);
    first.join();
    second.join();
    return turn;
}

// One waiter's acquisition ends only its own claim: the drain's next release
// still finds the other waiter counted and offers it the lock.
TEST(aWaitersClaimOutlivesAnotherWaitersTurn)
{
    int offered = 0;
    int unwanted = 0;
    int unobserved = 0;
    const auto deadline = std::chrono::steady_clock::now() + claimTrialBound;
    while (offered + unwanted < claimTrials && std::chrono::steady_clock::now() < deadline) {
        switch (secondWaitersTurn()) {
        case SecondTurn::Offered:
            ++offered;
            break;
        case SecondTurn::Unwanted:
            ++unwanted;
            break;
        case SecondTurn::Unobserved:
            ++unobserved;
            break;
        }
    }
    char message[192];
    std::snprintf(message, sizeof(message), "%d trials: second waiter offered the lock %d times, found uncounted %d times, served before a release %d times",
        offered + unwanted + unobserved, offered, unwanted, unobserved);
    std::printf("%s\n", message);
    CHECK_MSG(unwanted == 0, message);
    // The premise: the drain took the lock back before the second waiter in
    // as many trials as the case examines.
    CHECK_MSG(offered + unwanted >= claimTrials, message);
}

TEST(aDrainReacquiringItsOwnLockHandsNothingOff)
{
    ModelLock lock;
    int handoffs = 0;
    for (int bite = 0; bite < 1000; ++bite) {
        lock.lockForDrain();
        if (lock.unlockForDrain().handoff != ModelLockHandoff::Unwanted)
            ++handoffs;
    }
    CHECK_EQ(handoffs, 0);
}

// What one waiter cost a drain that releases the lock and takes it straight
// back, as a flood's drain does, until the waiter holds it.
struct WaiterHandoff {
    int releaseCount = 0;
    int timeoutCount = 0;
    // Releases that reported no waiter although a timed-out one had found the
    // waiter counted; the waiter stays counted until it holds the lock.
    int unwantedAfterTimeoutCount = 0;
    std::uint32_t shortestTimeoutMicroseconds = std::numeric_limits<std::uint32_t>::max();
    bool wasTaken = false;
    std::uint32_t takenMicroseconds = 0;
    bool isHeldByWaiterAfterTaken = false;
};

// Releases one waiter may cost. A waiter the scheduler runs late misses a
// band and stays counted, so a later release hands the lock over; the bound
// only ends a case whose releases never do.
constexpr int releasesPerWaiter = 1000;
// Fresh waiters a case may try before one is handed the lock. A waiter that
// takes the free lock in the instant between a timed-out release and the
// drain's re-take was not handed it, and the case tries the next one.
constexpr int waiterTrials = 8;

// Drives the lock's primitives in an order no one scope holds, as the drain
// and a waiter interleave them, which the thread-safety analysis does not
// follow.
WaiterHandoff handOffToOneWaiter(ModelLock& lock) UBIQUITTY_NO_THREAD_SAFETY_ANALYSIS
{
    WaiterHandoff result;
    std::atomic<bool> isReleasing { false };
    lock.lockForDrain();
    std::thread waiter([&] {
        const std::lock_guard hold(lock);
        while (!isReleasing.load(std::memory_order_acquire)) {
        }
    });
    // Long enough for the waiter to find the lock held and block on it.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    bool isDrainHolding = true;
    while (result.releaseCount < releasesPerWaiter) {
        const auto release = lock.unlockForDrain();
        isDrainHolding = false;
        ++result.releaseCount;
        if (release.handoff == ModelLockHandoff::Taken) {
            result.wasTaken = true;
            result.takenMicroseconds = release.waitedMicroseconds;
            result.isHeldByWaiterAfterTaken = !lock.try_lock();
            if (!result.isHeldByWaiterAfterTaken)
                lock.unlock();
            break;
        }
        if (release.handoff == ModelLockHandoff::TimedOut) {
            ++result.timeoutCount;
            result.shortestTimeoutMicroseconds = std::min(result.shortestTimeoutMicroseconds, release.waitedMicroseconds);
        } else if (result.timeoutCount) {
            ++result.unwantedAfterTimeoutCount;
        }
        // A try, because a waiter that took the free lock after the band ran
        // out holds it until released.
        if (!lock.try_lock())
            break;
        isDrainHolding = true;
    }
    if (isDrainHolding)
        lock.unlock();
    isReleasing.store(true, std::memory_order_release);
    waiter.join();
    return result;
}

TEST(aReleaseReturnsOnceTheWaiterHoldsTheLock)
{
    ModelLock lock;
    WaiterHandoff handoff;
    int trialCount = 0;
    while (trialCount < waiterTrials && !handoff.wasTaken) {
        handoff = handOffToOneWaiter(lock);
        ++trialCount;
        // A release that found the waiter counted spends the whole band unless
        // the waiter takes the lock inside it.
        if (handoff.timeoutCount)
            CHECK(handoff.shortestTimeoutMicroseconds >= Ubiquitty::modelLockHandoffMicroseconds);
        CHECK_EQ(handoff.unwantedAfterTimeoutCount, 0);
    }

    char message[192];
    std::snprintf(message, sizeof(message), "%d waiters; the last cost %d releases, %d timed out, taken after %uus",
        trialCount, handoff.releaseCount, handoff.timeoutCount, handoff.takenMicroseconds);
    std::printf("%s\n", message);
    CHECK_MSG(handoff.wasTaken, message);
    CHECK_MSG(handoff.isHeldByWaiterAfterTaken, message);
    CHECK_MSG(handoff.takenMicroseconds < Ubiquitty::modelLockHandoffMicroseconds, message);
}

TEST(aFailedTryIsNotAWaiter)
{
    ModelLock lock;
    lock.lockForDrain();
    bool didTake = true;
    std::thread trier([&] { didTake = lock.try_lock(); });
    trier.join();
    const auto release = lock.unlockForDrain();
    CHECK(!didTake);
    CHECK(release.handoff == ModelLockHandoff::Unwanted);
}

} // namespace

UBIQUITTY_TEST_MAIN()
