// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// PTYStream.cpp — PAL/Stream.h's pseudo-terminal factory on iOS.
//
// There is no local shell to open one for. The platform runs no child process
// of the application's choosing, which is not a gap in this port but the rule
// the platform is built on — so the capability reports itself absent at the
// point a caller asks, rather than the arm failing to link over a symbol naming
// a facility the target never had.
//
// This mirrors Source/Platform/Wasm/Spawn.cpp, which answers the same way for
// the same reason: a page has no process table either.
//
// A swapped backend rather than a trimmed translation unit. TerminalRuntime is
// what the shared surface's drive is built on, and it names openPTY
// unconditionally. Cutting the runtime out of the iOS Core subset therefore cut
// the surface out with it — the frontend lost the renderer to avoid a symbol it
// never calls. A backend that refuses is the smaller, truer statement: the
// session a terminal on this platform shows arrives over a carrier, and a
// caller that asks for a local one is answered no.

#include "PAL/Stream.h"
#include "PAL/UnixFileDescriptor.h"

namespace Ubiquitty::PAL {

std::unique_ptr<Stream> openPTY(std::span<const std::string> /*arguments*/, const PTYLaunchOptions& /*options*/)
{
    // Null is the shape a failed open takes on every other backend, so a caller
    // that already handles one needs no iOS-specific branch.
    return nullptr;
}

std::unique_ptr<Stream> inheritPTY(int masterFileDescriptor, ProcessID, std::span<const ProcessID>)
{
    const UnixFileDescriptor master(masterFileDescriptor);
    return nullptr;
}

} // namespace Ubiquitty::PAL
