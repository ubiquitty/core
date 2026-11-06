// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
//
// Carries `ubq -H [user@]host` to a daemon on another machine over the same
// ssh bootstrap the app's remote windows use: Session::remoteTransportPlan
// names the command, Transport::SubprocessChannel runs it, and
// Session::BootstrapResponse separates the remote shell's startup text from
// the Sync stream. The client speaks to one end of a socket pair as if it were
// the local daemon's socket, so every verb works unchanged over either.

#include "Core/Session/BootstrapResponse.h"
#include "Core/Transport/Channel.h"
#include "PAL/UnixFileDescriptor.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace Ubiquitty::Transport {
class SubprocessChannel;
}

namespace Ubiquitty::Agent {

/// A Sync carrier to a remote host's daemon, presented as a local descriptor.
class RemoteCarrier final : public Transport::ChannelObserver {
public:
    /// Creates a carrier that has not started.
    RemoteCarrier();
    /// Stops ssh and the thread forwarding what the client writes.
    ~RemoteCarrier() override;

    RemoteCarrier(const RemoteCarrier&) = delete;
    RemoteCarrier& operator=(const RemoteCarrier&) = delete;

    /// Starts ssh to the host and returns the descriptor the client speaks Sync on.
    ///
    /// - Parameters:
    ///   - host: A `[user@]host` the caller has validated with
    ///     `Transport::validateSSHHost`.
    ///   - error: Set to a sentence when the carrier cannot start.
    /// - Returns: The client's end of the carrier, or an empty descriptor.
    [[nodiscard]] PAL::UnixFileDescriptor open(const std::string& host, std::string& error);

    /// Returns why the carrier closed, or empty while it is open.
    [[nodiscard]] std::string closeReason() const;

private:
    /// Ignores the carrier opening: the client writes its HELLO to the pair
    /// whether or not ssh has connected yet, and the channel queues it.
    void onConnect() override { }
    /// Passes the remote's bytes to the client once the shell's startup text
    /// has ended at the Sync boundary.
    ///
    /// - Parameter data: The next chunk from ssh.
    void onData(std::span<const std::uint8_t> data) override;
    /// Ends the client's stream with the reason ssh closed.
    ///
    /// - Parameter reason: A sentence for the person.
    void onClose(const std::string& reason) override;
    /// Copies what the client writes to ssh until either end closes.
    void forwardOutbound();

    std::unique_ptr<Transport::SubprocessChannel> m_channel;
    // The carrier's end of the pair: what the remote sends is written here,
    // what the client writes is read from here.
    PAL::UnixFileDescriptor m_carrierEnd;
    Session::BootstrapResponse m_bootstrap;
    std::thread m_outbound;
    std::atomic<bool> m_isClosed { false };
    mutable std::mutex m_closeReasonLock;
    std::string m_closeReason;
};

} // namespace Ubiquitty::Agent
