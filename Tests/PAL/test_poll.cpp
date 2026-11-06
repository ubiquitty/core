// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// Tests/PAL/test_poll.cpp
//
// Readiness waiting and the waker that interrupts it (Source/PAL/Poll.h, backed
// by Source/Platform/POSIX/Poll.cpp). A pipe stands in for a carrier: the
// contract is about readiness, and a pipe becomes readable on demand without
// needing a peer process.
//
// Several cases pass an indefinite timeout: a backend that parked where the
// contract says it must return would hang the suite rather than fail it, and
// that is the regression worth catching loudly.
//
// Cases:
//   - an empty watch set returns without waiting
//   - a dead handle reports Invalid instead of parking forever
//   - a live handle reports nothing until bytes are pending, then Readable
//   - a write end reports Writable while it has room
//   - a waker wakes a wait, coalesces repeats, and goes quiet after drain

#include "PAL/Poll.h"

#include "PAL/UnixFileDescriptor.h"

#include <cstdio>

#include <unistd.h>

using namespace Ubiquitty;

TEST(poll)
{

    // ---- the flag set is a set ----
    {
        constexpr PAL::Ready both = PAL::Ready::Readable | PAL::Ready::Writable;
        static_assert(PAL::contains(both, PAL::Ready::Readable));
        static_assert(PAL::contains(both, PAL::Ready::Writable));
        static_assert(!PAL::contains(both, PAL::Ready::HangUp));
        static_assert(!PAL::contains(PAL::Ready::None, PAL::Ready::Readable));
        CHECK(PAL::contains(both, PAL::Ready::Readable));
    }

    // ---- an empty set is a no-op, not a park ----
    CHECK(PAL::waitForReadiness({}, -1) == 0);

    // ---- a dead handle is ready forever, so it must never be waited on ----
    {
        PAL::Watch watch { nullptr, PAL::Ready::Readable, PAL::Ready::None };
        CHECK(PAL::waitForReadiness({ &watch, 1 }, -1) == 1);
        CHECK(PAL::contains(watch.result, PAL::Ready::Invalid));

        PAL::UnixFileDescriptor closed;
        PAL::Watch closedWatch { &closed, PAL::Ready::Readable, PAL::Ready::None };
        CHECK(PAL::waitForReadiness({ &closedWatch, 1 }, -1) == 1);
        CHECK(PAL::contains(closedWatch.result, PAL::Ready::Invalid));
    }

    // ---- a live handle reports readable only once bytes are pending ----
    {
        PAL::UnixPipe pipe = PAL::makeUnixPipe();
        CHECK(pipe.isValid());
        if (pipe.isValid()) {
            PAL::Watch watch { &pipe.read, PAL::Ready::Readable, PAL::Ready::None };
            CHECK(PAL::waitForReadiness({ &watch, 1 }, 0) == 0);
            CHECK(watch.result == PAL::Ready::None);

            const char one = 'x';
            CHECK(::write(pipe.write.get(), &one, 1) == 1);
            watch.result = PAL::Ready::None;
            CHECK(PAL::waitForReadiness({ &watch, 1 }, 1000) == 1);
            CHECK(PAL::contains(watch.result, PAL::Ready::Readable));

            // An empty pipe with room is writable, and asking for readable on it
            // must not report the write end's readiness by mistake.
            PAL::Watch writable { &pipe.write, PAL::Ready::Writable, PAL::Ready::None };
            CHECK(PAL::waitForReadiness({ &writable, 1 }, 0) == 1);
            CHECK(PAL::contains(writable.result, PAL::Ready::Writable));

            // Two watches at once: each entry carries its own result.
            PAL::Watch pair[2] = {
                { &pipe.read, PAL::Ready::Readable, PAL::Ready::None },
                { &pipe.write, PAL::Ready::Writable, PAL::Ready::None },
            };
            CHECK(PAL::waitForReadiness({ pair, 2 }, 1000) == 2);
            CHECK(PAL::contains(pair[0].result, PAL::Ready::Readable));
            CHECK(PAL::contains(pair[1].result, PAL::Ready::Writable));
        }
    }

    // ---- the waker turns a submission on another thread into a wake ----
    {
        auto waker = PAL::makeWaker();
        CHECK(waker != nullptr);
        if (waker) {
            PAL::Watch watch { &waker->handle(), PAL::Ready::Readable, PAL::Ready::None };
            CHECK(PAL::waitForReadiness({ &watch, 1 }, 0) == 0);

            waker->wake();
            watch.result = PAL::Ready::None;
            CHECK(PAL::waitForReadiness({ &watch, 1 }, 1000) == 1);
            CHECK(PAL::contains(watch.result, PAL::Ready::Readable));

            // Repeated wakes coalesce: one drain leaves the handle quiet, or the
            // loop spins servicing a wake it already handled.
            waker->wake();
            waker->wake();
            waker->drain();
            watch.result = PAL::Ready::None;
            CHECK(PAL::waitForReadiness({ &watch, 1 }, 0) == 0);
            CHECK(watch.result == PAL::Ready::None);

            // And it re-arms: a drained waker still wakes the next wait.
            waker->wake();
            watch.result = PAL::Ready::None;
            CHECK(PAL::waitForReadiness({ &watch, 1 }, 1000) == 1);
            CHECK(PAL::contains(watch.result, PAL::Ready::Readable));
        }
    }
}

UBIQUITTY_TEST_MAIN()
