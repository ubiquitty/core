// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Spawn.cpp — PAL/Stream.h's process spawning under WebAssembly.
//
// A page has no process table: nothing to fork, nothing to redirect, nothing to
// reap. The capability reports itself absent so a caller sees one failure at the
// point it asks, rather than a link error naming a syscall the target never had.

#include "PAL/Stream.h"
#include "PAL/UnixFileDescriptor.h"

namespace Ubiquitty {
namespace PAL {

PipedCommand spawnPipedCommand(std::string_view, CommandStandardError)
{
    // Empty: `operator bool` is false and both endpoints are null, which is the
    // same shape a failed spawn takes anywhere else. A caller that already
    // handles a spawn failure needs no wasm-specific branch.
    return {};
}

std::unique_ptr<Stream> inheritPTY(int masterFileDescriptor, ProcessID, std::span<const ProcessID>)
{
    const UnixFileDescriptor master(masterFileDescriptor);
    return nullptr;
}

int processSpawnWithTerminalAndWait(const char* /*path*/, const char* const /*argv*/[], const char* /*workingDirectory*/, int /*stdioDescriptor*/, double /*limitSeconds*/)
{
    return -1;
}

} // namespace PAL
} // namespace Ubiquitty
