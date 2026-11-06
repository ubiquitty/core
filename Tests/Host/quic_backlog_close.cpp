// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Host/quic_backlog_close.cpp — a QUIC client dropped for falling behind
// the host's output reads why from the connection's close.
//
// A Bye on a QUIC stream would wait behind the very backlog the client is
// being dropped for, and the reap frees the connection with that stream, so
// the host carries the reason in an application CONNECTION_CLOSE instead.
// This drives that over real loopback QUIC: an in-process daemon with a live
// listener, pumped by the test, and the production QUIC client on its own I/O
// thread, presenting a token the daemon minted. The client is pushed past its
// high-water mark and reaped, and the production client must report the close
// with the code that names Bye{OutputBacklog}.
//
// Friend seam, QUIC host build. Exit 0 on success.
//

#include "Core/Protocol/Sync/Messages.h"
#include "Core/Protocol/Sync/NativeAttachment.h"
#include "Core/Transport/QUICClient.h"
#include "Platform/Host/Daemon.h"
#include "Platform/Host/HostQUIC.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <csignal>
#include <ctime>
#include <unistd.h>

namespace Ubiquitty::Host {

struct QUICBacklogCloseAccess {
    static bool startQUIC(Daemon& d) { return d.startQUIC(0); }
    static std::uint16_t port(const Daemon& d) { return d.m_quic->boundPort(); }
    static std::string pin(const Daemon& d) { return d.m_quic->certificateSHA256(); }
    static std::string mint(Daemon& d, const std::string& tokenPath)
    {
        d.m_tokenPath = tokenPath;
        return d.mintQUICToken(0, { Protocol::Sync::Scope::Observe });
    }
    /// One pass of the run loop's QUIC work: read, timers, write.
    static void pump(Daemon& d)
    {
        d.m_quic->processReadable();
        d.m_quic->serviceTimers();
        d.m_quic->flush();
    }
    /// The QUIC client whose token preamble the daemon accepted.
    static std::optional<std::size_t> authenticatedClient(const Daemon& d)
    {
        for (std::size_t index = 0; index < d.m_clients.size(); ++index) {
            if (d.m_clients[index].quicConnection && d.m_clients[index].quicAuthed && !d.m_clients[index].didOverflow)
                return index;
        }
        return std::nullopt;
    }
    static void queue(Daemon& d, std::size_t index, std::span<const std::uint8_t> bytes) { d.queueToClient(index, bytes); }
    static bool didOverflow(const Daemon& d, std::size_t index) { return d.m_clients[index].didOverflow; }
    static bool isReapDue(Daemon& d, std::size_t index)
    {
        struct timespec now;
        ::clock_gettime(CLOCK_MONOTONIC, &now);
        const std::uint64_t milliseconds = static_cast<std::uint64_t>(now.tv_sec) * 1000u + static_cast<std::uint64_t>(now.tv_nsec) / 1'000'000u;
        return d.isReapDue(d.m_clients[index], milliseconds);
    }
    static void reap(Daemon& d, std::size_t index) { d.reapClient(index); }
    static std::size_t clientCount(const Daemon& d) { return d.m_clients.size(); }
};

} // namespace Ubiquitty::Host

using Ubiquitty::Host::Daemon;
using A = Ubiquitty::Host::QUICBacklogCloseAccess;
namespace Sync = Ubiquitty::Protocol::Sync;
namespace Transport = Ubiquitty::Transport;

namespace {

int failures = 0;

void check(bool ok, const char* message) // bool-ok: established Tests/Host check pattern
{
    if (!ok) {
        std::fprintf(stderr, "quic_backlog_close: FAIL — %s\n", message);
        ++failures;
    }
}

/// Pumps the daemon until a condition holds or a bound passes.
template<typename Condition>
bool pumpUntil(Daemon& d, Condition condition)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        A::pump(d);
        if (condition())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

} // namespace

int main()
{
    ::signal(SIGPIPE, SIG_IGN);
    char home[] = "/tmp/ubq-quic-backlog-XXXXXX";
    if (!::mkdtemp(home)) {
        std::fprintf(stderr, "quic_backlog_close: FAIL — no temporary home\n");
        return 1;
    }
    ::setenv("UBIQUITTY_HOME", home, 1);

    int status = 1;
    {
        Daemon d;
        if (!A::startQUIC(d)) {
            std::fprintf(stderr, "quic_backlog_close: FAIL — the QUIC listener did not start\n");
            return 1;
        }
        const std::string token = A::mint(d, std::string(home) + "/tokens");
        check(!token.empty(), "the daemon minted a token");

        Transport::QUICClient carrier;
        std::atomic<bool> hasConnected { false };
        std::atomic<bool> hasClosed { false };
        std::atomic<Transport::Health> closeHealth { Transport::Health::Connecting };
        Transport::QUICClient::Callbacks callbacks;
        callbacks.onConnect = [&] { hasConnected = true; };
        callbacks.onData = [](std::span<const std::uint8_t>) { };
        callbacks.onDatagram = [](std::span<const std::uint8_t>) { };
        callbacks.onClose = [&](Transport::Health health, const std::string&) {
            closeHealth = health;
            hasClosed = true;
        };
        const auto started = carrier.start("127.0.0.1", A::port(d), Transport::BootstrapToken { token }, A::pin(d), std::move(callbacks));
        check(started.has_value(), "the production QUIC client started");

        std::optional<std::size_t> index;
        check(pumpUntil(d, [&] { return hasConnected.load() && (index = A::authenticatedClient(d)).has_value(); }),
            "the client connected and its token preamble was accepted");
        if (index) {
            // Past the high-water mark in one frame the mark cannot take.
            const std::vector<std::uint8_t> flood(Sync::maxPayloadLength + (1u << 20), 0u);
            A::queue(d, *index, flood);
            check(A::didOverflow(d, *index), "the client over its mark was marked for the drop");
            check(A::isReapDue(d, *index), "a QUIC client owes no stream Bye, so its reap is not held");
            A::reap(d, *index);
            check(A::clientCount(d) == 0, "the reap erased the client");
            check(pumpUntil(d, [&] { return hasClosed.load(); }), "the client never learnt the connection closed");
            check(closeHealth.load() == Transport::Health::Closed, "the close read as a failure, not the host's close");
            const std::optional<std::uint64_t> code = carrier.peerApplicationCloseCode();
            check(code.has_value(), "the close carried no application error code");
            check(code && Sync::byeReasonOfQUICClose(*code) == std::optional { Sync::ByeReason::OutputBacklog },
                "the close's code did not name Bye{OutputBacklog}");
        }
        carrier.stop();
        status = failures == 0 ? 0 : 1;
    }
    // The verdict is already taken: a home left under /tmp changes nothing it
    // says, so the removal's status is not this run's to check.
    const std::string cleanup = std::string("rm -rf '") + home + "'";
    [[maybe_unused]] const int removalStatus = std::system(cleanup.c_str());

    if (status == 0)
        std::printf("quic_backlog_close: a QUIC client pushed past its high-water mark is reaped at once, and the production client reads Bye{OutputBacklog} from the connection's application close\n");
    return status;
}
