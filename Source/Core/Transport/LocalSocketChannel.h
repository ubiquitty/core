// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// A Channel over an AF_UNIX socket: the same-box daemon path (connect to
// ~/.ubiquitty/host.sock, no ssh). Raw bytes ride one duplex socket fd; poll loop in PollChannel.

#include "Core/Transport/PollChannel.h"

#include <string>

namespace Ubiquitty::Transport {

class LocalSocketChannel final : public PollChannel {
public:
    // SECURITY: `socketPath` must be app-trusted (the fixed ~/.ubiquitty/host.sock).
    // An attacker-chosen path could point at a rogue daemon (confused deputy). A
    // non-default path should be confined to the control dir and verified via SO_PEERCRED.
    /// Creates a carrier over an existing local socket.
    ///
    /// - Parameters:
    ///   - socketPath: The socket to connect to.
    ///   - origin: Where this carrier reaches; returned by `origin()`.
    LocalSocketChannel(std::string socketPath, Transport::Origin);
    ~LocalSocketChannel() override;

    std::string description() const override { return "local socket " + m_path; }

protected:
    bool openTransport(std::unique_ptr<PAL::PollableByteStream>& readStream,
        std::unique_ptr<PAL::PollableByteStream>& writeStream, std::string& error) override;

private:
    std::string m_path;
};

} // namespace Ubiquitty::Transport
