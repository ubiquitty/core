// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// A QUIC connection survives a clock that steps backwards.
//
// ngtcp2 asserts that each timestamp a connection receives is at least the last
// (`conn->log.last_ts <= ts`) and aborts the process when it is not. The daemon
// holds every session in one process, so one connection given a timestamp a
// microsecond early ends all of them. Every call into a connection — reading a
// packet, writing one, firing a timer — takes its timestamp from the
// connection's own clamp, which never answers lower than before.
//
// This runs the production client against the production listener over
// loopback with both ends reading a clock that jumps back by up to twenty
// milliseconds at random, through a handshake, a transfer in each direction
// and the timers of an idle connection. It passes by finishing: without the
// clamp the first backward step aborts the process.

#include "Harness.h"

#include "Core/Transport/QUICClient.h"
#include "Platform/Host/HostQUIC.h"
#include "Platform/POSIX/QUICTimestamp.h"

#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

namespace {

using Ubiquitty::Host::QUICConnection;
using Ubiquitty::Host::QUICListener;
using Ubiquitty::Transport::QUICClient;

// A clock that reads the QUIC clock less a random lag of up to 20 ms, so
// consecutive readings go backwards about half the time.
std::function<std::uint64_t()> jitteringClock(std::uint64_t seed)
{
    return [state = seed | 1]() mutable {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        const std::uint64_t lag = state % 20'000'000u;
        return Ubiquitty::POSIX::quicClockNanoseconds() + 1'000'000'000u - lag;
    };
}

std::uint64_t nowMilliseconds() { return Ubiquitty::POSIX::quicClockNanoseconds() / 1'000'000u; }

} // namespace

TEST(aConnectionSurvivesABackwardStepAtEveryCallSite)
{
    char directory[] = "/tmp/ubq-quic-clock.XXXXXX";
    CHECK_MSG(::mkdtemp(directory) != nullptr, "no scratch directory");
    const std::string home = std::string(directory) + "/.ubiquitty";
    ::mkdir(home.c_str(), 0700);
    ::setenv("UBIQUITTY_HOME", home.c_str(), 1);

    QUICListener listener;
    if (!listener.start(0)) {
        std::printf("quic_timestamp_clamp: listener would not start (%s) — skipping\n", listener.lastError().c_str());
        return;
    }
    Ubiquitty::Host::setTestQUICClockSource(listener, jitteringClock(0x1234));

    constexpr std::uint64_t downBytes = 4u << 20;
    constexpr std::uint64_t upBytes = 2u << 20;
    std::atomic<QUICConnection*> peer { nullptr };
    std::atomic<std::uint64_t> hostReceived { 0 };
    listener.setStreamDataCallback([&](QUICConnection* connection, std::span<const std::uint8_t> bytes) {
        if (!peer.load()) {
            listener.markAuthenticated(connection);
            peer.store(connection);
        }
        hostReceived.fetch_add(bytes.size());
    });
    listener.setConnectionClosedCallback([](QUICConnection*) { });

    std::atomic<bool> stopPump { false };
    std::thread pump([&] {
        std::vector<std::uint8_t> chunk(64 * 1024, 0x42);
        std::uint64_t queued = 0;
        while (!stopPump.load()) {
            pollfd descriptor = { listener.fileDescriptor(), POLLIN, 0 };
            int timeout = listener.nextTimeoutMilliseconds();
            if (timeout < 0 || timeout > 2)
                timeout = 2;
            ::poll(&descriptor, 1, timeout);
            if (descriptor.revents & POLLIN)
                listener.processReadable();
            listener.serviceTimers();
            if (QUICConnection* connection = peer.load()) {
                while (queued < downBytes && listener.pendingBytes(connection) < (1u << 20)) {
                    listener.sendStream(connection, chunk);
                    queued += chunk.size();
                }
            }
            listener.flush();
        }
    });

    std::atomic<bool> connected { false };
    std::atomic<bool> closed { false };
    std::atomic<std::uint64_t> clientReceived { 0 };
    QUICClient client;
    Ubiquitty::Transport::QUICClockAccess::setSource(client, jitteringClock(0x5678));
    QUICClient::Callbacks callbacks;
    callbacks.onConnect = [&] { connected.store(true); };
    callbacks.onData = [&](std::span<const std::uint8_t> bytes) { clientReceived.fetch_add(bytes.size()); };
    callbacks.onClose = [&](Ubiquitty::Transport::Health, const std::string&) { closed.store(true); };
    CHECK_MSG(client.start("127.0.0.1", listener.boundPort(), Ubiquitty::Transport::QUICCredential {}, "", callbacks).has_value(), "the client refused to start");

    const std::uint64_t deadline = nowMilliseconds() + 60'000;
    while (!connected.load() && !closed.load() && nowMilliseconds() < deadline)
        ::usleep(5000);
    CHECK_MSG(connected.load(), "the client never connected");
    if (connected.load()) {
        std::vector<std::uint8_t> chunk(64 * 1024, 0x24);
        for (std::uint64_t sent = 0; sent < upBytes; sent += chunk.size()) {
            client.send(chunk);
            ::usleep(1000);
        }
        while ((hostReceived.load() < upBytes || clientReceived.load() < downBytes) && !closed.load() && nowMilliseconds() < deadline)
            ::usleep(5000);
        // An idle stretch, so the loss, ACK-delay and keepalive timers fire on both
        // ends with the clock still jumping.
        ::usleep(1'500'000);
    }
    CHECK_MSG(hostReceived.load() >= upBytes, "the upward transfer did not finish");
    CHECK_MSG(clientReceived.load() >= downBytes, "the downward transfer did not finish");
    CHECK_MSG(!closed.load(), "the connection closed");

    client.stop();
    stopPump.store(true);
    pump.join();
    listener.stop();
    std::printf("quic_timestamp_clamp: %llu bytes down, %llu up, through a clock stepping back up to 20 ms\n",
        static_cast<unsigned long long>(clientReceived.load()), static_cast<unsigned long long>(hostReceived.load()));
}

UBIQUITTY_TEST_MAIN()
