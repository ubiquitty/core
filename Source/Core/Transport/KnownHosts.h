// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// Host-key pinning policy: the fingerprint form, the endpoint key, and the
// verdict a presented key earns against a pinned one.
//
// Portable because it is policy, not plumbing. A frontend that speaks SSH
// in-process (iOS today, any frontend that stops shelling out to /usr/bin/ssh
// later) supplies the key bytes and the store; what "matches", "unknown" and
// "changed" mean is decided once, here. The rule this exists to hold is that a
// changed key is never silently re-pinned — verify() reports the mismatch and
// leaves the pin alone, because a changed key is either a rebuilt host or a
// machine in the middle and only a person can tell which.
//
// Storage is the frontend's (the Keychain on Apple platforms, a file elsewhere):
// this header decides, it does not persist.

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace Ubiquitty::Transport {

/// What a presented host key earned against what was pinned for its endpoint.
///
/// Three outcomes rather than a boolean, because "no pin yet" and "a different
/// key than last time" call for opposite handling and a boolean would collapse
/// them into the same refusal — or, worse, the same acceptance.
enum class HostKeyVerdict : std::uint8_t {
    // The endpoint has no pin. Whether that is allowed is the caller's policy
    // (trust on first use), which is why it is not an outcome here.
    Unknown = 0,
    // The presented key is the pinned one.
    Matched = 1,
    // A different key is pinned. Refuse; never re-pin from this verdict.
    Mismatched = 2,
};

/// Returns the SHA-256 fingerprint of an SSH host key.
///
/// The `SHA256:<base64>` form OpenSSH prints, with the base64 padding stripped
/// as OpenSSH strips it, so a fingerprint shown in our UI compares character by
/// character against `ssh-keygen -lf` output without anyone converting anything.
///
/// - Parameter keyBytes: The host key in SSH wire encoding — the same bytes a
///   `known_hosts` line base64s, so the digest matches OpenSSH's.
/// - Returns: The fingerprint string.
[[nodiscard]] std::string hostKeyFingerprint(std::span<const std::uint8_t> keyBytes);

/// Returns the key an endpoint's pin is stored under.
///
/// Host and port together, so two ports on one machine pin independently the
/// way `known_hosts` treats them as separate entries.
///
/// - Parameters:
///   - host: The host name or address as dialled.
///   - port: The TCP port.
/// - Returns: The store key.
[[nodiscard]] std::string hostKeyEndpoint(std::string_view host, int port);

/// Judges a presented fingerprint against the pinned one.
///
/// Comparison only: a caller that receives `Unknown` decides whether to pin,
/// and a caller that receives `Mismatched` must refuse. An empty
/// `pinnedFingerprint` means no pin is recorded.
///
/// - Parameters:
///   - pinnedFingerprint: What the store holds for the endpoint, or empty.
///   - presentedFingerprint: What the server offered this time.
/// - Returns: The verdict.
[[nodiscard]] HostKeyVerdict verifyHostKeyFingerprint(std::string_view pinnedFingerprint, std::string_view presentedFingerprint);

} // namespace Ubiquitty::Transport
