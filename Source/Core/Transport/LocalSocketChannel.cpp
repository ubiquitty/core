// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// LocalSocketChannel.cpp — see LocalSocketChannel.h.

#include "Core/Transport/LocalSocketChannel.h"

#include "PAL/Socket.h"

namespace Ubiquitty::Transport {

LocalSocketChannel::LocalSocketChannel(std::string socketPath, Transport::Origin origin)
    : PollChannel(std::move(origin))
    , m_path(std::move(socketPath))
{
}

LocalSocketChannel::~LocalSocketChannel() { stop(); }

bool LocalSocketChannel::openTransport(std::unique_ptr<PAL::PollableByteStream>& readStream,
    std::unique_ptr<PAL::PollableByteStream>& writeStream, std::string& error)
{
    if (m_path.empty()) {
        error = "empty socket path";
        return false;
    }
    // The path length ceiling, the address family and the blocking-connect
    // reasoning all live in the PAL backend, so no POSIX handle type reaches a
    // Core interface.
    PAL::LocalSocketError reason = PAL::LocalSocketError::None;
    readStream = PAL::connectLocal(m_path, &reason);
    if (!readStream) {
        error = std::string("connect(") + m_path + ") failed: " + PAL::describeLocalSocketError(reason);
        return false;
    }
    // One endpoint, two owners: the base closes each direction exactly once.
    writeStream = readStream->duplicateAsPollableStream();
    if (!writeStream) {
        error = std::string("duplicating the socket for writing failed");
        readStream.reset();
        return false;
    }
    return true;
}

} // namespace Ubiquitty::Transport
