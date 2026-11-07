// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0
#include "HostConnection.h"
#include "Core/Session/RemoteBootstrap.h"
#include "Core/Transport/SubprocessChannel.h"

namespace UbiquittyKit {
namespace {
std::string shellQuote(std::string_view text)
{
    std::string result = "'";
    for (char byte : text)
        result += byte == '\'' ? "'\\''" : std::string(1, byte);
    return result + '\'';
}

class HostChannelFactory final : public Ubiquitty::Session::ChannelFactory {
public:
    explicit HostChannelFactory(HostConnectionOptions options)
        : m_options(std::move(options))
    {
    }
    void createChannel(Ubiquitty::Session::TransportMode, Done done) override
    {
        const auto& socket = m_pinnedSocket.empty() ? m_options.socketName : m_pinnedSocket;
        const auto binary = shellQuote(m_options.hostBinary);
        std::string command;
        if (socket.empty())
            command = binary + " --ensure-daemon >/dev/null && ";
        command += "exec " + binary + " --attach";
        if (!socket.empty())
            command += " --socket " + shellQuote(socket);
        if (!m_options.sshDestination.empty())
            command = "exec ssh -T -o BatchMode=yes -- " + shellQuote(m_options.sshDestination) + " " + shellQuote(command);
        done(std::make_unique<Ubiquitty::Transport::SubprocessChannel>(command,
                 Ubiquitty::Transport::Origin { Ubiquitty::Transport::OriginKind::Daemon, m_options.sshDestination.empty() ? "local" : m_options.sshDestination }),
            false);
    }
    bool pinDaemonSocket(std::string_view socket) override
    {
        if (!Ubiquitty::Session::isDaemonSocketName(socket))
            return false;
        if ((!m_pinnedSocket.empty() && m_pinnedSocket != socket)
            || (!m_options.socketName.empty() && m_options.socketName != socket))
            return false;
        m_pinnedSocket = socket;
        return true;
    }

private:
    HostConnectionOptions m_options;
    std::string m_pinnedSocket;
};
}

std::expected<std::unique_ptr<Ubiquitty::Session::ChannelFactory>, std::string> makeHostChannelFactory(HostConnectionOptions options)
{
    if (options.hostBinary.empty() || options.hostBinary.size() > 4096 || options.hostBinary.find('\0') != std::string::npos)
        return std::unexpected("Host executable must be a nonempty path without NUL bytes");
    if (options.sshDestination.size() > 4096 || options.sshDestination.starts_with('-') || options.sshDestination.find('\0') != std::string::npos)
        return std::unexpected("SSH destination cannot start with '-' or contain NUL bytes");
    if (!options.socketName.empty() && !Ubiquitty::Session::isDaemonSocketName(options.socketName))
        return std::unexpected("Daemon socket must be a host socket leaf");
    return std::make_unique<HostChannelFactory>(std::move(options));
}
} // namespace UbiquittyKit
