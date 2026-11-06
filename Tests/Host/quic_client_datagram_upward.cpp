// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Host/quic_client_datagram_upward.cpp — a client's datagram reaches the
// daemon's listener.
//
// The screen plane answers a host's datagram `Probe` with a datagram `Echo`
// (Docs/sync.md §6.4). A QUICClient that cannot send one leaves the answer
// queued in the client, and the daemon, hearing nothing for 30 s with its
// probe outstanding, declares the idle path lost. This drives
// the real QUICListener in-process against the real QUICClient on loopback:
// the client refuses a datagram before its handshake completes, takes one
// after, and the listener's datagram callback receives exactly that payload.
//
// Built only under -DUBIQUITTY_QUIC=ON. Exit 0 on success.
//

#include "Core/Transport/QUICClient.h"
#include "Platform/Host/HostQUIC.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <span>
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

} // namespace

int main()
{
    QUICListener listener;
    check(listener.start(0), "the listener starts on an ephemeral loopback port");
    std::mutex receivedMutex;
    std::vector<std::vector<std::uint8_t>> received;
    listener.setDatagramCallback([&](QUICConnection*, std::span<const std::uint8_t> bytes) {
        const std::scoped_lock lock(receivedMutex);
        received.emplace_back(bytes.begin(), bytes.end());
    });

    std::atomic<bool> connected { false };
    QUICClient client;
    QUICClient::Callbacks callbacks;
    callbacks.onConnect = [&connected] { connected.store(true); };
    // An Echo, as the screen plane frames one: kind 6 on channel 0.
    const std::array<std::uint8_t, 15> echo { 6, 0, 12, 1, 2, 3, 4, 5, 6, 7, 8, 0, 0, 0, 0 };
    check(!client.sendDatagram(echo), "a datagram before the client starts is refused, so its owner uses the stream");
    check(client.start("127.0.0.1", listener.boundPort(), Ubiquitty::Transport::QUICCredential {}, "", callbacks).has_value(), "the client starts");
    for (int i = 0; i < 400 && !connected.load(); ++i) {
        listener.processReadable();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    check(connected.load(), "the handshake completes");
    check(client.sendDatagram(echo), "the client takes a datagram once the daemon has said it accepts them");
    bool isReceived = false;
    for (int i = 0; i < 400 && !isReceived; ++i) {
        listener.processReadable();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        const std::scoped_lock lock(receivedMutex);
        isReceived = !received.empty();
    }
    {
        const std::scoped_lock lock(receivedMutex);
        check(received.size() == 1, "the listener received exactly one datagram");
        check(!received.empty() && std::vector<std::uint8_t>(echo.begin(), echo.end()) == received.front(), "and it is the payload the client sent");
    }
    const std::vector<std::uint8_t> oversized(64 * 1024, 0);
    check(!client.sendDatagram(oversized), "a datagram larger than the daemon accepts is refused");

    client.stop();
    listener.stop();
    if (failures) {
        std::printf("FAIL: %d check(s) failed\n", failures);
        return 1;
    }
    std::printf("OK: quic_client_datagram_upward\n");
    return 0;
}
