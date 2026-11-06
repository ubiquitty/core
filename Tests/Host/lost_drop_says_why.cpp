// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Host/lost_drop_says_why.cpp — a connection the host drops as lost is
// told why.
//
// A connection whose bytes stay unacknowledged for the silence bound is Lost,
// and the host drops it. A path that is really gone delivers nothing, but a
// client that still reads, slowed rather than cut, would otherwise see a bare
// EOF and report that the host stopped answering. The host appends one
// Bye{Silent} on the way out and reaps the client once it leaves or a bounded
// wait ends.
//
// Friend seam, same QUIC-OFF host build as backlog_drop_says_why: the client
// reads what the host sends, but its acknowledgements never reach the host,
// and the host's clock is advanced past the bound by hand. Exit 0 on success.
//

#include "Assertions.h"

#include "Core/Protocol/Sync/Envelope.h"
#include "Core/TerminalOperations.h"
#include "Host/ScreenClient.h"
#include "Host/ScreenPlaneAccess.h"
#include "PAL/UnixFileDescriptor.h"
#include "Platform/Host/Daemon.h"

#include <csignal>
#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <sys/socket.h>

namespace Ubiquitty::Host {

struct LostDropAccess {
    static bool isReapDue(Daemon& d, std::size_t index, std::uint64_t now) { return d.isReapDue(d.m_clients[index], now); }
};

} // namespace Ubiquitty::Host

using A = Ubiquitty::Host::LostDropAccess;
using Ubiquitty::Host::Daemon;
using Screen = Ubiquitty::Host::ScreenPlaneAccess;
namespace Sync = Ubiquitty::Protocol::Sync;

namespace {

void run()
{
    int pair[2] = { -1, -1 };
    REQUIRE_MSG(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0, "no socketpair for the client");
    ::fcntl(pair[0], F_SETFL, ::fcntl(pair[0], F_GETFL) | O_NONBLOCK);
    const Ubiquitty::PAL::UnixFileDescriptor peer(pair[1]);

    constexpr int rows = 4, columns = 20;
    Daemon d;
    auto& session = Screen::makeSession(d, rows, columns);
    const std::size_t index = Screen::attach(d, session.id, Ubiquitty::PAL::UnixFileDescriptor(pair[0]));
    Screen::markTrustedLocal(d, index);
    UbiquittyTest::ScreenClient client(rows, columns);
    Screen::greet(d, index, client);
    session.isFrameDue = true;
    Screen::publish(d, session);
    Screen::deliver(d, index, client);
    REQUIRE_MSG(client.connection().state() == Sync::ConnectionState::Attached, "the client never attached");

    // New output the client reads and never acknowledges to the host.
    Ubiquitty::processBytes(session.term.get(), "unacknowledged");
    session.isFrameDue = true;
    Screen::publish(d, session);
    const std::uint64_t sentAt = Screen::now();
    Screen::service(d, sentAt);
    client.read(Screen::takeOutput(d, index), sentAt);
    (void)client.takeStream(sentAt);
    REQUIRE_MSG(!Screen::didOverflow(d, index), "the client was dropped before the silence bound");

    // Past the bound with nothing heard: Lost, dropped, and told why.
    const std::uint64_t pastBound = sentAt + 31'000'000;
    Screen::service(d, pastBound);
    CHECK_MSG(Screen::didOverflow(d, index), "a connection silent past the bound was not dropped");
    // The Bye's drain wait runs on the daemon's own clock.
    CHECK_MSG(!A::isReapDue(d, index, Screen::now() / 1000), "the client was reaped before its Bye could leave");
    client.read(Screen::takeOutput(d, index), pastBound);
    CHECK_MSG(client.connection().state() == Sync::ConnectionState::Closed, "the client was not closed by the host");
    const auto& bye = client.connection().bye();
    CHECK_MSG(bye.has_value(), "the client was dropped as lost without a Bye");
    CHECK_MSG(bye && bye->reason == Sync::ByeReason::Silent, "the Bye did not name the silence");
}

} // namespace

int main()
{
    ::signal(SIGPIPE, SIG_IGN);
    run();
    if (failures == 0)
        std::printf("lost_drop_says_why: a connection silent past the bound is dropped with Bye{Silent} that a client still reading receives\n");
    return failures == 0 ? 0 : 1;
}
