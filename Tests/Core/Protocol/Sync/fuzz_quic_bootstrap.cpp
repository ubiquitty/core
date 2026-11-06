// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// fuzz_quic_bootstrap.cpp — libFuzzer harness for the QUIC bootstrap-line
// parser (Source/Core/Protocol/Sync/QUICBootstrap.h: parseQUICBootstrapLine +
// decodeHexBytes).
//
// The bootstrap line is the cert-PIN carrier: `ubiquitty-host
// --quic-bootstrap` prints one line over the (ssh/local) trusted channel —
// `quic_port=N quic_token=<hex> quic_cert_sha256=<hex>` — and the dialer
// PINS that cert fingerprint so an active MITM presenting a different
// self-signed cert is refused. The hex-decode + length math is the fuzz
// surface: a malformed pin that half-decodes into a SUCCESS with the wrong
// byte length would weaken the MITM gate.
//
// Two faults are caught:
//
//   (1) memory/UB faults in the hex-decode + field-split (caught by
//       -fsanitize=fuzzer,address,undefined): the parser must never read OOB
//       on any byte string — empty, odd-length, embedded NULs, no spaces, a
//       single giant field, etc.
//
//   (2) a CONTRACT fault asserted directly here, so it is caught even without
//       a sanitizer trip: when the parser returns SUCCESS it promises a
//       "usable" dial info, yet the consumer (RemoteSessionCore's QUIC dial) hands
//       `info.certificateSHA256` straight to the pin and the pin is only valid at
//       exactly 32 bytes (SHA256_DIGEST_LENGTH — see QUICClient.cpp
//       peerCertMatchesPin / the size guard). A SUCCESS carrying a non-32-byte
//       pin (or an over-cap token, daemon cap = 64 bytes) is a half-decode the
//       parser's all-three gate failed to reject. checkBootstrapContract()
//       asserts the exact byte lengths so that half-decode is a hard finding.
//
// Header-only target (pure std::string / std::string_view), so the harness
// links standalone with no Sync / socket dependency. Same shape as
// Tests/Core/Protocol/Sync/fuzz_sync.cpp; build via Scripts/run-tests.sh
// --with-fuzz.

#include "Core/Protocol/Sync/QUICBootstrap.h"

#include <cassert>
#include <cstdint>
#include <stddef.h>
#include <string>
#include <string_view>

using Ubiquitty::Protocol::Sync::certificateSHA256Length;
using Ubiquitty::Protocol::Sync::decodeHexBytes;
using Ubiquitty::Protocol::Sync::maxQUICTokenLength;
using Ubiquitty::Protocol::Sync::parseQUICBootstrapLine;
using Ubiquitty::Protocol::Sync::QUICBootstrapInfo;

// The pin is a raw SHA-256 of the peer cert DER; the comparator
// (QUICClient.cpp peerCertMatchesPin) only accepts exactly this many bytes,
// and the size guard at dial time refuses anything else. The daemon mints a
// 32-byte token and caps the wire token at 64 bytes (Daemon.cpp
// maxQUICTokenLength). The parser is the gate that should never let a
// wrong-length value through as SUCCESS.

// On parser SUCCESS the dial info must be structurally usable: a decoded pin
// of exactly 32 bytes and a token within the producer/daemon cap. A success
// that violates either is a half-decode the all-three gate missed — exactly
// the MITM-gate-weakening case this harness exists to catch.
static void checkBootstrapContract(const QUICBootstrapInfo& info)
{
    // Success implies all three present (the documented return contract).
    assert(info.port != 0);
    assert(!info.token.empty());
    assert(!info.certificateSHA256.empty());

    // The invariants that matter: a SUCCESS must not carry a pin of the wrong
    // length (the MITM defense pins exactly 32 bytes) nor an over-cap token.
    assert(info.certificateSHA256.size() == certificateSHA256Length);
    assert(info.token.size() <= maxQUICTokenLength);
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    std::string_view text(reinterpret_cast<const char*>(data), size);

    // (a) the whole line through the public parser.
    {
        QUICBootstrapInfo info;
        if (parseQUICBootstrapLine(text, info))
            checkBootstrapContract(info);
    }

    // (b) the hex decoder directly — the inner length/nibble math. Either it
    // returns "" (rejected) or a string of exactly half the input length with
    // no OOB; assert that round-trip so a future change can't return a
    // truncated/over-long buffer. ASAN/UBSAN cover the read side.
    {
        std::string decoded = decodeHexBytes(text);
        if (!decoded.empty())
            assert(decoded.size() == text.size() / 2);
    }

    return 0;
}
