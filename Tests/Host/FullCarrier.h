// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "PAL/UnixFileDescriptor.h"

#include <array>
#include <cerrno>
#include <cstddef>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

namespace Ubiquitty::HostTest {

/// A local carrier whose reader never reads, for a screen-plane fixture
/// client: the daemon's end is non-blocking and its send buffer is already
/// full, so every write the daemon makes would block and the frames stay in
/// the client's output for `ScreenPlaneAccess::deliver` to hand over.
///
/// A fixture client with no descriptor at all is a write error, which the
/// daemon answers by dropping the client the first time anything flushes it
/// outside a service pass: a greet-time seed, a refusal, a teardown. A test
/// that holds a client attached across those gives it this carrier.
///
/// A process that closes the reader end before its daemon writes again must
/// ignore SIGPIPE, as the daemon's own process does.
class FullCarrier {
public:
    /// Makes the pair and fills the daemon's end; `isReady` says whether it worked.
    FullCarrier()
    {
        std::array<int, 2> ends { -1, -1 };
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, ends.data()) != 0)
            return;
        PAL::UnixFileDescriptor daemonEnd(ends[0]);
        PAL::UnixFileDescriptor readerEnd(ends[1]);
        if (::fcntl(daemonEnd.get(), F_SETFL, O_NONBLOCK) != 0)
            return;
        const std::array<char, 4096> filler {};
        // A smaller write can still fit; the buffer is full only when one
        // byte does not.
        for (std::size_t length = filler.size(); length;) {
            if (::write(daemonEnd.get(), filler.data(), length) >= 0)
                continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                return;
            length = length == 1 ? 0 : 1;
        }
        m_daemonEnd = std::move(daemonEnd);
        m_readerEnd = std::move(readerEnd);
    }

    /// Whether both ends exist and the daemon's end is full.
    [[nodiscard]] bool isReady() const { return static_cast<bool>(m_readerEnd); }

    /// Hands the daemon its end; the reader end stays owned here.
    /// - Returns: The descriptor to attach the fixture client with.
    [[nodiscard]] PAL::UnixFileDescriptor takeDaemonEnd() { return std::move(m_daemonEnd); }

private:
    PAL::UnixFileDescriptor m_daemonEnd;
    PAL::UnixFileDescriptor m_readerEnd;
};

} // namespace Ubiquitty::HostTest
