// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// The version this binary was built as, for every layer that states it.
//
// The build writes the version into a generated header that one translation
// unit, BuildVersion.cpp, includes (Scripts/cmake/BuildVersion.cmake), so a new
// commit recompiles that unit and no other. The terminal's own reports read it
// here, because the parser's layer cannot include the Sync protocol;
// Protocol::Sync::appVersion is this same text for a peer.

#include <string_view>

namespace Ubiquitty {

/// The version a binary compiled outside the build system states.
///
/// Not a canonical version, so it orders against nothing.
inline constexpr std::string_view unversionedBuildVersion = "0.0.0-dev";

/// Returns the app version this binary was built as.
///
/// - Returns: `MAJOR.MINOR.PATCH`, with a `.N` count of commits past a named
///   version, or ``unversionedBuildVersion`` outside the build system.
[[nodiscard]] std::string_view buildVersion();

/// Returns what identifies this build to a person beside its version.
///
/// - Returns: The commit's short hash and what qualifies it, such as
///   "1a2b3c4d, modified", or "no build" outside the build system.
[[nodiscard]] std::string_view buildIdentity();

} // namespace Ubiquitty
