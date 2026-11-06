// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// A QUIC stream arrives byte for byte through a link that loses and delays
// packets.
//
// ngtcp2 does not copy stream data: it keeps pointers to the bytes it took and
// rebuilds a lost packet from them, so the sender must hold every byte at the
// same address until it is acknowledged. A send buffer that reallocates on
// append, or shifts its acknowledged prefix down, hands the retransmission
// whichever bytes occupy those addresses. Nothing fails: the receiver takes the
// frame, its offsets are right, and its payload is wrong. Only loss exercises
// the path, so loopback alone can never catch it.
//
// This drives the production QUICClient against the production QUICListener
// through an in-process UDP relay that drops a seeded fraction of datagrams
// each way and delays the rest, and checks every delivered byte against a
// pattern derived from its stream offset, in both directions.

#include "Harness.h"

#include "Core/Transport/QUICClient.h"
#include "Platform/Host/HostQUIC.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using Ubiquitty::Host::QUICConnection;
using Ubiquitty::Host::QUICListener;
using Ubiquitty::Transport::QUICClient;

std::uint64_t nowMicroseconds()
{
    struct timespec ts;
    ::clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000u + static_cast<std::uint64_t>(ts.tv_nsec) / 1000u;
}

// Not periodic at any power of two, so bytes moved by a whole block or a whole
// pattern period still read as wrong.
std::uint8_t patternByte(std::uint64_t offset)
{
    std::uint64_t mixed = offset * 0x9E3779B97F4A7C15ull;
    mixed ^= mixed >> 29;
    return static_cast<std::uint8_t>(mixed >> 23);
}

void fillPattern(std::span<std::uint8_t> out, std::uint64_t offset)
{
    for (std::size_t index = 0; index < out.size(); ++index)
        out[index] = patternByte(offset + index);
}

// Checks a run of delivered bytes against the pattern and counts mismatches.
struct PatternCheck {
    std::uint64_t received = 0;
    std::uint64_t corrupt = 0;
    std::uint64_t firstCorruptOffset = ~std::uint64_t { 0 };

    void take(std::span<const std::uint8_t> bytes)
    {
        for (std::size_t index = 0; index < bytes.size(); ++index) {
            if (bytes[index] != patternByte(received + index)) {
                if (corrupt++ == 0)
                    firstCorruptOffset = received + index;
            }
        }
        received += bytes.size();
    }
};

// A UDP relay between the client and the listener: drops a seeded fraction of
// datagrams in each direction and holds the rest for a fixed delay. One socket
// faces the client and one the listener, so the direction is the socket.
class LossyRelay {
public:
    LossyRelay(std::uint16_t listenerPort, double lossFraction, int delayMilliseconds, std::uint64_t seed)
        : m_lossFraction(lossFraction)
        , m_delayMicroseconds(static_cast<std::uint64_t>(delayMilliseconds) * 1000u)
        , m_random(seed | 1)
    {
        m_clientFacing = openLoopbackSocket();
        m_listenerFacing = openLoopbackSocket();
        m_listener.sin_family = AF_INET;
        m_listener.sin_port = htons(listenerPort);
        m_listener.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        m_thread = std::thread([this] { run(); });
    }

    ~LossyRelay()
    {
        m_stop.store(true);
        m_thread.join();
        ::close(m_clientFacing);
        ::close(m_listenerFacing);
    }

    std::uint16_t port() const
    {
        sockaddr_in address {};
        socklen_t length = sizeof(address);
        ::getsockname(m_clientFacing, reinterpret_cast<sockaddr*>(&address), &length);
        return ntohs(address.sin_port);
    }

    std::uint64_t dropped() const { return m_dropped.load(); }

private:
    enum class Hop : std::uint8_t {
        TowardListener,
        TowardClient
    };

    struct Held {
        std::uint64_t dueAt;
        Hop hop;
        std::vector<std::uint8_t> bytes;
    };

    static int openLoopbackSocket()
    {
        const int descriptor = ::socket(AF_INET, SOCK_DGRAM, 0);
        sockaddr_in address {};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ::bind(descriptor, reinterpret_cast<sockaddr*>(&address), sizeof(address));
        const int bufferBytes = 8 * 1024 * 1024;
        ::setsockopt(descriptor, SOL_SOCKET, SO_RCVBUF, &bufferBytes, sizeof(bufferBytes));
        ::setsockopt(descriptor, SOL_SOCKET, SO_SNDBUF, &bufferBytes, sizeof(bufferBytes));
        return descriptor;
    }

    bool shouldDrop()
    {
        m_random ^= m_random << 13;
        m_random ^= m_random >> 7;
        m_random ^= m_random << 17;
        return static_cast<double>(m_random % 1000000u) < m_lossFraction * 1000000.0;
    }

    void receiveFrom(int descriptor, Hop hop)
    {
        std::uint8_t buffer[2048];
        for (;;) {
            sockaddr_storage from {};
            socklen_t fromLength = sizeof(from);
            const ssize_t length = ::recvfrom(descriptor, buffer, sizeof(buffer), MSG_DONTWAIT, reinterpret_cast<sockaddr*>(&from), &fromLength);
            if (length <= 0)
                return;
            if (hop == Hop::TowardListener) {
                std::memcpy(&m_client, &from, sizeof(sockaddr_in));
                m_hasClient = true;
            }
            if (shouldDrop()) {
                m_dropped.fetch_add(1);
                continue;
            }
            m_held.push_back({ nowMicroseconds() + m_delayMicroseconds, hop, { buffer, buffer + length } });
        }
    }

    void run()
    {
        while (!m_stop.load()) {
            int timeout = 1;
            if (!m_held.empty()) {
                const std::uint64_t now = nowMicroseconds();
                timeout = m_held.front().dueAt <= now ? 0 : static_cast<int>(std::min<std::uint64_t>((m_held.front().dueAt - now + 999) / 1000, 1));
            }
            pollfd descriptors[2] = { { m_clientFacing, POLLIN, 0 }, { m_listenerFacing, POLLIN, 0 } };
            ::poll(descriptors, 2, timeout);
            receiveFrom(m_clientFacing, Hop::TowardListener);
            receiveFrom(m_listenerFacing, Hop::TowardClient);
            const std::uint64_t now = nowMicroseconds();
            while (!m_held.empty() && m_held.front().dueAt <= now) {
                const Held& held = m_held.front();
                if (held.hop == Hop::TowardListener)
                    ::sendto(m_listenerFacing, held.bytes.data(), held.bytes.size(), 0, reinterpret_cast<const sockaddr*>(&m_listener), sizeof(m_listener));
                else if (m_hasClient)
                    ::sendto(m_clientFacing, held.bytes.data(), held.bytes.size(), 0, reinterpret_cast<const sockaddr*>(&m_client), sizeof(m_client));
                m_held.pop_front();
            }
        }
    }

    double m_lossFraction;
    std::uint64_t m_delayMicroseconds;
    std::uint64_t m_random;
    int m_clientFacing = -1;
    int m_listenerFacing = -1;
    sockaddr_in m_listener {};
    sockaddr_in m_client {};
    bool m_hasClient = false;
    std::deque<Held> m_held;
    std::atomic<bool> m_stop { false };
    std::atomic<std::uint64_t> m_dropped { 0 };
    std::thread m_thread;
};

enum class Direction : std::uint8_t {
    // Listener to client, behind a bounded backlog, as the daemon's output goes.
    Down,
    // Client to listener, fed in small pieces over time, as typing goes.
    Up,
    // Client to listener, queued in one go, as a large paste or a file push goes.
    UpInOneBurst,
};

struct TransferResult {
    bool didListenerStart = false;
    bool connected = false;
    bool didComplete = false;
    std::string closeReason;
    std::uint64_t received = 0;
    std::uint64_t corrupt = 0;
    std::uint64_t firstCorruptOffset = 0;
    std::uint64_t dropped = 0;
};

// The listener's home holds its certificate; a private one keeps the test off
// whatever the user's own daemon keeps there.
void usePrivateHome()
{
    static std::once_flag once;
    std::call_once(once, [] {
        char directory[] = "/tmp/ubq-quic-lossy.XXXXXX";
        if (::mkdtemp(directory)) {
            const std::string home = std::string(directory) + "/.ubiquitty";
            ::mkdir(home.c_str(), 0700);
            ::setenv("UBIQUITTY_HOME", home.c_str(), 1);
        }
    });
}

// Moves `bytes` across the relay in one direction and checks each one.
//
// Downward, the listener queues the stream behind a one-mebibyte backlog, as
// the daemon bounds a client's output. Upward, the client is fed in pieces.
TransferResult transfer(Direction direction, std::uint64_t bytes, double lossFraction, int delayMilliseconds, std::uint64_t seed)
{
    usePrivateHome();
    TransferResult result;
    QUICListener listener;
    if (!listener.start(0))
        return result;
    result.didListenerStart = true;

    PatternCheck hostCheck;
    PatternCheck clientCheck;
    std::atomic<QUICConnection*> peer { nullptr };
    std::atomic<std::uint64_t> hostReceived { 0 };
    std::atomic<std::uint64_t> clientReceived { 0 };
    listener.setStreamDataCallback([&](QUICConnection* connection, std::span<const std::uint8_t> data) {
        if (!peer.load()) {
            // The first byte is the start signal, not payload; the test carries no
            // token, so it stands in for the preamble the reaper waits on.
            listener.markAuthenticated(connection);
            peer.store(connection);
            data = data.subspan(1);
        }
        hostCheck.take(data);
        hostReceived.store(hostCheck.received);
    });
    listener.setConnectionClosedCallback([&](QUICConnection*) { });

    LossyRelay relay(listener.boundPort(), lossFraction, delayMilliseconds, seed);

    // The listener's single owner, as the daemon's poll loop is.
    std::atomic<bool> stopPump { false };
    std::thread pump([&] {
        constexpr std::size_t backlogBytes = 1u << 20;
        std::vector<std::uint8_t> chunk(64 * 1024);
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
            QUICConnection* connection = peer.load();
            if (direction == Direction::Down && connection) {
                while (queued < bytes && listener.pendingBytes(connection) < backlogBytes) {
                    const std::size_t take = static_cast<std::size_t>(std::min<std::uint64_t>(chunk.size(), bytes - queued));
                    fillPattern({ chunk.data(), take }, queued);
                    listener.sendStream(connection, { chunk.data(), take });
                    queued += take;
                }
            }
            listener.flush();
        }
    });

    std::atomic<bool> connected { false };
    std::atomic<bool> closed { false };
    std::mutex reasonMutex;
    QUICClient client;
    QUICClient::Callbacks callbacks;
    callbacks.onConnect = [&] { connected.store(true); };
    callbacks.onData = [&](std::span<const std::uint8_t> data) {
        clientCheck.take(data);
        clientReceived.store(clientCheck.received);
    };
    callbacks.onClose = [&](Ubiquitty::Transport::Health, const std::string& reason) {
        std::scoped_lock lock(reasonMutex);
        result.closeReason = reason;
        closed.store(true);
    };
    if (client.start("127.0.0.1", relay.port(), Ubiquitty::Transport::QUICCredential {}, "", callbacks).has_value()) {
        const std::uint64_t connectDeadline = nowMicroseconds() + 20'000'000;
        while (!connected.load() && !closed.load() && nowMicroseconds() < connectDeadline)
            ::usleep(5000);
    }
    result.connected = connected.load();

    if (result.connected) {
        const std::uint8_t start = 'S';
        client.send({ &start, 1 });
        if (direction != Direction::Down) {
            // Paced, the client appends to its buffer while earlier bytes are in
            // flight; in one burst, the whole transfer is queued before the first
            // acknowledgement and outruns the listener's stream window.
            std::vector<std::uint8_t> chunk(direction == Direction::Up ? 16 * 1024 : 256 * 1024);
            for (std::uint64_t queued = 0; queued < bytes && !closed.load();) {
                const std::size_t take = static_cast<std::size_t>(std::min<std::uint64_t>(chunk.size(), bytes - queued));
                fillPattern({ chunk.data(), take }, queued);
                client.send({ chunk.data(), take });
                queued += take;
                if (direction == Direction::Up)
                    ::usleep(500);
            }
        }
        // A stall is progress stopping, not the transfer being slow.
        const std::atomic<std::uint64_t>& progress = direction == Direction::Down ? clientReceived : hostReceived;
        std::uint64_t lastSeen = 0;
        std::uint64_t lastProgressAt = nowMicroseconds();
        while (progress.load() < bytes && !closed.load()) {
            ::usleep(10 * 1000);
            const std::uint64_t seen = progress.load();
            if (seen != lastSeen) {
                lastSeen = seen;
                lastProgressAt = nowMicroseconds();
            } else if (nowMicroseconds() - lastProgressAt > 15'000'000) {
                result.closeReason = "stalled";
                break;
            }
        }
        result.didComplete = progress.load() >= bytes;
    }

    client.stop();
    stopPump.store(true);
    pump.join();
    listener.stop();
    const PatternCheck& check = direction == Direction::Down ? clientCheck : hostCheck;
    result.received = check.received;
    result.corrupt = check.corrupt;
    result.firstCorruptOffset = check.firstCorruptOffset;
    result.dropped = relay.dropped();
    return result;
}

void expectExact(Direction direction, std::uint64_t bytes, double lossFraction, int delayMilliseconds)
{
    const TransferResult result = transfer(direction, bytes, lossFraction, delayMilliseconds, 0x5EED0000u + static_cast<std::uint64_t>(lossFraction * 1000));
    if (!result.didListenerStart) {
        std::printf("quic_lossy_transfer: listener would not start — skipping\n");
        return;
    }
    char report[400];
    std::snprintf(report, sizeof(report),
        "%s at %.0f%% loss, %d ms each way: %s, %llu of %llu bytes arrived, %llu wrong (first at %lld), %llu datagrams dropped%s%s",
        direction == Direction::Down ? "down" : direction == Direction::Up ? "up"
                                                                           : "up in one burst",
        lossFraction * 100, delayMilliseconds,
        result.didComplete ? "completed" : "incomplete",
        static_cast<unsigned long long>(result.received), static_cast<unsigned long long>(bytes),
        static_cast<unsigned long long>(result.corrupt),
        result.corrupt ? static_cast<long long>(result.firstCorruptOffset) : -1ll,
        static_cast<unsigned long long>(result.dropped),
        result.closeReason.empty() ? "" : "; closed: ", result.closeReason.c_str());
    std::printf("quic_lossy_transfer: %s\n", report);
    CHECK_MSG(result.connected, "the client never connected through the relay");
    CHECK_MSG(result.didComplete && result.received == bytes, report);
    CHECK_MSG(result.corrupt == 0, report);
}

constexpr std::uint64_t mebibyte = 1u << 20;

} // namespace

TEST(downwardStreamSurvivesOnePercentLoss) { expectExact(Direction::Down, 16 * mebibyte, 0.01, 5); }
TEST(downwardStreamSurvivesThreePercentLoss) { expectExact(Direction::Down, 16 * mebibyte, 0.03, 5); }
TEST(downwardStreamSurvivesTenPercentLoss) { expectExact(Direction::Down, 4 * mebibyte, 0.10, 5); }
TEST(upwardStreamSurvivesOnePercentLoss) { expectExact(Direction::Up, 8 * mebibyte, 0.01, 5); }
TEST(upwardStreamSurvivesThreePercentLoss) { expectExact(Direction::Up, 8 * mebibyte, 0.03, 5); }
TEST(upwardStreamSurvivesTenPercentLoss) { expectExact(Direction::Up, 3 * mebibyte, 0.10, 5); }
// Three times the listener's 4 MiB stream window, queued at once over a 50 ms
// round trip: the sender is blocked by flow control until MAX_STREAM_DATA
// arrives, which is a wait, not an error.
TEST(upwardBurstWaitsOutTheStreamWindow) { expectExact(Direction::UpInOneBurst, 12 * mebibyte, 0.0, 25); }
TEST(upwardBurstWaitsOutTheStreamWindowUnderLoss) { expectExact(Direction::UpInOneBurst, 12 * mebibyte, 0.01, 25); }

UBIQUITTY_TEST_MAIN()
