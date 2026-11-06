// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Spawn.cpp — PAL/Stream.h's process spawning on iOS.
//
// The platform runs no child process of the application's choosing. That is the
// rule iOS is built on rather than a gap in this port, so the capability reports
// itself absent where a caller asks — the same answer Source/Platform/Wasm/Spawn.cpp
// gives, for the same kind of reason.
//
// The refusal is what enforces the policy, and it enforces more than a trim did.
// Cutting the callers out of the Core subset only kept the code that would have
// asked; a backend that refuses means the binary contains no fork, no execve and
// no posix_spawn at all, while everything that merely names a spawn still links.
// A session on this platform arrives over a carrier the shell drives, and a
// caller asking for a local one is answered no rather than failing to build.

#include "PAL/Stream.h"

namespace Ubiquitty::PAL {

PipedCommand spawnPipedCommand(std::string_view, CommandStandardError)
{
    // Empty: `operator bool` is false and both endpoints are null, the shape a
    // failed spawn takes anywhere else, so a caller that already handles one
    // needs no branch for this platform.
    return {};
}

int processSpawnWithTerminalAndWait(const char* /*path*/, const char* const /*argv*/[], const char* /*workingDirectory*/, int /*stdioDescriptor*/, double /*limitSeconds*/)
{
    return -1;
}

} // namespace Ubiquitty::PAL
