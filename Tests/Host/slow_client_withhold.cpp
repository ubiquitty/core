// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0
//
// Tests/Host/slow_client_withhold.cpp — a client that cannot drain the
// session's frames is queued none, not dropped, and its screen converges.
//
// The per-client high-water (clientBufferHighWater) drops a client whose
// queued output reaches the cap; a thin client repainting in an interpreter
// under a flood would reach it in seconds if every frame were queued to it.
// The screen plane queues a client at most one frame's worth of output: while
// its output holds that much, the session's frames are owed to its connection,
// not written, and the first service after it drains sends one update from
// what it holds to the newest frame. So the queue never climbs to the cap, and
// the picture the client shows once it catches up is the session's — with no
// checkpoint sent to a client for the frames it missed.
//
// Friend seam, same QUIC-OFF host build as create_session_refusal: a client
// with a real socketpair end, whose peer is never read, parked by hand behind
// a backlog of other frames. Exit 0 on success.
//
#include "Assertions.h"
#include "LoopbackPaneLink.h"

#include "Core/Protocol/Sync/Envelope.h"
#include "Core/Terminal.h"
#include "Core/TerminalOperations.h"
#include "Host/ScreenClient.h"
#include "Host/ScreenPlaneAccess.h"
#include "PAL/UnixFileDescriptor.h"
#include "Platform/Host/Daemon.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <span>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace Ubiquitty::Host {
struct SlowClientWithholdAccess {
    // The session is a private type of the daemon, so the seam keeps it and
    // answers for it: its terminal, its id, and a publish of its screen.
    static Daemon::Session* session;
    static void useShell(Daemon& d, const std::string& path) { d.m_shellPath = path; }
    static bool createSession(Daemon& d)
    {
        session = d.createSession(80, 24, {}, -1, 0);
        return session != nullptr && session->term != nullptr;
    }
    static Ubiquitty::Terminal* terminal() { return session ? session->term.get() : nullptr; }
    static std::uint32_t sessionID() { return session ? session->id : 0; }
    // A client subscribed to the session, holding one end of a socketpair
    // nobody reads: the run loop is not running, so nothing is written to it
    // either; the queue is the subject.
    static std::size_t addSubscriber(Daemon& d, std::uint32_t sessionID, int fileDescriptor)
    {
        const auto index = ScreenPlaneAccess::attach(d, sessionID, PAL::UnixFileDescriptor(fileDescriptor));
        ScreenPlaneAccess::markTrustedLocal(d, index);
        return index;
    }
    static void park(Daemon& d, std::size_t index, std::size_t bytes)
    {
        d.m_clients[index].outputBuffer.assign(bytes, 0u);
        d.m_clients[index].outputConsumed = 0;
    }
    static void drain(Daemon& d, std::size_t index) { d.m_clients[index].clearPendingOutput(); }
    static std::size_t pending(const Daemon& d, std::size_t index) { return d.m_clients[index].pendingOutputBytes(); }
    static bool didOverflow(const Daemon& d, std::size_t index) { return d.m_clients[index].didOverflow; }
    static void destroySession(Daemon& d, std::uint32_t sessionID) { d.destroySession(sessionID); }
    static void flush(Daemon& d, std::size_t index) { d.flushClient(index); }
    static bool hasExitDeadline(const Daemon& d, std::size_t index) { return d.m_clients[index].sessionExitDeadlineMilliseconds != 0; }
    // The reap pass's question, asked as though the deadline had just passed.
    static bool isExitDrainOverAtDeadline(Daemon& d, std::size_t index)
    {
        auto& client = d.m_clients[index];
        return d.isSessionExitDrainOver(client, client.sessionExitDeadlineMilliseconds);
    }
    // The same question, asked a given time after the first deadline was armed.
    static bool isExitDrainOverAfter(Daemon& d, std::size_t index, std::uint64_t milliseconds)
    {
        auto& client = d.m_clients[index];
        const std::uint64_t now = client.sessionExitDrainStartMilliseconds + milliseconds;
        return now >= client.sessionExitDeadlineMilliseconds && d.isSessionExitDrainOver(client, now);
    }
    // Whole canonical Probes, so the backlog remains a decodable stream.
    static void parkFrames(Daemon& d, std::size_t index, std::size_t bytes)
    {
        std::vector<std::uint8_t> body, frame;
        Protocol::Sync::encode(body, Protocol::Sync::Probe { .time = 1 });
        CHECK(Protocol::Sync::appendFrame(frame, Protocol::Sync::FrameKind::Probe, 0, body));
        if (frame.empty())
            return;
        auto& client = d.m_clients[index];
        client.clearPendingOutput();
        while (client.outputBuffer.size() < bytes)
            client.outputBuffer.insert(client.outputBuffer.end(), frame.begin(), frame.end());
    }
};
Daemon::Session* SlowClientWithholdAccess::session = nullptr;
} // namespace Ubiquitty::Host

using Ubiquitty::Host::Daemon;
using A = Ubiquitty::Host::SlowClientWithholdAccess;

using Screen = Ubiquitty::Host::ScreenPlaneAccess;

int main()
{
    int pair[2] = { -1, -1 };
    CHECK_MSG(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0, "no socketpair for the client");
    // Non-blocking, as the daemon's accepted sockets are: the teardown flush
    // writes the queue to a peer nobody reads, and a blocking end would hold
    // the process there once the socket buffer is full.
    ::fcntl(pair[0], F_SETFL, ::fcntl(pair[0], F_GETFL) | O_NONBLOCK);
    Daemon d;
    A::useShell(d, "/bin/sh");
    CHECK_MSG(A::createSession(d), "the session was not made");
    const std::uint32_t sessionID = A::sessionID();
    const std::size_t client = A::addSubscriber(d, sessionID, pair[0]);
    UbiquittyTest::ScreenClient screen(24, 80);

    // Attached and reading, it holds the session's screen.
    Ubiquitty::processBytes(A::terminal(), "$ ");
    Screen::greet(d, client, screen);
    // The publish before any client bound captured nothing, so binding owes
    // the pane a frame; the one the rule takes next is the checkpoint.
    CHECK(A::session->isFrameDue);
    Screen::publish(d, *A::session, Ubiquitty::Runtime::FrameClass::Structural);
    Screen::deliver(d, client, screen);
    CHECK_MSG(screen.connection().counters().checkpointsApplied == 1, "the client was never sent its checkpoint");

    // Parked behind megabytes of other frames, the session's next frames queue
    // it nothing: its output already holds more than one frame. It is not
    // dropped.
    constexpr std::size_t parked = 5u * 1024u * 1024u;
    // The attach's own update, which tells the client the echo context, may
    // have gone already: what the backlog costs is counted from here.
    const std::uint64_t updatesBefore = screen.connection().counters().updatesApplied;
    // The peer reads nothing, so its socket is filled first: a flush the
    // daemon makes on the way, for a record a pane's facts owe, then moves
    // none of the backlog into the kernel, and what is pending is what is
    // queued. An empty socket takes a few kilobytes of it at the first flush,
    // which reads here as a frame queued when none was.
    {
        const char fill[4096] = {};
        while (::write(pair[0], fill, sizeof(fill)) > 0) {
        }
    }
    A::park(d, client, parked);
    for (int frame = 0; frame < 50; ++frame) {
        Ubiquitty::processBytes(A::terminal(), "\r\nwithheld " + std::to_string(frame));
        A::session->isFrameDue = true;
        Screen::publish(d, *A::session);
        Screen::service(d);
    }
    CHECK_MSG(A::pending(d, client) == parked, "a client whose output holds a backlog was queued a frame");
    CHECK_MSG(!A::didOverflow(d, client), "a client behind a backlog was marked for the drop");

    // Drained, and with nothing further written, the next service sends it the
    // frame it is owed: one update from the screen it holds to the newest.
    // Asking the terminal to change again would hide the defect this covers:
    // the moment a flood ends is the moment the session stops changing, so a
    // send that waits for a change leaves the client on its stale screen until
    // someone types.
    A::drain(d, client);
    CHECK_MSG(!A::session->isFrameDue, "the session was still due, so the next check proves nothing");
    Screen::deliver(d, client, screen);
    CHECK_MSG(screen.generation() == A::session->screenSource->generation(), "the drained client was not brought to the newest frame");
    const std::string difference = UbiquittyTest::firstDisagreement(*A::terminal(), *screen.pane());
    CHECK_MSG(difference.empty(), difference.c_str());
    CHECK_MSG(screen.count(Ubiquitty::Protocol::Sync::FrameKind::CheckpointHead) == 1, "a client that missed frames was sent a checkpoint rather than an update");
    CHECK_MSG(screen.connection().counters().updatesApplied == updatesBefore + 1, ("the drained client was replayed the frames it missed: " + std::to_string(screen.connection().counters().updatesApplied - updatesBefore) + " updates, " + std::to_string(Screen::connection(d, client)->counters().chainUpdatesSent) + " sent").c_str());

    // The session exits while a slow reader still owes megabytes of frames. Its
    // PaneUnbind and Bye sit behind them, and a deadline that passes while it is
    // still reading must not reap it: the reap discards them and leaves a bare
    // EOF, which the client takes for a dropped link. A reader that stalls is
    // still reaped.
    {
        int slowPair[2] = { -1, -1 };
        CHECK_MSG(::socketpair(AF_UNIX, SOCK_STREAM, 0, slowPair) == 0, "no socketpair for the slow reader");
        ::fcntl(slowPair[0], F_SETFL, ::fcntl(slowPair[0], F_GETFL) | O_NONBLOCK);
        const std::size_t slow = A::addSubscriber(d, sessionID, slowPair[0]);
        UbiquittyTest::ScreenClient slowScreen(24, 80);
        Screen::greet(d, slow, slowScreen);
        CHECK(slowScreen.connection().state() == Ubiquitty::Protocol::Sync::ConnectionState::Attached);
        const auto slowHandle = slowScreen.connection().handleForPane(A::session->screenSource->paneID());
        CHECK(slowHandle != 0);
        A::parkFrames(d, slow, 4u * 1024u * 1024u);
        // Two more readers owing the same backlog: one that reads nothing and
        // one that reads a few kilobytes a window. Small socket buffers, so what the
        // kernel takes off the queue is not mistaken for reading.
        const auto addLaggard = [&d, sessionID](int (&laggardPair)[2]) {
            CHECK_MSG(::socketpair(AF_UNIX, SOCK_STREAM, 0, laggardPair) == 0, "no socketpair for a laggard");
            const int socketBufferBytes = 4096;
            for (const int end : laggardPair) {
                ::setsockopt(end, SOL_SOCKET, SO_SNDBUF, &socketBufferBytes, sizeof(socketBufferBytes));
                ::setsockopt(end, SOL_SOCKET, SO_RCVBUF, &socketBufferBytes, sizeof(socketBufferBytes));
            }
            ::fcntl(laggardPair[0], F_SETFL, ::fcntl(laggardPair[0], F_GETFL) | O_NONBLOCK);
            const std::size_t index = A::addSubscriber(d, sessionID, laggardPair[0]);
            UbiquittyTest::ScreenClient laggard(24, 80);
            Screen::greet(d, index, laggard);
            CHECK(laggard.connection().state() == Ubiquitty::Protocol::Sync::ConnectionState::Attached);
            CHECK(laggard.connection().handleForPane(A::session->screenSource->paneID()) != 0);
            A::parkFrames(d, index, 4u * 1024u * 1024u);
            return index;
        };
        // The test's ends stay open for the daemon's life: its teardown writes
        // to every client.
        int stalledPair[2] = { -1, -1 };
        int tricklePair[2] = { -1, -1 };
        const std::size_t stalled = addLaggard(stalledPair);
        const std::size_t trickle = addLaggard(tricklePair);
        A::destroySession(d, sessionID);
        CHECK_MSG(A::hasExitDeadline(d, slow), "the session exit armed no drain deadline");
        CHECK_MSG(A::pending(d, slow) > 0, "premise: the socket took the whole backlog");

        A::flush(d, stalled);
        CHECK_MSG(A::isExitDrainOverAtDeadline(d, stalled), "a reader that read nothing was kept past its deadline");
        for (int windows = 0; windows < 3; ++windows) {
            std::uint8_t trickled[4096];
            (void)::recv(tricklePair[1], trickled, sizeof trickled, MSG_DONTWAIT);
            A::flush(d, trickle);
            if (A::isExitDrainOverAtDeadline(d, trickle))
                break;
            CHECK_MSG(windows < 2, "a reader taking a trickle a window kept its slot window after window");
        }
        CHECK_MSG(A::isExitDrainOverAtDeadline(d, trickle), "a reader taking a trickle a window was not reaped");

        std::vector<std::uint8_t> received;
        const auto readAvailable = [&] {
            std::uint8_t chunk[65536];
            for (;;) {
                const ssize_t n = ::recv(slowPair[1], chunk, sizeof chunk, MSG_DONTWAIT);
                if (n <= 0)
                    break;
                received.insert(received.end(), chunk, chunk + n);
            }
        };
        const std::size_t owedAtExit = A::pending(d, slow);
        for (int rounds = 0; owedAtExit - A::pending(d, slow) < 256u * 1024u && A::pending(d, slow) > 0 && rounds < 10000; ++rounds) {
            readAvailable();
            A::flush(d, slow);
        }
        CHECK_MSG(!A::isExitDrainOverAtDeadline(d, slow), "a reader still draining was reaped at the deadline");
        // However fast it reads, its extensions stop at the ceiling.
        const std::size_t owedAtExtension = A::pending(d, slow);
        for (int rounds = 0; owedAtExtension - A::pending(d, slow) < 256u * 1024u && A::pending(d, slow) > 0 && rounds < 10000; ++rounds) {
            readAvailable();
            A::flush(d, slow);
        }
        CHECK_MSG(A::pending(d, slow) > 0, "premise: the reader still owes bytes at the ceiling");
        CHECK_MSG(A::isExitDrainOverAfter(d, slow, 60'000), "a reader still draining a minute after its session exited kept its slot");

        int rounds = 0;
        while (A::pending(d, slow) > 0 && rounds++ < 10000) {
            readAvailable();
            A::flush(d, slow);
        }
        readAvailable();
        CHECK_MSG(A::pending(d, slow) == 0, "the backlog did not drain");
        namespace Sync = Ubiquitty::Protocol::Sync;
        Sync::FrameDecoder decoder;
        decoder.append(received);
        bool sawUnbind = false;
        bool sawBye = false;
        Sync::FrameKind lastKind = Sync::FrameKind::Probe;
        while (const auto frame = decoder.next()) {
            lastKind = frame->kind;
            if (frame->kind == Sync::FrameKind::PaneUnbind) {
                const auto unbind = Sync::decodePaneUnbind(frame->body);
                CHECK(unbind && unbind->handle == slowHandle && !sawBye);
                sawUnbind |= unbind && unbind->handle == slowHandle;
            } else if (frame->kind == Sync::FrameKind::Bye) {
                const auto bye = Sync::decodeBye(frame->body);
                CHECK(bye && bye->reason == Sync::ByeReason::Normal && sawUnbind);
                sawBye = bye && bye->reason == Sync::ByeReason::Normal;
            }
        }
        CHECK(decoder.error() == Sync::EnvelopeError::None && decoder.bufferedBytes() == 0);
        CHECK_MSG(sawUnbind && sawBye && lastKind == Sync::FrameKind::Bye, "the reader never received its ordered PaneUnbind and Normal Bye");
        CHECK_MSG(A::isExitDrainOverAtDeadline(d, slow), "a client with nothing left to read was not reaped at its deadline");
    }

    if (failures == 0)
        std::printf("slow_client_withhold: a client behind a backlog is queued no frame and keeps its place, and the first service after it drains brings it to the newest frame in one update\n");
    return failures == 0 ? 0 : 1;
}
