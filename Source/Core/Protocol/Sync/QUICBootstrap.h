// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// Mint a one-time QUIC token over the local socket. The QUIC data-plane faces the
// network with a self-signed cert, so a connection authenticates with a one-time
// token the daemon mints. This helper connects to the daemon's uid-gated AF_UNIX
// socket's daemon-local control exchange and returns the listener's port + the token.

#include "Core/Protocol/Local/Control.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace Ubiquitty::Protocol::Sync {

/// Why a token request answered no.
///
/// A caller deciding whether to RESTART a daemon needs the distinction the
/// error prose flattens: a daemon that answered "disabled" is one a restart
/// can fix, while one that never answered may be mid-boot or gone — acting on
/// that as if it were a stated refusal kills a daemon over a hiccup.
enum class QUICTokenRefusal : std::uint8_t {
    /// The token was granted.
    None,
    /// The daemon answered, and its answer is that QUIC is not enabled.
    Disabled,
    /// No usable answer: unreachable, timed out, or a protocol fault.
    Unreachable,
};

// Connect to the daemon at `socketPath`, mint a one-time QUIC token, and
// fill `port` + `token`. Returns false (with `error` set) if the daemon is
// unreachable, speaks no answer in time, or has QUIC disabled. Blocking with
// a short internal timeout.
bool requestQUICToken(const std::string& socketPath, std::uint16_t& port, std::string& token, std::string& certSHA256, std::string& error);

/// The same request, reporting which kind of no a refusal was.
///
/// - Parameters:
///   - socketPath: The daemon's control socket.
///   - port: Receives the QUIC port on a grant.
///   - token: Receives the one-time token on a grant.
///   - certSHA256: Receives the certificate pin on a grant.
///   - error: Receives the human-readable reason on a refusal.
///   - refusal: Receives which kind of no this was; `None` on a grant.
/// - Returns: Whether a token was granted.
bool requestQUICToken(const std::string& socketPath, std::uint16_t& port, std::string& token, std::string& certSHA256, std::string& error, QUICTokenRefusal& refusal);

// ---- the bootstrap LINE (producer <-> parser contract) --------------------
// `ubiquitty-host --quic-bootstrap` prints one line carrying the dial info. The ssh
// (or local) channel that runs it is authenticated, so it is the trusted carrier for
// the cert fingerprint — the dialer PINS it and refuses a MITM's different cert.
// This format string is the single source of truth for both the producer and
// parseQUICBootstrapLine() below: whitespace-separated, order-independent `key=value`
// fields; values are lowercase hex (token, certificateSHA256) and a decimal port.
inline constexpr char quicBootstrapLineFormat[] = "quic_port=%u quic_token=%s quic_cert_sha256=%s\n";

// Decoded-field caps the success gate enforces (single definition; the daemon reuses
// this header): a 32-byte SHA-256 pin and a token within the daemon's cap.
using Local::certificateSHA256Length;
using Local::maxQUICTokenLength;

// The dial info a successful bootstrap line carries. `token` and `certificateSHA256`
// are the DECODED bytes (not hex). A result is only usable when all three are
// present (see parseQUICBootstrapLine's return contract).
struct QUICBootstrapInfo {
    /// The numeric address the connection that ran the bootstrap reached,
    /// which the QUIC carrier dials: the token and the pin are that host's.
    /// It is never part of the line; the client that ran the bootstrap
    /// fills it in from its own connection (`Session::readSSHReach`, or an
    /// in-process SSH client's socket), so the host's name is not resolved a
    /// second time to what may be another machine.
    std::string reachedAddress;
    std::uint16_t port = 0;
    std::string token; // decoded one-time token bytes
    std::string certificateSHA256; // decoded cert fingerprint bytes (the pin)
};

// Decode hex into bytes. Returns "" on any malformed input (odd length or non-hex
// digit), so a corrupt field never half-decodes into a usable value.
inline std::string decodeHexBytes(std::string_view hex)
{
    if (hex.size() % 2 != 0)
        return {};
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    };
    std::string result;
    result.reserve(hex.size() / 2);
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        int hi = nibble(hex[i]);
        int lo = nibble(hex[i + 1]);
        if (hi < 0 || lo < 0)
            return {};
        result.push_back(static_cast<char>((hi << 4) | lo));
    }
    return result;
}

// Parse a `quic_port=N quic_token=<hex> quic_cert_sha256=<hex>` line into `result`.
// Returns true only when all three fields are present and well-formed (see the gate
// below): an unpinned token is MITM-stealable, so a line missing the cert (or any
// field) returns false and the caller must refuse QUIC rather than dial without a
// pin. Tolerant of extra whitespace, trailing newline, and unknown fields.
inline bool parseQUICBootstrapLine(std::string_view text, QUICBootstrapInfo& result)
{
    result = QUICBootstrapInfo {};

    // Split on any whitespace (the producer separates fields with a single
    // space + a trailing newline; tolerate runs and CR for robustness).
    auto isSpace = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; };
    std::size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && isSpace(text[i]))
            ++i;
        std::size_t start = i;
        while (i < text.size() && !isSpace(text[i]))
            ++i;
        if (i == start)
            break;
        std::string_view field = text.substr(start, i - start);

        auto valueAfter = [&](std::string_view key) -> std::string_view {
            if (field.starts_with(key) && field.size() > key.size())
                return field.substr(key.size());
            return {};
        };

        if (std::string_view portValue = valueAfter("quic_port="); !portValue.empty()) {
            std::uint32_t port = 0;
            bool digits = true;
            for (char c : portValue) {
                if (c < '0' || c > '9') {
                    digits = false;
                    break;
                }
                port = port * 10 + static_cast<std::uint32_t>(c - '0');
                if (port > 0xFFFF) {
                    digits = false;
                    break;
                }
            }
            if (digits)
                result.port = static_cast<std::uint16_t>(port);
        } else if (std::string_view tokenHex = valueAfter("quic_token="); !tokenHex.empty()) {
            result.token = decodeHexBytes(tokenHex);
        } else if (std::string_view certHex = valueAfter("quic_cert_sha256="); !certHex.empty()) {
            result.certificateSHA256 = decodeHexBytes(certHex);
        }
        // Unknown fields are ignored.
    }

    // Require all three: nonzero port, a token within the daemon's cap, and a cert pin
    // exactly 32 bytes (a SHA-256). decodeHexBytes accepts any even-length hex, so
    // without these size checks a short pin or over-cap token would pass as usable.
    return result.port != 0 && !result.token.empty() && result.token.size() <= maxQUICTokenLength && result.certificateSHA256.size() == certificateSHA256Length;
}

} // namespace Ubiquitty::Protocol::Sync
