// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

//
// Tests/Host/backlog_drop_says_why.cpp — a client dropped for falling behind
// the host's output is told why.
//
// A client whose queued output passes the per-client high-water mark is
// dropped. Closing the socket with nothing said leaves the client a bare EOF:
// it shows "Reconnecting" with no detail and dials straight back into the
// same backlog. The host instead discards what it had queued, keeps the rest
// of the frame the socket was partway through, so the stream stays framed,
// and appends one Bye{OutputBacklog}: a small frame written past the mark.
// The reap then waits for that Bye to drain, for a bounded time only, so a
// reader that never drains still loses its slot.
//
// Friend seam, same QUIC-OFF host build as overflowed_client_stays_unsynced:
// the client's socket end is real but never written, so the queue is the
// subject, and what the socket "already carried" is fed to the client by
// hand. Exit 0 on success.
//

#include "Assertions.h"

#include "Core/Protocol/Sync/Envelope.h"
#include "Core/Protocol/Sync/NativeAttachment.h"
#include "Host/ScreenClient.h"
#include "Host/ScreenPlaneAccess.h"
#include "PAL/UnixFileDescriptor.h"
#include "Platform/Host/Daemon.h"

#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <span>
#include <sys/socket.h>
#include <vector>

namespace Ubiquitty::Host {

struct BacklogDropAccess {
    /// Fills the client's output with whole canonical Probes, as a backlog of
    /// earlier frames would.
    ///
    /// - Returns: The bytes of one Probe frame.
    static std::size_t parkFrames(Daemon& d, std::size_t index, std::size_t bytes)
    {
        std::vector<std::uint8_t> body, frame;
        Protocol::Sync::encode(body, Protocol::Sync::Probe { .time = 1 });
        if (!Protocol::Sync::appendFrame(frame, Protocol::Sync::FrameKind::Probe, 0, body))
            return 0;
        auto& client = d.m_clients[index];
        client.clearPendingOutput();
        while (client.outputBuffer.size() < bytes)
            client.outputBuffer.insert(client.outputBuffer.end(), frame.begin(), frame.end());
        return frame.size();
    }
    /// Marks a prefix of the output as already written to the socket.
    static std::vector<std::uint8_t> write(Daemon& d, std::size_t index, std::size_t bytes)
    {
        auto& client = d.m_clients[index];
        client.outputConsumed = bytes;
        return { client.outputBuffer.begin(), client.outputBuffer.begin() + static_cast<std::ptrdiff_t>(bytes) };
    }
    static void queue(Daemon& d, std::size_t index, std::span<const std::uint8_t> bytes) { d.queueToClient(index, bytes); }
    static bool isReapDue(Daemon& d, std::size_t index, std::uint64_t now) { return d.isReapDue(d.m_clients[index], now); }
    static std::size_t retainedCapacity(const Daemon& d, std::size_t index) { return d.m_clients[index].outputBuffer.capacity(); }
};

} // namespace Ubiquitty::Host

using Ubiquitty::Host::Daemon;
using A = Ubiquitty::Host::BacklogDropAccess;
using Screen = Ubiquitty::Host::ScreenPlaneAccess;
namespace Sync = Ubiquitty::Protocol::Sync;

namespace {

std::uint64_t nowMilliseconds() { return Screen::now() / 1000; }

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

    // A backlog of whole frames, the socket partway through one of them.
    const std::size_t frameBytes = A::parkFrames(d, index, 1u << 20);
    REQUIRE(frameBytes > 2);
    client.read(A::write(d, index, 7 * frameBytes + 2), Screen::now());
    REQUIRE(client.connection().state() == Sync::ConnectionState::Attached);

    // A frame the mark cannot take.
    const std::vector<std::uint8_t> flood(Sync::maxPayloadLength + (1u << 20), 0u);
    A::queue(d, index, flood);
    CHECK_MSG(Screen::didOverflow(d, index), "a client over its high-water mark was not marked for the drop");
    // What stays is the rest of the frame in flight and the Bye: the backlog
    // and its allocation are gone.
    CHECK_MSG(Screen::pending(d, index) <= frameBytes + Sync::maxEnvelopeHeaderBytes + Sync::maxFrameBodyBytes / 64,
        "the dropped client's backlog was kept");
    CHECK_MSG(A::retainedCapacity(d, index) < (1u << 20), "the dropped client's backlog allocation was kept");

    // The reap waits for the Bye to leave, but not forever.
    const std::uint64_t now = nowMilliseconds();
    CHECK_MSG(!A::isReapDue(d, index, now), "the client was reaped before its Bye could leave");
    CHECK_MSG(A::isReapDue(d, index, now + 60'000), "a client that never drains its Bye holds its slot forever");

    // What the socket carries next completes the frame and says why.
    client.read(Screen::takeOutput(d, index), Screen::now());
    CHECK_MSG(client.connection().state() == Sync::ConnectionState::Closed, "the client was not closed by the host");
    const auto& bye = client.connection().bye();
    CHECK_MSG(bye.has_value(), "the client was dropped without a Bye, or the stream lost its framing");
    CHECK_MSG(bye && bye->reason == Sync::ByeReason::OutputBacklog, "the Bye did not name the backlog");
    CHECK_MSG(A::isReapDue(d, index, now), "a client whose Bye has left is not reaped");
}

} // namespace

int main()
{
    ::signal(SIGPIPE, SIG_IGN);
    run();
    if (failures == 0)
        std::printf("backlog_drop_says_why: a client pushed past its high-water mark keeps the frame in flight whole, is sent Bye{OutputBacklog} past the mark, and is reaped once that leaves or a bounded wait ends\n");
    return failures == 0 ? 0 : 1;
}
