// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

//
// Sync/WireVersion.h — what a build says about itself to a peer: its wire
// version (Envelope.h), which two builds must share to connect, and the app
// version and identity a person reads beside it (Docs/sync.md §7.3,
// "Versions").
//

#include "Core/BuildVersion.h"
#include "Core/Protocol/Sync/Admission.h"
#include "Core/Protocol/Sync/Envelope.h"
#include "Core/Protocol/Sync/Messages.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace Ubiquitty::Protocol::Sync {

/// What a test binary compiled outside the build system states as its
/// version; not a canonical version, so it orders against nothing.
inline constexpr std::string_view unversionedAppVersion = Ubiquitty::unversionedBuildVersion;

/// The most bytes a build identity takes.
inline constexpr std::size_t maxBuildIdentityBytes = 64;

/// Reads this build's version.
///
/// - Returns: `MAJOR.MINOR.PATCH.N` as the build derived it from its commit,
///   with no `.N` at a named version (AppVersion.h).
[[nodiscard]] std::string_view appVersion();

/// Reads what identifies this build to a person beside its version.
///
/// The commit's short hash and what qualifies it, e.g. "1a2b3c4d, modified".
/// It is shown and logged, never compared and never sent in a handshake.
///
/// - Returns: The identity, at most ``maxBuildIdentityBytes``.
[[nodiscard]] std::string_view buildIdentity();

/// Writes this build's version and identity as a surface prints them.
///
/// - Returns: "0.2.0.3 (1a2b3c4d, modified)".
[[nodiscard]] std::string appVersionWithIdentity();

/// Describes this build as a peer reads it.
///
/// - Returns: This build's wire version and app version.
[[nodiscard]] PeerBuild localPeerBuild();

/// Reads an app version a peer sent.
///
/// The text reaches a sentence a person reads, so only what a version is made
/// of passes: ASCII letters, digits, '.', '-' and '+', at most
/// `maxAppVersionBytes`.
///
/// - Parameter bytes: The bytes the peer sent.
/// - Returns: The version, or empty when the bytes are not one.
[[nodiscard]] std::string appVersionFromPeer(std::span<const std::uint8_t> bytes);

} // namespace Ubiquitty::Protocol::Sync
