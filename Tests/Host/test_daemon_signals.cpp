// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"
#include "Platform/Host/DaemonSignals.h"

#include <poll.h>

using Ubiquitty::Host::DaemonSignalEvent;
using Ubiquitty::Host::DaemonSignals;

TEST(aControlSignalBeforePollLeavesAReadableWake)
{
    DaemonSignals signals;
    REQUIRE(signals.isReady());
    CHECK(signals.takePending() == 0);
    signals.post(SIGTERM);
    pollfd waiting { signals.fileDescriptor(), POLLIN, 0 };
    CHECK(::poll(&waiting, 1, 0) == 1);
    CHECK(waiting.revents & POLLIN);
    CHECK(DaemonSignals::hasEvent(signals.takePending(), DaemonSignalEvent::Stop));
    signals.drain();
    CHECK(::poll(&waiting, 1, 0) == 0);
    CHECK(signals.takePending() == 0);
}

TEST(drainingAnEarlierWakeKeepsLaterControlWork)
{
    DaemonSignals signals;
    REQUIRE(signals.isReady());
    signals.post(SIGHUP);
    CHECK(DaemonSignals::hasEvent(signals.takePending(), DaemonSignalEvent::ReloadCredentials));
    signals.post(SIGUSR1);
    signals.drain();
    CHECK(DaemonSignals::hasEvent(signals.takePending(), DaemonSignalEvent::LogHoldState));
    pollfd waiting { signals.fileDescriptor(), POLLIN, 0 };
    CHECK(::poll(&waiting, 1, 0) == 0);
}

TEST(controlSignalsCoalesceWithoutBlockingOrChangingErrno)
{
    DaemonSignals signals;
    REQUIRE(signals.isReady());
    errno = EDOM;
    for (int index = 0; index < 100'000; ++index)
        signals.post(SIGINT);
    signals.post(SIGUSR2);
    CHECK(errno == EDOM);
    const auto pending = signals.takePending();
    CHECK(DaemonSignals::hasEvent(pending, DaemonSignalEvent::Stop));
    CHECK(DaemonSignals::hasEvent(pending, DaemonSignalEvent::StopIfIdle));
    signals.drain();
    pollfd waiting { signals.fileDescriptor(), POLLIN, 0 };
    CHECK(::poll(&waiting, 1, 0) == 0);
}

UBIQUITTY_TEST_MAIN()
