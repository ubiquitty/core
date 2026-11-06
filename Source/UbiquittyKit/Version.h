// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

//
// Source/UbiquittyKit/Version.h — the UbiquittyKit version stamp.
//
// kitVersion versions the two surfaces an embedder builds against: the C++
// FACADE (UbiquittyKit/UbiquittyKit.h and the headers it re-exports) and the
// public Swift interface of UbiquittyKit.framework, whose Info.plist carries
// kitVersionString as its bundle version. It moves independently of both
// coreVersion (the Tier-1 libUbiquittyCore surface,
// Source/UbiquittyCore/Version.h) and the app marketing VERSION.
//
// Semantic-versioning policy, applied to both surfaces:
//   MAJOR — a breaking change reachable through UbiquittyKit.h or the
//           framework's module interface: a removed or renamed member, a
//           changed signature, or a NEW PURE-VIRTUAL on Session::Observer (it
//           breaks every embedder subclass).
//   MINOR — a backward-compatible addition: a new method, property or type,
//           or a new Observer hook with a { } default (which is why every
//           Observer method ships one).
//   PATCH — a behaviour-preserving fix with no surface change.
//

namespace UbiquittyKit {

// Edit here (single source) when either surface changes per the policy.
inline constexpr int kitVersionMajor = 1;
inline constexpr int kitVersionMinor = 4;
inline constexpr int kitVersionPatch = 0;

// Monotonic packed form for ordered comparison: major*10000 + minor*100 + patch.
inline constexpr int kitVersion = kitVersionMajor * 10000 + kitVersionMinor * 100 + kitVersionPatch;

// Human-readable "x.y.z". A literal — keep it in lock-step with the ints above.
inline constexpr const char* kitVersionString = "1.4.0";

} // namespace UbiquittyKit

// Compile-time minimum-version guard, mirroring UBIQUITTY_CORE_REQUIRE:
//   UBIQUITTY_KIT_REQUIRE(1, 0, 0);
#define UBIQUITTY_KIT_REQUIRE(major, minor, patch)                                           \
    static_assert(::UbiquittyKit::kitVersion >= ((major) * 10000 + (minor) * 100 + (patch)), \
        "UbiquittyKit is older than the version this consumer requires")
