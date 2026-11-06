// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Host/quic_retry_validation.cpp — security negative for QUIC Retry validation:
// a pre-auth QUIC Initial flood can't allocate connection state, and a real
// client still completes its handshake despite the flood.
//
// The flaw in an eviction-only design: admitting a new Initial by
// evicting the OLDEST pre-handshake slot only raises the required flood rate —
// a legit client, once admitted, still ages into "oldest pre-handshake" and
// gets evicted before its real (network) RTT handshake completes if the flood
// arrives fast enough. It never closes the DoS, only makes it more expensive.
//
// The listener does stateless address validation (RFC 9000 §8.1) before allocating
// anything. A fresh Initial with no Retry token gets a Retry packet — sent to
// the packet's CLAIMED source address, never wherever it actually came from —
// and nothing is allocated (no ngtcp2_conn, no TLS context, no QUICConnection).
// A spoofed source never receives that Retry and so can never resend an
// Initial that echoes its token, so it can never pass the verify gate that
// precedes allocation. The flood's cost to us is one ngtcp2_accept() parse +
// one HMAC-token generate per packet — O(1), no allocation, no dependency on
// how many prior (abandoned) attempts exist.
//
// This file drives the REAL QUICListener (Source/Platform/Host/HostQUIC.cpp)
// against the REAL QUICClient (Source/Platform/POSIX/QUICClient.cpp) in-process,
// over real loopback UDP with real ngtcp2 + AWS-LC on both sides:
//
//   (a) sustained flood allocates nothing — many QUICClient instances each
//       start() (guaranteed to send exactly one real, well-formed Initial —
//       ioLoop's first, unconditional statement is the client-Initial write,
//       before the stopRequested check) and are immediately stop()ped, torn
//       down before they could ever read back a Retry. The listener services
//       every one of these Initials (each gets a Retry sent to it) but the
//       flood is torn down before completing the round trip — indistinguishable
//       in effect from a genuinely spoofed source that never responds.
//       testQUICConnectionCount(listener) must stay 0 throughout.
//   (b) the legit client still completes — a real, un-stopped QUICClient
//       started after (and interleaved with servicing) the flood completes its
//       full handshake (onConnect fires) and testQUICConnectionCount == 1 —
//       exactly the one real connection, proving the flood cost it nothing.
//
// Verify-by-revert: skip the Retry gate (accept unconditionally) and
// (a)'s assertion fails — every flood attempt allocates a
// QUICConnection that sits until its 5 s handshake_timeout.
//
// Built only under -DUBIQUITTY_QUIC=ON. Exit 0 on success.
//

#include "Core/Transport/QUICClient.h"
#include "Platform/Host/HostQUIC.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

using Ubiquitty::Host::QUICListener;
using Ubiquitty::Host::testQUICConnectionCount;
using Ubiquitty::Transport::QUICClient;

static int failures = 0;
static void check(bool ok, const char* what) // bool-ok: matches every Tests/Host/*.cpp check() harness
{
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        ++failures;
}

// Service the listener's socket for `iterations` short ticks, giving it a
// chance to read pending datagrams and send Retries/handshake responses —
// mirrors the daemon's real poll()-driven cadence, without needing poll() here.
static void pump(QUICListener& listener, int iterations, int sleepMilliseconds)
{
    for (int i = 0; i < iterations; ++i) {
        listener.processReadable();
        std::this_thread::sleep_for(std::chrono::milliseconds(sleepMilliseconds));
    }
}

int main()
{
    QUICListener listener;
    check(listener.start(0), "the listener starts on an ephemeral loopback port");
    const std::uint16_t port = listener.boundPort();

    // (a) sustained flood allocates nothing.
    constexpr int floodCount = 100;
    for (int i = 0; i < floodCount; ++i) {
        QUICClient flood;
        QUICClient::Callbacks callbacks; // no-op: this attempt is torn down before any fire
        (void)flood.start("127.0.0.1", port, Ubiquitty::Transport::QUICCredential {}, "", callbacks); // outcome unread: only the Initial it sends matters here
        flood.stop(); // joins the I/O thread — the one guaranteed Initial is already sent;
                      // torn down before it could ever read a Retry back
        pump(listener, 2, 2); // let the listener see + Retry this Initial (nobody reads it)
    }
    pump(listener, 10, 5); // drain anything still in flight
    check(testQUICConnectionCount(listener) == 0,
        "a sustained flood of real, well-formed Initials allocates ZERO connections "
        "when none of them complete the Retry round trip");

    // (b) the legit client still completes, despite the preceding flood.
    std::atomic<bool> connected { false };
    QUICClient real;
    QUICClient::Callbacks realCallbacks;
    realCallbacks.onConnect = [&connected] { connected.store(true); };
    check(real.start("127.0.0.1", port, Ubiquitty::Transport::QUICCredential {}, "", realCallbacks).has_value(), "the real client's start() succeeds");
    for (int i = 0; i < 400 && !connected.load(); ++i) {
        listener.processReadable();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    check(connected.load(),
        "a real client completes its FULL handshake (Initial -> Retry -> Initial+token "
        "-> handshake) despite the preceding 100-attempt flood — not starved");
    check(testQUICConnectionCount(listener) == 1, "exactly ONE connection is allocated total — the flood consumed no slots");

    real.stop();
    listener.stop();

    if (failures) {
        std::printf("FAIL: %d check(s) failed\n", failures);
        return 1;
    }
    std::printf("OK: quic_retry_validation\n");
    return 0;
}
