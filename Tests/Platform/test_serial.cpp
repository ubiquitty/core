// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"

// Tests/Platform/test_serial.cpp
//
// Host-side coverage of the portable serial / RS-232 Stream backend
// (Source/Platform/POSIX/SerialStream.cpp). A real /dev/cu.* device isn't present in
// CI, so we stand in a forkpty'd PTY pair: the SLAVE tty path (/dev/ttysNNN)
// behaves like a serial line for termios purposes, so openSerial() can open it,
// put it in raw 8N1, and round-trip bytes against the MASTER. Darwin-only
// (forkpty / openpty live in <util.h>); gated in Scripts/run-tests.sh.
//
// Cases:
//   - openSerial rejects an empty path and a nonexistent device
//   - openSerial on a real tty configures raw mode + returns a live Stream
//   - write to the serial fileDescriptor is visible on the peer (master) end
//   - read from the serial fileDescriptor returns bytes the peer wrote
//   - process() reports an empty session (no child / shell), ttyName set

#include "PAL/Stream.h"

#include <cstdio>
#include <cstring>

#include <fcntl.h>
#include <memory>
#include <string>
#include <termios.h>
#include <unistd.h>
// openpty's home differs per platform, exactly as Platform/POSIX/PTYStream.cpp
// spells it — the test and the code it exercises must agree on that.
#if defined(__linux__)
#include <pty.h>
#elif defined(__FreeBSD__) || defined(__DragonFly__)
#include <libutil.h>
#else
#include <util.h>
#endif

using namespace Ubiquitty;

static int status(PAL::StreamStatus s) { return static_cast<int>(s); }

// Drain up to cap bytes from a non-blocking fileDescriptor within a short budget.
static int drainFileDescriptor(int fileDescriptor, char* buf, int cap, int budgetMs)
{
    int got = 0;
    int ticks = 0;
    const int sleepUs = 2000;
    const int maxTicks = budgetMs * 1000 / sleepUs;
    while (got < cap && ticks < maxTicks) {
        ssize_t n = ::read(fileDescriptor, buf + got, static_cast<size_t>(cap - got));
        if (n > 0) {
            got += static_cast<int>(n);
            continue;
        }
        ++ticks;
        ::usleep(sleepUs);
    }
    return got;
}

TEST(serialCases)
{

    // ---- rejects bad paths ----
    CHECK(PAL::openSerial("") == nullptr);
    CHECK(PAL::openSerial("/dev/this-device-does-not-exist-xyz") == nullptr);

    // ---- stand in a PTY pair for a serial line ----
    int master = -1;
    int slave = -1;
    char slaveName[128] = { 0 };
    if (::openpty(&master, &slave, slaveName, nullptr, nullptr) != 0) {
        std::fprintf(stderr, "openpty failed; cannot run serial round-trip\n");
        return;
    }
    // openSerial reopens the slave by PATH, so close our slave fileDescriptor to avoid two
    // openers fighting; the slave tty persists as long as the master is open.
    ::close(slave);
    // Make the master non-blocking so our drain doesn't park.
    int mf = ::fcntl(master, F_GETFL, 0);
    ::fcntl(master, F_SETFL, mf | O_NONBLOCK);

    std::unique_ptr<PAL::Stream> serial = PAL::openSerial(slaveName, 115200);
    CHECK(serial != nullptr);
    if (!serial) {
        ::close(master);
        return;
    }
    CHECK(serial->fileDescriptor() >= 0);

    // ---- process() facet: empty session ----
    PAL::ProcessFacts& facts = serial->process();
    CHECK(facts.foregroundName().empty());
    CHECK(facts.foregroundIsShell()); // a serial line is "at prompt" (no fg job)
    CHECK(facts.hasRunningChild() == false);
    CHECK(!facts.foregroundProcessGroup().has_value());
    // ttyName is the device basename.
    std::string tty = facts.ttyName();
    CHECK(!tty.empty());

    // ---- write to the serial fileDescriptor → visible on the master ----
    {
        const char payload[] = "AT\r";
        int rc = serial->write({ payload, sizeof(payload) - 1 });
        CHECK(rc == static_cast<int>(sizeof(payload) - 1) || rc >= 0);
        char buf[64] = { 0 };
        int got = drainFileDescriptor(master, buf, sizeof(buf) - 1, 500);
        CHECK(got >= 3);
        CHECK(std::strncmp(buf, "AT\r", 3) == 0);
    }

    // ---- master writes → serial fileDescriptor reads ----
    {
        const char reply[] = "OK\r\n";
        ssize_t w = ::write(master, reply, sizeof(reply) - 1);
        CHECK(w == static_cast<ssize_t>(sizeof(reply) - 1));
        char buf[64] = { 0 };
        int got = 0;
        int ticks = 0;
        while (got < 4 && ticks < 250) {
            int n = serial->read({ buf + got, sizeof(buf) - 1 - static_cast<size_t>(got) });
            if (n > 0) {
                got += n;
                continue;
            }
            CHECK(n == status(PAL::StreamStatus::Again) || n > 0);
            ++ticks;
            ::usleep(2000);
        }
        CHECK(got >= 4);
        CHECK(std::strncmp(buf, "OK\r\n", 4) == 0);
    }

    // ---- closing the Stream releases the fileDescriptor (RAII) ----
    serial.reset();
    ::close(master);
}

UBIQUITTY_TEST_MAIN()
