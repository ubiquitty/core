// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Core/BuildVersion.h"

// The build writes this header into its own directory and rewrites it when the
// commit moves (Scripts/cmake/BuildVersion.cmake). This file is the one that
// includes it, so a new commit recompiles this translation unit and no other.
// A test compiled outside the build system finds no such header and states no
// version.
#if __has_include("UbiquittyBuildVersion.h")
#include "UbiquittyBuildVersion.h"
#define UBIQUITTY_HAS_BUILD_VERSION 1
#else
#define UBIQUITTY_HAS_BUILD_VERSION 0
#endif

namespace Ubiquitty {

std::string_view buildVersion()
{
#if UBIQUITTY_HAS_BUILD_VERSION
    return Generated::buildVersion;
#else
    return unversionedBuildVersion;
#endif
}

std::string_view buildIdentity()
{
#if UBIQUITTY_HAS_BUILD_VERSION
    return Generated::buildIdentity;
#else
    return "no build";
#endif
}

} // namespace Ubiquitty
