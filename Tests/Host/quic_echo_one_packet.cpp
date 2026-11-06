// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Host/quic_echo_one_packet.cpp — a keystroke echo is one QUIC packet.
//
// A publish that echoes a keystroke sends the pane's superset update as a
// datagram and the frames beside it on the relay stream (Docs/sync.md §6.5).
// Written apart, the two are two packets, and the second can wait a pacing
// interval behind the first. The listener writes a datagram with
// NGTCP2_WRITE_DATAGRAM_FLAG_MORE while stream data waits, so ngtcp2 packs the
// stream frame into the datagram's packet.
//
// Drives the real QUICListener in-process against the real QUICClient on
// loopback: once the connection is quiet, a datagram and stream bytes queued
// together leave in one packet, and the client receives both.
//
// Built only under -DUBIQUITTY_QUIC=ON. Exit 0 on success.
//

#include "Core/Transport/QUICClient.h"
#include "Platform/Host/HostQUIC.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

using Ubiquitty::Host::QUICConnection;
using Ubiquitty::Host::QUICListener;
using Ubiquitty::Transport::QUICClient;

namespace {

int failures = 0;

void check(bool ok, const char* what) // bool-ok: matches every Tests/Host/*.cpp check() harness
{
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        ++failures;
}

/// Services the listener until it has sent nothing for a while, so the
/// handshake's and the acknowledgements' packets are behind the count.
///
/// - Parameter listener: The listener.
void settle(QUICListener& listener)
{
    std::uint64_t sent = listener.packetsSent();
    for (int quietTicks = 0, tick = 0; quietTicks < 20 && tick < 2'000; ++tick) {
        listener.processReadable();
        listener.serviceTimers();
        listener.flush();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        quietTicks = listener.packetsSent() == sent ? quietTicks + 1 : 0;
        sent = listener.packetsSent();
    }
}

/// Services the listener as ``settle`` does until a condition holds or a
/// deadline passes.
///
/// The listener retransmits only from its timers, so a wait that reads
/// without servicing them stalls on the first lost packet. The deadline is
/// wall time rather than a count of ticks: under a loaded suite a tick takes
/// several times its sleep, and a retransmission waits out a backoff that a
/// count sized for an idle machine does not cover.
///
/// - Parameters:
///   - listener: The listener.
///   - isDone: Whether the wait is over.
/// - Returns: Whether the condition held before the deadline.
template<typename Condition>
bool serviceUntil(QUICListener& listener, Condition isDone)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!isDone()) {
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        listener.processReadable();
        listener.serviceTimers();
        listener.flush();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}

} // namespace

int main()
{
    QUICListener listener;
    check(listener.start(0), "the listener starts on an ephemeral loopback port");
    std::atomic<QUICConnection*> connection { nullptr };
    listener.setStreamDataCallback([&](QUICConnection* from, std::span<const std::uint8_t>) { connection.store(from); });

    std::atomic<bool> connected { false };
    std::mutex receivedMutex;
    std::size_t streamBytes = 0;
    std::size_t datagrams = 0;
    QUICClient client;
    QUICClient::Callbacks callbacks;
    callbacks.onConnect = [&connected] { connected.store(true); };
    callbacks.onData = [&](std::span<const std::uint8_t> bytes) {
        const std::scoped_lock lock(receivedMutex);
        streamBytes += bytes.size();
    };
    callbacks.onDatagram = [&](std::span<const std::uint8_t>) {
        const std::scoped_lock lock(receivedMutex);
        ++datagrams;
    };
    check(client.start("127.0.0.1", listener.boundPort(), Ubiquitty::Transport::QUICCredential {}, "", callbacks).has_value(), "the client starts");
    check(serviceUntil(listener, [&] { return connected.load(); }), "the handshake completes");
    // The client's first bytes open the relay stream the listener answers on.
    const std::vector<std::uint8_t> hello(16, 'h');
    client.send(hello);
    check(serviceUntil(listener, [&] { return connection.load() != nullptr; }), "the client's relay stream reached the listener");
    if (!connection.load()) {
        client.stop();
        listener.stop();
        return 1;
    }
    check(listener.datagramCapable(connection.load()), "the client accepts datagrams");
    settle(listener);

    // The echo: a superset update as a datagram, and the frames beside it on
    // the stream, queued in one pass and flushed once.
    const std::uint64_t before = listener.packetsSent();
    const std::vector<std::uint8_t> update(48, 'u');
    const std::vector<std::uint8_t> frames(96, 's');
    listener.sendDatagram(connection.load(), update);
    listener.sendStream(connection.load(), frames);
    listener.flush();
    const std::uint64_t echoPackets = listener.packetsSent() - before;
    std::printf("  the echo took %llu packet(s)\n", static_cast<unsigned long long>(echoPackets));
    check(echoPackets == 1, "a datagram and the stream bytes queued with it leave in one packet");

    const bool isReceived = serviceUntil(listener, [&] {
        const std::scoped_lock lock(receivedMutex);
        return datagrams == 1 && streamBytes == frames.size();
    });
    check(isReceived, "the client received the datagram and the stream bytes");

    client.stop();
    listener.stop();
    if (failures) {
        std::printf("FAIL: %d check(s) failed\n", failures);
        return 1;
    }
    std::printf("OK: quic_echo_one_packet\n");
    return 0;
}
