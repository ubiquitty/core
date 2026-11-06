// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Socket.cpp — PAL/Socket.h under WebAssembly.
//
// A page reaches a peer through the embedder's transport, not through a name in
// a filesystem, so there is nothing to connect to and the capability reports
// itself absent rather than failing one call at a time.

#include "PAL/Socket.h"

namespace Ubiquitty {
namespace PAL {

std::unique_ptr<PollableByteStream> connectLocal(std::string_view, LocalSocketError* outError)
{
    if (outError)
        *outError = LocalSocketError::NotSupported;
    return nullptr;
}

bool localSocketsAvailable() { return false; }

std::optional<PeerIdentity> peerIdentity(const FileDescriptor&)
{
    // No kernel to ask, so no peer to report — and the contract says a caller
    // deciding access must read that as a refusal rather than as permission.
    return std::nullopt;
}

bool peerIsSameUser(const FileDescriptor&) { return false; }

std::unique_ptr<PollableByteStream> adoptByteStream(int, ReadinessWaitStrategy)
{
    // A page holds no descriptor to adopt; the embedder owns the transport and
    // hands bytes across, so there is nothing here to take ownership of.
    return nullptr;
}

const char* describeLocalSocketError(LocalSocketError reason)
{
    // The one reason this target can produce, plus the no-error case: every
    // other enumerator names a way a real connection attempt failed, and none
    // is ever attempted here.
    return reason == LocalSocketError::None ? "no error"
                                            : "local sockets are not available on this platform";
}

SocketReadOutcome readSocket(int, std::span<char>, std::size_t*)
{
    return SocketReadOutcome::Failed;
}

bool writeSocketAll(int, std::span<const char>)
{
    return false;
}

void shutdownSocket(int)
{
}

SocketAcceptOutcome acceptConnection(const UnixFileDescriptor&, UnixFileDescriptor*)
{
    // Nothing can be listening, so nothing can arrive.
    return SocketAcceptOutcome::Unusable;
}

bool setReceiveTimeoutSeconds(int, int)
{
    return false;
}

bool setSendTimeoutSeconds(int, int)
{
    return false;
}

} // namespace PAL
} // namespace Ubiquitty
