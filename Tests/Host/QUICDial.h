// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
//
// Tests/Host/QUICDial.h — one dial of a spawned host over real loopback QUIC:
// the production QUIC client carrying a Sync client, for the stanzas that
// drive both against a daemon (quic_resume_proof, quic_client_migration,
// quic_workspace_isolation).
//

#include "Core/Protocol/Sync/Connection.h"
#include "Core/Transport/QUICClient.h"
#include "Host/ScreenClient.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace UbiquittyTest {

namespace Sync = Ubiquitty::Protocol::Sync;
namespace Transport = Ubiquitty::Transport;

/// Reads a monotonic clock.
///
/// - Returns: The time, in microseconds.
inline std::uint64_t nowMicroseconds()
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

/// Finds a UDP port on loopback that was free a moment ago.
///
/// - Returns: The port, in host order.
inline std::uint16_t freeUDPPort()
{
    const int socket = ::socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ::bind(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    socklen_t length = sizeof(address);
    ::getsockname(socket, reinterpret_cast<sockaddr*>(&address), &length);
    ::close(socket);
    return ntohs(address.sin_port);
}

/// One dial of the host: the production QUIC client carrying a Sync client.
/// The carrier's callbacks run on its I/O thread, so the client is touched
/// under the lock.
struct Dial {
    Transport::QUICClient carrier;
    UbiquittyTest::ScreenClient client;
    std::mutex lock;
    std::atomic<bool> hasConnected { false };
    std::atomic<bool> hasClosed { false };
    /// The stream and datagram bytes the host sent: none for a refused connection.
    std::atomic<std::size_t> bytesReceived { 0 };
    /// A receive cut has frozen delivery; the driver must stop the carrier.
    std::atomic<bool> hasReachedStreamCut { false };
    /// Remaining stream bytes before a test-requested cut, protected by lock.
    std::optional<std::size_t> streamBytesBeforeCut;
    /// Whether the dial sends nothing after its preamble.
    bool isSilent = false;

    /// Builds a carrier owner whose retained picture outlives each dial.
    /// - Parameters:
    ///   - replicas: The retained pictures and effect cursors.
    ///   - resume: The attachment to continue, or nothing for a fresh one.
    ///   - clientInstance: A distinct client's identity, or the fixture default.
    Dial(Sync::ReplicaSet& replicas, std::optional<Sync::ResumeRequest> resume, std::optional<Sync::ID128> clientInstance = std::nullopt)
        : client(24, 80, replicas, std::move(resume), clientInstance)
    {
    }

    ~Dial()
    {
        // Join transport callbacks before their client, mutex, and flags die.
        carrier.stop();
    }

    /// Freezes ingress and acknowledgement at an exact future stream byte.
    /// The caller stops the carrier outside its callback thread after the flag
    /// is set. Datagram boundaries remain atomic until that stream cut.
    /// - Parameter bytes: Stream bytes to deliver before freezing the client.
    void cutStreamAfter(std::size_t bytes)
    {
        const std::scoped_lock held(lock);
        streamBytesBeforeCut = bytes;
    }

    /// What a dial sends once its preamble has left.
    enum class Speaks : std::uint8_t {
        /// The one canonical Greet after its authenticated preamble.
        ItsHandshake,
        /// Nothing: a connection that goes before it has greeted.
        Nothing,
    };

    /// Dials, leading the stream with what `credential` presents and then the
    /// handshake.
    ///
    /// - Parameters:
    ///   - port: The host's QUIC port on loopback.
    ///   - credential: What the connection presents in its preamble.
    ///   - pin: The host's certificate pin.
    ///   - request: The exact fixture-owned target and serving-daemon pin.
    ///     A resumed dial must keep its original target, not infer it from panes.
    ///   - speaks: What the dial sends once its preamble has left.
    /// - Returns: Whether the carrier started.
    bool start(std::uint16_t port, Transport::QUICCredential credential, const std::string& pin, Sync::AdmissionRequest request, Speaks speaks = Speaks::ItsHandshake)
    {
        Transport::QUICClient::Callbacks callbacks;
        isSilent = speaks == Speaks::Nothing;
        if (!isSilent && !client.start(std::move(request)))
            return false;
        callbacks.onConnect = [this, speaks] {
            if (speaks == Speaks::Nothing) {
                hasConnected.store(true);
                return;
            }
            std::vector<std::uint8_t> frames;
            {
                const std::scoped_lock held(lock);
                frames = client.takeUpstream(nowMicroseconds());
            }
            if (!frames.empty())
                carrier.send(frames);
            hasConnected.store(true);
        };
        callbacks.onData = [this](std::span<const std::uint8_t> bytes) {
            bytesReceived.fetch_add(bytes.size());
            std::vector<std::uint8_t> upstream;
            {
                const std::scoped_lock held(lock);
                if (hasReachedStreamCut.load())
                    return;
                if (streamBytesBeforeCut) {
                    bytes = bytes.first(std::min(bytes.size(), *streamBytesBeforeCut));
                    *streamBytesBeforeCut -= bytes.size();
                }
                client.read(bytes, nowMicroseconds());
                if (streamBytesBeforeCut && !*streamBytesBeforeCut) {
                    hasReachedStreamCut.store(true);
                    return;
                }
                upstream = client.takeUpstream(nowMicroseconds());
            }
            if (!upstream.empty())
                carrier.send(upstream);
        };
        callbacks.onDatagram = [this](std::span<const std::uint8_t> bytes) {
            bytesReceived.fetch_add(bytes.size());
            std::vector<std::uint8_t> upstream;
            std::vector<std::vector<std::uint8_t>> datagrams;
            {
                const std::scoped_lock held(lock);
                if (hasReachedStreamCut.load())
                    return;
                client.readDatagram(bytes, nowMicroseconds());
                upstream = client.takeUpstream(nowMicroseconds());
                datagrams = client.takeDatagrams();
            }
            if (!upstream.empty())
                carrier.send(upstream);
            for (const auto& datagram : datagrams) {
                if (!carrier.sendDatagram(datagram))
                    carrier.send(datagram);
            }
        };
        callbacks.onClose = [this](Transport::Health, const std::string&) { hasClosed.store(true); };
        return carrier.start("127.0.0.1", port, std::move(credential), pin, std::move(callbacks)).has_value();
    }

    /// Runs the client's clock and reads it under the lock until `isDone`
    /// says so or the time is up.
    ///
    /// - Parameters:
    ///   - milliseconds: The longest to pump.
    ///   - isDone: Asked of the client after each pass, under the lock.
    /// - Returns: Whether `isDone` said so before the time was up.
    bool pumpUntil(int milliseconds, const std::function<bool(const UbiquittyTest::ScreenClient&)>& isDone)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
        for (;;) {
            std::vector<std::uint8_t> upstream;
            bool done = false;
            {
                const std::scoped_lock held(lock);
                if (hasConnected.load() && !isSilent && !hasReachedStreamCut.load())
                    upstream = client.takeUpstream(nowMicroseconds());
                done = isDone(client);
            }
            if (!upstream.empty())
                carrier.send(upstream);
            if (done)
                return true;
            if (std::chrono::steady_clock::now() >= deadline)
                return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    /// Reads the client's `Welcome` under the lock.
    ///
    /// - Returns: The `Welcome`, or `nullopt` before the client has one.
    std::optional<Sync::Welcome> welcome()
    {
        const std::scoped_lock held(lock);
        return client.welcome();
    }
};

} // namespace UbiquittyTest
