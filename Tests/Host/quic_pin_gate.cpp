// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Host/quic_pin_gate.cpp — security negative for the QUIC cert pin gate:
//
// TLS is SSL_VERIFY_NONE (QUICClient.cpp) — the SHA-256 cert pin is the only
// daemon authentication. Receive handlers (recv_stream_data / recv_datagram)
// that fire unconditionally deliver to onData/onDatagram (the Sync decoder) the
// instant a 1-RTT frame decrypts — before a pin check in ioLoop after
// readDatagrams ever runs. An active MITM completes the SSL_VERIFY_NONE
// handshake with its own cert, then QUIC-coalesces the server's Finished with a
// 1-RTT STREAM/DATAGRAM frame into one UDP datagram: one recvfrom + one
// read_pkt fires handshake_completed then the receive handlers, both before
// readDatagrams returns to ioLoop. A spoofed Sync frame gets decoded and
// applied to the terminal model.
//
// So the pin is checked synchronously inside handshakeCompletedHandler (the
// instant the handshake completes), not later in ioLoop, and onData/
// onDatagram are gated on that verified flag. This closes the race for every
// case, not just the described coalesced-packet sequence, because it's a QUIC protocol
// guarantee (not incidental ordering): 1-RTT decrypt keys don't exist until
// the handshake completes, so handshakeCompletedHandler is always invoked
// before any 1-RTT receive handler that could fire in the same read_pkt call.
//
// This file drives the REAL (file-scope) ngtcp2 callbacks directly via a
// friend seam — no live network needed, since the invariant under test is
// purely "does isPinVerified gate delivery," independent of how the
// handshake completed:
//   (a) before verification — a receive handler fires with isPinVerified
//       still false (the pre-handshake-complete state, or a case where the
//       pin genuinely mismatched): onData/onDatagram must not fire.
//   (b) after verification (no pin configured — the empty-pin case) —
//       simulateHandshakeCompleted sets isPinVerified; a subsequent receive
//       handler call does deliver the exact bytes.
//   (c) the ordering guarantee itself — a fresh client (isPinVerified
//       defaults false) receiving data before handshakeCompletedHandler ever
//       runs is dropped; the same bytes delivered after are not — proving
//       the gate, not just a static flag check, actually depends on when
//       handshakeCompletedHandler ran.
//
// Verify-by-revert: removing the `isPinVerified` gate from the receive
// handlers (deliver unconditionally, with no pin gate) fails (a) and (c)'s
// "not delivered" assertions.
//
// Built only under -DUBIQUITTY_QUIC=ON. Exit 0 on success.
//

#include "Core/Transport/QUICClient.h"

#include <cstdio>
#include <span>
#include <string>
#include <vector>

using Ubiquitty::Transport::PinGateAccess;
using Ubiquitty::Transport::QUICClient;

static int failures = 0;
static void check(bool ok, const char* what) // bool-ok: matches every Tests/Host/*.cpp check() harness
{
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        ++failures;
}

int main()
{
    const std::vector<std::uint8_t> payload = { 0xDE, 0xAD, 0xBE, 0xEF };

    // A failed trust check is a typed fact before any close reaches the
    // session; no diagnostic substring decides whether fallback is safe.
    {
        QUICClient client;
        check(!client.peerAuthenticationFailed(), "an unused client has no authentication refusal");
        PinGateAccess::setPinnedCertSHA256ForTest(client, std::string(32, 'x'));
        PinGateAccess::simulateHandshakeCompleted(client);
        check(client.peerAuthenticationFailed(), "an absent peer certificate fails the configured pin as a typed refusal");
        check(!PinGateAccess::isPinVerified(client), "a typed authentication refusal never verifies the peer");
    }

    // (a) before verification — a fresh client (isPinVerified defaults false,
    // no handshake has completed) must not deliver anything to onData/onDatagram.
    {
        QUICClient client;
        bool didOnDataFire = false, didOnDatagramFire = false;
        PinGateAccess::installDataCallbacksForTest(client, [&didOnDataFire](std::span<const std::uint8_t>) { didOnDataFire = true; }, [&didOnDatagramFire](std::span<const std::uint8_t>) { didOnDatagramFire = true; });

        check(!PinGateAccess::isPinVerified(client), "a fresh client starts unverified");
        PinGateAccess::simulateReceiveStreamData(client, payload);
        PinGateAccess::simulateReceiveDatagram(client, payload);
        check(!didOnDataFire, "onData must NOT fire before the pin is verified");
        check(!didOnDatagramFire, "onDatagram must NOT fire before the pin is verified");
    }

    // (b) after verification (empty pin — no pin configured) — delivery proceeds
    // normally, with the exact bytes, once the handshake-complete callback runs.
    {
        QUICClient client;
        std::vector<std::uint8_t> receivedData, receivedDatagram;
        PinGateAccess::installDataCallbacksForTest(client, [&receivedData](std::span<const std::uint8_t> bytes) { receivedData.assign(bytes.begin(), bytes.end()); }, [&receivedDatagram](std::span<const std::uint8_t> bytes) { receivedDatagram.assign(bytes.begin(), bytes.end()); });
        // pinnedCertSHA256 left empty (default) — the "no pin configured" case.

        PinGateAccess::simulateHandshakeCompleted(client);
        check(PinGateAccess::isPinVerified(client), "handshakeCompletedHandler verifies immediately when no pin is configured");

        PinGateAccess::simulateReceiveStreamData(client, payload);
        PinGateAccess::simulateReceiveDatagram(client, payload);
        check(receivedData == payload, "onData delivers the exact bytes once verified");
        check(receivedDatagram == payload, "onDatagram delivers the exact bytes once verified");
    }

    // (c) the ordering guarantee — the same client, same callbacks: receive
    // before the handshake completes is dropped; receive after is delivered.
    // Proves the gate tracks the real handshake-complete event, not a
    // permanently-true/false static condition.
    {
        QUICClient client;
        int onDataCount = 0;
        PinGateAccess::installDataCallbacksForTest(client, [&onDataCount](std::span<const std::uint8_t>) { ++onDataCount; }, [](std::span<const std::uint8_t>) { });

        PinGateAccess::simulateReceiveStreamData(client, payload);
        check(onDataCount == 0, "receive BEFORE handshake-complete: dropped (count stays 0)");

        PinGateAccess::simulateHandshakeCompleted(client);
        PinGateAccess::simulateReceiveStreamData(client, payload);
        check(onDataCount == 1, "receive AFTER handshake-complete: delivered exactly once");
    }

    if (failures) {
        std::printf("FAIL: %d check(s) failed\n", failures);
        return 1;
    }
    std::printf("OK: quic_pin_gate\n");
    return 0;
}
