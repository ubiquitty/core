// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// The daemon's socket leaf is a function of the ubiquitty-host binary's
// CONTENTS (Session::socketLeafForBinary): the same bytes reuse a running
// daemon, and different bytes start a new daemon alongside the old, which keeps
// its live sessions and self-exits when its last one ends. What the host adds
// here is its own path, resolved from the kernel rather than argv[0], so the leaf
// a PATH-launched daemon hashes is the one its binary names.

#include "Core/Session/DaemonSocketLeaf.h"

#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace Ubiquitty::Host {

// Resolve the REAL running executable's absolute path. A PATH-launched daemon has
// argv[0] == "ubiquitty-host" (no directory), usable by neither fopen nor execv —
// callers resolve self-exe here and hash that, so the socket leaf is a stable
// per-binary function regardless of launch. Empty when resolution fails.
inline std::string resolveSelfExecutablePath()
{
#if defined(__APPLE__)
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size); // query the required length
    if (size == 0)
        return std::string();
    std::string buffer(size, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0)
        return std::string();
    char resolved[PATH_MAX];
    if (::realpath(buffer.c_str(), resolved))
        return std::string(resolved);
    return std::string(buffer.c_str()); // trim at the embedded NUL
#elif defined(__linux__)
    char resolved[PATH_MAX];
    const ssize_t n = ::readlink("/proc/self/exe", resolved, sizeof(resolved) - 1);
    if (n <= 0)
        return std::string();
    resolved[n] = '\0';
    return std::string(resolved);
#else
    return std::string();
#endif
}

// The leaf a binary's daemon binds is Core's (Core/Session/DaemonSocketLeaf.h),
// because every frontend that connects to that daemon computes the same one.
using Session::socketLeafForBinary;

} // namespace Ubiquitty::Host
