// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// Source/Agent/RemoteCarrier.cpp — see RemoteCarrier.h.

#include "Agent/RemoteCarrier.h"

#include "Core/Session/RemoteBootstrap.h"
#include "Core/Transport/SSHHost.h"
#include "Core/Transport/SubprocessChannel.h"

#include <array>
#include <cerrno>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace Ubiquitty::Agent {

namespace {

// Writes all of a chunk to a blocking descriptor; false once the peer is gone.
bool writeAll(int descriptor, std::span<const std::uint8_t> bytes)
{
    while (!bytes.empty()) {
        const ssize_t written = ::write(descriptor, bytes.data(), bytes.size());
        if (written > 0) {
            bytes = bytes.subspan(static_cast<std::size_t>(written));
            continue;
        }
        if (written < 0 && errno == EINTR)
            continue;
        return false;
    }
    return true;
}

} // namespace

RemoteCarrier::RemoteCarrier() = default;

RemoteCarrier::~RemoteCarrier()
{
    m_isClosed = true;
    // Stopping the channel joins its I/O thread, so no callback writes into
    // the pair after this; shutting the pair down wakes the outbound reader.
    if (m_channel)
        m_channel->stop();
    if (m_carrierEnd)
        ::shutdown(m_carrierEnd.get(), SHUT_RDWR);
    if (m_outbound.joinable())
        m_outbound.join();
}

PAL::UnixFileDescriptor RemoteCarrier::open(const std::string& host, std::string& error)
{
    if (!Transport::validateSSHHost(host)) {
        error = "'" + host + "' is not a host ssh can be given";
        return {};
    }
    std::array<int, 2> pair { -1, -1 };
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, pair.data()) != 0) {
        error = "could not create the carrier's socket pair";
        return {};
    }
    m_carrierEnd = PAL::UnixFileDescriptor(pair[0]);
    PAL::UnixFileDescriptor clientEnd(pair[1]);
    m_channel = std::make_unique<Transport::SubprocessChannel>(Session::remoteTransportPlan(host).shellCommand,
        Transport::Origin { Transport::OriginKind::Daemon, host });
    if (!m_channel->start(this)) {
        error = "could not run ssh";
        return {};
    }
    m_outbound = std::thread([this] { forwardOutbound(); });
    return clientEnd;
}

std::string RemoteCarrier::closeReason() const
{
    std::lock_guard lock(m_closeReasonLock);
    return m_closeReason;
}

void RemoteCarrier::onData(std::span<const std::uint8_t> data)
{
    const auto result = m_bootstrap.consume(data);
    switch (result.status) {
    case Session::BootstrapResponse::Status::Ready:
        if (!result.protocolBytes.empty() && !writeAll(m_carrierEnd.get(), result.protocolBytes))
            ::shutdown(m_carrierEnd.get(), SHUT_RDWR);
        return;
    case Session::BootstrapResponse::Status::Waiting:
        return;
    case Session::BootstrapResponse::Status::Deploy:
        onClose("the host has no ubiquitty-host in ~/.ubiquitty for " + m_bootstrap.platform() + "; install it there first");
        return;
    case Session::BootstrapResponse::Status::InstanceGone:
        onClose("the host's daemon on " + m_bootstrap.goneSocketName() + " is no longer running");
        return;
    case Session::BootstrapResponse::Status::Invalid:
        onClose("the host's shell printed something other than the Sync stream");
        return;
    }
}

void RemoteCarrier::onClose(const std::string& reason)
{
    {
        std::lock_guard lock(m_closeReasonLock);
        if (m_closeReason.empty())
            m_closeReason = reason.empty() ? std::string("ssh closed") : reason;
    }
    m_isClosed = true;
    // The client sees end of file on its end, the same as a local daemon
    // hanging up.
    ::shutdown(m_carrierEnd.get(), SHUT_RDWR);
}

void RemoteCarrier::forwardOutbound()
{
    std::array<std::uint8_t, 16384> buffer {};
    while (!m_isClosed) {
        const ssize_t got = ::read(m_carrierEnd.get(), buffer.data(), buffer.size());
        if (got > 0) {
            m_channel->send({ buffer.data(), static_cast<std::size_t>(got) });
            continue;
        }
        if (got < 0 && errno == EINTR)
            continue;
        return;
    }
}

} // namespace Ubiquitty::Agent
