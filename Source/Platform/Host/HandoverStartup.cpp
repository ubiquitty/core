// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#include "Platform/Host/HandoverStartup.h"

#include "Core/Protocol/Sync/HandoverBody.h"
#include "PAL/Log.h"
#include "Platform/Host/CodeSignature.h"
#include "Platform/Host/Daemon.h"
#include "Platform/Host/HostHandover.h"
#include "Platform/Host/HostPaths.h"
#include "Platform/POSIX/HandoverTransport.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <poll.h>
#include <ranges>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

#if defined(__APPLE__)
#include <libproc.h>
#elif defined(__linux__)
#include <asm/socket.h>
#endif

namespace Ubiquitty::Host {
namespace {

using Clock = std::chrono::steady_clock;
using Refusal = Protocol::Sync::HandoverRefuseReason;

int remainingMilliseconds(Clock::time_point deadline)
{
    const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now()).count();
    return static_cast<int>(std::clamp<std::int64_t>(remaining, 0, std::numeric_limits<int>::max()));
}

bool waitWritable(int descriptor, Clock::time_point deadline)
{
    for (;;) {
        const int remaining = remainingMilliseconds(deadline);
        if (!remaining)
            return false;
        pollfd item { descriptor, POLLOUT, 0 };
        const int result = ::poll(&item, 1, remaining);
        if (result > 0)
            return item.revents & POLLOUT;
        if (result < 0 && errno == EINTR)
            continue;
        return false;
    }
}

struct PredecessorChannel {
    PAL::UnixFileDescriptor channel;
    PAL::UnixFileDescriptor lifetime;
    std::uint32_t processID = 0;
};

std::optional<PredecessorChannel> connectPredecessor(const std::string& path, Clock::time_point deadline)
{
    sockaddr_un address {};
    if (path.empty() || path.front() != '/' || path.size() >= sizeof(address.sun_path) || path.find('\0') != std::string::npos)
        return std::nullopt;
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    PredecessorChannel peer;
    peer.channel.reset(::socket(AF_UNIX, SOCK_STREAM, 0));
    if (!peer.channel || ::fcntl(peer.channel.get(), F_SETFD, FD_CLOEXEC) != 0
        || ::fcntl(peer.channel.get(), F_SETFL, O_NONBLOCK) != 0)
        return std::nullopt;
#if defined(SO_NOSIGPIPE)
    const int suppress = 1;
    if (::setsockopt(peer.channel.get(), SOL_SOCKET, SO_NOSIGPIPE, &suppress, sizeof(suppress)) != 0)
        return std::nullopt;
#endif
    if (::connect(peer.channel.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        if ((errno != EINPROGRESS && errno != EAGAIN) || !waitWritable(peer.channel.get(), deadline))
            return std::nullopt;
        int error = 0;
        socklen_t length = sizeof(error);
        if (::getsockopt(peer.channel.get(), SOL_SOCKET, SO_ERROR, &error, &length) != 0 || length != sizeof(error) || error)
            return std::nullopt;
    }
    if (!handoverPeerHasSameUser(peer.channel.get()))
        return std::nullopt;
#if defined(__APPLE__)
    pid_t processID = 0;
    socklen_t length = sizeof(processID);
    audit_token_t token {};
    socklen_t tokenLength = sizeof(token);
    std::array<char, PROC_PIDPATHINFO_MAXSIZE> executable {};
    if (::getsockopt(peer.channel.get(), SOL_LOCAL, LOCAL_PEERPID, &processID, &length) != 0
        || length != sizeof(processID) || processID <= 0 || processID == ::getpid()
        || ::getsockopt(peer.channel.get(), SOL_LOCAL, LOCAL_PEERTOKEN, &token, &tokenLength) != 0
        || tokenLength != sizeof(token) || ::proc_pidpath_audittoken(&token, executable.data(), executable.size()) <= 0)
        return std::nullopt;
    peer.processID = static_cast<std::uint32_t>(processID);
#elif defined(__linux__)
    struct ucred credentials {};
    socklen_t length = sizeof(credentials);
    int lifetime = -1;
    socklen_t lifetimeLength = sizeof(lifetime);
    const int lifetimeResult = ::getsockopt(peer.channel.get(), SOL_SOCKET, SO_PEERPIDFD, &lifetime, &lifetimeLength);
    peer.lifetime.reset(lifetime);
    pollfd alive { peer.lifetime.get(), POLLIN, 0 };
    if (::getsockopt(peer.channel.get(), SOL_SOCKET, SO_PEERCRED, &credentials, &length) != 0
        || length != sizeof(credentials) || credentials.pid <= 0 || credentials.pid == ::getpid()
        || lifetimeResult != 0 || lifetimeLength != sizeof(lifetime) || !peer.lifetime
        || ::fcntl(peer.lifetime.get(), F_SETFD, FD_CLOEXEC) != 0 || ::poll(&alive, 1, 0) != 0)
        return std::nullopt;
    peer.processID = static_cast<std::uint32_t>(credentials.pid);
#else
    return std::nullopt;
#endif
    return peer;
}

bool sendPreamble(int channel, Clock::time_point deadline)
{
    std::span<const std::uint8_t> remaining = Protocol::Sync::handoverPreamble;
    while (!remaining.empty()) {
        if (!remainingMilliseconds(deadline))
            return false;
#ifdef MSG_NOSIGNAL
        constexpr int flags = MSG_DONTWAIT | MSG_NOSIGNAL;
#else
        constexpr int flags = MSG_DONTWAIT;
#endif
        const auto sent = ::send(channel, remaining.data(), remaining.size(), flags);
        if (sent > 0) {
            remaining = remaining.subspan(static_cast<std::size_t>(sent));
            continue;
        }
        if (sent < 0 && errno == EINTR)
            continue;
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && waitWritable(channel, deadline))
            continue;
        return false;
    }
    return true;
}

void refuseRequest(int channel, Refusal reason)
{
    // A refusal is six bytes and grants nothing. Best effort only: a blocked or
    // partially written response is abandoned with this exclusive channel.
    const auto body = Protocol::Sync::encodeHandoverMessage(Protocol::Sync::HandoverRefuse { reason });
    (void)POSIX::sendHandoverRecord(channel, body, {}, 1);
}

} // namespace

void HandoverStartup::initialize(Daemon& daemon, std::string_view installPath)
{
    if (daemon.m_handoverInstallPath.empty() && !installPath.empty() && installPath.front() == '/' && installPath.find('\0') == std::string_view::npos)
        daemon.m_handoverInstallPath = pathWithoutDeletedMarker(std::string(installPath));
    // Read once, before anything can replace the file this daemon runs from:
    // the successors it admits by signature are judged against it.
    daemon.m_handoverSignature = signatureOfSelf();
}

bool HandoverStartup::route(Daemon& daemon, std::size_t clientIndex, std::uint64_t nowMilliseconds, std::uint64_t deadlineMilliseconds)
{
    if (clientIndex >= daemon.m_clients.size())
        return false;
    auto& client = daemon.m_clients[clientIndex];
    if (client.helloSeen || client.quicConnection || !client.fileDescriptor)
        return true;
    const auto state = client.handoverRequest.receive(client.fileDescriptor.get(), nowMilliseconds, deadlineMilliseconds);
    if (state == HandoverRequestState::Ordinary)
        return true;
    if (state == HandoverRequestState::Awaiting)
        return false;
    auto request = client.handoverRequest.take();
    const auto slash = daemon.m_socketPath.find_last_of('/');
    const std::string directory = slash == std::string::npos ? std::string {} : daemon.m_socketPath.substr(0, slash);
    // No peer state can retarget this slot after its channel moves away.
    auto channel = std::move(client.fileDescriptor);
    client.didOverflow = true;
    if (!request) {
        refuseRequest(channel.get(), Refusal::Malformed);
        return false;
    }
    auto admitted = admitHandoverRequest(channel.get(), std::move(request->listener), request->request,
        { .isFirstRecord = true, .installPath = daemon.m_handoverInstallPath, .controlDirectory = directory, .holderSignature = daemon.m_handoverSignature });
    if (!admitted) {
        refuseRequest(channel.get(), admitted.error());
        return false;
    }
    // One successor at a time: a second asks again once the first's
    // transaction has ended.
    if (daemon.m_pendingHandover) {
        refuseRequest(channel.get(), Refusal::RequestPending);
        return false;
    }
    daemon.m_pendingHandover.emplace(Daemon::PendingHandover { std::move(channel), std::move(*admitted), nowMilliseconds,
        nowMilliseconds + HostHandover::maxOwedOutputWaitMilliseconds });
    return false;
}

void HandoverStartup::servicePending(Daemon& daemon, std::uint64_t nowMilliseconds)
{
    if (!daemon.m_pendingHandover)
        return;
    const auto blocker = daemon.handoverBlocker();
    if (blocker && nowMilliseconds < daemon.m_pendingHandover->deadlineMilliseconds)
        return;
    auto pending = std::move(*daemon.m_pendingHandover);
    daemon.m_pendingHandover.reset();
    if (blocker)
        PAL::logLine(stderr, "ubiquitty-host: the handover to process {} waited {} ms for this host to owe its clients nothing", pending.admitted.peerProcessID,
            nowMilliseconds - pending.startedMilliseconds);
    // A blocker still standing is the transaction's refusal.
    (void)HostHandover::offer(daemon, std::move(pending.channel), std::move(pending.admitted));
}

HandoverStartupReport HandoverStartup::takeOverSiblings(Daemon& daemon, int timeoutMilliseconds)
{
    HandoverStartupReport report;
    if (timeoutMilliseconds <= 0 || !daemon.m_clients.empty() || !daemon.m_sessions.empty() || !daemon.m_listenFileDescriptor
        || daemon.m_handoverInstallPath.empty() || daemon.m_hostInstanceID == Protocol::Sync::ID128 {})
        return report;
    // Every socket the walk reads, so the oldest, beside which a chain keeps
    // its records, is never the one cut off.
    std::vector<std::string> paths;
    eachHostSocketNewestFirst([](const char* path, void* context) {
        static_cast<std::vector<std::string>*>(context)->emplace_back(path);
        return false;
    },
        &paths);
    const auto deadline = Clock::now() + std::chrono::milliseconds(std::min(timeoutMilliseconds, HostHandover::transactionTimeoutMilliseconds));
    // A host serves several of these paths after handovers of its own. It is
    // asked once, at the oldest: the one its records are kept beside. A path
    // this daemon took over is its own.
    std::vector<std::uint32_t> asked;
    const auto isServedHere = [&](const std::string& path) {
        return path == daemon.m_socketPath
            || std::ranges::any_of(daemon.m_inheritedListeners, [&](const auto& listener) { return listener.path == path; });
    };
    for (const auto& path : paths | std::views::reverse) {
        if (!remainingMilliseconds(deadline))
            break;
        if (isServedHere(path))
            continue;
        auto predecessor = connectPredecessor(path, deadline);
        if (!predecessor || std::ranges::find(asked, predecessor->processID) != asked.end())
            continue;
        asked.push_back(predecessor->processID);
        if (!sendPreamble(predecessor->channel.get(), deadline)) {
            ++report.refusedPredecessors;
            continue;
        }
        const auto request = Protocol::Sync::encodeHandoverMessage(Protocol::Sync::HandoverRequest {
            Protocol::Sync::handoverBodyVersion, daemon.m_hostInstanceID });
        const std::array listener { daemon.m_listenFileDescriptor.get() };
        const int remaining = remainingMilliseconds(deadline);
        if (!remaining || !POSIX::sendHandoverRecord(predecessor->channel.get(), request, listener, remaining)) {
            ++report.refusedPredecessors;
            continue;
        }
        const auto inherited = HostHandover::inherit(daemon, std::move(predecessor->channel), path, predecessor->processID, deadline);
        if (inherited)
            report.movedSessions += *inherited;
        else
            ++report.refusedPredecessors;
    }
    return report;
}

} // namespace Ubiquitty::Host
