// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// SocketPath.cpp — PAL/Socket.h's path preparation under WebAssembly.
//
// A page has no filesystem and no local sockets, so both answers are "no" and
// the caller sees one refusal at the point it asks.

#include "PAL/Socket.h"

#include <string>
#include <string_view>

namespace Ubiquitty {
namespace PAL {

bool ensurePrivateSocketDirectory(std::string_view)
{
    // A page has no filesystem to place a socket in, so there is no directory to
    // make private and nothing a caller could do with a success.
    return false;
}

std::expected<void, std::string> prepareLocalSocketPathForBind(std::string_view)
{
    return std::unexpected(std::string("local sockets are not available on this platform"));
}

std::expected<UnixFileDescriptor, std::string> listenLocal(std::string_view, int)
{
    return std::unexpected(std::string("local sockets are not available on this platform"));
}

void removeLocalSocket(std::string_view)
{
    // Nothing was ever bound, so there is no name to remove.
}

} // namespace PAL
} // namespace Ubiquitty
