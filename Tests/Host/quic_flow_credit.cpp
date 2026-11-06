// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// A transfer larger than the initial flow-control window has to finish.
//
// QUIC grants a receiver-controlled window per stream and per connection, and
// the initial grant is all a sender ever holds unless the receiver extends it
// as it consumes. Both ends here consume synchronously inside the stream-data
// callback, so the credit must come back there; a receiver that never extends
// wedges every transfer at the first window edge — the sender's queue holds
// the rest forever and no error surfaces anywhere.
//
// This drives the production QUICClient against the production QUICListener
// over loopback, no daemon, and pushes half again the connection window from
// client to host. The assertion is that delivery leaves the initial stream
// grant far behind: a receiver that never extends sits at exactly 4,194,304
// bytes forever, and every fixed run sails past six mebibytes.

#include "Harness.h"

#include "Core/Transport/QUICClient.h"
#include "Platform/Host/HostQUIC.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <poll.h>
#include <string>
#include <thread>
#include <vector>

namespace {

// Half again the 8 MiB connection window, so the push outlives both grants.
constexpr std::size_t transferBytes = 12u * 1024u * 1024u;
// The verdict line: the whole push. A receiver that never extends sits at
// exactly 4,194,304 or 8,388,608, so delivering all of it needs the credit
// flowing back on both the stream and the connection.
constexpr std::size_t windowsCrossedBytes = transferBytes;

std::uint64_t nowMilliseconds()
{
    struct timespec ts;
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000u + static_cast<std::uint64_t>(ts.tv_nsec) / 1000000u;
}

} // namespace

TEST(aTransferCrossesTheInitialWindows)
{
    using Ubiquitty::Host::QUICConnection;
    using Ubiquitty::Host::QUICListener;
    using Ubiquitty::Transport::QUICClient;

    QUICListener listener;
    if (!listener.start(0)) {
        // No loopback UDP or no cert material: the environment cannot carry the
        // test, which is a skip rather than a failure.
        std::printf("quic_flow_credit: listener would not start (%s) — skipping\n", listener.lastError().c_str());
        return;
    }

    std::atomic<std::uint64_t> hostReceived { 0 };
    std::atomic<QUICConnection*> peer { nullptr };
    listener.setStreamDataCallback([&](QUICConnection* connection, std::span<const std::uint8_t> bytes) {
        // The daemon authenticates a connection off its token preamble and only
        // then lifts the pre-auth reaper's deadline. This test carries no
        // tokens, so the first stream byte stands in for the preamble — without
        // this the reaper kills the connection mid-transfer.
        if (!peer.load())
            listener.markAuthenticated(connection);
        peer.store(connection);
        hostReceived.fetch_add(bytes.size());
    });
    std::atomic<bool> peerClosed { false };
    listener.setConnectionClosedCallback([&](QUICConnection*) { peerClosed.store(true); });

    // The listener is single-owner — the daemon pumps it from one poll loop —
    // so this thread is that owner, and the host->client push runs here too
    // rather than racing the pump from the outside. The main thread only moves
    // the target and reads the counters.
    std::atomic<bool> stopPump { false };
    std::thread pump([&] {
        while (!stopPump.load()) {
            struct pollfd pfd = { listener.fileDescriptor(), POLLIN, 0 };
            int timeout = listener.nextTimeoutMilliseconds();
            // A flood at loopback rate overflows the socket buffer in the time a
            // lazy poll sleeps, and every drop is a retransmission the sender's
            // congestion window pays for.
            if (timeout < 0 || timeout > 2)
                timeout = 2;
            ::poll(&pfd, 1, timeout);
            if (pfd.revents & POLLIN)
                listener.processReadable();
            listener.serviceTimers();
            listener.flush();
        }
    });

    std::atomic<bool> connected { false };
    std::atomic<bool> closed { false };

    QUICClient client;
    QUICClient::Callbacks callbacks;
    callbacks.onConnect = [&] { connected.store(true); };
    callbacks.onData = [](std::span<const std::uint8_t>) { };
    callbacks.onClose = [&](Ubiquitty::Transport::Health, const std::string&) { closed.store(true); };

    // No pin: certificateSHA256() answers in hex while the pin gate compares
    // raw digest bytes, and a mismatched pin silently discards every delivered
    // byte. MITM defense is not what this test proves, and an empty pin is the
    // documented way to waive it.
    const auto started = client.start("127.0.0.1", listener.boundPort(), Ubiquitty::Transport::QUICCredential {}, "", callbacks);
    CHECK_MSG(started.has_value(), "the client refused to start");

    const std::uint64_t connectDeadline = nowMilliseconds() + 10000;
    while (!connected.load() && !closed.load() && nowMilliseconds() < connectDeadline)
        ::usleep(10 * 1000);
    CHECK_MSG(connected.load(), "the client never connected over loopback");

    if (connected.load()) {
        // --- client -> host: past both windows -----------------------------
        std::vector<std::uint8_t> chunk(256 * 1024);
        for (std::size_t i = 0; i < chunk.size(); ++i)
            chunk[i] = static_cast<std::uint8_t>(i * 131u + 17u);
        // A wedge is progress STOPPING, not the transfer being slow. A fixed
        // deadline cannot tell those apart, and on a loaded machine it reports
        // the second as the first — which it did, roughly one run in four.
        // Waiting on progress instead measures the thing the case is named for:
        // a receiver that never extends credit stops advancing and stays
        // stopped, at a window edge, however long it is given.
        constexpr std::uint64_t stallMilliseconds = 5000;
        std::size_t queued = 0;
        std::uint64_t lastQueued = 0;
        std::uint64_t lastQueueProgressAt = nowMilliseconds();
        while (queued < transferBytes && !closed.load()) {
            if (queued != lastQueued) {
                lastQueued = queued;
                lastQueueProgressAt = nowMilliseconds();
            } else if (nowMilliseconds() - lastQueueProgressAt > stallMilliseconds) {
                break;
            }
            // The client's own outbound cap is the reason to feed rather than
            // dump: what matters is that DELIVERY advances past the window.
            const std::size_t inFlight = queued - static_cast<std::size_t>(hostReceived.load());
            if (QUICClient::outboundWouldOverflow(inFlight, chunk.size())) {
                ::usleep(5 * 1000);
                continue;
            }
            client.send(chunk);
            queued += chunk.size();
        }
        std::uint64_t lastSeen = hostReceived.load();
        std::uint64_t lastDeliveryAt = nowMilliseconds();
        while (hostReceived.load() < windowsCrossedBytes && !closed.load()) {
            ::usleep(10 * 1000);
            const std::uint64_t seen = hostReceived.load();
            if (seen != lastSeen) {
                lastSeen = seen;
                lastDeliveryAt = nowMilliseconds();
                continue;
            }
            if (nowMilliseconds() - lastDeliveryAt > stallMilliseconds)
                break; // stopped, which is the wedge the assertion below names
        }
        char report[200];
        std::snprintf(report, sizeof(report),
            "client->host wedged: %llu bytes arrived; the stream window ends at 4194304 and the connection window at 8388608, and a receiver that never extends sits at one of those edges forever",
            static_cast<unsigned long long>(hostReceived.load()));
        CHECK_MSG(hostReceived.load() >= windowsCrossedBytes, report);
        CHECK_MSG(peer.load() != nullptr, "the listener never saw the client's stream");
    }

    client.stop();
    stopPump.store(true);
    pump.join();
    listener.stop();

    std::printf("quic_flow_credit: %llu bytes delivered\n",
        static_cast<unsigned long long>(hostReceived.load()));
}

UBIQUITTY_TEST_MAIN()
