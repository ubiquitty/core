// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "POSIXChildProcess.h"

#include "Harness.h"

#include <array>
#include <cstddef>
#include <cstring>
#include <initializer_list>

namespace {

struct WaitResult {
    pid_t process;
    int error = 0;
    int status = 0;
};

struct WaitCall {
    pid_t process;
    int flags;
};

struct SignalCall {
    pid_t process;
    int signal;
};

struct ChildCallState {
    struct sigaction disposition {};
    int dispositionError = 0;
    int signalError = 0;
    std::size_t dispositionQueries = 0;
    std::array<WaitResult, 16> results {};
    std::size_t resultCount = 0;
    std::array<WaitCall, 32> waits {};
    std::size_t waitCount = 0;
    std::array<SignalCall, 16> signals {};
    std::size_t signalCount = 0;

    /// Adds one scripted result without invoking a system call.
    void queueWait(pid_t process, int error = 0, int status = 0)
    {
        REQUIRE(resultCount < results.size());
        results[resultCount++] = { process, error, status };
    }
};

struct MockChildCalls {
    ChildCallState* state;

    /// Supplies the scripted child disposition without querying the process.
    int childDisposition(struct sigaction& disposition) const
    {
        ++state->dispositionQueries;
        if (state->dispositionError) {
            errno = state->dispositionError;
            return -1;
        }
        disposition = state->disposition;
        return 0;
    }

    /// Records an exact-child wait and returns its scripted outcome.
    pid_t wait(pid_t process, int* status, int flags) const
    {
        CHECK(state->waitCount < state->waits.size());
        if (state->waitCount >= state->waits.size()) {
            errno = EOVERFLOW;
            return -1;
        }
        const auto index = state->waitCount++;
        state->waits[index] = { process, flags };
        if (index >= state->resultCount) {
            errno = ECHILD;
            return -1;
        }
        const auto& result = state->results[index];
        if (result.process > 0)
            *status = result.status;
        errno = result.error;
        return result.process;
    }

    /// Records a signal attempt without delivering any signal.
    int sendSignal(pid_t process, int signal) const
    {
        CHECK(state->signalCount < state->signals.size());
        if (state->signalCount >= state->signals.size()) {
            errno = EOVERFLOW;
            return -1;
        }
        state->signals[state->signalCount++] = { process, signal };
        errno = state->signalError;
        return state->signalError ? -1 : 0;
    }
};

using MockChild = UbiquittyTest::OwnedChildProcess<MockChildCalls>;

/// Supplies a nondefault signal handler value without installing it.
void childHandler(int) { }

} // namespace

TEST(invalidForkResultsCannotBecomeSignalOrWaitTargets)
{
    ChildCallState calls;
    MockChild child(MockChildCalls { &calls });
    for (const pid_t invalid : { -1, 0, 1 }) {
        CHECK(!child.adoptForkResult(invalid));
        CHECK_EQ(child.processID(), -1);
        CHECK(!child.signal(SIGKILL));
        CHECK_EQ(child.reap(WNOHANG), -1);
        CHECK_EQ(errno, ECHILD);
        CHECK(child.stop());
    }
    CHECK_EQ(calls.dispositionQueries, 0u);
    CHECK_EQ(calls.waitCount, 0u);
    CHECK_EQ(calls.signalCount, 0u);
}

TEST(anOwnedChildCannotBeReplacedByAnotherForkResult)
{
    ChildCallState calls;
    MockChild child(MockChildCalls { &calls });
    REQUIRE(child.adoptForkResult(17));
    CHECK(!child.adoptForkResult(23));
    CHECK_EQ(child.processID(), 17);
    CHECK_EQ(calls.dispositionQueries, 1u);
}

TEST(aSecondForkFailureCleansOnlyTheFirstOwnedChild)
{
    ChildCallState firstCalls;
    ChildCallState secondCalls;
    firstCalls.queueWait(0);
    firstCalls.queueWait(17);
    {
        MockChild first(MockChildCalls { &firstCalls });
        MockChild second(MockChildCalls { &secondCalls });
        REQUIRE(first.adoptForkResult(17));
        CHECK(!second.adoptForkResult(-1));
    }
    REQUIRE(firstCalls.signalCount == 1);
    CHECK_EQ(firstCalls.signals[0].process, 17);
    CHECK_EQ(secondCalls.signalCount, 0u);
    CHECK_EQ(secondCalls.waitCount, 0u);
}

TEST(aMatchingReapRetiresThePIDAndPublishesOnlyItsStatus)
{
    ChildCallState calls;
    calls.queueWait(17, 0, 42);
    MockChild child(MockChildCalls { &calls });
    REQUIRE(child.adoptForkResult(17));
    int status = -7;
    CHECK_EQ(child.reap(0, &status), 17);
    CHECK_EQ(status, 42);
    CHECK_EQ(child.processID(), -1);
    CHECK(!child.signal(SIGTERM));
    CHECK(child.stop());
    REQUIRE(calls.waitCount == 1);
    CHECK_EQ(calls.waits[0].process, 17);
    CHECK_EQ(calls.waits[0].flags, 0);
    CHECK_EQ(calls.signalCount, 0u);
}

TEST(aMissingChildRetractsAuthorityBeforeAnySignal)
{
    ChildCallState calls;
    calls.queueWait(-1, ECHILD);
    MockChild child(MockChildCalls { &calls });
    REQUIRE(child.adoptForkResult(17));
    CHECK(!child.signal(SIGKILL));
    CHECK_EQ(child.processID(), -1);
    CHECK(child.stop());
    CHECK(!child.signal(SIGTERM));
    CHECK_EQ(calls.waitCount, 1u);
    CHECK_EQ(calls.signalCount, 0u);
}

TEST(anExitedChildIsReapedBeforeCleanupCanSignalAReusedPID)
{
    ChildCallState calls;
    calls.queueWait(17);
    MockChild child(MockChildCalls { &calls });
    REQUIRE(child.adoptForkResult(17));
    CHECK(!child.signal(SIGTERM));
    CHECK_EQ(child.processID(), -1);
    CHECK_EQ(calls.signalCount, 0u);
    CHECK(child.stop());
}

TEST(interruptedOwnershipChecksRetryTheSameExactChild)
{
    ChildCallState calls;
    calls.queueWait(-1, EINTR);
    calls.queueWait(0);
    MockChild child(MockChildCalls { &calls });
    REQUIRE(child.adoptForkResult(17));
    CHECK(child.signal(SIGTERM));
    REQUIRE(calls.waitCount == 2);
    for (std::size_t index = 0; index < calls.waitCount; ++index) {
        CHECK_EQ(calls.waits[index].process, 17);
        CHECK_EQ(calls.waits[index].flags, WNOHANG);
    }
    REQUIRE(calls.signalCount == 1);
    CHECK_EQ(calls.signals[0].process, 17);
    CHECK_EQ(calls.signals[0].signal, SIGTERM);
}

TEST(unexpectedWaitErrorsNeverGrantSignalAuthority)
{
    for (const int error : { EINVAL, EIO, EPERM }) {
        ChildCallState calls;
        calls.queueWait(-1, error);
        MockChild child(MockChildCalls { &calls });
        REQUIRE(child.adoptForkResult(17));
        CHECK(!child.signal(SIGKILL));
        CHECK_EQ(child.processID(), 17);
        CHECK_EQ(calls.signalCount, 0u);
    }
}

TEST(nonmatchingWaitResultsCannotAuthorizeASignalOrPublishStatus)
{
    ChildCallState calls;
    calls.queueWait(23, 0, 42);
    calls.queueWait(-1, EIO);
    MockChild child(MockChildCalls { &calls });
    REQUIRE(child.adoptForkResult(17));
    CHECK(!child.signal(SIGKILL));
    CHECK_EQ(child.processID(), 17);
    CHECK_EQ(calls.signalCount, 0u);
    int status = -7;
    CHECK_EQ(child.reap(WNOHANG, &status), -1);
    CHECK_EQ(status, -7);
    CHECK_EQ(child.processID(), 17);
}

TEST(autoReapingOrUnknownChildDispositionsCannotAcquireOwnership)
{
    for (unsigned scenario = 0; scenario < 4; ++scenario) {
        ChildCallState calls;
        switch (scenario) {
        case 0:
            calls.disposition.sa_handler = SIG_IGN;
            break;
        case 1:
            calls.disposition.sa_flags = SA_NOCLDWAIT;
            break;
        case 2:
            calls.disposition.sa_handler = childHandler;
            break;
        case 3:
            calls.dispositionError = EIO;
            break;
        }
        MockChild child(MockChildCalls { &calls });
        CHECK(!child.canOwnChildren());
        CHECK(!child.adoptForkResult(17));
        CHECK(!child.signal(SIGKILL));
        CHECK(child.stop());
        CHECK_EQ(child.processID(), -1);
        CHECK_EQ(calls.waitCount, 0u);
        CHECK_EQ(calls.signalCount, 0u);
    }
}

TEST(aChangedChildDispositionPreventsSignalsToPreviouslyOwnedPIDs)
{
    for (unsigned scenario = 0; scenario < 3; ++scenario) {
        ChildCallState calls;
        MockChild child(MockChildCalls { &calls });
        REQUIRE(child.adoptForkResult(17));
        if (scenario == 0)
            calls.disposition.sa_handler = SIG_IGN;
        else if (scenario == 1)
            calls.disposition.sa_flags = SA_NOCLDWAIT;
        else
            calls.dispositionError = EIO;
        CHECK(!child.signal(SIGKILL));
        CHECK_EQ(child.processID(), -1);
        calls.disposition = {};
        calls.dispositionError = 0;
        calls.queueWait(0);
        CHECK(!child.signal(SIGTERM));
        CHECK(child.stop());
        CHECK_EQ(calls.waitCount, 0u);
        CHECK_EQ(calls.signalCount, 0u);
    }
}

TEST(aWaitCannotReviveOwnershipAfterChildDispositionChanges)
{
    ChildCallState calls;
    MockChild child(MockChildCalls { &calls });
    REQUIRE(child.adoptForkResult(17));
    calls.disposition.sa_flags = SA_NOCLDWAIT;
    CHECK_EQ(child.reap(WNOHANG), -1);
    CHECK_EQ(child.processID(), -1);
    calls.disposition = {};
    calls.queueWait(0);
    CHECK_EQ(child.reap(WNOHANG), -1);
    CHECK(!child.signal(SIGKILL));
    CHECK_EQ(calls.waitCount, 0u);
    CHECK_EQ(calls.signalCount, 0u);
}

TEST(stoppingAChildUsesOnlyItsPositivePIDAndIsIdempotent)
{
    ChildCallState calls;
    calls.queueWait(0);
    calls.queueWait(17, 0, 42);
    {
        MockChild child(MockChildCalls { &calls });
        REQUIRE(child.adoptForkResult(17));
        CHECK(child.stop());
        CHECK_EQ(child.processID(), -1);
        CHECK(child.stop());
    }
    REQUIRE(calls.waitCount == 2);
    CHECK_EQ(calls.waits[0].process, 17);
    CHECK_EQ(calls.waits[0].flags, WNOHANG);
    CHECK_EQ(calls.waits[1].process, 17);
    CHECK_EQ(calls.waits[1].flags, 0);
    REQUIRE(calls.signalCount == 1);
    CHECK_EQ(calls.signals[0].process, 17);
    CHECK_EQ(calls.signals[0].signal, SIGKILL);
}

TEST(theDestructorUsesTheSameVerifiedChildCleanup)
{
    ChildCallState calls;
    calls.queueWait(0);
    calls.queueWait(-1, EINTR);
    calls.queueWait(17);
    {
        MockChild child(MockChildCalls { &calls });
        REQUIRE(child.adoptForkResult(17));
    }
    CHECK_EQ(calls.waitCount, 3u);
    REQUIRE(calls.signalCount == 1);
    CHECK_EQ(calls.signals[0].process, 17);
    CHECK_EQ(calls.signals[0].signal, SIGKILL);
}

TEST(aFailedSignalIsNotEscalatedAndDoesNotPretendTheChildWasReaped)
{
    for (const int error : { EPERM, ESRCH, EINVAL }) {
        ChildCallState calls;
        calls.signalError = error;
        calls.queueWait(0);
        MockChild child(MockChildCalls { &calls });
        REQUIRE(child.adoptForkResult(17));
        CHECK(!child.stop());
        CHECK_EQ(child.processID(), 17);
        REQUIRE(calls.signalCount == 1);
        CHECK_EQ(calls.signals[0].process, 17);
        CHECK_EQ(calls.signals[0].signal, SIGKILL);
        CHECK_EQ(calls.waitCount, 1u);
        CHECK(child.stop()); // The next exact wait reports ECHILD.
        CHECK_EQ(child.processID(), -1);
        CHECK_EQ(calls.signalCount, 1u);
    }
}

TEST(aFailedCleanupWaitCannotSignalWithoutConfirmedOwnership)
{
    ChildCallState calls;
    calls.queueWait(-1, EIO);
    MockChild child(MockChildCalls { &calls });
    REQUIRE(child.adoptForkResult(17));
    CHECK(!child.stop());
    CHECK_EQ(child.processID(), 17);
    CHECK_EQ(calls.waitCount, 1u);
    CHECK_EQ(calls.signalCount, 0u);
}

TEST(shortOrInvalidProcessReportsCannotSupplyAnObservedPID)
{
    for (const ssize_t length : { ssize_t { -1 }, ssize_t { 0 }, static_cast<ssize_t>(sizeof(pid_t) - 1), static_cast<ssize_t>(sizeof(pid_t) + 1) }) {
        const auto observed = UbiquittyTest::readReportedProcessID([&](void* output, std::size_t capacity) {
            CHECK_EQ(capacity, sizeof(pid_t));
            CHECK_EQ(*static_cast<pid_t*>(output), -1);
            const pid_t candidate = 17;
            std::memcpy(output, &candidate, sizeof(candidate));
            return length;
        });
        CHECK(!observed);
    }
    for (const pid_t candidate : { -1, 0, 1 }) {
        const auto observed = UbiquittyTest::readReportedProcessID([&](void* output, std::size_t capacity) {
            CHECK_EQ(capacity, sizeof(pid_t));
            std::memcpy(output, &candidate, sizeof(candidate));
            return static_cast<ssize_t>(sizeof(candidate));
        });
        CHECK(!observed);
    }
}

TEST(anExactPositiveProcessReportRemainsAnObservationOnly)
{
    const auto observed = UbiquittyTest::readReportedProcessID([](void* output, std::size_t capacity) {
        CHECK_EQ(capacity, sizeof(pid_t));
        CHECK_EQ(*static_cast<pid_t*>(output), -1);
        constexpr pid_t candidate = 17;
        std::memcpy(output, &candidate, sizeof(candidate));
        return static_cast<ssize_t>(sizeof(candidate));
    });
    REQUIRE(observed);
    CHECK_EQ(*observed, 17);
    ChildCallState calls;
    MockChild child(MockChildCalls { &calls });
    CHECK_EQ(child.processID(), -1);
    CHECK(!child.signal(SIGKILL));
    CHECK_EQ(calls.signalCount, 0u);
}

UBIQUITTY_TEST_MAIN()
