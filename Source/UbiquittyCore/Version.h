// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

//
// Source/UbiquittyCore/Version.h — the libUbiquittyCore public-API version stamp.
//
// This is the version of the SUPPORTED PUBLIC SURFACE (Source/UbiquittyCore/UbiquittyCore.h
// and the headers it re-exports), not the Ubiquitty app marketing version (the
// repo-root VERSION file). The two version independently: the app can ship many
// releases while the library's contract holds steady, and vice-versa.
//
// Semantic-versioning policy for the public surface
// -------------------------------------------------
//   MAJOR — bump on a breaking change to anything reachable through
//           Source/UbiquittyCore/UbiquittyCore.h: a removed/renamed function, a changed
//           signature, a layout-affecting change to a public struct
//           (Terminal/Cell/Pen), or a new PAL function the caller must implement.
//   MINOR — bump on a backward-compatible addition: a new public function,
//           a new optional PAL hook (defaulted to a no-op), a new public
//           header pulled into the umbrella.
//   PATCH — bump on a behaviour-preserving fix with no surface change.
//
// A library consumer pins against this triple; the compile-time guard below
// lets a caller fail fast on a too-old header.
//

namespace Ubiquitty {

// Public-API version of libUbiquittyCore. Edit here (single source) when the
// supported surface changes per the policy above.
inline constexpr int coreVersionMajor = 1;
inline constexpr int coreVersionMinor = 0;
inline constexpr int coreVersionPatch = 0;

// Monotonic packed form for ordered comparison: major*10000 + minor*100 + patch.
inline constexpr int coreVersion = coreVersionMajor * 10000 + coreVersionMinor * 100 + coreVersionPatch;

// Human-readable "x.y.z". A literal — keep it in lock-step with the ints above.
inline constexpr const char* coreVersionString = "1.0.0";

} // namespace Ubiquitty

// Compile-time minimum-version guard. A consumer writes
//   #define UBIQUITTY_CORE_REQUIRE 0x000100   // 0.1.0, BCD nibbles M.m.p
// before including the umbrella to assert the header is at least that version.
#define UBIQUITTY_CORE_VERSION_HEX ((Ubiquitty::coreVersionMajor << 16) | (Ubiquitty::coreVersionMinor << 8) | Ubiquitty::coreVersionPatch)
