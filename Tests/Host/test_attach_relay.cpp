// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Harness.h"
#include "POSIXChildProcess.h"
#include "StreamPartitions.h"

#include "PAL/UnixFileDescriptor.h"
#include "Platform/Host/AttachRelay.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <fcntl.h>
#include <poll.h>
#include <span>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using Bytes = std::vector<std::uint8_t>;

bool awaitReady(int fileDescriptor, short events, Clock::time_point deadline)
{
    for (;;) {
        const auto remaining = deadline - Clock::now();
        if (remaining <= Clock::duration::zero())
            return false;
        pollfd descriptor { fileDescriptor, events, 0 };
        const auto milliseconds = std::chrono::ceil<std::chrono::milliseconds>(remaining).count();
        const int result = ::poll(&descriptor, 1, static_cast<int>(milliseconds));
        if (result >= 0)
            return result > 0;
        if (errno != EINTR)
            return false;
    }
}

bool writeAll(int fileDescriptor, std::span<const std::uint8_t> bytes)
{
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    while (!bytes.empty()) {
        const ssize_t count = ::write(fileDescriptor, bytes.data(), bytes.size());
        if (count > 0)
            bytes = bytes.subspan(static_cast<std::size_t>(count));
        else if (count < 0 && errno == EINTR)
            continue;
        else if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (!awaitReady(fileDescriptor, POLLOUT, deadline))
                return false;
        } else
            return false;
    }
    return true;
}

bool readExactly(int fileDescriptor, std::span<std::uint8_t> bytes)
{
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    while (!bytes.empty()) {
        const ssize_t count = ::read(fileDescriptor, bytes.data(), bytes.size());
        if (count > 0)
            bytes = bytes.subspan(static_cast<std::size_t>(count));
        else if (count < 0 && errno == EINTR)
            continue;
        else if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (!awaitReady(fileDescriptor, POLLIN, deadline))
                return false;
        } else
            return false;
    }
    return true;
}

bool collectToEOF(int fileDescriptor, Bytes& bytes)
{
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    std::array<std::uint8_t, 4096> buffer;
    for (;;) {
        const ssize_t count = ::read(fileDescriptor, buffer.data(), buffer.size());
        if (count > 0)
            bytes.insert(bytes.end(), buffer.begin(), buffer.begin() + count);
        else if (count == 0)
            return true;
        else if (errno == EINTR)
            continue;
        else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (!awaitReady(fileDescriptor, POLLIN, deadline))
                return false;
        } else
            return false;
    }
}

struct SocketPair {
    Ubiquitty::PAL::UnixFileDescriptor relay;
    Ubiquitty::PAL::UnixFileDescriptor peer;

    bool open()
    {
        std::array<int, 2> descriptors;
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors.data()) != 0)
            return false;
        relay.reset(descriptors[0]);
        peer.reset(descriptors[1]);
        for (const int descriptor : descriptors) {
            const int flags = ::fcntl(descriptor, F_GETFL);
            if (flags < 0 || ::fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) != 0)
                return false;
        }
        return true;
    }
};

struct RelayFixture {
    SocketPair daemon;
    SocketPair input;
    SocketPair output;
    Ubiquitty::PAL::UnixPipe completion;
    UbiquittyTest::OwnedChildProcess<> child;
    std::size_t prefillBytes = 0;

    bool open()
    {
        (void)::signal(SIGPIPE, SIG_IGN);
        completion = Ubiquitty::PAL::makeUnixPipe();
        if (!completion || !child.canOwnChildren() || !daemon.open() || !input.open() || !output.open())
            return false;
        const int capacity = 4096;
        const int daemonCapacity = 65536;
        return ::setsockopt(output.relay.get(), SOL_SOCKET, SO_SNDBUF, &capacity, sizeof(capacity)) == 0
            && ::setsockopt(daemon.peer.get(), SOL_SOCKET, SO_SNDBUF, &daemonCapacity, sizeof(daemonCapacity)) == 0;
    }

    // Fill stdout before the child exists. No timer or scheduler assumption
    // decides whether the relay encounters backpressure.
    bool fillOutput()
    {
        std::array<std::uint8_t, 4096> padding;
        padding.fill(0xFF);
        for (;;) {
            const ssize_t count = ::write(output.relay.get(), padding.data(), padding.size());
            if (count > 0)
                prefillBytes += static_cast<std::size_t>(count);
            else if (count < 0 && errno == EINTR)
                continue;
            else
                return count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && prefillBytes > 0;
        }
    }

    bool start(std::chrono::milliseconds budget = Ubiquitty::Host::attachWriteFailureDrainBudget)
    {
        const pid_t process = ::fork();
        if (process < 0)
            return false;
        if (process == 0) {
            daemon.peer.reset();
            input.peer.reset();
            output.peer.reset();
            completion.read.reset();
            const int result = Ubiquitty::Host::relayAttach(daemon.relay.get(), input.relay.get(), output.relay.get(), budget);
            const std::uint8_t report = static_cast<std::uint8_t>(result);
            // A report that never arrives fails `finish` by itself; the exit
            // status says so too, rather than reading as a clean relay.
            if (!writeAll(completion.write.get(), { &report, sizeof(report) }))
                ::_exit(1);
            ::_exit(result);
        }
        if (!child.adoptForkResult(process))
            return false;
        daemon.relay.reset();
        input.relay.reset();
        output.relay.reset();
        completion.write.reset();
        return true;
    }

    bool finish()
    {
        if (!awaitReady(completion.read.get(), POLLIN, Clock::now() + std::chrono::seconds(5)))
            return false;
        std::uint8_t report = 0xFF;
        if (::read(completion.read.get(), &report, sizeof(report)) != static_cast<ssize_t>(sizeof(report)))
            return false;
        int status = 0;
        return child.reap(0, &status) > 0 && report == 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    }

    bool collect(Bytes& bytes)
    {
        if (!collectToEOF(output.peer.get(), bytes) || bytes.size() < prefillBytes)
            return false;
        const bool hasPadding = std::all_of(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(prefillBytes),
            [](std::uint8_t byte) { return byte == 0xFF; });
        bytes.erase(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(prefillBytes));
        return hasPadding;
    }
};

Bytes replyBytes(std::size_t count)
{
    Bytes bytes(count);
    for (std::size_t index = 0; index < count; ++index)
        bytes[index] = static_cast<std::uint8_t>(index % 251);
    return bytes;
}

} // namespace

TEST(daemonWriteFailurePreservesItsReplyThroughPartialOutput)
{
    RelayFixture fixture;
    REQUIRE(fixture.open());
    REQUIRE(fixture.fillOutput());
    const Bytes expected = replyBytes(32 * 1024);
    REQUIRE(writeAll(fixture.daemon.peer.get(), expected));
    REQUIRE(::shutdown(fixture.daemon.peer.get(), SHUT_WR) == 0);
    // Locally shutting down the relay's write half guarantees EPIPE on both
    // Darwin and Linux; the first write after a peer close need not fail.
    REQUIRE(::shutdown(fixture.daemon.relay.get(), SHUT_WR) == 0);
    const Bytes input(49, 0x41);
    REQUIRE(writeAll(fixture.input.peer.get(), input));
    REQUIRE(fixture.start());
    Bytes actual;
    REQUIRE(fixture.collect(actual));
    CHECK(actual == expected);
    CHECK(fixture.finish());
    std::uint8_t byte = 0;
    CHECK_EQ(::read(fixture.daemon.peer.get(), &byte, 1), 0);
}

TEST(daemonHangupDoesNotTruncateAtThePendingBufferCapacity)
{
    RelayFixture fixture;
    REQUIRE(fixture.open());
    REQUIRE(fixture.fillOutput());
    REQUIRE(fixture.start());
    const Bytes prefix = replyBytes(1u << 20);
    REQUIRE(writeAll(fixture.daemon.peer.get(), prefix));
    // The input barrier is forwarded after the same iteration's daemon
    // read. With stdout still full, receiving it proves the entire prefix
    // was read into the relay's pending buffer, which is now at its cap.
    constexpr std::array<std::uint8_t, 4> barrier { 'c', 'a', 'p', '!' };
    REQUIRE(writeAll(fixture.input.peer.get(), barrier));
    std::array<std::uint8_t, barrier.size()> echoed;
    REQUIRE(readExactly(fixture.daemon.peer.get(), echoed));
    REQUIRE(echoed == barrier);
    const Bytes suffix = replyBytes(4096);
    REQUIRE(writeAll(fixture.daemon.peer.get(), suffix));
    REQUIRE(::shutdown(fixture.daemon.peer.get(), SHUT_WR) == 0);
    fixture.daemon.peer.reset();
    // Only now release stdout. Its first writable wake also observes the
    // daemon's close while the in-memory buffer is full.
    Bytes actual;
    REQUIRE(fixture.collect(actual));
    Bytes expected = prefix;
    expected.insert(expected.end(), suffix.begin(), suffix.end());
    CHECK(actual == expected);
    CHECK(fixture.finish());
}

TEST(failedWriteDrainHasAnAbsoluteBoundWhenOutputStalls)
{
    RelayFixture fixture;
    REQUIRE(fixture.open());
    REQUIRE(fixture.fillOutput());
    REQUIRE(writeAll(fixture.daemon.peer.get(), replyBytes(1024)));
    REQUIRE(::shutdown(fixture.daemon.relay.get(), SHUT_WR) == 0);
    constexpr std::array<std::uint8_t, 1> input { 0x41 };
    REQUIRE(writeAll(fixture.input.peer.get(), input));
    constexpr std::chrono::milliseconds budget { 100 };
    const auto started = Clock::now();
    REQUIRE(fixture.start(budget));
    // No stdout reader and no daemon EOF: only the failure deadline can
    // complete this relay. The completion pipe is the readiness barrier.
    CHECK(fixture.finish());
    CHECK(Clock::now() - started >= budget);
    Bytes actual;
    REQUIRE(fixture.collect(actual));
    CHECK(actual.empty());
}

TEST(inputEOFFlushesItsInputAndDetachesFromALiveDaemon)
{
    RelayFixture fixture;
    REQUIRE(fixture.open());
    const Bytes expected = replyBytes(128);
    REQUIRE(writeAll(fixture.input.peer.get(), expected));
    REQUIRE(::shutdown(fixture.input.peer.get(), SHUT_WR) == 0);
    REQUIRE(fixture.start());
    Bytes actual(expected.size());
    REQUIRE(readExactly(fixture.daemon.peer.get(), actual));
    CHECK(actual == expected);
    CHECK(fixture.finish());
}

TEST(relayPreservesBothDirectionsAcrossEverySplitAndSeededPartitions)
{
    RelayFixture fixture;
    REQUIRE(fixture.open());
    REQUIRE(fixture.start());
    const Bytes expected = replyBytes(129);
    CHECK(UbiquittyTest::sweepStreamPartitions(expected.size(), UbiquittyTest::PartitionCoverage::Exhaustive, [&](auto ends, const std::string& label) {
        Bytes fromInput;
        Bytes fromDaemon;
        bool isComplete = true;
        UbiquittyTest::feedStreamPartition(std::span<const std::uint8_t>(expected), ends, [&](auto piece) {
            if (!isComplete || piece.empty())
                return;
            Bytes received(piece.size());
            // Reading back before sending the next piece prevents the socket
            // from coalescing away the boundary this partition exercises.
            isComplete = writeAll(fixture.input.peer.get(), piece) && readExactly(fixture.daemon.peer.get(), received);
            fromInput.insert(fromInput.end(), received.begin(), received.end());
            isComplete = isComplete && writeAll(fixture.daemon.peer.get(), piece) && readExactly(fixture.output.peer.get(), received);
            fromDaemon.insert(fromDaemon.end(), received.begin(), received.end());
        });
        const bool matches = isComplete && fromInput == expected && fromDaemon == expected;
        CHECK_MSG(matches, label.c_str());
        return matches;
    }));
    REQUIRE(::shutdown(fixture.input.peer.get(), SHUT_WR) == 0);
    CHECK(fixture.finish());
}

UBIQUITTY_TEST_MAIN()
